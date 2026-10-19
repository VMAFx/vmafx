// Copyright 2016-2020 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Gaussian taps and the separable filter of SpEED. Mirrors
// `vif_get_filter_size()`, `vif_get_filter()`, `speed_get_antialias_filter()`,
// `vif_validate_kernelscale()`, `vif_filter1d_s()` and `vif_filter1d_dec16_s()`
// of core/src/feature/vif_tools.c; ported statement by statement from Netflix
// code. Types follow the C: every `float` stays `f32`, a double literal in a
// float expression makes the expression `f64`.

use crate::fexapi::libm;

/// `valid_kernelscales` of vif_tools.h, in the C's order and float spelling.
const VALID_KERNELSCALES: [f32; 21] = [
    1.0,
    1.0 / 2.0,
    3.0 / 2.0,
    2.0,
    2.0 / 3.0,
    24.0 / 10.0,
    360.0 / 97.0,
    4.0 / 3.0,
    3.5 / 3.0,
    3.75 / 3.0,
    4.25 / 3.0,
    5.0 / 3.0,
    3.0,
    1.0 / 2.25,
    1.4746,
    1.54,
    1.6,
    1.06667,
    0.711_111,
    0.740_740,
    1.111_111,
];

/// Largest tap count: scale 1 at kernelscale 4 gives 37 (`float filter[128]`
/// in the C).
pub const MAX_TAPS: usize = 128;

/// `vif_validate_kernelscale()`: the C passes the option's double as `float`.
#[must_use]
pub fn validate_kernelscale(kernelscale: f32) -> bool {
    VALID_KERNELSCALES
        .iter()
        .any(|&v| f64::from((kernelscale - v).abs()) < 1e-3)
}

/// `round_up_to_odd()`.
fn round_up_to_odd(f: f32) -> i32 {
    let ceiling = f.ceil() as i32;
    if ceiling % 2 == 0 {
        ceiling + 1
    } else {
        ceiling
    }
}

/// `vif_get_filter_size()`.
#[must_use]
pub fn filter_size(scale: u32, kernelscale: f32) -> usize {
    let n = (1_i32 << (4 - scale)) + 1;
    round_up_to_odd(n as f32 * kernelscale).max(3) as usize
}

/// `get_gaussian_pdf()`: `(x - mean)` is a float, the rest is double.
fn gaussian_pdf(x: f32, mean: f32, stdev: f32) -> f32 {
    let delta = f64::from(x - mean);
    let sd = f64::from(stdev);
    let num = libm::exp(((-0.5 * delta) / sd * delta) / sd) as f32;
    let den = (1.0 / (sd * (2.0 * core::f64::consts::PI).sqrt())) as f32;
    num / den
}

/// `get_1d_gaussian_kernel()`.
fn gaussian_kernel(out: &mut [f32], stdev: f32) {
    let size = out.len();
    let k = (size as i32 - 1) / 2;
    let mut sum = 0.0_f32;
    for (i, tap) in out.iter_mut().enumerate() {
        *tap = gaussian_pdf((i as i32 - k) as f32, 0.0, stdev);
        sum += *tap;
    }
    for tap in out.iter_mut() {
        *tap /= sum;
    }
}

/// `vif_get_filter()`: the Gaussian of `scale`; returns the tap count.
pub fn fill_filter(out: &mut [f32; MAX_TAPS], scale: u32, kernelscale: f32) -> usize {
    let size = filter_size(scale, kernelscale);
    gaussian_kernel(&mut out[..size], size as f32 / 5.0);
    size
}

/// `speed_get_antialias_filter()` at scale `NUM_SCALES`: the size is the
/// scale-1 size, the deviation `sqrt(scale) * size / 5.0f` in double.
pub fn fill_antialias(out: &mut [f32; MAX_TAPS], scale: u32, kernelscale: f32) -> usize {
    let size = filter_size(1, kernelscale);
    let stdev = (f64::from(scale).sqrt() * size as f64 / 5.0) as f32;
    gaussian_kernel(&mut out[..size], stdev);
    size
}

/// `vif_mirror_index()`: reflect-101 into `[0, n)`.
fn mirror_index(idx: i32, n: i32) -> usize {
    (if idx < 0 {
        -idx
    } else if idx >= n {
        2 * n - idx - 2
    } else {
        idx
    }) as usize
}

/// A plane of `w` x `h` floats at row stride `stride`.
#[derive(Clone, Copy)]
pub struct Geom {
    pub w: usize,
    pub h: usize,
    pub stride: usize,
}

/// Vertical pass for source row `i` into `tmp[..w]`
/// (`vif_filter1d_vertical_s()`). The taps are added in order per column, so
/// walking the taps in the outer loop leaves every column's sum unchanged.
fn vertical_row(taps: &[f32], src: &[f32], g: Geom, i: usize, tmp: &mut [f32]) {
    let tmp = &mut tmp[..g.w];
    tmp.fill(0.0);
    let half = (taps.len() / 2) as i32;
    for (fi, &coeff) in taps.iter().enumerate() {
        let ii = mirror_index(i as i32 - half + fi as i32, g.h as i32);
        let row = &src[ii * g.stride..ii * g.stride + g.w];
        for (acc, &px) in tmp.iter_mut().zip(row) {
            *acc += coeff * px;
        }
    }
}

/// Horizontal pass at every `step`-th column of `tmp[..w]` into `dst_row`
/// (`vif_filter1d_horizontal_s()` / `_dec16_s()`).
fn horizontal_row(taps: &[f32], tmp: &[f32], w: usize, step: usize, dst_row: &mut [f32]) {
    let half = (taps.len() / 2) as i32;
    for (j, out) in dst_row.iter_mut().enumerate() {
        let mut accum = 0.0_f32;
        for (fj, &coeff) in taps.iter().enumerate() {
            let jj = mirror_index((j * step) as i32 - half + fj as i32, w as i32);
            accum += coeff * tmp[jj];
        }
        *out = accum;
    }
}

/// `vif_filter1d_s()` over the whole plane. `tmp` holds at least `w` floats.
pub fn filter_plane(taps: &[f32], src: &[f32], dst: &mut [f32], tmp: &mut [f32], g: Geom) {
    for i in 0..g.h {
        vertical_row(taps, src, g, i, tmp);
        horizontal_row(
            taps,
            tmp,
            g.w,
            1,
            &mut dst[i * g.stride..i * g.stride + g.w],
        );
    }
}

/// `vif_filter1d_s()` followed by `vif_dec16_s()`: only the retained samples
/// (`vif_filter1d_dec16_s()`, bit-identical to the two-step path). The
/// `h / 16` x `w / 16` result lands at the top left of `dst`.
pub fn filter_dec16(taps: &[f32], src: &[f32], dst: &mut [f32], tmp: &mut [f32], g: Geom) {
    for i in 0..g.h / 16 {
        vertical_row(taps, src, g, i * 16, tmp);
        horizontal_row(
            taps,
            tmp,
            g.w,
            16,
            &mut dst[i * g.stride..i * g.stride + g.w / 16],
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sizes_match_the_c_at_default_kernelscale() {
        let sizes: Vec<usize> = (0..=4).map(|s| filter_size(s, 1.0)).collect();
        assert_eq!(sizes, [17, 9, 5, 3, 3]);
    }

    #[test]
    fn kernelscale_table_is_checked() {
        assert!(validate_kernelscale(1.0));
        assert!(validate_kernelscale(1.5));
        assert!(!validate_kernelscale(1.2));
    }

    #[test]
    fn taps_sum_to_one() {
        let mut f = [0.0_f32; MAX_TAPS];
        let n = fill_filter(&mut f, 4, 1.0);
        let sum: f32 = f[..n].iter().sum();
        assert!((sum - 1.0).abs() < 1e-6);
    }
}
