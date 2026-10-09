// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// C ABI of the Rust feature-extractor twins (ADR-1713).
//
// `core/src/rust/include/vmafx_rs.h` is generated from this file by cbindgen
// (`scripts/dev/rust-abi-header.sh`) and committed. The field order of every
// struct is the contract the C shim (`core/src/rust/shim/rust_twins.cpp`)
// relies on; `vmafx_rs_abi_layout()` reports it for the layout test.

use core::ffi::{c_char, c_void};

/// Version of this ABI. The shim refuses a twin with another value. It stays 1
/// until a release ships the ABI: `VmafxRsTwin.advance` (ADR-2090, lane request
/// MI-1) joined the layout before any release did.
pub const VMAFX_RS_ABI_VERSION: u32 = 1;

/// Success.
pub const VMAFX_RS_OK: i32 = 0;
/// `flush`: the extractor has nothing more to emit.
pub const VMAFX_RS_DONE: i32 = 1;
/// Invalid argument (`-EINVAL`).
pub const VMAFX_RS_E_INVAL: i32 = -1;
/// Valid but unsupported option or input (`-ENOTSUP`).
pub const VMAFX_RS_E_NOTSUP: i32 = -2;
/// Allocation failed (`-ENOMEM`).
pub const VMAFX_RS_E_NOMEM: i32 = -3;
/// Value out of range (`-ERANGE`).
pub const VMAFX_RS_E_RANGE: i32 = -4;
/// A score is not finite (`-EINVAL`, as `vmaf_feature_emit_finite_scores`).
pub const VMAFX_RS_E_NONFINITE: i32 = -5;
/// A host callback failed; the shim returns the errno it recorded.
pub const VMAFX_RS_E_HOST: i32 = -6;

/// `VmafLogLevel` values, as `libvmaf.h` numbers them.
pub const VMAFX_RS_LOG_ERROR: i32 = 1;
/// Warning.
pub const VMAFX_RS_LOG_WARNING: i32 = 2;
/// Info.
pub const VMAFX_RS_LOG_INFO: i32 = 3;
/// Debug.
pub const VMAFX_RS_LOG_DEBUG: i32 = 4;

/// `VmafOptionType` values, as `core/src/opt.h` numbers them.
pub const VMAFX_RS_OPT_BOOL: u32 = 0;
/// Integer option.
pub const VMAFX_RS_OPT_INT: u32 = 1;
/// Double option.
pub const VMAFX_RS_OPT_DOUBLE: u32 = 2;
/// String option (`s` may be NULL).
pub const VMAFX_RS_OPT_STRING: u32 = 3;

/// One parsed option of the C extractor's table, read back from its priv.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VmafxRsOption {
    /// Option name as in the C table (never the alias).
    pub name: *const c_char,
    /// `VMAFX_RS_OPT_*`.
    pub kind: u32,
    /// Bool value (0 / 1) when `kind` is bool.
    pub b: i32,
    /// Int value when `kind` is int.
    pub i: i32,
    /// Double value when `kind` is double.
    pub d: f64,
    /// String value when `kind` is string; may be NULL.
    pub s: *const c_char,
}

/// The geometry `init` receives (`VmafFeatureExtractor.init` arguments).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VmafxRsGeometry {
    /// `VmafPixelFormat` value.
    pub pix_fmt: u32,
    /// Bits per component.
    pub bpc: u32,
    /// Luma width.
    pub w: u32,
    /// Luma height.
    pub h: u32,
}

/// One plane of a picture, borrowed for one call.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VmafxRsPlane {
    /// First sample; `uint8_t` when bpc <= 8, `uint16_t` otherwise.
    pub data: *const c_void,
    /// Row stride in bytes.
    pub stride: isize,
    /// Width in samples.
    pub w: u32,
    /// Height in rows.
    pub h: u32,
}

/// A picture, borrowed for one call (`VmafPicture` without its internals).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VmafxRsPicture {
    /// `VmafPixelFormat` value.
    pub pix_fmt: u32,
    /// Bits per component.
    pub bpc: u32,
    /// Number of valid entries in `plane` (1 for YUV400P, else 3).
    pub n_planes: u32,
    /// Planes; index 0 = Y, 1 = U, 2 = V.
    pub plane: [VmafxRsPlane; 3],
}

/// The pictures of one `extract` call.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VmafxRsFrame {
    /// Frame index.
    pub index: u32,
    /// Reference picture.
    pub ref_pic: *const VmafxRsPicture,
    /// Distorted picture.
    pub dist_pic: *const VmafxRsPicture,
    /// Reference of frame n-1 (`fex->prev_ref`); NULL when empty.
    pub prev_ref: *const VmafxRsPicture,
    /// Reference of frame n-2 (`fex->prev_prev_ref`); NULL when empty.
    pub prev_prev_ref: *const VmafxRsPicture,
}

/// Feature-collector callbacks of the shim. Names are the BASE names of the C
/// extractor's `provided_features`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VmafxRsHost {
    /// Shim context passed back to every callback.
    pub ctx: *mut c_void,
    /// `vmaf_feature_collector_append_with_dict` (option-decorated name).
    pub emit: Option<unsafe extern "C" fn(*mut c_void, *const c_char, u32, f64) -> i32>,
    /// `vmaf_feature_collector_append` (name as given).
    pub emit_raw: Option<unsafe extern "C" fn(*mut c_void, *const c_char, u32, f64) -> i32>,
    /// `vmaf_feature_collector_get_score` on the decorated name; 0 = found.
    pub get: Option<unsafe extern "C" fn(*mut c_void, *const c_char, u32, *mut f64) -> i32>,
    /// `vmaf_feature_collector_set_aggregate` (name as given).
    pub set_aggregate: Option<unsafe extern "C" fn(*mut c_void, *const c_char, f64) -> i32>,
    /// `vmaf_log(level, "%s\n", message)`; `level` is a `VMAFX_RS_LOG_*`
    /// value (`VmafLogLevel`). Also valid in the host `close` receives.
    pub log: Option<unsafe extern "C" fn(*mut c_void, i32, *const c_char)>,
}

/// One Rust twin of a C feature extractor.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VmafxRsTwin {
    /// `VMAFX_RS_ABI_VERSION` the twin was built against.
    pub abi_version: u32,
    /// Name of the C extractor it replaces (`"cambi"`).
    pub c_name: *const c_char,
    /// Name of the twin (`"cambi_rust"`).
    pub rust_name: *const c_char,
    /// Create the state from the options and the geometry; on error store a
    /// static NUL-terminated message in the last argument.
    pub init: Option<
        unsafe extern "C" fn(
            *mut *mut c_void,
            *const VmafxRsOption,
            usize,
            *const VmafxRsGeometry,
            *mut *const c_char,
        ) -> i32,
    >,
    /// Extract one frame.
    pub extract: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const VmafxRsFrame,
            *const VmafxRsHost,
            *mut *const c_char,
        ) -> i32,
    >,
    /// Flush; `VMAFX_RS_OK` = call again, `VMAFX_RS_DONE` = finished.
    pub flush:
        Option<unsafe extern "C" fn(*mut c_void, *const VmafxRsHost, *mut *const c_char) -> i32>,
    /// Free the state. The host is log-only: its collector callbacks fail,
    /// because the collector may be gone when a context closes.
    pub close: Option<unsafe extern "C" fn(*mut c_void, *const VmafxRsHost)>,
    /// Append the scores the collector's contents now make final (ADR-2090);
    /// the shim calls it only when the C extractor has an `advance`. On the
    /// thread that feeds frames, never next to `extract` or `flush` of the
    /// same state.
    pub advance:
        Option<unsafe extern "C" fn(*mut c_void, *const VmafxRsHost, *mut *const c_char) -> i32>,
}

// SAFETY: a VmafxRsTwin holds pointers to NUL-terminated string literals with
// 'static lifetime and to functions; nothing it points to is ever written, so
// sharing it between threads cannot race.
unsafe impl Sync for VmafxRsTwin {}

/// Number of `usize` entries `vmafx_rs_abi_layout` writes.
pub const VMAFX_RS_ABI_LAYOUT_LEN: usize = 39;

/// The sizes and field offsets of every ABI struct, in the order
/// `core/test/test_rust_abi_layout.c` lists them.
#[must_use]
pub const fn abi_layout() -> [usize; VMAFX_RS_ABI_LAYOUT_LEN] {
    use core::mem::{offset_of, size_of};
    [
        size_of::<VmafxRsOption>(),
        offset_of!(VmafxRsOption, name),
        offset_of!(VmafxRsOption, kind),
        offset_of!(VmafxRsOption, b),
        offset_of!(VmafxRsOption, i),
        offset_of!(VmafxRsOption, d),
        offset_of!(VmafxRsOption, s),
        size_of::<VmafxRsGeometry>(),
        offset_of!(VmafxRsGeometry, pix_fmt),
        offset_of!(VmafxRsGeometry, bpc),
        offset_of!(VmafxRsGeometry, w),
        offset_of!(VmafxRsGeometry, h),
        size_of::<VmafxRsPlane>(),
        offset_of!(VmafxRsPlane, data),
        offset_of!(VmafxRsPlane, stride),
        offset_of!(VmafxRsPlane, w),
        offset_of!(VmafxRsPlane, h),
        size_of::<VmafxRsPicture>(),
        offset_of!(VmafxRsPicture, n_planes),
        offset_of!(VmafxRsPicture, plane),
        size_of::<VmafxRsFrame>(),
        offset_of!(VmafxRsFrame, index),
        offset_of!(VmafxRsFrame, ref_pic),
        offset_of!(VmafxRsFrame, dist_pic),
        offset_of!(VmafxRsFrame, prev_ref),
        offset_of!(VmafxRsFrame, prev_prev_ref),
        size_of::<VmafxRsHost>(),
        offset_of!(VmafxRsHost, emit),
        offset_of!(VmafxRsHost, emit_raw),
        offset_of!(VmafxRsHost, get),
        offset_of!(VmafxRsHost, set_aggregate),
        offset_of!(VmafxRsHost, log),
        size_of::<VmafxRsTwin>(),
        offset_of!(VmafxRsTwin, c_name),
        offset_of!(VmafxRsTwin, rust_name),
        offset_of!(VmafxRsTwin, init),
        offset_of!(VmafxRsTwin, flush),
        offset_of!(VmafxRsTwin, close),
        offset_of!(VmafxRsTwin, advance),
    ]
}

/// The ABI version this archive was built with.
#[unsafe(no_mangle)]
pub extern "C" fn vmafx_rs_abi_version() -> u32 {
    VMAFX_RS_ABI_VERSION
}

/// Write up to `n` entries of the ABI layout table (sizes and field offsets,
/// in the order of `abi_layout`) to `out`; return the table length.
///
/// # Safety
///
/// `out` is NULL or valid for `n` writes of `size_t`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmafx_rs_abi_layout(out: *mut usize, n: usize) -> usize {
    let table = abi_layout();
    if !out.is_null() {
        let count = n.min(VMAFX_RS_ABI_LAYOUT_LEN);
        // SAFETY: the caller guarantees `out` is valid for `n` writes; `count <= n`.
        let dst = unsafe { core::slice::from_raw_parts_mut(out, count) };
        dst.copy_from_slice(&table[..count]);
    }
    VMAFX_RS_ABI_LAYOUT_LEN
}
