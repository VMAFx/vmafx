// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// Safe views of the pictures the shim hands to a twin. Every plane is checked
// once when the view is built, so kernels read rows through slices.

use core::marker::PhantomData;

use crate::abi::{VmafxRsGeometry, VmafxRsPicture, VmafxRsPlane};
use crate::error::Error;

/// `VmafPixelFormat`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PixFmt {
    /// `VMAF_PIX_FMT_UNKNOWN`.
    Unknown,
    /// 4:2:0.
    Yuv420p,
    /// 4:2:2.
    Yuv422p,
    /// 4:4:4.
    Yuv444p,
    /// Luma only.
    Yuv400p,
}

impl PixFmt {
    /// From the C enum value.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` for a value outside the C enum.
    pub const fn from_raw(v: u32) -> Result<Self, Error> {
        match v {
            0 => Ok(Self::Unknown),
            1 => Ok(Self::Yuv420p),
            2 => Ok(Self::Yuv422p),
            3 => Ok(Self::Yuv444p),
            4 => Ok(Self::Yuv400p),
            _ => Err(Error::InvalidArgument(c"unknown pixel format")),
        }
    }
}

/// The geometry of every picture of a run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Geometry {
    /// Pixel format.
    pub pix_fmt: PixFmt,
    /// Bits per component.
    pub bpc: u32,
    /// Luma width.
    pub w: u32,
    /// Luma height.
    pub h: u32,
}

impl Geometry {
    /// From the ABI struct.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` for an unknown pixel format.
    pub const fn from_abi(g: &VmafxRsGeometry) -> Result<Self, Error> {
        match PixFmt::from_raw(g.pix_fmt) {
            Ok(pix_fmt) => Ok(Self {
                pix_fmt,
                bpc: g.bpc,
                w: g.w,
                h: g.h,
            }),
            Err(e) => Err(e),
        }
    }
}

/// A sample type of a plane: `u8` (bpc <= 8) or `u16`.
pub trait Sample: Copy + 'static {}
impl Sample for u8 {}
impl Sample for u16 {}

/// Rows of one plane. `row(y)` is a slice of exactly `width()` samples.
#[derive(Clone, Copy)]
pub struct PlaneView<'a, T: Sample> {
    data: &'a [T],
    stride: usize,
    width: usize,
    height: usize,
}

impl<'a, T: Sample> PlaneView<'a, T> {
    /// View an ABI plane.
    ///
    /// # Safety
    ///
    /// `p.data` is NULL or points to `p.h` rows of `p.stride` bytes each
    /// (the last row needs only `p.w` samples), valid and unmodified for `'a`.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` for a NULL plane, a zero size, a stride
    /// shorter than a row or not a multiple of the sample size, or data not
    /// aligned for `T`.
    pub unsafe fn from_abi(p: &VmafxRsPlane) -> Result<Self, Error> {
        let size = core::mem::size_of::<T>();
        let width = p.w as usize;
        let height = p.h as usize;
        let Ok(stride_bytes) = usize::try_from(p.stride) else {
            return Err(Error::InvalidArgument(c"negative plane stride"));
        };
        if p.data.is_null() || width == 0 || height == 0 {
            return Err(Error::InvalidArgument(c"empty plane"));
        }
        if stride_bytes % size != 0 || stride_bytes / size < width {
            return Err(Error::InvalidArgument(c"plane stride shorter than a row"));
        }
        if p.data.cast::<T>().align_offset(core::mem::align_of::<T>()) != 0 {
            return Err(Error::InvalidArgument(c"plane data not aligned"));
        }
        let stride = stride_bytes / size;
        let Some(len) = (height - 1)
            .checked_mul(stride)
            .and_then(|n| n.checked_add(width))
        else {
            return Err(Error::InvalidArgument(c"plane too large"));
        };
        // SAFETY: the caller guarantees `height` rows of `stride` samples from `data` (the last
        // one `width` long), aligned (checked above) and valid for 'a; `len` covers exactly that.
        let data = unsafe { core::slice::from_raw_parts(p.data.cast::<T>(), len) };
        Ok(Self {
            data,
            stride,
            width,
            height,
        })
    }

    /// View a buffer owned by Rust (tests, intermediate planes).
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` when `data` is shorter than the geometry.
    pub fn from_slice(
        data: &'a [T],
        width: usize,
        height: usize,
        stride: usize,
    ) -> Result<Self, Error> {
        let need = height
            .checked_sub(1)
            .and_then(|r| r.checked_mul(stride))
            .map(|n| n + width);
        match need {
            Some(n) if width > 0 && stride >= width && n <= data.len() => Ok(Self {
                data,
                stride,
                width,
                height,
            }),
            _ => Err(Error::InvalidArgument(c"buffer shorter than the plane")),
        }
    }

    /// Width in samples.
    #[must_use]
    pub const fn width(&self) -> usize {
        self.width
    }

    /// Height in rows.
    #[must_use]
    pub const fn height(&self) -> usize {
        self.height
    }

    /// Stride in samples.
    #[must_use]
    pub const fn stride(&self) -> usize {
        self.stride
    }

    /// Row `y` (`y < height()`); its length is `width()`. An out-of-range row
    /// is an invariant violation and aborts.
    #[must_use]
    pub fn row(&self, y: usize) -> &'a [T] {
        let start = y * self.stride;
        &self.data[start..start + self.width]
    }

    /// Sample at (`x`, `y`).
    #[must_use]
    pub fn at(&self, x: usize, y: usize) -> T {
        self.row(y)[x]
    }
}

/// One plane with the sample type the bit depth selects.
#[derive(Clone, Copy)]
pub enum Plane<'a> {
    /// bpc <= 8.
    U8(PlaneView<'a, u8>),
    /// bpc > 8.
    U16(PlaneView<'a, u16>),
}

impl Plane<'_> {
    /// Width in samples.
    #[must_use]
    pub const fn width(&self) -> usize {
        match self {
            Self::U8(p) => p.width(),
            Self::U16(p) => p.width(),
        }
    }

    /// Height in rows.
    #[must_use]
    pub const fn height(&self) -> usize {
        match self {
            Self::U8(p) => p.height(),
            Self::U16(p) => p.height(),
        }
    }
}

/// A picture borrowed for one call.
#[derive(Clone, Copy)]
pub struct Picture<'a> {
    raw: &'a VmafxRsPicture,
    _lt: PhantomData<&'a ()>,
}

impl<'a> Picture<'a> {
    /// Wrap an ABI picture.
    ///
    /// # Safety
    ///
    /// Every plane below `raw.n_planes` satisfies `PlaneView::from_abi`'s
    /// contract for `'a`.
    #[must_use]
    pub const unsafe fn from_abi(raw: &'a VmafxRsPicture) -> Self {
        Self {
            raw,
            _lt: PhantomData,
        }
    }

    /// Pixel format.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` for an unknown value.
    pub const fn pix_fmt(&self) -> Result<PixFmt, Error> {
        PixFmt::from_raw(self.raw.pix_fmt)
    }

    /// Bits per component.
    #[must_use]
    pub const fn bpc(&self) -> u32 {
        self.raw.bpc
    }

    /// Number of planes (1 for YUV400P, else 3).
    #[must_use]
    pub const fn n_planes(&self) -> usize {
        self.raw.n_planes as usize
    }

    /// Plane `p` (0 = Y, 1 = U, 2 = V), typed by the bit depth.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` for a missing or malformed plane.
    pub fn plane(&self, p: usize) -> Result<Plane<'a>, Error> {
        if p >= self.n_planes() {
            return Err(Error::InvalidArgument(c"plane index out of range"));
        }
        let Some(raw) = self.raw.plane.get(p) else {
            return Err(Error::InvalidArgument(c"plane index out of range"));
        };
        // SAFETY: `from_abi`'s contract covers every plane below n_planes for 'a.
        unsafe {
            if self.raw.bpc <= 8 {
                PlaneView::from_abi(raw).map(Plane::U8)
            } else {
                PlaneView::from_abi(raw).map(Plane::U16)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rows_respect_stride() {
        let buf: Vec<u8> = (0..12).collect();
        let v = PlaneView::from_slice(&buf, 3, 3, 4);
        assert!(v.is_ok());
        if let Ok(v) = v {
            assert_eq!(v.row(1), &[4, 5, 6]);
            assert_eq!(v.at(2, 2), 10);
        }
    }

    #[test]
    fn short_buffer_is_refused() {
        let buf = [0u8; 5];
        assert!(PlaneView::from_slice(&buf, 3, 2, 4).is_err());
    }

    #[test]
    fn abi_plane_checks_stride() {
        let buf = [0u16; 8];
        let p = VmafxRsPlane {
            data: buf.as_ptr().cast(),
            stride: 2,
            w: 4,
            h: 2,
        };
        // SAFETY: `buf` outlives the view and holds the declared geometry.
        assert!(unsafe { PlaneView::<u16>::from_abi(&p) }.is_err());
        let p = VmafxRsPlane {
            data: buf.as_ptr().cast(),
            stride: 8,
            w: 4,
            h: 2,
        };
        // SAFETY: `buf` outlives the view and holds 2 rows of 4 u16 samples at stride 8 bytes.
        assert!(unsafe { PlaneView::<u16>::from_abi(&p) }.is_ok());
    }
}
