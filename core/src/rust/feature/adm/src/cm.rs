// Copyright 2016-2020 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Contrast-masking (numerator) reductions of the integer ADM pipeline, ported
// statement by statement from core/src/feature/integer_adm.c (adm_cm(),
// i4_adm_cm()), integer_adm_kernels.h (adm_cm_thresh(), i4_adm_cm_thresh(),
// adm_cm_accum_round(), i4_adm_cm_accum_round(), adm_cm_ctx_init(),
// i4_adm_cm_ctx_init(), the row / fold / result helpers) and
// adm_cm_accumulator.h (adm_cm_excess_s0(), adm_cm_round_row_total(), fork
// code).

use vmafx_fex::libm;

use crate::bands::Hvd;
use crate::csf::{I4_SHIFT_DST, I4_SHIFT_FLT, i4_round_terms};
use crate::region::{CmBounds, ceil_log2_u32, cm_bounds, half_shift};
use crate::tables::{I4_ONE_BY_15, ONE_BY_15};

/// `AdmCmBand`: fixed-point parameters of one band's cube reduction.
#[derive(Clone, Copy)]
struct CmBand {
    shift_sub: u32,
    add_shift_sq: i32,
    shift_sq: u32,
    add_shift_cub: u32,
    shift_cub: u32,
}

/// The band sets one contrast-masking reduction reads.
pub struct CmBands<'a, T> {
    /// Decoupled source (`decouple_r`, or `decouple_a` for AIM).
    pub src: &'a Hvd<T>,
    /// CSF-weighted centre bands (`csf_a`, or `csf_f` for AIM).
    pub angles: &'a Hvd<T>,
    /// 1/30-filtered neighbour bands (`csf_f`, or `csf_a` for AIM).
    pub flt: &'a Hvd<T>,
}

/// Geometry and options of one reduction.
#[derive(Clone, Copy)]
pub struct CmArgs {
    pub w: i32,
    pub h: i32,
    pub stride: usize,
    pub noise_weight: f64,
    pub p_norm: f64,
}

/// One row of every band a reduction reads: the source and centre bands at
/// row i, the filtered bands at rows i - 1, i and i + 1 (edge rules applied).
struct RowSlices<'a, T> {
    src: [&'a [T]; 3],
    angles: [&'a [T]; 3],
    flt: [[&'a [T]; 3]; 3],
}

/// The row before the first edge mirrors to 1, the one past the last clamps
/// to the last index (`adm_cm_thresh()`).
#[allow(clippy::cast_sign_loss)]
fn row_slices<'a, T>(b: &CmBands<'a, T>, a: &CmArgs, i: i32) -> RowSlices<'a, T> {
    let i_m1 = if i == 0 { 1 } else { i - 1 };
    let i_p1 = if i == a.h - 1 { a.h - 1 } else { i + 1 };
    let w = a.w as usize;
    let at = |v: &'a Vec<T>, r: i32| -> &'a [T] {
        let off = r as usize * a.stride;
        &v[off..off + w]
    };
    let f = |th: usize| {
        [
            at(&b.flt[th], i_m1),
            at(&b.flt[th], i),
            at(&b.flt[th], i_p1),
        ]
    };
    RowSlices {
        src: [at(&b.src[0], i), at(&b.src[1], i), at(&b.src[2], i)],
        angles: [
            at(&b.angles[0], i),
            at(&b.angles[1], i),
            at(&b.angles[2], i),
        ],
        flt: [f(0), f(1), f(2)],
    }
}

/// Columns j - 1, j, j + 1 with the same edge rules as the rows.
#[allow(clippy::cast_sign_loss)]
fn cols(a: &CmArgs, j: i32) -> [usize; 3] {
    let j_m1 = if j == 0 { 1 } else { j - 1 };
    let j_p1 = if j == a.w - 1 { a.w - 1 } else { j + 1 };
    [j_m1 as usize, j as usize, j_p1 as usize]
}

/// The eight filtered neighbours and the centre term of one band, in the C
/// order, accumulated in int32.
fn thresh_sum<T: Copy + Into<i32>>(f: &[&[T]; 3], c: [usize; 3], centre: i32) -> i32 {
    let mut sum = 0_i32;
    sum += f[0][c[0]].into();
    sum += f[0][c[1]].into();
    sum += f[0][c[2]].into();
    sum += f[1][c[0]].into();
    sum += centre;
    sum += f[1][c[2]].into();
    sum += f[2][c[0]].into();
    sum += f[2][c[1]].into();
    sum += f[2][c[2]].into();
    sum
}

/// `adm_cm_thresh()`: masking threshold at column `c[1]`. The 1/15 centre
/// tap stays in int32 (ADR-1402).
fn thresh_s0(r: &RowSlices<'_, i16>, c: [usize; 3]) -> i32 {
    let mut accum = 0_i32;
    for theta in 0..3 {
        let src = i32::from(r.angles[theta][c[1]]);
        let centre = ((ONE_BY_15 * src.wrapping_abs()) + 2048) >> 12;
        accum += thresh_sum(&r.flt[theta], c, centre);
    }
    accum
}

/// `i4_adm_cm_thresh()`: the centre tap is rounded with ADR-0155's INT32_MIN.
#[allow(clippy::cast_possible_truncation)]
fn thresh_s123(r: &RowSlices<'_, i32>, c: [usize; 3]) -> i32 {
    let (_, add_flt) = i4_round_terms();
    let mut accum = 0_i32;
    for theta in 0..3 {
        let src = r.angles[theta][c[1]];
        let prod = i64::from(I4_ONE_BY_15) * i64::from(src.wrapping_abs());
        let centre = ((prod + i64::from(add_flt)) >> I4_SHIFT_FLT) as i32;
        accum += thresh_sum(&r.flt[theta], c, centre);
    }
    accum
}

/// `adm_cm_excess_s0()`: `clamp(|x| - thr * 2^shift, 0, INT32_MAX)` in int64.
#[allow(clippy::cast_possible_truncation)]
fn excess_s0(x: i32, thr: i32, shift: u32) -> i32 {
    let magnitude = i64::from(x).abs();
    let excess = magnitude - i64::from(thr) * (1_i64 << shift);
    let floored = if excess < 0 { 0 } else { excess };
    (if floored > i64::from(i32::MAX) {
        i64::from(i32::MAX)
    } else {
        floored
    }) as i32
}

/// The rounded cube of an excess `v` (shared tail of both accum_round forms).
#[allow(clippy::cast_possible_truncation)]
fn cube(v: i32, p: &CmBand) -> i64 {
    let v_sq = (((i64::from(v) * i64::from(v)) + i64::from(p.add_shift_sq)) >> p.shift_sq) as i32;
    ((i64::from(v_sq) * i64::from(v)) + i64::from(p.add_shift_cub)) >> p.shift_cub
}

/// `adm_cm_accum_round()`.
fn accum_round_s0(x: i32, thr: i32, p: &CmBand) -> i64 {
    cube(excess_s0(x, thr, p.shift_sub), p)
}

/// `i4_adm_cm_accum_round()`.
fn accum_round_s123(x: i32, thr: i32, p: &CmBand) -> i64 {
    let v = x.wrapping_abs() - (thr >> p.shift_sub);
    cube(if v < 0 { 0 } else { v }, p)
}

/// `adm_cm_fold()` / `adm_cm_round_row_total()`: fold a row into the frame
/// accumulators and clear it.
fn fold(inner: &mut [i64; 3], accum: &mut [i64; 3], add: u32, shift: u32) {
    for k in 0..3 {
        accum[k] += (inner[k] + i64::from(add)) >> shift;
        inner[k] = 0;
    }
}

/// The per-sample kernel of one pipeline width.
trait CmKernel {
    type T;
    fn bands(&self) -> &CmBands<'_, Self::T>;
    fn args(&self) -> &CmArgs;
    fn inner_shift(&self) -> (u32, u32);
    /// `adm_cm_accum_px()` / `i4_adm_cm_accum_px()` at column `c[1]`.
    fn accum_px(&self, r: &RowSlices<'_, Self::T>, c: [usize; 3], inner: &mut [i64; 3]);
}

/// `adm_cm_row()` / `i4_adm_cm_row()`.
fn row<K: CmKernel>(k: &K, i: i32, bd: &CmBounds, inner: &mut [i64; 3]) {
    let a = k.args();
    let r = row_slices(k.bands(), a, i);
    if bd.left_edge {
        k.accum_px(&r, cols(a, 0), inner);
    }
    for j in bd.start_col..bd.end_col {
        k.accum_px(&r, cols(a, j), inner);
    }
    if bd.right_edge {
        k.accum_px(&r, cols(a, a.w - 1), inner);
    }
}

/// `adm_cm_rows()` / `i4_adm_cm_rows()`: the first and last row only when the
/// border reaches them; a fold after each, present or not.
fn rows<K: CmKernel>(k: &K, bd: &CmBounds) -> [i64; 3] {
    let (add, shift) = k.inner_shift();
    let h = k.args().h;
    let mut accum = [0_i64; 3];
    let mut inner = [0_i64; 3];
    if bd.b.top <= 0 {
        row(k, 0, bd, &mut inner);
    }
    fold(&mut inner, &mut accum, add, shift);
    for i in bd.start_row..bd.end_row {
        row(k, i, bd, &mut inner);
        fold(&mut inner, &mut accum, add, shift);
    }
    if bd.b.bottom > (h - 1) {
        row(k, h - 1, bd, &mut inner);
    }
    fold(&mut inner, &mut accum, add, shift);
    accum
}

/// `adm_num_scale()` of the three bands, summed in float.
#[allow(clippy::cast_possible_truncation)]
fn num_scale(f_accum: [f32; 3], bd: &CmBounds, a: &CmArgs) -> f32 {
    let p_norm_exp = 1.0_f32 / (a.p_norm as f32);
    let noise = (f64::from(bd.b.area()) * a.noise_weight) as f32;
    let [h, v, d] = f_accum.map(|f| libm::powf(f, p_norm_exp) + libm::powf(noise, p_norm_exp));
    h + v + d
}

/// Scale-0 (16-bit) contrast-masking state (`AdmCmCtx`).
struct CmS0<'a> {
    bands: CmBands<'a, i16>,
    args: CmArgs,
    rfactor: [u32; 3],
    band: [CmBand; 3],
    inner: (u32, u32),
}

impl CmKernel for CmS0<'_> {
    type T = i16;
    #[allow(clippy::cast_possible_wrap)]
    fn accum_px(&self, r: &RowSlices<'_, i16>, c: [usize; 3], inner: &mut [i64; 3]) {
        // int16 * uint16: both promote to int.
        let x = [0, 1, 2].map(|k| i32::from(r.src[k][c[1]]) * self.rfactor[k] as i32);
        let thr = thresh_s0(r, c);
        for k in 0..3 {
            inner[k] += accum_round_s0(x[k], thr, &self.band[k]);
        }
    }
    fn bands(&self) -> &CmBands<'_, i16> {
        &self.bands
    }
    fn args(&self) -> &CmArgs {
        &self.args
    }
    fn inner_shift(&self) -> (u32, u32) {
        self.inner
    }
}

/// Per-band shift budgets of the scale-0 cube (`adm_cm_ctx_init()`).
fn band_s0(w: i32) -> [CmBand; 3] {
    let shift_xhcub = ceil_log2_u32(w, 4.0);
    let shift_xdcub = ceil_log2_u32(w, 3.0);
    let hv = CmBand {
        shift_sub: 10,
        add_shift_sq: 268_435_456,
        shift_sq: 29,
        add_shift_cub: half_shift(shift_xhcub),
        shift_cub: shift_xhcub,
    };
    let d = CmBand {
        shift_sub: 12,
        add_shift_sq: 536_870_912,
        shift_sq: 30,
        add_shift_cub: half_shift(shift_xdcub),
        shift_cub: shift_xdcub,
    };
    [hv, hv, d]
}

/// `adm_cm_restore_accum()`.
#[allow(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_possible_wrap
)]
fn restore_s0(accum: i64, base_exp: i32, norm_shift: u32, shift_cub: u32, inner: u32) -> f32 {
    let divisor_exp = base_exp - 3 * norm_shift as i32 - shift_cub as i32 - inner as i32;
    (accum as f64 / libm::pow(2.0, f64::from(divisor_exp))) as f32
}

/// `adm_cm()`: scale-0 numerator.
pub fn cm_s0(bands: CmBands<'_, i16>, rfactor: [u32; 3], norm_shift: u32, a: &CmArgs) -> f32 {
    let shift_inner = ceil_log2_u32(a.h, 0.0);
    let k = CmS0 {
        bands,
        args: *a,
        rfactor,
        band: band_s0(a.w),
        inner: (half_shift(shift_inner), shift_inner),
    };
    let bd = cm_bounds(a.w, a.h);
    let accum = rows(&k, &bd);
    let f_accum = [
        restore_s0(accum[0], 52, norm_shift, k.band[0].shift_cub, shift_inner),
        restore_s0(accum[1], 52, norm_shift, k.band[1].shift_cub, shift_inner),
        restore_s0(accum[2], 57, norm_shift, k.band[2].shift_cub, shift_inner),
    ];
    num_scale(f_accum, &bd, a)
}

/// Scale 1..3 (32-bit) contrast-masking state (`I4AdmCmCtx`).
struct CmS123<'a> {
    bands: CmBands<'a, i32>,
    args: CmArgs,
    rfactor: [u32; 3],
    band: CmBand,
    inner: (u32, u32),
}

impl CmKernel for CmS123<'_> {
    type T = i32;
    /// With `i4_adm_cm_scale()` on the three source bands.
    #[allow(clippy::cast_possible_truncation)]
    fn accum_px(&self, r: &RowSlices<'_, i32>, c: [usize; 3], inner: &mut [i64; 3]) {
        let (add_dst, _) = i4_round_terms();
        let x = [0, 1, 2].map(|k| {
            let v = i64::from(r.src[k][c[1]]) * i64::from(self.rfactor[k]);
            ((v + i64::from(add_dst)) >> I4_SHIFT_DST) as i32
        });
        let thr = thresh_s123(r, c);
        for k in 0..3 {
            inner[k] += accum_round_s123(x[k], thr, &self.band);
        }
    }
    fn bands(&self) -> &CmBands<'_, i32> {
        &self.bands
    }
    fn args(&self) -> &CmArgs {
        &self.args
    }
    fn inner_shift(&self) -> (u32, u32) {
        self.inner
    }
}

/// `i4_adm_cm_result()`'s per-scale divisor, narrowed to float.
#[allow(clippy::cast_possible_truncation, clippy::cast_possible_wrap)]
fn final_shift_s123(scale: usize, norm_shift: u32, shift_cub: u32, inner: u32) -> f32 {
    const BASE: [i32; 3] = [45, 39, 36];
    let restored_bits = 3 * norm_shift as i32;
    let e = BASE[scale - 1] - restored_bits - shift_cub as i32 - inner as i32;
    libm::pow(2.0, f64::from(e)) as f32
}

/// `i4_adm_cm()`: scale 1..3 numerator.
#[allow(clippy::cast_precision_loss)]
pub fn cm_s123(
    bands: CmBands<'_, i32>,
    scale: usize,
    rfactor: [u32; 3],
    norm_shift: u32,
    a: &CmArgs,
) -> f32 {
    let shift_cub = ceil_log2_u32(a.w, 0.0);
    let shift_inner = ceil_log2_u32(a.h, 0.0);
    let k = CmS123 {
        bands,
        args: *a,
        rfactor,
        band: CmBand {
            shift_sub: 0,
            add_shift_sq: 536_870_912,
            shift_sq: 30,
            add_shift_cub: half_shift(shift_cub),
            shift_cub,
        },
        inner: (half_shift(shift_inner), shift_inner),
    };
    let bd = cm_bounds(a.w, a.h);
    let accum = rows(&k, &bd);
    let fs = final_shift_s123(scale, norm_shift, shift_cub, shift_inner);
    // int64_t / float: the usual arithmetic conversions turn the integer into
    // a float first, then divide in float.
    let f_accum = accum.map(|v| v as f32 / fs);
    num_scale(f_accum, &bd, a)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn excess_clamps_like_the_c() {
        assert_eq!(excess_s0(5000, 1, 10), 5000 - 1024);
        assert_eq!(excess_s0(-5000, 10, 10), 0);
        assert_eq!(excess_s0(i32::MAX, -2_000_000, 10), i32::MAX);
    }

    #[test]
    fn i4_excess_floors_at_zero() {
        let p = CmBand {
            shift_sub: 0,
            add_shift_sq: 1 << 29,
            shift_sq: 30,
            add_shift_cub: 0,
            shift_cub: 0,
        };
        assert_eq!(accum_round_s123(-3, 10, &p), 0);
    }
}
