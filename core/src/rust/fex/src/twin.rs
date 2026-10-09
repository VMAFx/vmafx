// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// The `Extractor` trait a Rust twin implements and the generic C entry points
// (`twin::<E>()`) that adapt it to the ABI the shim calls.

use core::ffi::{CStr, c_char, c_void};

use crate::abi::{
    VMAFX_RS_ABI_VERSION, VMAFX_RS_DONE, VMAFX_RS_OK, VmafxRsFrame, VmafxRsGeometry, VmafxRsHost,
    VmafxRsOption, VmafxRsPicture, VmafxRsTwin,
};
use crate::error::Error;
use crate::host::Host;
use crate::options::{FromOptions, OptionValues};
use crate::picture::{Geometry, Picture};

/// What `flush` asks the framework to do next (the C `flush` return value).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Flush {
    /// Call `flush` again (C returns 0).
    Again,
    /// Finished (C returns a positive value).
    Done,
}

/// The pictures of one `extract` call.
#[derive(Clone, Copy)]
pub struct Frame<'a> {
    /// Frame index.
    pub index: u32,
    /// Reference picture.
    pub reference: Picture<'a>,
    /// Distorted picture.
    pub distorted: Picture<'a>,
    /// Reference of frame n-1 when the framework holds one (PREV_REF).
    pub prev_ref: Option<Picture<'a>>,
    /// Reference of frame n-2 when the framework holds one (ADR-1478).
    pub prev_prev_ref: Option<Picture<'a>>,
}

impl<'a> Frame<'a> {
    /// Wrap the ABI frame.
    ///
    /// # Safety
    ///
    /// `ref_pic` and `dist_pic` are valid; the two previous pictures are valid
    /// or NULL; every picture satisfies `Picture::from_abi` for `'a`.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` when a current picture is NULL.
    pub unsafe fn from_abi(f: &'a VmafxRsFrame) -> Result<Self, Error> {
        // SAFETY: the caller guarantees each pointer is valid for 'a or NULL.
        let (r, d, p1, p2) = unsafe {
            (
                f.ref_pic.as_ref(),
                f.dist_pic.as_ref(),
                f.prev_ref.as_ref(),
                f.prev_prev_ref.as_ref(),
            )
        };
        let (Some(r), Some(d)) = (r, d) else {
            return Err(Error::InvalidArgument(
                c"frame without reference or distorted picture",
            ));
        };
        // SAFETY: the caller guarantees the planes of every non-NULL picture for 'a.
        let wrap = |p: &'a VmafxRsPicture| unsafe { Picture::from_abi(p) };
        Ok(Self {
            index: f.index,
            reference: wrap(r),
            distorted: wrap(d),
            prev_ref: p1.map(wrap),
            prev_prev_ref: p2.map(wrap),
        })
    }
}

/// A Rust twin of a C feature extractor. `close` runs, then the state drops.
pub trait Extractor: Sized + Send {
    /// The options, read from the C extractor's table.
    type Options: FromOptions;

    /// After option parsing, before the first frame: allocate every buffer.
    ///
    /// # Errors
    ///
    /// What the C `init` refuses, with the same meaning.
    fn init(opts: &Self::Options, geom: &Geometry) -> Result<Self, Error>;

    /// One frame; emit what the C `extract` emits, in the same order.
    ///
    /// # Errors
    ///
    /// What the C `extract` refuses, or a host error.
    fn extract(&mut self, frame: &Frame<'_>, host: &mut Host<'_>) -> Result<(), Error>;

    /// Mirrors the C `flush`. Only called when the C extractor has one.
    ///
    /// # Errors
    ///
    /// What the C `flush` refuses, or a host error.
    fn flush(&mut self, _host: &mut Host<'_>) -> Result<Flush, Error> {
        Ok(Flush::Done)
    }

    /// Mirrors the C `advance` (ADR-2090): append every score the
    /// collector's contents now make final, never one a later frame could
    /// still change; `flush` appends the rest. Called on the thread that
    /// feeds frames, never next to `extract` or `flush` of this state, and
    /// only when the C extractor has an `advance`. Default: nothing, so
    /// `flush` appends every score as before.
    ///
    /// # Errors
    ///
    /// What the C `advance` refuses, or a host error.
    fn advance(&mut self, _host: &mut Host<'_>) -> Result<(), Error> {
        Ok(())
    }

    /// Mirrors what the C `close` reports (log lines) before the state is
    /// dropped. The host is log-only: emitting fails.
    fn close(&mut self, _host: &Host<'_>) {}
}

/// Store the error message for the shim and return the status.
///
/// # Safety
///
/// `message` is NULL or valid for one write.
unsafe fn report(e: Error, message: *mut *const c_char) -> i32 {
    if !message.is_null() {
        // SAFETY: the caller guarantees a non-NULL `message` is writable.
        unsafe { *message = e.message().as_ptr() };
    }
    e.status()
}

fn init_inner<E: Extractor>(
    opts: &OptionValues<'_>,
    geom: &VmafxRsGeometry,
) -> Result<Box<E>, Error> {
    let geometry = Geometry::from_abi(geom)?;
    let parsed = E::Options::from_options(opts)?;
    Ok(Box::new(E::init(&parsed, &geometry)?))
}

/// # Safety
///
/// The shim's contract: `state` writable, `opts`/`n_opts` as for
/// `OptionValues::from_raw`, `geom` valid, `message` NULL or writable.
unsafe extern "C" fn init_tramp<E: Extractor>(
    state: *mut *mut c_void,
    opts: *const VmafxRsOption,
    n_opts: usize,
    geom: *const VmafxRsGeometry,
    message: *mut *const c_char,
) -> i32 {
    if state.is_null() || geom.is_null() {
        // SAFETY: forwarded caller contract on `message`.
        return unsafe { report(Error::InvalidArgument(c"init: NULL argument"), message) };
    }
    // SAFETY: the shim passes `n_opts` options valid for this call.
    let values = match unsafe { OptionValues::from_raw(opts, n_opts) } {
        Ok(v) => v,
        // SAFETY: forwarded caller contract on `message`.
        Err(e) => return unsafe { report(e, message) },
    };
    // SAFETY: `geom` is non-NULL and valid for this call (shim contract).
    match init_inner::<E>(&values, unsafe { &*geom }) {
        Ok(b) => {
            // SAFETY: `state` is non-NULL and writable (shim contract).
            unsafe { *state = Box::into_raw(b).cast::<c_void>() };
            VMAFX_RS_OK
        }
        // SAFETY: forwarded caller contract on `message`.
        Err(e) => unsafe { report(e, message) },
    }
}

/// # Safety
///
/// `state` came from `init_tramp::<E>`; `frame` and `host` are valid for the
/// call as `Frame::from_abi` and `Host::from_abi` require.
unsafe fn extract_inner<E: Extractor>(
    state: *mut c_void,
    frame: *const VmafxRsFrame,
    host: *const VmafxRsHost,
) -> Result<(), Error> {
    // SAFETY: caller contract: valid or NULL pointers for this call.
    let (Some(e), Some(f), Some(h)) = (
        unsafe { state.cast::<E>().as_mut() },
        unsafe { frame.as_ref() },
        unsafe { host.as_ref() },
    ) else {
        return Err(Error::InvalidArgument(c"extract: NULL argument"));
    };
    // SAFETY: caller contract on the frame's pictures and the host's callbacks.
    let (frame, mut host) = unsafe { (Frame::from_abi(f)?, Host::from_abi(h)?) };
    e.extract(&frame, &mut host)
}

/// # Safety
///
/// As `extract_inner`; `message` NULL or writable.
unsafe extern "C" fn extract_tramp<E: Extractor>(
    state: *mut c_void,
    frame: *const VmafxRsFrame,
    host: *const VmafxRsHost,
    message: *mut *const c_char,
) -> i32 {
    // SAFETY: forwarded shim contract.
    match unsafe { extract_inner::<E>(state, frame, host) } {
        Ok(()) => VMAFX_RS_OK,
        // SAFETY: forwarded caller contract on `message`.
        Err(e) => unsafe { report(e, message) },
    }
}

/// # Safety
///
/// `state` came from `init_tramp::<E>`; `host` valid for the call; `message`
/// NULL or writable.
unsafe extern "C" fn flush_tramp<E: Extractor>(
    state: *mut c_void,
    host: *const VmafxRsHost,
    message: *mut *const c_char,
) -> i32 {
    // SAFETY: shim contract: valid or NULL pointers for this call.
    let (Some(e), Some(h)) = (unsafe { state.cast::<E>().as_mut() }, unsafe {
        host.as_ref()
    }) else {
        // SAFETY: forwarded caller contract on `message`.
        return unsafe { report(Error::InvalidArgument(c"flush: NULL argument"), message) };
    };
    // SAFETY: shim contract on the host callbacks.
    let result = unsafe { Host::from_abi(h) }.and_then(|mut host| e.flush(&mut host));
    match result {
        Ok(Flush::Again) => VMAFX_RS_OK,
        Ok(Flush::Done) => VMAFX_RS_DONE,
        // SAFETY: forwarded caller contract on `message`.
        Err(err) => unsafe { report(err, message) },
    }
}

/// # Safety
///
/// `state` came from `init_tramp::<E>`; `host` valid for the call; `message`
/// NULL or writable.
unsafe extern "C" fn advance_tramp<E: Extractor>(
    state: *mut c_void,
    host: *const VmafxRsHost,
    message: *mut *const c_char,
) -> i32 {
    // SAFETY: shim contract: valid or NULL pointers for this call.
    let (Some(e), Some(h)) = (unsafe { state.cast::<E>().as_mut() }, unsafe {
        host.as_ref()
    }) else {
        // SAFETY: forwarded caller contract on `message`.
        return unsafe { report(Error::InvalidArgument(c"advance: NULL argument"), message) };
    };
    // SAFETY: shim contract on the host callbacks.
    let result = unsafe { Host::from_abi(h) }.and_then(|mut host| e.advance(&mut host));
    match result {
        Ok(()) => VMAFX_RS_OK,
        // SAFETY: forwarded caller contract on `message`.
        Err(err) => unsafe { report(err, message) },
    }
}

/// # Safety
///
/// `state` is NULL or came from `init_tramp::<E>` and is not used afterwards;
/// `host` is NULL or valid for the call.
unsafe extern "C" fn close_tramp<E: Extractor>(state: *mut c_void, host: *const VmafxRsHost) {
    if state.is_null() {
        return;
    }
    // SAFETY: `state` is the Box `init_tramp::<E>` leaked; ownership returns here once.
    let mut e = unsafe { Box::from_raw(state.cast::<E>()) };
    // SAFETY: shim contract: a non-NULL host is valid for this call.
    if let Some(h) = unsafe { host.as_ref() } {
        // SAFETY: shim contract on the host callbacks.
        if let Ok(host) = unsafe { Host::from_abi(h) } {
            e.close(&host);
        }
    }
    drop(e);
}

/// The ABI entry of the twin `E` of the C extractor `c_name`. Use in a lane
/// crate's `pub const TWINS`.
#[must_use]
pub const fn twin<E: Extractor>(c_name: &'static CStr, rust_name: &'static CStr) -> VmafxRsTwin {
    VmafxRsTwin {
        abi_version: VMAFX_RS_ABI_VERSION,
        c_name: c_name.as_ptr(),
        rust_name: rust_name.as_ptr(),
        init: Some(init_tramp::<E>),
        extract: Some(extract_tramp::<E>),
        flush: Some(flush_tramp::<E>),
        close: Some(close_tramp::<E>),
        advance: Some(advance_tramp::<E>),
    }
}
