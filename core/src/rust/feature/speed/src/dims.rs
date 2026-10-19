// Copyright 2016-2025 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// SpEED geometry. Mirrors `speed_init_dimensions()`, `speed_prescale_resamples()`
// and `speed_chroma_dimensions()` of core/src/feature/speed.c and
// speed_internal.h; ported statement by statement from Netflix code.

use crate::fexapi::Error;

/// Side of one SpEED block (`DEFAULT_BLOCK_SIZE`).
pub const BLOCK: usize = 5;
/// Elements of one block (`block_size * block_size`).
pub const ELEMS: usize = BLOCK * BLOCK;
/// Number of 2x scales (`NUM_SCALES`); the plane is decimated by 2^4.
pub const NUM_SCALES: u32 = 4;

/// Dimensions of one SpEED channel (`SpeedDimensions`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Dims {
    pub orig_w: usize,
    pub orig_h: usize,
    pub scaled_w: usize,
    pub scaled_h: usize,
    pub alloc_w: usize,
    pub alloc_h: usize,
    pub trunc_w: usize,
    pub trunc_h: usize,
    pub blocks_h: usize,
    pub blocks_v: usize,
    pub num_blocks: usize,
    pub sub_w: usize,
    pub sub_h: usize,
}

/// `(int)lround((double)n * prescale)`.
fn scale_dim(n: usize, prescale: f64) -> usize {
    let scaled = (n as f64 * prescale).round();
    // The C narrows the long to int and widens it to size_t; the value is
    // positive and below 2^31 for every option the parser accepts.
    scaled as i64 as i32 as usize
}

impl Dims {
    /// `speed_init_dimensions()`.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` when the operating plane is smaller than one
    /// block.
    pub fn new(w: usize, h: usize, prescale: f64) -> Result<Self, Error> {
        let scaled_h = scale_dim(h, prescale);
        let scaled_w = scale_dim(w, prescale);
        let operating_h = scaled_h >> NUM_SCALES;
        let operating_w = scaled_w >> NUM_SCALES;
        let trunc_w = (operating_w / BLOCK) * BLOCK;
        let trunc_h = operating_h / BLOCK * BLOCK;
        if trunc_h == 0 || trunc_w == 0 {
            return Err(Error::InvalidArgument(
                c"SpEED: image too small, operating width or height is 0",
            ));
        }
        let blocks_h = trunc_w / BLOCK;
        let blocks_v = trunc_h / BLOCK;
        Ok(Self {
            orig_w: w,
            orig_h: h,
            scaled_w,
            scaled_h,
            alloc_w: w.max(scaled_w),
            alloc_h: h.max(scaled_h),
            trunc_w,
            trunc_h,
            blocks_h,
            blocks_v,
            num_blocks: blocks_h * blocks_v,
            sub_w: trunc_w - BLOCK + 1,
            sub_h: trunc_h - BLOCK + 1,
        })
    }

    /// `speed_prescale_resamples()`: an identity prescale that keeps the size
    /// skips the resample.
    #[must_use]
    pub fn resamples(&self, prescale: f64) -> bool {
        let identity = (prescale - 1.0).abs() < 1.0e-3;
        !identity || self.scaled_w != self.orig_w || self.scaled_h != self.orig_h
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_prescale_keeps_size() {
        let d = Dims::new(288, 162, 1.0).ok();
        assert_eq!(d.map(|d| (d.scaled_w, d.scaled_h)), Some((288, 162)));
        assert_eq!(d.map(|d| (d.trunc_w, d.trunc_h)), Some((15, 10)));
        assert_eq!(d.map(|d| d.num_blocks), Some(6));
        assert_eq!(d.map(|d| d.resamples(1.0)), Some(false));
    }

    #[test]
    fn small_plane_is_refused() {
        assert!(Dims::new(79, 200, 1.0).is_err());
    }

    #[test]
    fn odd_identity_prescale_resamples_when_size_changes() {
        let d = Dims::new(576, 324, 1.0009).ok();
        assert_eq!(d.map(|d| d.scaled_w), Some(577));
        assert_eq!(d.map(|d| d.resamples(1.0009)), Some(true));
    }
}
