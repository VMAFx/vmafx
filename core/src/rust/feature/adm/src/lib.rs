// Copyright 2016-2020 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
//! Rust twin of the integer ADM extractor `adm` (RC4, #1723).
//!
//! A statement-by-statement port of the scalar path of
//! `core/src/feature/integer_adm.c` and `integer_adm_kernels.h`: the same
//! Daubechies-2 DWT, decouple, CSF, denominator and contrast-masking stages
//! in the same integer widths and the same float evaluation order, with the
//! same libm calls. It emits every feature the C extractor emits (`adm2`,
//! `aim`, `adm3`, the four scale ratios and, with `debug`, the numerators and
//! denominators), bit for bit. Registered as `adm_rust`; selected with
//! `VMAF_FEATURE_IMPL=rust` or `--feature adm_rust`.
//!
//! With `adm_norm_view_dist_extra` it evaluates a second viewing distance on
//! the same DWT and decouple and files those scores under the
//! [`score::EXTRA_VIEW_NAMES`] keys, which the C descriptor's
//! `extend_name_dict` maps to that distance's feature names (ADR-2795).

#![forbid(unsafe_code)]

mod bands;
mod cm;
mod csf;
mod csf_weights;
mod decouple;
mod den;
mod dwt;
mod options;
mod region;
mod score;
mod tables;
#[cfg(test)]
mod tables_c;

use vmafx_fex::{Error, Extractor, Frame, Geometry, Host, Picture, Plane, VmafxRsTwin, twin};

use crate::bands::AdmBuffers;
use crate::cm::{CmArgs, CmBands};
use crate::csf::CsfArgs;
use crate::csf_weights::{ScaleWeights, cos_1deg_sq, scale_weights, viewing_geometry_check};
use crate::decouple::DecoupleArgs;
use crate::den::DenArgs;
use crate::dwt::{DwtGeom, Source};
use crate::score::AdmResult;

pub use crate::options::AdmOptions;

/// The twins this crate provides.
pub const TWINS: &[VmafxRsTwin] = &[twin::<IntegerAdm>(c"adm", c"adm_rust")];

/// `ADM_MIN_FRAME_DIM`: below 17 pixels the scale-3 band is one sample.
const ADM_MIN_FRAME_DIM: u32 = 17;

/// Numerator, denominator and AIM numerator of one scale (`AdmScaleScores`).
#[derive(Clone, Copy, Default)]
struct ScaleScores {
    num: f32,
    den: f32,
    aim_num: f32,
}

/// The integer ADM twin (`AdmState`).
pub struct IntegerAdm {
    opts: AdmOptions,
    /// CSF weights of scales 0..3 per viewing distance, valid when
    /// `config_err` is `None`.
    weights: [[ScaleWeights; 4]; 2],
    /// `csf_config_err`: decided in init, returned by every extract.
    config_err: Option<Error>,
    cos_1deg_sq: f32,
    buf: AdmBuffers,
}

/// The luma plane of a picture, typed by its bit depth.
fn source<'a>(p: &Picture<'a>) -> Result<Source<'a>, Error> {
    match p.plane(0)? {
        Plane::U8(v) => Ok(Source::U8(v)),
        Plane::U16(v) => Ok(Source::U16(v, p.bpc())),
    }
}

/// Width and height of a source plane.
const fn dims(s: &Source<'_>) -> (usize, usize) {
    match s {
        Source::U8(v) => (v.width(), v.height()),
        Source::U16(v, _) => (v.width(), v.height()),
    }
}

/// C `int` of a frame dimension (bounded by the picture allocation).
#[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
const fn as_int(x: usize) -> i32 {
    x as i32
}

impl IntegerAdm {
    fn decouple_args(&self, w2: usize, h2: usize) -> DecoupleArgs {
        DecoupleArgs {
            w: as_int(w2),
            h: as_int(h2),
            stride: self.buf.stride,
            gain: self.opts.enhn_gain_limit,
            cos_1deg_sq: self.cos_1deg_sq,
        }
    }

    fn cm_args(&self, w2: usize, h2: usize, noise_weight: f64) -> CmArgs {
        CmArgs {
            w: as_int(w2),
            h: as_int(h2),
            stride: self.buf.stride,
            noise_weight,
            p_norm: self.opts.p_norm,
        }
    }

    /// The scale-0 DWT of both pictures (low-pass only with `lo_only`) and
    /// the widening of band_a for scale 1.
    fn dwt0(&mut self, r: Source<'_>, d: Source<'_>, g: DwtGeom, lo_only: bool) {
        let b = &mut self.buf;
        let ind = (&b.ind_y, &b.ind_x);
        dwt::dwt_scale0(r, &mut b.p16.ref_dwt, &mut b.tmp16, ind, g, lo_only);
        dwt::dwt_scale0(d, &mut b.p16.dis_dwt, &mut b.tmp16, ind, g, lo_only);
        dwt::widen_band_a(&b.p16.ref_dwt.a, &mut b.p32.ref_dwt.a, g);
        dwt::widen_band_a(&b.p16.dis_dwt.a, &mut b.p32.dis_dwt.a, g);
    }

    fn den_args(&self, w2: usize, h2: usize) -> DenArgs {
        DenArgs {
            w: as_int(w2),
            h: as_int(h2),
            stride: self.buf.stride,
            noise_weight: self.opts.noise_weight,
        }
    }

    fn csf_args(&self, w2: usize, h2: usize, measure_aim: bool) -> CsfArgs {
        CsfArgs {
            w: as_int(w2),
            h: as_int(h2),
            stride: self.buf.stride,
            measure_aim,
        }
    }

    /// Scale 0: the CSF stage and the contrast-masking reduction of the
    /// numerator, or with `measure_aim` of the AIM numerator (noise weight 0).
    fn masked_s0(&mut self, w2: usize, h2: usize, view: usize, measure_aim: bool) -> f32 {
        let wt = self.weights[view][0];
        let ca = self.csf_args(w2, h2, measure_aim);
        csf::csf_s0(&mut self.buf.p16, wt.fixed, &ca);
        let nw = if measure_aim {
            0.0
        } else {
            self.opts.noise_weight
        };
        let args = self.cm_args(w2, h2, nw);
        let p = &self.buf.p16;
        let bands = if measure_aim {
            CmBands {
                src: &p.decouple_a,
                angles: &p.csf_f,
                flt: &p.csf_a,
            }
        } else {
            CmBands {
                src: &p.decouple_r,
                angles: &p.csf_a,
                flt: &p.csf_f,
            }
        };
        cm::cm_s0(bands, wt.fixed, wt.norm_shift, &args)
    }

    /// Scales 1..3: the 32-bit twin of `masked_s0`.
    fn masked_s123(
        &mut self,
        w2: usize,
        h2: usize,
        scale: usize,
        view: usize,
        measure_aim: bool,
    ) -> f32 {
        let wt = self.weights[view][scale];
        let ca = self.csf_args(w2, h2, measure_aim);
        csf::csf_s123(&mut self.buf.p32, wt.fixed, &ca);
        let nw = if measure_aim {
            0.0
        } else {
            self.opts.noise_weight
        };
        let args = self.cm_args(w2, h2, nw);
        let p = &self.buf.p32;
        let bands = if measure_aim {
            CmBands {
                src: &p.decouple_a,
                angles: &p.csf_f,
                flt: &p.csf_a,
            }
        } else {
            CmBands {
                src: &p.decouple_r,
                angles: &p.csf_a,
                flt: &p.csf_f,
            }
        };
        cm::cm_s123(bands, scale, wt.fixed, wt.norm_shift, &args)
    }

    /// `integer_adm_scale0_transform()`: the DWT of both pictures and the
    /// decouple; with `adm_skip_scale0` only the low-pass DWT.
    fn transform0(&mut self, r: Source<'_>, d: Source<'_>, w: usize, h: usize) {
        let g = DwtGeom {
            w,
            h,
            stride: self.buf.stride,
        };
        if self.opts.skip_scale0 {
            self.dwt0(r, d, g, true);
            return;
        }
        self.dwt0(r, d, g, false);
        let da = self.decouple_args(w.div_ceil(2), h.div_ceil(2));
        decouple::decouple_s0(&mut self.buf.p16, &da);
    }

    /// `integer_adm_scale0_weigh()` at viewing distance `view`: with
    /// `adm_skip_scale0` the denominator is seeded with 1e-10.
    fn weigh0(&mut self, w2: usize, h2: usize, view: usize) -> ScaleScores {
        if self.opts.skip_scale0 {
            // `sc->den = 1e-10`, a float.
            #[allow(clippy::cast_possible_truncation)]
            return ScaleScores {
                num: 0.0,
                den: 1e-10_f64 as f32,
                aim_num: 0.0,
            };
        }
        let factors = self.weights[view][0].factors;
        let den = den::den_s0(&self.buf.p16.ref_dwt.hvd, factors, &self.den_args(w2, h2));
        let num = self.masked_s0(w2, h2, view, false);
        let aim_num = if self.opts.skip_aim {
            0.0
        } else {
            self.masked_s0(w2, h2, view, true)
        };
        ScaleScores { num, den, aim_num }
    }

    /// `integer_adm_scale_s123_transform()`.
    fn transform_s123(&mut self, w: usize, h: usize, scale: usize) {
        let g = DwtGeom {
            w,
            h,
            stride: self.buf.stride,
        };
        let b = &mut self.buf;
        let ind = (&b.ind_y, &b.ind_x);
        dwt::dwt_s123(
            &mut b.p32.ref_dwt,
            &mut b.p32.dis_dwt,
            &mut b.tmp32,
            ind,
            g,
            scale,
        );
        let da = self.decouple_args(w.div_ceil(2), h.div_ceil(2));
        decouple::decouple_s123(&mut self.buf.p32, &da);
    }

    /// `integer_adm_scale_s123_weigh()` at viewing distance `view`.
    fn weigh_s123(&mut self, w2: usize, h2: usize, scale: usize, view: usize) -> ScaleScores {
        let factors = self.weights[view][scale].factors;
        let den = den::den_s123(
            &self.buf.p32.ref_dwt.hvd,
            scale,
            factors,
            &self.den_args(w2, h2),
        );
        let num = self.masked_s123(w2, h2, scale, view, false);
        let aim_num = if self.opts.skip_aim {
            0.0
        } else {
            self.masked_s123(w2, h2, scale, view, true)
        };
        ScaleScores { num, den, aim_num }
    }

    /// The two pictures' luma planes, checked against the init geometry.
    fn sources<'a>(
        &self,
        frame: &Frame<'a>,
    ) -> Result<(Source<'a>, Source<'a>, usize, usize), Error> {
        let (r, d) = (source(&frame.reference)?, source(&frame.distorted)?);
        let (w, h) = dims(&r);
        let same_kind = matches!(
            (r, d),
            (Source::U8(_), Source::U8(_)) | (Source::U16(..), Source::U16(..))
        );
        if dims(&d) != (w, h) || !same_kind {
            return Err(Error::InvalidArgument(
                c"integer_adm: reference and distorted pictures differ",
            ));
        }
        let min = ADM_MIN_FRAME_DIM as usize;
        if w > self.buf.max_w || h > self.buf.max_h || w < min || h < min {
            return Err(Error::InvalidArgument(
                c"integer_adm: picture size differs from init",
            ));
        }
        Ok((r, d, w, h))
    }

    /// `integer_compute_adm()`: the four scales and the aggregate of every
    /// viewing distance; the DWT and decouple run once per scale.
    fn compute(&mut self, frame: &Frame<'_>) -> Result<[AdmResult; 2], Error> {
        let (r, d, w0, h0) = self.sources(frame)?;
        let numden_limit = 1e-10 * f64::from(as_int(w0) * as_int(h0)) / (1920.0 * 1080.0);
        let views = self.opts.view_count();
        let mut res = [AdmResult::default(); 2];
        let mut sums = [[0.0_f64; 3]; 2];
        let (mut w, mut h) = (w0, h0);
        for scale in 0..4 {
            dwt::src_indices(&mut self.buf.ind_y, h);
            dwt::src_indices(&mut self.buf.ind_x, w);
            if scale == 0 {
                self.transform0(r, d, w, h);
            } else {
                self.transform_s123(w, h, scale);
            }
            w = w.div_ceil(2);
            h = h.div_ceil(2);
            for view in 0..views {
                let sc = if scale == 0 {
                    self.weigh0(w, h, view)
                } else {
                    self.weigh_s123(w, h, scale, view)
                };
                sums[view][0] += f64::from(sc.num);
                sums[view][1] += f64::from(sc.den);
                sums[view][2] += f64::from(sc.aim_num);
                res[view].scores[2 * scale] = f64::from(sc.num);
                res[view].scores[2 * scale + 1] = f64::from(sc.den);
            }
        }
        for view in 0..views {
            let [num, den, aim_num] = sums[view];
            score::finalise(&mut res[view], num, den, aim_num, numden_limit)?;
        }
        Ok(res)
    }

    /// The scores of one viewing distance, filed (`finish_adm_view()`).
    fn finish_view(
        &self,
        host: &mut Host<'_>,
        index: u32,
        r: &AdmResult,
        view: usize,
    ) -> Result<(), Error> {
        if !r.score.is_finite() || !r.score_aim.is_finite() {
            return Err(Error::NonFinite(c"integer_adm: non-finite score"));
        }
        let adm3 = score::adm3(
            r.score,
            r.score_aim,
            self.opts.dlm_weight,
            self.opts.min_val,
        )?;
        let mut scale = [0.0_f64; 4];
        score::scale_ratios(&r.scores, &mut scale)?;
        score::emit(host, index, r, adm3, &scale, self.opts.debug, view)
    }
}

impl Extractor for IntegerAdm {
    type Options = AdmOptions;

    /// `init()`: frame-size check, CSF configuration check (its verdict is
    /// returned by every extract, as in C), buffers.
    fn init(opts: &AdmOptions, geom: &Geometry) -> Result<Self, Error> {
        if geom.w < ADM_MIN_FRAME_DIM || geom.h < ADM_MIN_FRAME_DIM {
            return Err(Error::InvalidArgument(
                c"integer_adm requires width >= 17 and height >= 17",
            ));
        }
        let mut weights = [[ScaleWeights::default(); 4]; 2];
        let mut config_err = None;
        'views: for (view, row) in weights.iter_mut().enumerate().take(opts.view_count()) {
            let cfg = opts.csf_config(view);
            for (scale, w) in row.iter_mut().enumerate() {
                match scale_weights(scale, &cfg) {
                    Ok(v) => *w = v,
                    Err(e) => {
                        config_err = Some(e);
                        break 'views;
                    }
                }
            }
        }
        let buf = AdmBuffers::new(geom.w as usize, geom.h as usize)?;
        Ok(Self {
            opts: *opts,
            weights,
            config_err,
            cos_1deg_sq: cos_1deg_sq(),
            buf,
        })
    }

    /// `extract()`.
    fn extract(&mut self, frame: &Frame<'_>, host: &mut Host<'_>) -> Result<(), Error> {
        for view in 0..self.opts.view_count() {
            viewing_geometry_check(&self.opts.csf_config(view))?;
        }
        if let Some(e) = self.config_err {
            return Err(e);
        }
        let results = self.compute(frame)?;
        for (view, r) in results.iter().enumerate().take(self.opts.view_count()) {
            self.finish_view(host, frame.index, r, view)?;
        }
        Ok(())
    }
}
