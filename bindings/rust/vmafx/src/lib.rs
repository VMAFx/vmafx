// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// vmafx — safe, idiomatic Rust bindings to libvmaf.
//
// This crate sits on top of `vmafx-sys` and provides RAII-managed wrappers
// around the libvmaf C API. The goals are:
//
//   - No `unsafe` in the public API surface.
//   - Errors are returned as `Result<T, Error>` so callers can use `?`.
//   - Resources (contexts, models, pictures) free themselves on `Drop`.
//   - Types are `Send` where the underlying C object is self-contained and
//     thread-safe to migrate; `!Sync` until libvmaf documents otherwise.
//
// Phase 1 (ADR-0929) ships the essential scoring loop: load a model, open a
// context, push reference/distorted pictures frame by frame, and read out a
// pooled VMAF score. Advanced surfaces (model collections, per-feature
// scores, output writers, dmabuf import) are intentionally deferred — the
// raw FFI in `vmafx_sys` remains the escape hatch.
//
// Migration from `vmafx_sys::safe::*`:
//   - `VmafContext`   -> `vmafx::Context`
//   - `VmafModel`     -> `vmafx::Model`
//   - `VmafPicture`   -> `vmafx::Picture`
//   - `alloc_yuv420p_8bit(w, h)` -> `vmafx::Picture::new_yuv420p_8bit(w, h)`
//   - `VmafxError`    -> `vmafx::Error`
//
// The legacy `vmafx_sys::safe` module continues to ship from `vmafx-sys` for
// existing callers; it will be deprecated in a follow-on PR once `vmafx`
// exposes feature parity with it.
//
// References: ADR-0706 (`vmafx-sys`), ADR-0929 (this crate).

#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_docs)]
#![warn(rust_2018_idioms)]
// In library code, callers should receive a `Result` rather than a panic.
// `expect` / `unwrap` in non-test lib code is disallowed by default so that
// hidden panic paths are caught at review time.  Test modules and examples
// may opt back in with `#[allow(clippy::expect_used)]`.
#![warn(clippy::expect_used)]
#![warn(clippy::unwrap_used)]

//! # vmafx — safe Rust bindings to libvmaf
//!
//! Query the libvmaf version and open a context with custom settings:
//!
//! ```no_run
//! use vmafx::{ContextBuilder, LogLevel};
//!
//! # fn main() -> vmafx::Result<()> {
//! // Declare `model` before `ctx` so that drop order is ctx → model
//! // (variables are dropped in reverse declaration order).  libvmaf stores
//! // a raw pointer to the model inside the context; the model must therefore
//! // outlive the context.
//! let model = Model::from_path("/usr/local/share/model/vmaf_v0.6.1.json")?;
//! let mut ctx = Context::new()?;
//! ctx.use_features_from_model(&model)?;
//!
//! // Push one frame of all-grey YUV420p (W=576, H=324).
//! let r = Picture::new_yuv420p_8bit(576, 324)?;
//! let d = Picture::new_yuv420p_8bit(576, 324)?;
//! ctx.read_pictures(r, d, 0)?;
//! ctx.flush()?;
//!
//! let score = ctx.score_pooled(&model, PoolingMethod::Mean, 0, 0)?;
//! println!("VMAF mean = {score}");
//! # Ok(())
//! # }
//! ```
//!
//! For a full scoring loop (model load → picture push → score readout) see the
//! integration tests in `bindings/rust/vmafx/tests/` and the C-level example in
//! `docs/api/rust-bindings.md`.

mod context;
mod error;
mod model;
mod picture;
mod score;

pub use context::{Context, ContextBuilder, LogLevel};
pub use error::{Error, Result};
pub use model::Model;
pub use picture::{Picture, PixelFormat};
pub use score::{PoolingMethod, Score};

/// Return the version string of the libvmaf the crate was linked against.
///
/// Safe wrapper around `vmaf_version()`; the returned slice has `'static`
/// lifetime because it points at C static storage.
#[must_use]
pub fn version() -> &'static str {
    // Delegates to the safe wrapper already shipped in vmafx-sys.
    vmafx_sys::safe::version()
}

#[cfg(test)]
#[allow(clippy::expect_used, clippy::unwrap_used)]
mod lib_tests {
    use super::*;

    // ---- Error Display + source (exercise paths not covered in error::tests) ----

    #[test]
    fn error_display_all_variants_exhaustive() {
        use std::ffi::CString;
        let nul = CString::new("ab\0cd").unwrap_err();
        let cases: Vec<Box<dyn std::fmt::Display>> = vec![
            Box::new(Error::OutOfMemory),
            Box::new(Error::InvalidArgument),
            Box::new(Error::NotSupported),
            Box::new(Error::PermissionDenied),
            Box::new(Error::NotFound),
            Box::new(Error::Libvmaf { code: -42 }),
            Box::new(Error::Nul(nul)),
            Box::new(Error::InvalidState("test state")),
            Box::new(Error::Io(std::io::Error::new(std::io::ErrorKind::Other, "io"))),
        ];
        for case in &cases {
            let s = format!("{case}");
            assert!(!s.is_empty(), "Display impl must not return empty string");
        }
    }

    #[test]
    fn error_source_nul_and_io_have_source() {
        use std::error::Error as StdError;
        use std::ffi::CString;

        let nul = CString::new("ab\0cd").unwrap_err();
        let e_nul = Error::Nul(nul);
        assert!(e_nul.source().is_some(), "Nul error should have a source");

        let e_io = Error::Io(std::io::Error::new(std::io::ErrorKind::Other, "io error"));
        assert!(e_io.source().is_some(), "Io error should have a source");

        // Other variants have no source.
        assert!(Error::OutOfMemory.source().is_none());
        assert!(Error::NotSupported.source().is_none());
        assert!(Error::InvalidState("s").source().is_none());
    }

    #[test]
    fn error_from_nul_conversion() {
        use std::ffi::CString;
        let nul = CString::new("ab\0cd").unwrap_err();
        let e: Error = nul.into();
        assert!(matches!(e, Error::Nul(_)));
    }

    #[test]
    fn error_from_io_conversion() {
        let io = std::io::Error::new(std::io::ErrorKind::NotFound, "missing");
        let e: Error = io.into();
        assert!(matches!(e, Error::Io(_)));
    }

    #[test]
    fn error_not_supported_enosys_and_enotsup() {
        // ENOSYS (38) and ENOTSUP/EOPNOTSUPP (95) both map to NotSupported.
        assert!(matches!(
            Error::from_libvmaf_rc(-38).unwrap_err(),
            Error::NotSupported
        ));
        assert!(matches!(
            Error::from_libvmaf_rc(-95).unwrap_err(),
            Error::NotSupported
        ));
    }

    #[test]
    fn error_permission_denied() {
        assert!(matches!(
            Error::from_libvmaf_rc(-13).unwrap_err(),
            Error::PermissionDenied
        ));
    }

    // ---- LogLevel ----

    #[test]
    fn log_level_all_variants_debug_format() {
        let variants = [
            LogLevel::None,
            LogLevel::Error,
            LogLevel::Warning,
            LogLevel::Info,
            LogLevel::Debug,
        ];
        for lv in &variants {
            // Exercises Debug, Clone, Copy, PartialEq, Eq derived impls.
            let cloned = *lv;
            assert_eq!(cloned, *lv);
            assert!(!format!("{lv:?}").is_empty());
        }
    }

    // ---- Score ----

    #[test]
    fn score_struct_fields_accessible() {
        let s = score::Score::new(7, 83.2);
        assert_eq!(s.index, 7);
        assert!((s.value - 83.2).abs() < f64::EPSILON);
    }

    #[test]
    fn score_clone_and_copy() {
        let s = score::Score::new(0, 50.0);
        let s2 = s; // Copy
        assert_eq!(s, s2);
    }
}
