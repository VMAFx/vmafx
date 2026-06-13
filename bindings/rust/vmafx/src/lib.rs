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
//! ```no_run
//! use vmafx::{Context, Model, Picture, PoolingMethod};
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
