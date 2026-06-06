// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause
//
// vmafx-sys/src/safe.rs — thin safe Rust wrapper over the raw libvmaf FFI.
//
// Design principles:
//   - `unsafe` is used only at the FFI call sites themselves.
//   - All public functions return `Result<T, VmafxError>` so callers can use `?`.
//   - Ownership follows RAII: `VmafContext` closes the underlying context on drop.
//   - `VmafModel` destroys the C model on drop.

// This module is hand-written (not generated); enforce the unsafe-op lint so
// every unsafe operation inside a safe function body must be explicitly wrapped
// in its own `unsafe {}` block.
#![deny(unsafe_op_in_unsafe_fn)]

use std::ffi::{CStr, CString};
use std::ptr;

use crate::{
    vmaf_close, vmaf_init, vmaf_model_destroy, vmaf_model_load_from_path, vmaf_picture_alloc,
    vmaf_picture_unref, vmaf_read_pictures, vmaf_score_pooled, vmaf_use_features_from_model,
    VmafConfiguration, VmafContext as RawVmafContext, VmafLogLevel_VMAF_LOG_LEVEL_WARNING,
    VmafModel as RawVmafModel, VmafModelConfig, VmafPicture, VmafPixelFormat_VMAF_PIX_FMT_YUV420P,
    VmafPoolingMethod_VMAF_POOL_METHOD_MEAN,
};

/// Errors returned by the safe wrapper.
#[derive(Debug)]
pub enum VmafxError {
    /// libvmaf returned a negative errno value.
    LibvmafError(i32),
    /// A string passed to the API contained an interior NUL byte.
    NulError(std::ffi::NulError),
    /// An operation was attempted that requires a resource that has been consumed.
    InvalidState(&'static str),
}

impl std::fmt::Display for VmafxError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            VmafxError::LibvmafError(code) => write!(f, "libvmaf error: {code}"),
            VmafxError::NulError(e) => write!(f, "nul error in string: {e}"),
            VmafxError::InvalidState(msg) => write!(f, "invalid state: {msg}"),
        }
    }
}

impl std::error::Error for VmafxError {}

impl From<std::ffi::NulError> for VmafxError {
    fn from(e: std::ffi::NulError) -> Self {
        VmafxError::NulError(e)
    }
}

fn check(rc: i32) -> Result<(), VmafxError> {
    if rc >= 0 {
        Ok(())
    } else {
        Err(VmafxError::LibvmafError(rc))
    }
}

// ---------------------------------------------------------------------------
// VmafContext
// ---------------------------------------------------------------------------

/// RAII wrapper around a `*mut VmafContext`.
pub struct VmafContext {
    inner: *mut RawVmafContext,
}

// Safety: libvmaf contexts are self-contained; no thread-local state is accessed.
unsafe impl Send for VmafContext {}

// NOTE: `Default` is intentionally NOT implemented for `VmafContext`.
// A panicking `Default` impl (i.e. `Self::new().unwrap()`) would hide
// allocation failures behind a generic trait call with no ergonomic error
// path.  Callers that want a context with default settings should call
// `VmafContext::new()` directly and handle the `Result`.

impl VmafContext {
    /// Allocate and open a new VMAF context with sensible defaults.
    ///
    /// - `n_threads = 0` lets libvmaf choose the number of threads.
    /// - Log level is `WARNING` to suppress per-frame debug spam.
    pub fn new() -> Result<Self, VmafxError> {
        let cfg = VmafConfiguration {
            log_level: VmafLogLevel_VMAF_LOG_LEVEL_WARNING,
            n_threads: 0,
            n_subsample: 1,
            cpumask: 0,
            gpumask: 0,
        };
        let mut ctx: *mut RawVmafContext = ptr::null_mut();
        // SAFETY: `cfg` is fully initialised; `ctx` is a valid out-pointer.
        check(unsafe { vmaf_init(&mut ctx, cfg) })?;
        Ok(VmafContext { inner: ctx })
    }

    /// Return the raw pointer. For use in advanced FFI scenarios.
    pub fn as_ptr(&mut self) -> *mut RawVmafContext {
        self.inner
    }

    /// Register all feature extractors required by `model`.
    pub fn use_features_from_model(&mut self, model: &mut VmafModel) -> Result<(), VmafxError> {
        // SAFETY: both pointers are valid for the duration of the call.
        check(unsafe { vmaf_use_features_from_model(self.inner, model.inner) })
    }

    /// Queue a pair of pictures for feature extraction at `index`.
    ///
    /// Ownership of both pictures is transferred to the context; they will be
    /// unref'd by libvmaf internally.
    pub fn read_pictures(
        &mut self,
        ref_pic: &mut VmafPicture,
        dist_pic: &mut VmafPicture,
        index: u32,
    ) -> Result<(), VmafxError> {
        // SAFETY: both VmafPicture pointers are valid; libvmaf takes ownership.
        check(unsafe { vmaf_read_pictures(self.inner, ref_pic, dist_pic, index) })
    }

    /// Flush the feature extraction pipeline (call after all frames are queued).
    pub fn flush(&mut self) -> Result<(), VmafxError> {
        // Passing NULL, NULL signals EOF to the feature extractor.
        // SAFETY: NULL is the documented sentinel for "flush".
        check(unsafe { vmaf_read_pictures(self.inner, ptr::null_mut(), ptr::null_mut(), 0) })
    }

    /// Compute the mean-pooled VMAF score over `index_low..=index_high`.
    pub fn score_pooled(
        &mut self,
        model: &mut VmafModel,
        index_low: u32,
        index_high: u32,
    ) -> Result<f64, VmafxError> {
        let mut score = 0.0f64;
        // SAFETY: `score` is a valid out-pointer; the other args are valid.
        check(unsafe {
            vmaf_score_pooled(
                self.inner,
                model.inner,
                VmafPoolingMethod_VMAF_POOL_METHOD_MEAN,
                &mut score,
                index_low,
                index_high,
            )
        })?;
        Ok(score)
    }
}

impl Drop for VmafContext {
    fn drop(&mut self) {
        if !self.inner.is_null() {
            // SAFETY: we are the sole owner; no further access occurs after drop.
            unsafe { vmaf_close(self.inner) };
            self.inner = ptr::null_mut();
        }
    }
}

// ---------------------------------------------------------------------------
// VmafModel
// ---------------------------------------------------------------------------

/// RAII wrapper around a `*mut VmafModel`.
pub struct VmafModel {
    inner: *mut RawVmafModel,
}

// Safety: VmafModel is an opaque C struct with no thread-local state.
unsafe impl Send for VmafModel {}

impl VmafModel {
    /// Load a VMAF model from the filesystem.
    pub fn from_path(path: &str) -> Result<Self, VmafxError> {
        let c_path = CString::new(path)?;
        let mut cfg = VmafModelConfig {
            name: ptr::null(),
            flags: 0,
        };
        let mut model: *mut RawVmafModel = ptr::null_mut();
        // SAFETY: `c_path` lives for the duration of the call.
        check(unsafe { vmaf_model_load_from_path(&mut model, &mut cfg, c_path.as_ptr()) })?;
        Ok(VmafModel { inner: model })
    }
}

impl Drop for VmafModel {
    fn drop(&mut self) {
        if !self.inner.is_null() {
            // SAFETY: sole owner; no further access.
            unsafe { vmaf_model_destroy(self.inner) };
            self.inner = ptr::null_mut();
        }
    }
}

// ---------------------------------------------------------------------------
// Picture helpers
// ---------------------------------------------------------------------------

/// Allocate an 8-bit YUV 4:2:0 picture of the given dimensions.
pub fn alloc_yuv420p_8bit(w: u32, h: u32) -> Result<VmafPicture, VmafxError> {
    let mut pic = unsafe { std::mem::zeroed::<VmafPicture>() };
    // SAFETY: `pic` is a zero-initialised struct; all params are valid.
    check(unsafe { vmaf_picture_alloc(&mut pic, VmafPixelFormat_VMAF_PIX_FMT_YUV420P, 8, w, h) })?;
    Ok(pic)
}

/// Release a picture's memory.
pub fn unref_picture(pic: &mut VmafPicture) -> Result<(), VmafxError> {
    // SAFETY: `pic` was allocated by vmaf_picture_alloc.
    check(unsafe { vmaf_picture_unref(pic) })
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

/// Return the libvmaf version string.
pub fn version() -> &'static str {
    // SAFETY: vmaf_version returns a pointer to a static C string.
    unsafe { CStr::from_ptr(crate::vmaf_version()) }
        .to_str()
        .unwrap_or("<invalid utf8>")
}
