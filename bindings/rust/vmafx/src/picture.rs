// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// `Picture` — RAII wrapper around `vmaf_picture`.
//
// libvmaf's `VmafPicture` is a value-type struct that owns three plane
// buffers allocated by `vmaf_picture_alloc()`. `vmaf_picture_unref()` frees
// them. The C API also transfers ownership when you call
// `vmaf_read_pictures()` — at that point the buffers are no longer ours to
// touch.
//
// We model this by tracking an "owned" bit. `Drop` unrefs only owned
// pictures. When `Context::read_pictures` consumes the picture, it calls
// `Picture::into_raw_owned()` which strips the owned bit and returns the
// raw value so the FFI call can take it by-value.

use std::mem::MaybeUninit;

use vmafx_sys::{
    vmaf_picture_alloc, vmaf_picture_unref, VmafPicture as RawPicture,
    VmafPixelFormat_VMAF_PIX_FMT_YUV400P, VmafPixelFormat_VMAF_PIX_FMT_YUV420P,
    VmafPixelFormat_VMAF_PIX_FMT_YUV422P, VmafPixelFormat_VMAF_PIX_FMT_YUV444P,
};

use crate::error::{Error, Result};

/// Pixel formats supported by libvmaf picture allocation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum PixelFormat {
    /// Luma only (4:0:0 / monochrome).
    Yuv400p,
    /// 4:2:0 chroma subsampling (most common for SDR video).
    Yuv420p,
    /// 4:2:2 chroma subsampling.
    Yuv422p,
    /// 4:4:4 chroma subsampling (no subsampling).
    Yuv444p,
}

impl PixelFormat {
    const fn to_raw(self) -> u32 {
        match self {
            Self::Yuv400p => VmafPixelFormat_VMAF_PIX_FMT_YUV400P,
            Self::Yuv420p => VmafPixelFormat_VMAF_PIX_FMT_YUV420P,
            Self::Yuv422p => VmafPixelFormat_VMAF_PIX_FMT_YUV422P,
            Self::Yuv444p => VmafPixelFormat_VMAF_PIX_FMT_YUV444P,
        }
    }
}

/// RAII wrapper around a libvmaf picture.
///
/// Holds the three plane buffers allocated by `vmaf_picture_alloc()`.
/// `Drop` calls `vmaf_picture_unref()` for any picture still owned by the
/// Rust side. Pictures that have been handed to `Context::read_pictures` are
/// no longer owned and will not be double-freed.
pub struct Picture {
    raw: RawPicture,
    /// `true` iff the underlying buffers are still our responsibility.
    /// Cleared by [`Picture::into_raw_owned`] when ownership transfers
    /// across the FFI boundary.
    owned: bool,
}

// SAFETY: libvmaf pictures are self-contained heap buffers with no
// thread-local state. Migrating ownership between threads is safe; the C
// API does not document concurrent access from multiple threads, so we do
// not implement `Sync`.
unsafe impl Send for Picture {}

impl Picture {
    /// Allocate a YUV 4:2:0 8-bit picture of the given dimensions.
    ///
    /// Convenience for the most common SDR case. Equivalent to
    /// `Picture::new(PixelFormat::Yuv420p, 8, w, h)`.
    ///
    /// # Errors
    /// Returns [`Error::Libvmaf`] (or a mapped variant) if libvmaf fails to allocate.
    pub fn new_yuv420p_8bit(w: u32, h: u32) -> Result<Self> {
        Self::new(PixelFormat::Yuv420p, 8, w, h)
    }

    /// Allocate a picture with the given format, bit depth, and dimensions.
    ///
    /// `bpc` must be in `8..=16`. libvmaf rejects out-of-range values with
    /// `EINVAL`, which surfaces as [`Error::InvalidArgument`].
    ///
    /// # Errors
    /// Returns [`Error::InvalidArgument`] for unsupported `bpc`, or
    /// [`Error::Libvmaf`] (or a mapped variant) for other libvmaf failures.
    pub fn new(format: PixelFormat, bpc: u32, w: u32, h: u32) -> Result<Self> {
        // Zero-init via MaybeUninit so we don't read uninitialised memory
        // even momentarily; `vmaf_picture_alloc` populates every field on
        // success.
        let mut raw: MaybeUninit<RawPicture> = MaybeUninit::zeroed();
        // SAFETY: `raw` points to writable, properly-aligned storage for
        // exactly one `RawPicture`. All scalar arguments are passed by
        // value. On success libvmaf fully initialises `*raw`. On failure
        // it returns a negative errno and the struct stays zero-initialised
        // (which is the safe sentinel state — no plane pointers are set).
        let rc = unsafe { vmaf_picture_alloc(raw.as_mut_ptr(), format.to_raw(), bpc, w, h) };
        Error::from_libvmaf_rc(rc)?;
        // SAFETY: rc >= 0 means libvmaf fully populated the struct.
        let raw = unsafe { raw.assume_init() };
        Ok(Picture { raw, owned: true })
    }

    /// Picture width in pixels (Y-plane).
    #[must_use]
    pub fn width(&self) -> u32 {
        self.raw.w[0]
    }

    /// Picture height in pixels (Y-plane).
    #[must_use]
    pub fn height(&self) -> u32 {
        self.raw.h[0]
    }

    /// Bits per component.
    #[must_use]
    pub fn bpc(&self) -> u32 {
        self.raw.bpc
    }

    /// Borrow the raw `VmafPicture` immutably. Escape hatch for advanced
    /// callers; the caller must respect libvmaf's ownership rules.
    #[must_use]
    pub fn as_raw(&self) -> &RawPicture {
        &self.raw
    }

    /// Borrow the raw `VmafPicture` mutably. Escape hatch for advanced
    /// callers (e.g. blitting decoded pixels into the plane buffers).
    #[must_use]
    pub fn as_raw_mut(&mut self) -> &mut RawPicture {
        &mut self.raw
    }

    /// Consume the wrapper and return the raw value, transferring ownership
    /// to the caller. Used by [`crate::Context::read_pictures`] to hand the
    /// picture to libvmaf without risking a double-free at `Drop` time.
    pub(crate) fn into_raw_owned(mut self) -> RawPicture {
        debug_assert!(self.owned, "into_raw_owned called on non-owned Picture");
        self.owned = false;
        // Take a copy of the struct; `Drop` runs but does nothing because
        // `owned == false`.
        self.raw
    }
}

impl Drop for Picture {
    fn drop(&mut self) {
        if !self.owned {
            return;
        }
        // SAFETY: `self.raw` was populated by a successful
        // `vmaf_picture_alloc` and has not been handed to libvmaf via
        // `vmaf_read_pictures`. Calling `vmaf_picture_unref` here is the
        // documented free.
        let _ = unsafe { vmaf_picture_unref(&raw mut self.raw) };
        self.owned = false;
    }
}

#[cfg(test)]
#[allow(clippy::expect_used, clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn alloc_and_drop_yuv420p_8bit() {
        let p = Picture::new_yuv420p_8bit(64, 64).expect("alloc");
        assert_eq!(p.width(), 64);
        assert_eq!(p.height(), 64);
        assert_eq!(p.bpc(), 8);
        // Drop runs here and unrefs cleanly.
    }

    #[test]
    fn alloc_various_formats() {
        for fmt in [
            PixelFormat::Yuv400p,
            PixelFormat::Yuv420p,
            PixelFormat::Yuv422p,
            PixelFormat::Yuv444p,
        ] {
            let _p = Picture::new(fmt, 8, 32, 32).expect("alloc");
        }
    }

    #[test]
    fn alloc_10bit() {
        let p = Picture::new(PixelFormat::Yuv420p, 10, 32, 32).expect("alloc");
        assert_eq!(p.bpc(), 10);
    }
}
