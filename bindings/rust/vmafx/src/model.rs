// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// `Model` — RAII wrapper around `*mut VmafModel`.

use std::ffi::CString;
use std::path::Path;
use std::ptr;

use vmafx_sys::{
    vmaf_model_destroy, vmaf_model_load_from_path, VmafModel as RawModel, VmafModelConfig,
};

use crate::error::{Error, Result};

/// RAII wrapper around a `*mut VmafModel`.
///
/// Models are loaded from disk by [`Model::from_path`]. The Drop impl calls
/// `vmaf_model_destroy` to release the C-side memory.
pub struct Model {
    inner: *mut RawModel,
}

// SAFETY: `VmafModel` is an opaque, heap-allocated C struct with no
// thread-local state. Migrating ownership between threads is safe; the C
// API does not document concurrent mutation, so we do not implement `Sync`.
unsafe impl Send for Model {}

impl Model {
    /// Load a VMAF model from a `.json` file on disk.
    ///
    /// # Errors
    /// Returns [`Error::Nul`] if `path` contains an interior NUL byte, or
    /// [`Error::Libvmaf`] (or one of the mapped variants) if libvmaf rejects
    /// the file.
    pub fn from_path<P: AsRef<Path>>(path: P) -> Result<Self> {
        let path_str = path.as_ref().to_string_lossy();
        let c_path = CString::new(path_str.as_bytes())?;
        let mut cfg = VmafModelConfig {
            name: ptr::null(),
            flags: 0,
        };
        let mut model: *mut RawModel = ptr::null_mut();
        // SAFETY: `c_path` lives until the end of this call. `model` is a
        // valid out-pointer. `cfg` is fully initialised.
        let rc = unsafe { vmaf_model_load_from_path(&raw mut model, &raw mut cfg, c_path.as_ptr()) };
        Error::from_libvmaf_rc(rc)?;
        if model.is_null() {
            // libvmaf reported success but did not populate the pointer.
            // Treat as an internal contract violation; bail with InvalidState.
            return Err(Error::InvalidState(
                "vmaf_model_load_from_path returned success but null pointer",
            ));
        }
        Ok(Self { inner: model })
    }

    /// Borrow the raw `*mut VmafModel`. Escape hatch for advanced callers.
    ///
    /// The pointer remains valid for the lifetime of the [`Model`] borrow.
    #[must_use]
    pub const fn as_ptr(&self) -> *mut RawModel {
        self.inner
    }

    /// Same as [`as_ptr`](Self::as_ptr) but takes `&mut self`.
    #[must_use]
    pub const fn as_mut_ptr(&mut self) -> *mut RawModel {
        self.inner
    }
}

impl Drop for Model {
    fn drop(&mut self) {
        if !self.inner.is_null() {
            // SAFETY: we are the sole owner; the pointer was produced by
            // `vmaf_model_load_from_path` and has not been freed.
            unsafe { vmaf_model_destroy(self.inner) };
            self.inner = ptr::null_mut();
        }
    }
}
