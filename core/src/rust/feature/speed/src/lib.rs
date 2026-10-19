// Copyright 2016-2025 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
//! Rust twin of the C `speed_chroma` extractor (`core/src/feature/speed.c`,
//! its helpers in `vif_tools.c` and `picture_copy.cpp`), RC4 lane S
//! (ADR-1713), ported statement by statement from Netflix code. Scores equal
//! the C extractor's bit for bit (`scripts/ci/rust_twin_diff.py --feature
//! speed_chroma`): the arithmetic keeps the C's float and double steps, its
//! evaluation order and its libm calls (`exp`, `sin`, `log2`).
//!
//! The C option table, its defaults, aliases and ranges stay in C: this crate
//! only reads the parsed values.

#![forbid(unsafe_code)]
// The ported loops index several arrays with computed offsets, as the C does;
// iterator rewrites would hide which element of which array is read.
#![allow(clippy::needless_range_loop)]

mod cov;
mod dims;
mod eigen;
mod engine;
mod filter;
mod qr;
mod scale;
mod score;

use vmafx_fex::picture::PlaneView;
use vmafx_fex::{
    Error, Extractor, Frame, FromOptions, Geometry, Host, LogLevel, OptionValues, PixFmt, Plane,
    VmafxRsTwin, twin,
};

pub use engine::Tally;
use engine::{Engine, Params, PlaneSource};
use scale::Method;

/// What the sibling modules need from the framework.
mod fexapi {
    pub use vmafx_fex::libm;
    pub use vmafx_fex::{Error, try_filled_vec};
}

/// The twins this crate registers.
pub const TWINS: &[VmafxRsTwin] = &[twin::<SpeedChroma>(c"speed_chroma", c"speed_chroma_rust")];

/// `picture_copy()`'s offset for SpEED.
const OFFSET: f32 = -128.0;

/// The options of `options_chroma[]` in speed.c.
#[derive(Clone, Copy, Debug)]
pub struct SpeedChromaOptions {
    params: Params,
    max_val: f64,
}

impl FromOptions for SpeedChromaOptions {
    fn from_options(o: &OptionValues<'_>) -> Result<Self, Error> {
        let Some(name) = o.str(c"speed_prescale_method")? else {
            return Err(Error::InvalidArgument(c"speed_prescale_method is not set"));
        };
        let Some(method) = Method::from_name(name) else {
            return Err(Error::InvalidArgument(
                c"invalid speed_prescale_method: supported are nearest, bilinear, bicubic, lanczos4",
            ));
        };
        let weight_var_mode = o.int(c"speed_weight_var_mode")?;
        if !(0..=6).contains(&weight_var_mode) {
            return Err(Error::InvalidArgument(c"invalid speed_weight_var_mode"));
        }
        Ok(Self {
            params: Params {
                kernelscale: o.f64(c"speed_kernelscale")?,
                prescale: o.f64(c"speed_prescale")?,
                method,
                sigma_nn: o.f64(c"speed_sigma_nn")?,
                nn_floor: o.f64(c"speed_nn_floor")?,
                weight_var_mode,
            },
            max_val: o.f64(c"speed_max_val")?,
        })
    }
}

/// `speed_chroma_dimensions()`: the chroma plane extents.
fn chroma_dimensions(w: u32, h: u32, pix_fmt: PixFmt) -> Result<(usize, usize), Error> {
    let half = |n: u32| ((n >> 1) + (n & 1)) as usize;
    match pix_fmt {
        PixFmt::Yuv420p => Ok((half(w), half(h))),
        PixFmt::Yuv422p => Ok((half(w), h as usize)),
        PixFmt::Yuv444p => Ok((w as usize, h as usize)),
        _ => Err(Error::InvalidArgument(
            c"speed_chroma needs a picture with chroma planes",
        )),
    }
}

/// One chroma plane of a picture, read the way `picture_copy()` reads it.
struct ChromaPlane<'a> {
    plane: Plane<'a>,
    bpc: u32,
    w: usize,
    h: usize,
}

/// `picture_copy()`'s 8-bit path: `(float)sample + (float)offset`.
fn load_u8(p: &PlaneView<'_, u8>, dst: &mut [f32], stride: usize) {
    for y in 0..p.height() {
        for (d, &s) in dst[y * stride..].iter_mut().zip(p.row(y)) {
            *d = f32::from(s) + OFFSET;
        }
    }
}

/// `picture_copy_hbd()`: `(float)sample / scaler + (float)offset`.
fn load_u16(p: &PlaneView<'_, u16>, scaler: f32, dst: &mut [f32], stride: usize) {
    for y in 0..p.height() {
        for (d, &s) in dst[y * stride..].iter_mut().zip(p.row(y)) {
            *d = f32::from(s) / scaler + OFFSET;
        }
    }
}

impl PlaneSource for ChromaPlane<'_> {
    fn load(&self, dst: &mut [f32], stride: usize) -> Result<(), Error> {
        if self.plane.width() != self.w || self.plane.height() != self.h {
            return Err(Error::InvalidArgument(
                c"speed_chroma: plane size differs from init",
            ));
        }
        match (self.plane, self.bpc) {
            (Plane::U8(p), _) => load_u8(&p, dst, stride),
            (Plane::U16(p), 10) => load_u16(&p, 4.0, dst, stride),
            (Plane::U16(p), 12) => load_u16(&p, 16.0, dst, stride),
            (Plane::U16(p), 16) => load_u16(&p, 256.0, dst, stride),
            _ => return Err(Error::Unsupported(c"speed_chroma: unsupported bit depth")),
        }
        Ok(())
    }
}

/// Per-context state of the twin (`SpeedChromaState`).
pub struct SpeedChroma {
    engine: Engine,
    max_val: f64,
    w: usize,
    h: usize,
}

/// `speed_internal_clamp_score()`: a non-finite score fails the frame, a
/// finite one is bounded by `max_val`.
fn clamp_score(score: f64, max_val: f64, what: &'static core::ffi::CStr) -> Result<f64, Error> {
    if !score.is_finite() {
        return Err(Error::NonFinite(what));
    }
    Ok(if score < max_val { score } else { max_val })
}

/// `extract_chroma()`'s combination of the two channel scores: a channel
/// whose solve failed is imputed from the other one.
fn combine_uv(u: (f32, bool), v: (f32, bool)) -> f32 {
    match (u.1, v.1) {
        (true, false) => v.0,
        (false, true) => u.0,
        _ => (f64::from(u.0 + v.0) / 2.0) as f32,
    }
}

/// Chroma plane `p` of a picture with the geometry the twin was built for.
fn chroma_plane<'a>(
    pic: &vmafx_fex::Picture<'a>,
    p: usize,
    (w, h): (usize, usize),
) -> Result<ChromaPlane<'a>, Error> {
    Ok(ChromaPlane {
        plane: pic.plane(p)?,
        bpc: pic.bpc(),
        w,
        h,
    })
}

impl SpeedChroma {
    /// Singular-solve and eigen-cap counters of the run.
    #[must_use]
    pub fn tally(&self) -> Tally {
        self.engine.tally()
    }

    /// The C's log lines of the solves since `before`: the eigenvalue cap
    /// warning every time, the singular warning once (the count follows at
    /// close).
    fn log_new(&self, host: &Host<'_>, before: Tally) {
        let now = self.tally();
        for _ in before.eigen_cap_hits..now.eigen_cap_hits {
            host.log(
                LogLevel::Warning,
                c"compute_eigenvalues_tridiagonal: max iterations reached, possible non-convergence",
            );
        }
        if before.singular == 0 && now.singular > 0 {
            host.log(
                LogLevel::Warning,
                c"est_params: covariance matrix singular, zeroing solution \u{2014} further occurrences are counted and reported once at close",
            );
        }
    }

    /// `extract_channel()`: the score of chroma plane `p` and whether a
    /// covariance matrix was singular.
    fn channel(&mut self, frame: &Frame<'_>, p: usize) -> Result<(f32, bool), Error> {
        let dims = (self.w, self.h);
        let reference = chroma_plane(&frame.reference, p, dims)?;
        let distorted = chroma_plane(&frame.distorted, p, dims)?;
        self.engine.channel_score(&reference, &distorted)
    }
}

impl Extractor for SpeedChroma {
    type Options = SpeedChromaOptions;

    fn init(opts: &SpeedChromaOptions, geom: &Geometry) -> Result<Self, Error> {
        let (w, h) = chroma_dimensions(geom.w, geom.h, geom.pix_fmt)?;
        Ok(Self {
            engine: Engine::new(w, h, opts.params)?,
            max_val: opts.max_val,
            w,
            h,
        })
    }

    fn extract(&mut self, frame: &Frame<'_>, host: &mut Host<'_>) -> Result<(), Error> {
        let before = self.tally();
        let u = self.channel(frame, 1)?;
        self.log_new(host, before);
        let before = self.tally();
        let v = self.channel(frame, 2)?;
        self.log_new(host, before);
        let uv = combine_uv(u, v);
        let max = self.max_val;
        let clamped_u = clamp_score(f64::from(u.0), max, c"speed_chroma_u is not finite")?;
        let clamped_v = clamp_score(f64::from(v.0), max, c"speed_chroma_v is not finite")?;
        let clamped_uv = clamp_score(f64::from(uv), max, c"speed_chroma_uv is not finite")?;
        let eu = host.emit(
            c"Speed_chroma_feature_speed_chroma_u_score",
            frame.index,
            clamped_u,
        );
        let ev = host.emit(
            c"Speed_chroma_feature_speed_chroma_v_score",
            frame.index,
            clamped_v,
        );
        let euv = host.emit(
            c"Speed_chroma_feature_speed_chroma_uv_score",
            frame.index,
            clamped_uv,
        );
        eu.and(ev).and(euv)
    }

    /// `speed_close()`: report the singular solves once.
    fn close(&mut self, host: &Host<'_>) {
        let t = self.tally();
        if t.singular > 0 {
            host.log_fmt(
                LogLevel::Warning,
                format_args!(
                    "est_params: covariance matrix was singular on {} of {} solves",
                    t.singular, t.solves
                ),
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `s >> 24` of the numerical-recipes LCG, one value per sample.
    fn lcg(seed: u32, n: usize) -> Vec<u32> {
        let mut s = seed;
        (0..n)
            .map(|_| {
                s = s.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                s >> 24
            })
            .collect()
    }

    /// The synthetic chroma plane of `scripts`' generator: a gradient plus
    /// noise for the reference, the reference plus a little noise for the
    /// distorted picture.
    fn plane(seed: u32, (w, h): (usize, usize), maxv: u32, reference: Option<&[u32]>) -> Vec<u32> {
        let noise = lcg(seed, w * h);
        let range = maxv + 1;
        (0..w * h)
            .map(|i| match reference {
                None => (noise[i] * range / 256 + (i % w) as u32 * 3 + (i / w) as u32 * 2) % range,
                Some(r) => (r[i] + (noise[i] >> 5) * (range / 256)).min(maxv),
            })
            .collect()
    }

    /// One synthetic picture's plane in the sample type of `bpc`.
    struct Samples {
        u8s: Vec<u8>,
        u16s: Vec<u16>,
        bpc: u32,
        dims: (usize, usize),
    }

    impl Samples {
        fn new(v: &[u32], bpc: u32, dims: (usize, usize)) -> Self {
            Self {
                u8s: v.iter().map(|&x| x as u8).collect(),
                u16s: v.iter().map(|&x| x as u16).collect(),
                bpc,
                dims,
            }
        }

        fn chroma(&self) -> Option<ChromaPlane<'_>> {
            let (w, h) = self.dims;
            let plane = if self.bpc <= 8 {
                PlaneView::from_slice(&self.u8s, w, h, w).map(Plane::U8)
            } else {
                PlaneView::from_slice(&self.u16s, w, h, w).map(Plane::U16)
            };
            Some(ChromaPlane {
                plane: plane.ok()?,
                bpc: self.bpc,
                w,
                h,
            })
        }
    }

    /// The (score, singular) of chroma channel `seed` through the real
    /// sample-loading path.
    fn channel(engine: &mut Engine, seed: u32, bpc: u32, dims: (usize, usize)) -> (f32, bool) {
        let maxv = (1_u32 << bpc) - 1;
        let r = plane(seed, dims, maxv, None);
        let d = plane(seed + 100, dims, maxv, Some(&r));
        let (rs, ds) = (Samples::new(&r, bpc, dims), Samples::new(&d, bpc, dims));
        match (rs.chroma(), ds.chroma()) {
            (Some(rp), Some(dp)) => engine.channel_score(&rp, &dp).unwrap_or((f32::NAN, true)),
            _ => (f32::NAN, true),
        }
    }

    /// The scores `extract()` publishes for one synthetic picture pair
    /// (clamped u, v, uv), as bit patterns.
    fn scores(luma: (usize, usize), bpc: u32, params: Params, max_val: f64) -> [u64; 3] {
        let dims = (luma.0.div_ceil(2), luma.1.div_ceil(2));
        let Ok(mut engine) = Engine::new(dims.0, dims.1, params) else {
            return [0; 3];
        };
        let u = channel(&mut engine, 11, bpc, dims);
        let v = channel(&mut engine, 23, bpc, dims);
        let clamp = |s: f32| f64::from(s).min(max_val).to_bits();
        [clamp(u.0), clamp(v.0), clamp(combine_uv(u, v))]
    }

    fn params(prescale: f64, method: Method, sigma_nn: f64, nn_floor: f64, mode: i32) -> Params {
        Params {
            kernelscale: 1.0,
            prescale,
            method,
            sigma_nn,
            nn_floor,
            weight_var_mode: mode,
        }
    }

    // Expected bits are the C extractor's output (`vmaf --precision max`, build
    // of this tree) for the same synthetic frames.
    #[test]
    fn default_options_match_the_c_extractor() {
        let p = params(1.0, Method::Nearest, 0.29, 0.0, 0);
        let got = scores((1280, 768), 8, p, 1000.0);
        assert_eq!(
            got,
            [
                0x4000_a3f0_0000_0000,
                0x3ffc_7942_c000_0000,
                0x3ffe_e091_6000_0000
            ]
        );
    }

    #[test]
    fn bilinear_prescale_matches_the_c_extractor() {
        let p = params(0.5, Method::Bilinear, 0.19, 0.1, 5);
        let got = scores((1920, 1152), 8, p, 45.0);
        assert_eq!(
            got,
            [
                0x3fe2_065d_6000_0000,
                0x3fe1_2b82_a000_0000,
                0x3fe1_98f0_0000_0000
            ]
        );
    }

    #[test]
    fn ten_bit_samples_match_the_c_extractor() {
        let p = params(1.0, Method::Nearest, 0.29, 0.0, 0);
        let got = scores((1280, 768), 10, p, 1000.0);
        assert_eq!(
            got,
            [
                0x4001_850a_0000_0000,
                0x3ff9_7290_c000_0000,
                0x3ffe_3e52_6000_0000
            ]
        );
    }

    #[test]
    fn uv_is_imputed_from_the_regular_channel() {
        assert_eq!(
            combine_uv((1.0, true), (3.0, false)).to_bits(),
            3.0_f32.to_bits()
        );
        assert_eq!(
            combine_uv((1.0, false), (3.0, true)).to_bits(),
            1.0_f32.to_bits()
        );
        assert_eq!(
            combine_uv((1.0, false), (3.0, false)).to_bits(),
            2.0_f32.to_bits()
        );
    }

    #[test]
    fn non_finite_scores_fail_and_finite_ones_are_clamped() {
        assert!(clamp_score(f64::NAN, 45.0, c"x").is_err());
        assert_eq!(clamp_score(50.0, 45.0, c"x"), Ok(45.0));
        assert_eq!(clamp_score(3.5, 45.0, c"x"), Ok(3.5));
    }

    #[test]
    fn chroma_extents_round_up_for_odd_sizes() {
        assert_eq!(chroma_dimensions(5, 7, PixFmt::Yuv420p), Ok((3, 4)));
        assert_eq!(chroma_dimensions(5, 7, PixFmt::Yuv422p), Ok((3, 7)));
        assert_eq!(chroma_dimensions(5, 7, PixFmt::Yuv444p), Ok((5, 7)));
        assert!(chroma_dimensions(5, 7, PixFmt::Yuv400p).is_err());
    }
}
