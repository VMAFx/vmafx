// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// `Context` — RAII wrapper around `*mut VmafContext`.
//
// A context is created with a [`ContextBuilder`] (or the `Context::new`
// default) and drives the scoring pipeline: model registration, picture
// ingestion, pooled-score readout.

use std::ptr;

use vmafx_sys::{
    VmafConfiguration, VmafContext as RawContext, VmafLogLevel_VMAF_LOG_LEVEL_DEBUG,
    VmafLogLevel_VMAF_LOG_LEVEL_ERROR, VmafLogLevel_VMAF_LOG_LEVEL_INFO,
    VmafLogLevel_VMAF_LOG_LEVEL_NONE, VmafLogLevel_VMAF_LOG_LEVEL_WARNING, vmaf_close, vmaf_init,
    vmaf_read_pictures, vmaf_score_pooled, vmaf_use_features_from_model,
};

use crate::error::{Error, Result};
use crate::model::Model;
use crate::picture::Picture;
use crate::score::PoolingMethod;

/// Log verbosity for libvmaf's internal logger.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[non_exhaustive]
pub enum LogLevel {
    /// Suppress all log output.
    None,
    /// Error messages only.
    Error,
    /// Errors + warnings.
    #[default]
    Warning,
    /// Errors + warnings + informational messages.
    Info,
    /// Full debug output (very chatty).
    Debug,
}

impl LogLevel {
    const fn to_raw(self) -> u32 {
        match self {
            Self::None => VmafLogLevel_VMAF_LOG_LEVEL_NONE,
            Self::Error => VmafLogLevel_VMAF_LOG_LEVEL_ERROR,
            Self::Warning => VmafLogLevel_VMAF_LOG_LEVEL_WARNING,
            Self::Info => VmafLogLevel_VMAF_LOG_LEVEL_INFO,
            Self::Debug => VmafLogLevel_VMAF_LOG_LEVEL_DEBUG,
        }
    }
}

/// Builder for fine-tuning a [`Context`] before it is constructed.
///
/// Most callers will use [`Context::new`] for sensible defaults.
#[derive(Debug, Clone)]
pub struct ContextBuilder {
    log_level: LogLevel,
    n_threads: u32,
    n_subsample: u32,
    cpumask: u64,
}

impl Default for ContextBuilder {
    fn default() -> Self {
        Self {
            log_level: LogLevel::default(),
            n_threads: 0,
            n_subsample: 1,
            cpumask: 0,
        }
    }
}

impl ContextBuilder {
    /// Start a new builder with libvmaf defaults.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Set the log verbosity. Default is [`LogLevel::Warning`].
    #[must_use]
    pub const fn log_level(mut self, level: LogLevel) -> Self {
        self.log_level = level;
        self
    }

    /// Set the number of worker threads (0 = libvmaf chooses).
    #[must_use]
    pub const fn n_threads(mut self, n: u32) -> Self {
        self.n_threads = n;
        self
    }

    /// Set the frame subsampling stride (1 = every frame).
    #[must_use]
    pub fn n_subsample(mut self, n: u32) -> Self {
        // `u32::max` is not `const fn` in stable yet, so this setter stays non-const.
        self.n_subsample = n.max(1);
        self
    }

    /// Set the CPU feature mask (advanced; usually leave as 0).
    #[must_use]
    pub const fn cpumask(mut self, mask: u64) -> Self {
        self.cpumask = mask;
        self
    }

    /// Build the context.
    ///
    /// # Errors
    /// Returns [`Error::Libvmaf`] (or a mapped variant) if libvmaf fails to
    /// initialise the context, or [`Error::InvalidState`] if libvmaf reports
    /// success but returns a null pointer.
    pub fn build(self) -> Result<Context> {
        let cfg = VmafConfiguration {
            log_level: self.log_level.to_raw(),
            n_threads: self.n_threads,
            n_subsample: self.n_subsample,
            cpumask: self.cpumask,
            gpumask: 0,
        };
        let mut ctx: *mut RawContext = ptr::null_mut();
        // SAFETY: `cfg` is fully initialised; `ctx` is a valid out-pointer.
        let rc = unsafe { vmaf_init(&raw mut ctx, cfg) };
        Error::from_libvmaf_rc(rc)?;
        if ctx.is_null() {
            return Err(Error::InvalidState(
                "vmaf_init returned success but null pointer",
            ));
        }
        Ok(Context { inner: ctx })
    }
}

/// RAII wrapper around a `*mut VmafContext`.
pub struct Context {
    inner: *mut RawContext,
}

// SAFETY: a libvmaf context is self-contained heap state; moving it across
// threads is safe. The C API documents that a single context must not be
// driven concurrently from multiple threads, so we do not implement `Sync`.
unsafe impl Send for Context {}

impl Context {
    /// Build a [`Context`] with libvmaf defaults.
    ///
    /// Equivalent to `ContextBuilder::new().build()`.
    ///
    /// # Errors
    /// Returns [`Error::Libvmaf`] (or a mapped variant) if libvmaf fails to
    /// initialise the context.
    pub fn new() -> Result<Self> {
        ContextBuilder::new().build()
    }

    /// Start a builder for finer-grained configuration.
    #[must_use]
    pub fn builder() -> ContextBuilder {
        ContextBuilder::new()
    }

    /// Register every feature extractor required by `model`.
    ///
    /// Must be called once per model before [`Self::read_pictures`].
    ///
    /// # Errors
    /// Returns [`Error::Libvmaf`] (or a mapped variant) if libvmaf rejects the model.
    pub fn use_features_from_model(&mut self, model: &mut Model) -> Result<()> {
        // SAFETY: both pointers are non-null and valid for the call's duration.
        let rc = unsafe { vmaf_use_features_from_model(self.inner, model.as_mut_ptr()) };
        Error::from_libvmaf_rc(rc)
    }

    /// Push a pair of pictures into the scoring pipeline at `index`.
    ///
    /// Ownership of both pictures is transferred to libvmaf; the picture
    /// buffers will be unref'd internally and **must not** be touched after
    /// this call.
    ///
    /// # Errors
    /// Returns [`Error::Libvmaf`] (or a mapped variant) if libvmaf fails to enqueue the frame.
    pub fn read_pictures(&mut self, ref_pic: Picture, dist_pic: Picture, index: u32) -> Result<()> {
        // Take ownership of the raw structs; the `Picture` wrappers' `Drop`
        // will no-op because `into_raw_owned` cleared the `owned` flag.
        let mut r_raw = ref_pic.into_raw_owned();
        let mut d_raw = dist_pic.into_raw_owned();
        // SAFETY: both pointers are non-null and the structs are
        // fully-initialised owners of allocated planes.
        let rc = unsafe { vmaf_read_pictures(self.inner, &raw mut r_raw, &raw mut d_raw, index) };
        Error::from_libvmaf_rc(rc)
    }

    /// Signal end-of-stream to the feature extractor.
    ///
    /// Call exactly once after the final [`Self::read_pictures`] and before
    /// any score-readout.
    ///
    /// # Errors
    /// Returns [`Error::Libvmaf`] (or a mapped variant) if libvmaf fails during flush.
    pub fn flush(&mut self) -> Result<()> {
        // libvmaf treats (NULL, NULL) as EOF.
        // SAFETY: NULL is the documented sentinel for the flush call.
        let rc = unsafe { vmaf_read_pictures(self.inner, ptr::null_mut(), ptr::null_mut(), 0) };
        Error::from_libvmaf_rc(rc)
    }

    /// Compute the pooled VMAF score over `index_low..=index_high`.
    ///
    /// `index_low` must be ≤ `index_high` and both must refer to frames
    /// already pushed via [`Self::read_pictures`].
    ///
    /// # Errors
    /// Returns [`Error::Libvmaf`] (or a mapped variant) if libvmaf fails to compute the score.
    pub fn score_pooled(
        &mut self,
        model: &mut Model,
        method: PoolingMethod,
        index_low: u32,
        index_high: u32,
    ) -> Result<f64> {
        let mut score = 0.0_f64;
        // SAFETY: `score` is a valid out-pointer; the other args are valid.
        let rc = unsafe {
            vmaf_score_pooled(
                self.inner,
                model.as_mut_ptr(),
                method.to_raw(),
                &raw mut score,
                index_low,
                index_high,
            )
        };
        Error::from_libvmaf_rc(rc)?;
        Ok(score)
    }

    /// Borrow the raw `*mut VmafContext`. Escape hatch for advanced callers.
    #[must_use]
    pub const fn as_ptr(&mut self) -> *mut RawContext {
        self.inner
    }
}

impl Drop for Context {
    fn drop(&mut self) {
        if !self.inner.is_null() {
            // SAFETY: sole owner; no further access after this point.
            unsafe { vmaf_close(self.inner) };
            self.inner = ptr::null_mut();
        }
    }
}
