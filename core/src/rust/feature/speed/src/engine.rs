// Copyright 2016-2025 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The SpEED pipeline of one channel pair. Mirrors `filter_and_downscale()`,
// `solve_covariance_system()`, `est_params()`, `speed_extract_score()` and
// `speed_init()` of core/src/feature/speed.c; ported statement by statement
// from Netflix code. All buffers are sized in `Engine::new`; nothing is
// allocated per frame.

use crate::cov::{compute_covariance, compute_independent_term};
use crate::dims::{Dims, ELEMS, NUM_SCALES};
use crate::eigen::{EIGENVALUE_EPS, EigenWork, eigenvalues};
use crate::fexapi::{Error, try_filled_vec};
use crate::filter::{
    Geom, MAX_TAPS, fill_antialias, fill_filter, filter_dec16, filter_plane, validate_kernelscale,
};
use crate::qr::{QrWork, solve};
use crate::scale::{Method, Plane, scale_frame};
use crate::score::{
    Stats, base_entropy, entropy_constant, pointwise_product_and_division, speed_score,
    sum_columns, update_entropy,
};

/// The options of `SpeedOptions` the pipeline reads.
#[derive(Clone, Copy, Debug)]
pub struct Params {
    pub kernelscale: f64,
    pub prescale: f64,
    pub method: Method,
    pub sigma_nn: f64,
    pub nn_floor: f64,
    pub weight_var_mode: i32,
}

/// Where the pixels of one picture channel come from; fills the float plane
/// the way `picture_copy()` does.
pub trait PlaneSource {
    /// Write the plane into `dst` (row stride `stride` floats).
    ///
    /// # Errors
    ///
    /// An error when the plane does not have the size the engine was built for.
    fn load(&self, dst: &mut [f32], stride: usize) -> Result<(), Error>;
}

/// Singular-solve counters of `SpeedInternalSingularTally`, plus the number
/// of eigen iterations that hit the cap.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Tally {
    pub solves: u64,
    pub singular: u64,
    pub eigen_cap_hits: u64,
}

/// Everything `est_params()` needs besides the image.
struct Work {
    curr: Vec<f32>,
    tmp: Vec<f32>,
    cov: [f32; ELEMS * ELEMS],
    means: [f32; ELEMS],
    eigen: [f32; ELEMS],
    indep: Vec<f32>,
    sol: Vec<f32>,
    rect: Vec<f32>,
    eigen_work: EigenWork,
    qr_work: QrWork,
}

/// The per-instance constants of a run.
struct Config {
    dims: Dims,
    params: Params,
    stride: usize,
    taps_antialias: [f32; MAX_TAPS],
    n_antialias: usize,
    taps_gauss: [f32; MAX_TAPS],
    n_gauss: usize,
    sigma_nn: f32,
    entropy_constant: f64,
    base_entropy: f32,
}

/// The pipeline of one chroma channel size.
pub struct Engine {
    cfg: Config,
    frame: Vec<f32>,
    work: Work,
    ref_stats: Stats,
    dis_stats: Stats,
    tally: Tally,
}

impl Engine {
    /// `speed_init()` for a plane of `w` x `h`.
    ///
    /// # Errors
    ///
    /// `Error::InvalidArgument` for a plane that is too small or an invalid
    /// kernelscale; `Error::OutOfMemory` when a buffer cannot be reserved.
    pub fn new(w: usize, h: usize, params: Params) -> Result<Self, Error> {
        let dims = Dims::new(w, h, params.prescale)?;
        if !validate_kernelscale(params.kernelscale as f32) {
            return Err(Error::InvalidArgument(c"invalid speed_kernelscale"));
        }
        let stride = dims.alloc_w;
        let frame_size = stride * dims.alloc_h;
        let nb = dims.num_blocks;
        let mut taps_antialias = [0.0_f32; MAX_TAPS];
        let mut taps_gauss = [0.0_f32; MAX_TAPS];
        let ks = params.kernelscale as f32;
        let n_antialias = fill_antialias(&mut taps_antialias, NUM_SCALES, ks);
        let n_gauss = fill_filter(&mut taps_gauss, NUM_SCALES, ks);
        let sigma_nn = params.sigma_nn as f32;
        let entropy_constant = entropy_constant();
        let cfg = Config {
            dims,
            params,
            stride,
            taps_antialias,
            n_antialias,
            taps_gauss,
            n_gauss,
            sigma_nn,
            entropy_constant,
            base_entropy: base_entropy(sigma_nn, params.nn_floor as f32, entropy_constant),
        };
        let stats = || -> Result<Stats, Error> {
            Ok(Stats {
                entropies: try_filled_vec(nb, 0.0)?,
                variances: try_filled_vec(nb, 0.0)?,
            })
        };
        let work = Work {
            curr: try_filled_vec(frame_size, 0.0)?,
            tmp: try_filled_vec(frame_size, 0.0)?,
            cov: [0.0; ELEMS * ELEMS],
            means: [0.0; ELEMS],
            eigen: [0.0; ELEMS],
            indep: try_filled_vec(ELEMS * nb, 0.0)?,
            sol: try_filled_vec(ELEMS * nb, 0.0)?,
            rect: try_filled_vec(ELEMS * nb, 0.0)?,
            eigen_work: EigenWork::new(),
            qr_work: QrWork::new(),
        };
        Ok(Self {
            cfg,
            frame: try_filled_vec(frame_size, 0.0)?,
            work,
            ref_stats: stats()?,
            dis_stats: stats()?,
            tally: Tally::default(),
        })
    }

    /// The counters of `speed_close()`'s report.
    #[must_use]
    pub fn tally(&self) -> Tally {
        self.tally
    }

    /// `speed_extract_score()` for one channel: the score and whether the
    /// reference or the distorted image had a singular covariance matrix.
    ///
    /// # Errors
    ///
    /// Any error of the plane sources.
    pub fn channel_score(
        &mut self,
        reference: &dyn PlaneSource,
        distorted: &dyn PlaneSource,
    ) -> Result<(f32, bool), Error> {
        let err_ref = self.side(reference, true)?;
        let err_dis = self.side(distorted, false)?;
        // If only one of the two was numerically unstable the C returns 0
        // instead of an inflated score.
        let score = if err_ref != err_dis {
            0.0
        } else {
            speed_score(
                &self.ref_stats,
                &self.dis_stats,
                self.cfg.base_entropy,
                self.cfg.params.weight_var_mode,
            )
        };
        Ok((score, err_ref || err_dis))
    }

    /// Load, filter and estimate one image; `true` when its solve failed.
    fn side(&mut self, src: &dyn PlaneSource, is_ref: bool) -> Result<bool, Error> {
        src.load(&mut self.frame, self.cfg.stride)?;
        filter_and_downscale(&self.cfg, &mut self.frame, &mut self.work);
        let stats = if is_ref {
            &mut self.ref_stats
        } else {
            &mut self.dis_stats
        };
        Ok(est_params(
            &self.cfg,
            &mut self.work,
            &self.frame,
            stats,
            &mut self.tally,
        ))
    }
}

/// `filter_and_downscale()`: prescale, anti-alias filter, decimate by 16,
/// then subtract the local mean.
fn filter_and_downscale(cfg: &Config, frame: &mut [f32], work: &mut Work) {
    let d = &cfg.dims;
    let stride = cfg.stride;
    if d.resamples(cfg.params.prescale) {
        let n = stride * d.alloc_h;
        work.tmp[..n].copy_from_slice(&frame[..n]);
        let plane = Plane {
            stride,
            src_w: d.orig_w,
            src_h: d.orig_h,
            dst_w: d.scaled_w,
            dst_h: d.scaled_h,
        };
        scale_frame(cfg.params.method, &work.tmp, frame, plane);
    }
    let down_w = d.scaled_w >> NUM_SCALES;
    let down_h = d.scaled_h >> NUM_SCALES;
    let full = Geom {
        w: d.scaled_w,
        h: d.scaled_h,
        stride,
    };
    filter_dec16(
        &cfg.taps_antialias[..cfg.n_antialias],
        frame,
        &mut work.curr,
        &mut work.tmp,
        full,
    );
    for i in 0..down_h {
        frame[i * stride..i * stride + down_w]
            .copy_from_slice(&work.curr[i * stride..i * stride + down_w]);
    }
    let small = Geom {
        w: down_w,
        h: down_h,
        stride,
    };
    filter_plane(
        &cfg.taps_gauss[..cfg.n_gauss],
        frame,
        &mut work.curr,
        &mut work.tmp,
        small,
    );
    for i in 0..down_h {
        let row = i * stride..i * stride + down_w;
        for (f, &c) in frame[row.clone()].iter_mut().zip(&work.curr[row]) {
            *f -= c;
        }
    }
}

/// `solve_covariance_system()`; `true` when the matrix cannot be inverted.
fn solve_covariance_system(cfg: &Config, w: &mut Work, data: &[f32], tally: &mut Tally) -> bool {
    let d = &cfg.dims;
    compute_covariance(d, data, &mut w.cov, &mut w.means, cfg.stride);
    if eigenvalues(&w.cov, &mut w.eigen, &mut w.eigen_work) {
        tally.eigen_cap_hits += 1;
    }
    compute_independent_term(d, data, &mut w.indep, cfg.stride);
    let regular = !w.eigen.iter().any(|&e| f64::from(e) < EIGENVALUE_EPS);
    let failed = regular
        && solve(
            &mut w.qr_work,
            &w.cov,
            &w.indep,
            d.num_blocks,
            &mut w.sol,
            &mut w.rect,
        )
        .is_err();
    let cannot_invert = !regular || failed;
    tally.solves += 1;
    tally.singular += u64::from(cannot_invert);
    if cannot_invert {
        w.sol.fill(0.0);
    }
    cannot_invert
}

/// `est_params()`; `true` when the matrix could not be inverted.
fn est_params(
    cfg: &Config,
    w: &mut Work,
    data: &[f32],
    out: &mut Stats,
    tally: &mut Tally,
) -> bool {
    let nb = cfg.dims.num_blocks;
    let cannot_invert = solve_covariance_system(cfg, w, data, tally);
    pointwise_product_and_division(&mut w.sol, &w.indep, ELEMS as f32);
    sum_columns(&mut w.sol, nb);
    out.entropies.fill(0.0);
    for &ev in &w.eigen {
        let l = if ev < 0.0 { 0.0 } else { ev };
        update_entropy(
            &mut out.entropies,
            &w.sol,
            l,
            cfg.sigma_nn,
            cfg.entropy_constant,
        );
    }
    out.variances.copy_from_slice(&w.sol[..nb]);
    cannot_invert
}
