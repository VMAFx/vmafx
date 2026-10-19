// Copyright 2016-2025 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Entropy and score of SpEED. Mirrors `compute_pointwise_product_and_division()`,
// `sum_columns()`, `update_entropy()` and `get_speed_score()` of
// core/src/feature/speed.c; ported statement by statement from Netflix code.
// Every `log2` is the platform libm's (double), called at run time.

use crate::dims::ELEMS;
use crate::fexapi::libm;

/// Per-block entropies and variances of one image (`SpeedResultBuffers`).
pub struct Stats {
    pub entropies: Vec<f32>,
    pub variances: Vec<f32>,
}

/// `X = (X * Y) / denominator` over the `ELEMS` x `num_blocks` matrix.
pub fn pointwise_product_and_division(x: &mut [f32], y: &[f32], denominator: f32) {
    for (xv, &yv) in x.iter_mut().zip(y) {
        *xv = (*xv * yv) / denominator;
    }
}

/// `sum_columns()`: row 0 becomes the column sums, added in row order.
pub fn sum_columns(x: &mut [f32], num_blocks: usize) {
    for i in 1..ELEMS {
        for j in 0..num_blocks {
            x[j] += x[i * num_blocks + j];
        }
    }
}

/// `update_entropy()` for eigenvalue `l`:
/// `entropy += log2(l * s + sigma_nn) + entropy_constant`.
///
/// Netflix's statement: the argument is the float expression promoted to
/// double, the sum and the addition are double, and the result is rounded to
/// float once (ADR-1477).
pub fn update_entropy(
    entropy: &mut [f32],
    s: &[f32],
    l: f32,
    sigma_nn: f32,
    entropy_constant: f64,
) {
    for (e, &sv) in entropy.iter_mut().zip(s) {
        let term = libm::log2(f64::from(l * sv + sigma_nn)) + entropy_constant;
        *e = (f64::from(*e) + term) as f32;
    }
}

/// `log2(2 * M_PI * M_E)`: the constant the C folds at compile time.
#[must_use]
pub fn entropy_constant() -> f64 {
    libm::log2(2.0 * core::f64::consts::PI * core::f64::consts::E)
}

/// `log2(1 + v)` of a float variance: the sum is float, the logarithm double.
fn lg_float(v: f32) -> f64 {
    libm::log2(f64::from(1.0_f32 + v))
}

/// `log2(1 + x)` of a double argument.
fn lg_double(x: f64) -> f64 {
    libm::log2(1.0 + x)
}

/// The two spatially weighted entropies of one block for
/// `speed_weight_var_mode` 0..=6: `entropy * log2(..)` is a double product
/// rounded to float once.
fn spatial(mode: i32, e: (f32, f32), v: (f32, f32)) -> (f32, f32) {
    let (er, ed) = (f64::from(e.0), f64::from(e.1));
    let (vr, vd) = v;
    let mean = || lg_double(f64::from(vr + vd) / 2.0);
    let (lr, ld) = match mode {
        0 => (lg_float(vr), lg_float(vd)),
        1 => (lg_float(vr), lg_float(vr)),
        2 => (lg_float(vd), lg_float(vd)),
        3 => (mean(), mean()),
        4 => (lg_float(vr), mean()),
        5 => (
            lg_float(vr),
            lg_double(0.75 * f64::from(vr) + 0.25 * f64::from(vd)),
        ),
        _ => (
            lg_float(vr),
            lg_double(0.25 * f64::from(vr) + 0.75 * f64::from(vd)),
        ),
    };
    ((er * lr) as f32, (ed * ld) as f32)
}

/// `get_speed_score()`: `base_entropy` is `elements * (log2((1 + nn_floor) *
/// sigma_nn) + entropy_constant)`.
#[must_use]
pub fn speed_score(r: &Stats, d: &Stats, base_entropy: f32, mode: i32) -> f32 {
    let mut score = 0.0_f32;
    let n = r.entropies.len();
    for i in 0..n {
        let below = r.entropies[i] < base_entropy && d.entropies[i] < base_entropy;
        if below {
            continue;
        }
        let (sr, sd) = spatial(
            mode,
            (r.entropies[i], d.entropies[i]),
            (r.variances[i], d.variances[i]),
        );
        score += (sr - sd).abs();
    }
    score / n as f32
}

/// `get_speed_score()`'s `base_entropy`: the argument `(1 + nn_floor) *
/// sigma_nn` is a float product, everything after it is double.
#[must_use]
pub fn base_entropy(sigma_nn: f32, nn_floor: f32, entropy_constant: f64) -> f32 {
    let arg = f64::from((1.0 + nn_floor) * sigma_nn);
    (ELEMS as f64 * (libm::log2(arg) + entropy_constant)) as f32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn equal_images_score_zero() {
        let s = Stats {
            entropies: vec![30.0, 31.0],
            variances: vec![1.0, 2.0],
        };
        let t = Stats {
            entropies: vec![30.0, 31.0],
            variances: vec![1.0, 2.0],
        };
        for mode in 0..=6 {
            assert!(speed_score(&s, &t, 10.0, mode).abs() < 1e-6);
        }
    }

    #[test]
    fn blocks_below_the_floor_do_not_count() {
        let s = Stats {
            entropies: vec![1.0],
            variances: vec![1.0],
        };
        let t = Stats {
            entropies: vec![2.0],
            variances: vec![9.0],
        };
        assert_eq!(speed_score(&s, &t, 10.0, 0).to_bits(), 0.0_f32.to_bits());
    }
}
