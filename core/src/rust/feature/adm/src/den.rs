// Copyright 2016-2020 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Denominator (reference-energy) reductions of the integer ADM pipeline,
// ported statement by statement from core/src/feature/integer_adm.c
// (adm_csf_den_scale(), adm_csf_den_s123()), integer_adm_kernels.h
// (adm_den_scale_finalise(), adm_csf_den_ctx_init(), adm_csf_den_cols(),
// adm_csf_den_result(), i4_cube_term(), i4_adm_csf_den_*()) and
// adm_cm_accumulator.h (adm_csf_den_round_row_total(), fork code).

use vmafx_fex::libm;

use crate::bands::Hvd;
use crate::csf_weights::CsfFactors;
use crate::region::{Border, border, ceil_log2_u32, half_shift};

/// Band geometry of one reduction.
#[derive(Clone, Copy)]
pub struct DenArgs {
    pub w: i32,
    pub h: i32,
    pub stride: usize,
    pub noise_weight: f64,
}

/// `adm_den_scale_finalise()`: cube roots of the band energies plus the noise
/// floor of the area, summed in float.
#[allow(clippy::cast_possible_truncation)]
fn finalise(csf: [f64; 3], area: i32, noise_weight: f64) -> f32 {
    let third = 1.0_f32 / 3.0_f32;
    let powf_add = libm::powf((f64::from(area) * noise_weight) as f32, third);
    let h = libm::powf(csf[0] as f32, third) + powf_add;
    let v = libm::powf(csf[1] as f32, third) + powf_add;
    let d = libm::powf(csf[2] as f32, third) + powf_add;
    h + v + d
}

/// `adm_csf_den_round_row_total()`.
const fn round_row(row_sum: u64, add: u32, shift: u32) -> u64 {
    row_sum.wrapping_add(add as u64) >> shift
}

/// The weights of the three bands: { factor1, factor1, factor2 }.
const fn rfactor(f: CsfFactors) -> [f32; 3] {
    [f.factor1, f.factor1, f.factor2]
}

/// `(double)(accum / shift_csf) * pow(rfactor, 3)` of the three bands.
#[allow(clippy::cast_precision_loss)]
fn band_energies(accum: [u64; 3], shift_csf: f64, rf: [f32; 3]) -> [f64; 3] {
    [0, 1, 2].map(|k| (accum[k] as f64 / shift_csf) * libm::pow(f64::from(rf[k]), 3.0))
}

#[allow(clippy::cast_sign_loss)]
fn ranges(b: &Border) -> (core::ops::Range<usize>, core::ops::Range<usize>) {
    (
        b.top as usize..b.bottom as usize,
        b.left as usize..b.right as usize,
    )
}

/// Scale-0 row: cubes of |band| in uint64 (`adm_csf_den_cols()`).
fn cube_row_s0(bands: [&[i16]; 3], inner: &mut [u64; 3]) {
    for k in 0..3 {
        for x in bands[k] {
            // `(uint16_t)abs(x)`: |-32768| = 32768 still fits.
            #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
            let a = u64::from(i32::from(*x).wrapping_abs() as u16);
            inner[k] = inner[k].wrapping_add((a * a) * a);
        }
    }
}

/// `adm_csf_den_scale()`: cubed reference-band energy inside the border.
#[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
pub fn den_s0(src: &Hvd<i16>, f: CsfFactors, a: &DenArgs) -> f32 {
    let b = border(a.w, a.h);
    let area = b.area();
    let shift_accum = ((libm::log2(f64::from(area)) - 20.0).ceil() as i32).max(0);
    let add_shift_accum = if shift_accum > 0 {
        1_i32 << (shift_accum - 1)
    } else {
        0
    };
    let (rows, cols) = ranges(&b);
    let mut accum = [0_u64; 3];
    for i in rows {
        let seg = i * a.stride + cols.start..i * a.stride + cols.end;
        let mut inner = [0_u64; 3];
        cube_row_s0(
            [&src[0][seg.clone()], &src[1][seg.clone()], &src[2][seg]],
            &mut inner,
        );
        for k in 0..3 {
            let row = round_row(inner[k], add_shift_accum as u32, shift_accum as u32);
            accum[k] = accum[k].wrapping_add(row);
        }
    }
    let shift_csf = libm::pow(2.0, f64::from(18 - shift_accum));
    finalise(
        band_energies(accum, shift_csf, rfactor(f)),
        area,
        a.noise_weight,
    )
}

/// Shifts of the scale 1..3 denominator (`I4AdmDenCtx`).
#[derive(Clone, Copy)]
struct I4DenShifts {
    shift_sq: u32,
    add_shift_sq: u32,
    shift_cub: u32,
    add_shift_cub: u32,
    shift_accum: u32,
    add_shift_accum: u32,
    accum_convert_float: u32,
}

/// `i4_adm_csf_den_ctx_init()`.
fn i4_shifts(scale: usize, b: &Border) -> I4DenShifts {
    const SHIFT_SQ: [u32; 3] = [31, 30, 31];
    const ACCUM_CONVERT_FLOAT: [u32; 3] = [32, 27, 23];
    let slot = scale - 1;
    let shift_cub = ceil_log2_u32(b.right - b.left, 0.0);
    let shift_accum = ceil_log2_u32(b.bottom - b.top, 0.0);
    I4DenShifts {
        shift_sq: SHIFT_SQ[slot],
        add_shift_sq: 1_u32 << SHIFT_SQ[slot],
        shift_cub,
        add_shift_cub: half_shift(shift_cub),
        shift_accum,
        add_shift_accum: half_shift(shift_accum),
        accum_convert_float: ACCUM_CONVERT_FLOAT[slot],
    }
}

/// `i4_cube_term()`.
fn i4_cube_term(x: u32, s: &I4DenShifts) -> u64 {
    let x = u64::from(x);
    let sq = (x * x).wrapping_add(u64::from(s.add_shift_sq)) >> s.shift_sq;
    (sq * x).wrapping_add(u64::from(s.add_shift_cub)) >> s.shift_cub
}

/// Scale 1..3 row (`i4_adm_csf_den_cols()`).
fn cube_row_s123(bands: [&[i32]; 3], s: &I4DenShifts, inner: &mut [u64; 3]) {
    for k in 0..3 {
        for x in bands[k] {
            // `(uint32_t)abs(x)`.
            #[allow(clippy::cast_sign_loss)]
            let a = x.wrapping_abs() as u32;
            inner[k] = inner[k].wrapping_add(i4_cube_term(a, s));
        }
    }
}

/// `adm_csf_den_s123()`.
pub fn den_s123(src: &Hvd<i32>, scale: usize, f: CsfFactors, a: &DenArgs) -> f32 {
    let b = border(a.w, a.h);
    let s = i4_shifts(scale, &b);
    let (rows, cols) = ranges(&b);
    let mut accum = [0_u64; 3];
    for i in rows {
        let seg = i * a.stride + cols.start..i * a.stride + cols.end;
        let mut inner = [0_u64; 3];
        cube_row_s123(
            [&src[0][seg.clone()], &src[1][seg.clone()], &src[2][seg]],
            &s,
            &mut inner,
        );
        for k in 0..3 {
            let row = round_row(inner[k], s.add_shift_accum, s.shift_accum);
            accum[k] = accum[k].wrapping_add(row);
        }
    }
    // uint32_t arithmetic in C: wraps when the shifts exceed the budget.
    let exp = s
        .accum_convert_float
        .wrapping_sub(s.shift_accum)
        .wrapping_sub(s.shift_cub);
    let shift_csf = libm::pow(2.0, f64::from(exp));
    finalise(
        band_energies(accum, shift_csf, rfactor(f)),
        b.area(),
        a.noise_weight,
    )
}
