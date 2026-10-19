// Copyright 2016-2020 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Decouple stage of the integer ADM pipeline, ported statement by statement
// from core/src/feature/integer_adm_kernels.h (adm_decouple_band(),
// adm_decouple_cols(), get_best15_from32(), adm_decouple_band_s123(),
// adm_decouple_s123_cols()) and adm_angle_flag.h (adm_angle_flag_fp64()).

use crate::bands::Pipe;
use crate::region::border_filt;
use crate::tables::DIV_LOOKUP;

/// `adm_angle_flag_fp64()`: the frozen upstream predicate. Each int64 operand
/// is narrowed to float, then the comparison runs in double.
#[allow(clippy::cast_precision_loss)]
fn angle_flag(ot_dp: i64, o_mag_sq: i64, t_mag_sq: i64, cos_1deg_sq: f32) -> bool {
    let ot = f64::from(ot_dp as f32) / 4096.0;
    let o = f64::from(o_mag_sq as f32) / 4096.0;
    let t = f64::from(t_mag_sq as f32) / 4096.0;
    ot >= 0.0 && ot * ot >= f64::from(cos_1deg_sq) * o * t
}

/// The angle flag of one sample from its (oh, ov) and (th, tv) pairs.
fn sample_angle_flag(o: [i64; 2], t: [i64; 2], cos_1deg_sq: f32) -> bool {
    angle_flag(
        o[0] * t[0] + o[1] * t[1],
        o[0] * o[0] + o[1] * o[1],
        t[0] * t[0] + t[1] * t[1],
        cos_1deg_sq,
    )
}

/// `ADM_KERNEL_MIN` / `ADM_KERNEL_MAX` of `rst * gain` against `t`, both in
/// double, as the C ternaries evaluate them.
fn bound_gain(rst: f64, gain: f64, t: f64, upper: bool) -> f64 {
    let g = rst * gain;
    if upper {
        if g < t { g } else { t }
    } else if g > t {
        g
    } else {
        t
    }
}

/// C conversion of a double in int16 range to `int16_t` (truncation).
#[allow(clippy::cast_possible_truncation)]
const fn f64_to_i16(x: f64) -> i16 {
    x as i16
}

/// C conversion of a double in int32 range to `int32_t` (truncation).
#[allow(clippy::cast_possible_truncation)]
const fn f64_to_i32(x: f64) -> i32 {
    x as i32
}

/// `((float)k / 32768) * ((float)o / 64)`: only its sign is used.
#[allow(clippy::cast_precision_loss)]
fn rst_f(k: i64, o: i32) -> f64 {
    f64::from((k as f32 / 32768.0) * (o as f32 / 64.0))
}

/// `adm_decouple_band()`: one band of the 16-bit decouple.
#[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
fn decouple_band(gain: f64, angle: bool, o: i16, t: i16) -> i16 {
    let tmp_k: i32 = if o == 0 {
        32768
    } else {
        let lut = DIV_LOOKUP[(i32::from(o) + 32768) as usize];
        ((i64::from(lut) * i64::from(t) + 16384) >> 15) as i32
    };
    let k = tmp_k.clamp(0, 32768);
    let mut rst = ((k * i32::from(o) + 16384) >> 15) as i16;
    let sign = rst_f(i64::from(k), i32::from(o));
    if angle && sign > 0.0 {
        rst = f64_to_i16(bound_gain(f64::from(rst), gain, f64::from(t), true));
    }
    if angle && sign < 0.0 {
        rst = f64_to_i16(bound_gain(f64::from(rst), gain, f64::from(t), false));
    }
    rst
}

/// `get_best15_from32()`: the 15 most significant bits of `temp` (>= 32768),
/// rounded, and how far they were shifted.
#[allow(clippy::cast_possible_truncation)]
fn best15_from32(temp: u32) -> (u16, i32) {
    let k = 17 - temp.leading_zeros();
    let rounded = temp.wrapping_add(1_u32 << (k - 1)) >> k;
    // k is in [1, 17], so the shifted value fits 16 bits.
    (rounded as u16, k as i32)
}

/// `adm_decouple_band_s123()`: one band of the 32-bit decouple, the division
/// carried out with the reciprocal table on the 15 leading bits of |o|.
#[allow(clippy::cast_possible_truncation)]
fn decouple_band_s123(gain: f64, angle: bool, o: i32, t: i32) -> i32 {
    let abs_o = o.unsigned_abs();
    let k_sign: i64 = if o < 0 { -1 } else { 1 };
    let (k_msb, k_shift) = if abs_o < 32768 {
        (abs_o as u16, 0)
    } else {
        best15_from32(abs_o)
    };
    let tmp_k: i64 = if o == 0 {
        32768
    } else {
        let lut = i64::from(DIV_LOOKUP[usize::from(k_msb) + 32768]);
        ((lut * i64::from(t)) * k_sign + i64::from(1_u32 << (14 + k_shift))) >> (15 + k_shift)
    };
    let k = tmp_k.clamp(0, 32768);
    let mut rst = ((k * i64::from(o) + 16384) >> 15) as i32;
    let sign = rst_f(k, o);
    if angle && sign > 0.0 {
        rst = f64_to_i32(bound_gain(f64::from(rst), gain, f64::from(t), true));
    }
    if angle && sign < 0.0 {
        rst = f64_to_i32(bound_gain(f64::from(rst), gain, f64::from(t), false));
    }
    rst
}

/// Region and constants of one decouple call.
#[derive(Clone, Copy)]
pub struct DecoupleArgs {
    /// Band width and height of this scale.
    pub w: i32,
    pub h: i32,
    pub stride: usize,
    pub gain: f64,
    pub cos_1deg_sq: f32,
}

/// The columns and rows `adm_border_filt()` selects, as index ranges.
#[allow(clippy::cast_sign_loss)]
fn filt_ranges(a: &DecoupleArgs) -> (core::ops::Range<usize>, core::ops::Range<usize>) {
    let b = border_filt(a.w, a.h);
    (
        b.top as usize..b.bottom as usize,
        b.left as usize..b.right as usize,
    )
}

/// `adm_decouple()`: restored (`decouple_r`) and additive-impairment
/// (`decouple_a`) bands of the 16-bit pipeline.
#[allow(clippy::cast_possible_truncation)]
pub fn decouple_s0(p: &mut Pipe<i16>, a: &DecoupleArgs) {
    let (rows, cols) = filt_ranges(a);
    for i in rows {
        for j in cols.clone() {
            let idx = i * a.stride + j;
            let o = [0, 1, 2].map(|th| p.ref_dwt.hvd[th][idx]);
            let t = [0, 1, 2].map(|th| p.dis_dwt.hvd[th][idx]);
            let flag = sample_angle_flag(
                [i64::from(o[0]), i64::from(o[1])],
                [i64::from(t[0]), i64::from(t[1])],
                a.cos_1deg_sq,
            );
            for th in 0..3 {
                let rst = decouple_band(a.gain, flag, o[th], t[th]);
                p.decouple_r[th][idx] = rst;
                p.decouple_a[th][idx] = (i32::from(t[th]) - i32::from(rst)) as i16;
            }
        }
    }
}

/// `adm_decouple_s123()`: the 32-bit twin of `decouple_s0`.
pub fn decouple_s123(p: &mut Pipe<i32>, a: &DecoupleArgs) {
    let (rows, cols) = filt_ranges(a);
    for i in rows {
        for j in cols.clone() {
            let idx = i * a.stride + j;
            let o = [0, 1, 2].map(|th| p.ref_dwt.hvd[th][idx]);
            let t = [0, 1, 2].map(|th| p.dis_dwt.hvd[th][idx]);
            let flag = sample_angle_flag(
                [i64::from(o[0]), i64::from(o[1])],
                [i64::from(t[0]), i64::from(t[1])],
                a.cos_1deg_sq,
            );
            for th in 0..3 {
                let rst = decouple_band_s123(a.gain, flag, o[th], t[th]);
                p.decouple_r[th][idx] = rst;
                p.decouple_a[th][idx] = t[th] - rst;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn best15_rounds_the_leading_bits() {
        assert_eq!(best15_from32(32768), (16384, 1));
        assert_eq!(best15_from32(0x7fff_ffff), (32768, 16));
        assert_eq!(best15_from32(1 << 31), (16384, 17));
    }

    #[test]
    fn zero_reference_keeps_the_distorted_sample() {
        assert_eq!(decouple_band(1.0, false, 0, 100), 0);
        assert_eq!(decouple_band(1.0, false, 100, 100), 100);
        assert_eq!(decouple_band_s123(1.0, false, 0, 7), 0);
    }

    #[test]
    fn angle_flag_is_the_fp64_predicate() {
        assert!(angle_flag(4096, 4096, 4096, 0.999_695_4));
        assert!(!angle_flag(-1, 1, 1, 0.999_695_4));
    }
}
