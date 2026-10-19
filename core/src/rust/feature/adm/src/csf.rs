// Copyright 2016-2020 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Contrast-sensitivity filtering of the integer ADM pipeline, ported statement
// by statement from core/src/feature/integer_adm.c (adm_csf(), i4_adm_csf())
// and integer_adm_kernels.h (adm_csf_cols(), i4_adm_round_terms(),
// i4_adm_csf_cols()).

use crate::bands::Pipe;
use crate::region::border_filt;
use crate::tables::{FIX_ONE_BY_30, I4_FIX_ONE_BY_30};

/// `adm_csf_shifts`: Q16 conversion shifts of the h, v and d bands.
const CSF_SHIFTS: [u32; 3] = [15, 15, 17];
/// `adm_csf_shiftsadd`: their rounding terms. The diagonal term is 65535,
/// not 2^16 (upstream quirk, kept).
const CSF_SHIFTS_ADD: [i32; 3] = [16384, 16384, 65535];

/// `i4_shift_dst` / `i4_shift_flt` (the same for every scale).
pub const I4_SHIFT_DST: u32 = 28;
pub const I4_SHIFT_FLT: u32 = 32;

/// `i4_adm_round_terms()`: `(int32_t)(1u << (shift - 1))`. For the flt shift
/// of 32 that is `(int32_t)0x80000000` = INT32_MIN, so every rounding by it
/// subtracts 2^31 (Netflix#955 / ADR-0155; kept for golden parity).
#[allow(clippy::cast_possible_wrap)]
pub const fn i4_round_terms() -> (i32, i32) {
    (
        (1_u32 << (I4_SHIFT_DST - 1)) as i32,
        (1_u32 << (I4_SHIFT_FLT - 1)) as i32,
    )
}

/// Region of one CSF call.
#[derive(Clone, Copy)]
pub struct CsfArgs {
    pub w: i32,
    pub h: i32,
    pub stride: usize,
    pub measure_aim: bool,
}

#[allow(clippy::cast_sign_loss)]
fn ranges(a: &CsfArgs) -> (core::ops::Range<usize>, core::ops::Range<usize>) {
    let b = border_filt(a.w, a.h);
    (
        b.top as usize..b.bottom as usize,
        b.left as usize..b.right as usize,
    )
}

/// `adm_csf_cols()` over one row segment of band `theta`.
#[allow(clippy::cast_possible_truncation)]
fn csf_row_s0(src: &[i16], dst: &mut [i16], flt: &mut [i16], rfactor: u32, theta: usize) {
    for ((s, d), f) in src.iter().zip(dst.iter_mut()).zip(flt.iter_mut()) {
        // `i_rfactor` is a uint16_t (< 65536): the C product is int * int.
        let dst_val = (rfactor as i32) * i32::from(*s);
        let i16_dst_val = ((dst_val + CSF_SHIFTS_ADD[theta]) >> CSF_SHIFTS[theta]) as i16;
        *d = i16_dst_val;
        *f = (((FIX_ONE_BY_30 * i32::from(i16_dst_val).wrapping_abs()) + 2048) >> 12) as i16;
    }
}

/// `adm_csf()`: CSF-weight the decoupled bands of the 16-bit pipeline into
/// the weighted (`dst`) and 1/30-filtered (`flt`) bands. With `measure_aim`
/// the restored signal replaces the additive impairment and the two outputs
/// swap roles (`adm_csf_bands()`).
pub fn csf_s0(p: &mut Pipe<i16>, rfactor: [u32; 3], a: &CsfArgs) {
    let (rows, cols) = ranges(a);
    let (src, dst, flt) = if a.measure_aim {
        (&p.decouple_r, &mut p.csf_f, &mut p.csf_a)
    } else {
        (&p.decouple_a, &mut p.csf_a, &mut p.csf_f)
    };
    for theta in 0..3 {
        for i in rows.clone() {
            let seg = i * a.stride + cols.start..i * a.stride + cols.end;
            csf_row_s0(
                &src[theta][seg.clone()],
                &mut dst[theta][seg.clone()],
                &mut flt[theta][seg],
                rfactor[theta],
                theta,
            );
        }
    }
}

/// `i4_adm_csf_cols()` over one row segment.
#[allow(clippy::cast_possible_truncation)]
fn csf_row_s123(src: &[i32], dst: &mut [i32], flt: &mut [i32], rfactor: u32) {
    let (add_dst, add_flt) = i4_round_terms();
    for ((s, d), f) in src.iter().zip(dst.iter_mut()).zip(flt.iter_mut()) {
        let dst_val =
            (((i64::from(rfactor) * i64::from(*s)) + i64::from(add_dst)) >> I4_SHIFT_DST) as i32;
        *d = dst_val;
        let prod = i64::from(I4_FIX_ONE_BY_30) * i64::from(dst_val.wrapping_abs());
        *f = ((prod + i64::from(add_flt)) >> I4_SHIFT_FLT) as i32;
    }
}

/// `i4_adm_csf()`: the 32-bit twin of `csf_s0`.
pub fn csf_s123(p: &mut Pipe<i32>, rfactor: [u32; 3], a: &CsfArgs) {
    let (rows, cols) = ranges(a);
    let (src, dst, flt) = if a.measure_aim {
        (&p.decouple_r, &mut p.csf_f, &mut p.csf_a)
    } else {
        (&p.decouple_a, &mut p.csf_a, &mut p.csf_f)
    };
    for theta in 0..3 {
        for i in rows.clone() {
            let seg = i * a.stride + cols.start..i * a.stride + cols.end;
            csf_row_s123(
                &src[theta][seg.clone()],
                &mut dst[theta][seg.clone()],
                &mut flt[theta][seg],
                rfactor[theta],
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn netflix_955_rounding_term_is_int32_min() {
        assert_eq!(i4_round_terms(), (134_217_728, i32::MIN));
    }
}
