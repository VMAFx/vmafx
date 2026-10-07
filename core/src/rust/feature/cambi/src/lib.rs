// Copyright 2016-2026 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
//! Rust twin of the C `cambi` feature extractor (RC4 lane C, ADR-1713).
//!
//! A statement-by-statement port of the scalar path of
//! `core/src/feature/cambi.c` (with `cambi.h` and the luminance model of
//! `luminance_tools.cpp`): the C extractor of the same build is the reference
//! and every score is bit-identical to it. The twin `cambi_rust` inherits the
//! C option table; it implements every option except `heatmaps_path`, which it
//! refuses at init.
//!
//! Per frame: preprocess plane 0 to 10 bits at the encode size, spatial mask,
//! then five scales of decimate, mode filter, c-values and top-k pooling;
//! with `full_ref` the same for the reference at the source size.

// No unsafe code; the test hooks' `no_mangle` attribute is the one exception
// (contract section 5: `testhook.rs` is the lane's single unsafe module).
#![deny(unsafe_code)]

mod cvalues;
mod filter;
mod luminance;
mod lut;
mod mask;
mod options;
mod pooling;
mod preprocess;
#[allow(unsafe_code)]
pub mod testhook;

use vmafx_fex::{Error, Extractor, Frame, Geometry, Host, Picture, VmafxRsTwin, try_filled_vec};

use crate::cvalues::{Scale, Tables};
use crate::luminance::{Band, Contrast};
use crate::mask::{MaskScratch, Plane10};
use crate::options::{CambiOptions, PassGeometry, Settings};
use crate::pooling::NUM_SCALES;
use crate::preprocess::Work;

/// The twins this crate registers.
pub const TWINS: &[VmafxRsTwin] = &[vmafx_fex::twin::<Cambi>(c"cambi", c"cambi_rust")];

/// The working buffers, sized for the larger pass in `init`.
struct Buffers {
    /// The 10-bit image (`pics[0]`), `alloc_w` samples per row.
    image: Vec<u16>,
    /// The spatial mask (`pics[1]`), same geometry.
    mask: Vec<u16>,
    /// One c-value per pixel of a scale.
    c_values: Vec<f32>,
    /// `v_band_size` histogram rows of `alloc_w` cells.
    histograms: Vec<u16>,
    /// The cyclic dp matrix of the spatial mask.
    mask_dp: Vec<u32>,
    /// The 3-line ring of `filter_mode`.
    filter_mode: Vec<u16>,
    /// One derivative row.
    derivative: Vec<u16>,
}

impl Buffers {
    fn new(s: &Settings, band: Band) -> Result<Self, Error> {
        let (w, h) = (s.alloc_w, s.alloc_h);
        Ok(Self {
            image: try_filled_vec(w * h, 0)?,
            mask: try_filled_vec(w * h, 0)?,
            c_values: try_filled_vec(w * h, 0.0)?,
            histograms: try_filled_vec(w * usize::from(band.size), 0)?,
            mask_dp: try_filled_vec(mask::DP_HEIGHT * mask::dp_width(w), 0)?,
            filter_mode: try_filled_vec(3 * w, 0)?,
            derivative: try_filled_vec(w, 0)?,
        })
    }
}

/// One `cambi` context.
pub struct Cambi {
    settings: Settings,
    contrast: Contrast,
    band: Band,
    buf: Buffers,
}

impl Extractor for Cambi {
    type Options = CambiOptions;

    fn init(opts: &CambiOptions, geom: &Geometry) -> Result<Self, Error> {
        let settings = Settings::new(opts, geom)?;
        let (contrast, band) = Contrast::new(
            settings.num_diffs,
            settings.tvi_threshold,
            settings.vis_lum_threshold,
            settings.eotf,
        )?;
        let buf = Buffers::new(&settings, band)?;
        Ok(Self {
            settings,
            contrast,
            band,
            buf,
        })
    }

    fn extract(&mut self, frame: &Frame<'_>, host: &mut Host<'_>) -> Result<(), Error> {
        let s = self.settings;
        let dist = self.score(&frame.distorted, s.enc)?;
        host.emit(
            c"Cambi_feature_cambi_score",
            frame.index,
            min(dist, s.max_val),
        )?;
        if s.full_ref {
            let src = self.score(&frame.reference, s.src)?;
            host.emit(c"cambi_source", frame.index, min(src, s.max_val))?;
            // MAX(0, dist - src): `0 > d ? 0 : d`.
            let d = dist - src;
            let combined = if 0.0 > d { 0.0 } else { d };
            host.emit(
                c"cambi_full_reference",
                frame.index,
                min(combined, s.max_val),
            )?;
        }
        Ok(())
    }
}

/// C `MIN(x, y)`: `x < y ? x : y`.
fn min(x: f64, y: f64) -> f64 {
    if x < y { x } else { y }
}

impl Cambi {
    /// `preprocess_and_extract_cambi` for one picture.
    fn score(&mut self, pic: &Picture<'_>, pass: PassGeometry) -> Result<f64, Error> {
        let plane = pic.plane(0)?;
        preprocess::validate(&plane, pic.bpc())?;
        let stride = self.settings.alloc_w;
        let mut work = Work {
            data: &mut self.buf.image,
            stride,
            width: pass.width,
            height: pass.height,
        };
        preprocess::convert(&plane, pic.bpc(), &mut work, self.settings.enc_bitdepth);
        Ok(self.cambi_score(pass))
    }

    /// `cambi_score`: spatial mask, then the five scales.
    fn cambi_score(&mut self, pass: PassGeometry) -> f64 {
        let stride = self.settings.alloc_w;
        let b = &mut self.buf;
        let full = Plane10 {
            data: &b.image,
            stride,
            width: pass.width,
            height: pass.height,
        };
        let mut scratch = MaskScratch {
            dp: &mut b.mask_dp,
            deriv: &mut b.derivative,
        };
        mask::spatial_mask(&full, &mut b.mask, &mut scratch);
        let tables = Tables {
            contrast: &self.contrast,
            band: self.band,
        };
        let mut scores = [0.0_f64; NUM_SCALES];
        let (mut w, mut h) = (pass.width, pass.height);
        for (scale, out) in scores.iter_mut().enumerate() {
            if scale > 0 || self.settings.high_res_speedup {
                w = (w + 1) >> 1;
                h = (h + 1) >> 1;
                filter::decimate(&mut b.image, stride, w, h);
                filter::decimate(&mut b.mask, stride, w, h);
            }
            filter::filter_mode(&mut b.image, stride, w, h, &mut b.filter_mode);
            let level = Scale {
                image: Plane10 {
                    data: &b.image,
                    stride,
                    width: w,
                    height: h,
                },
                mask: &b.mask,
            };
            cvalues::calculate(
                &mut b.c_values,
                &mut b.histograms,
                &level,
                pass.window,
                &tables,
            );
            *out = pooling::spatial_pooling(&mut b.c_values, self.settings.topk, w, h);
        }
        pooling::weight_scores(&scores, pooling::pixels_in_window(pass.window))
    }
}
