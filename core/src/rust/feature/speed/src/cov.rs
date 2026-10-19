// Copyright 2016-2025 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Block statistics of SpEED. Mirrors `compute_mean()`,
// `compute_cov_kernel_scalar()`, `compute_covariance_matrix()` and
// `compute_independent_term()` of core/src/feature/speed.c; ported statement
// by statement from Netflix code.
//
// Covariance contract (speed_cov.h): every element is
// `sum = sum + ((x - mean_x) * (y - mean_y))` in binary64, row-major, one
// rounding per operation. Several y blocks of one block row are summed side
// by side; every lane keeps its own running sum in the reference's order.

use crate::dims::{BLOCK, Dims, ELEMS};

/// Mean of every block offset (`compute_mean()`), row-major `[row][col]`.
pub fn compute_means(d: &Dims, data: &[f32], stride: usize, means: &mut [f32; ELEMS]) {
    let denominator = (d.sub_w * d.sub_h) as f32;
    for start_row in 0..BLOCK {
        for start_col in 0..BLOCK {
            let mut result = 0.0_f32;
            for i in 0..d.sub_h {
                let row = &data[(start_row + i) * stride + start_col..][..d.sub_w];
                for &px in row {
                    result += px;
                }
            }
            means[start_row * BLOCK + start_col] = result / denominator;
        }
    }
}

/// Sums of block `x_off` against `count` y blocks at consecutive columns
/// from `y_off` (`speed_cov_row_scalar()`).
fn cov_row(
    d: &Dims,
    data: &[f32],
    stride: usize,
    offs: (usize, usize),
    means: (f64, &[f64]),
    sums: &mut [f64; BLOCK],
) {
    let (x_off, y_off) = offs;
    let (mean_x, mean_y) = means;
    sums.fill(0.0);
    for i in 0..d.sub_h {
        let xs = &data[x_off + i * stride..][..d.sub_w];
        let ys = &data[y_off + i * stride..][..d.sub_w + BLOCK - 1];
        for (j, &px) in xs.iter().enumerate() {
            let vx = f64::from(px) - mean_x;
            for (k, sum) in sums.iter_mut().enumerate().take(mean_y.len()) {
                let product = vx * (f64::from(ys[j + k]) - mean_y[k]);
                *sum += product;
            }
        }
    }
}

/// Covariances of block `x_index` against the `count` blocks of row `row_y`,
/// stored on both sides of the diagonal (`compute_covariance_row()`).
fn covariance_row(
    d: &Dims,
    data: &[f32],
    cov: &mut [f32],
    means: &[f32; ELEMS],
    stride: usize,
    at: (usize, usize, usize),
) {
    let (x_index, row_y, count) = at;
    let y_first = row_y * BLOCK;
    let x_off = (x_index / BLOCK) * stride + (x_index % BLOCK);
    let mut mean_y = [0.0_f64; BLOCK];
    for (k, m) in mean_y.iter_mut().enumerate().take(count) {
        *m = f64::from(means[y_first + k]);
    }
    let mut sums = [0.0_f64; BLOCK];
    let mean_x = f64::from(means[x_index]);
    cov_row(
        d,
        data,
        stride,
        (x_off, row_y * stride),
        (mean_x, &mean_y[..count]),
        &mut sums,
    );
    let denominator = (d.sub_w * d.sub_h) as f64;
    for (k, &sum) in sums.iter().enumerate().take(count) {
        let covariance = (sum / denominator) as f32;
        cov[x_index * ELEMS + y_first + k] = covariance;
        cov[(y_first + k) * ELEMS + x_index] = covariance;
    }
}

/// `compute_covariance_matrix()`: the lower triangle one row of y blocks at
/// a time, mirrored to the upper one.
pub fn compute_covariance(
    d: &Dims,
    data: &[f32],
    cov: &mut [f32],
    means: &mut [f32; ELEMS],
    stride: usize,
) {
    compute_means(d, data, stride, means);
    for x_index in 0..ELEMS {
        let row_x = x_index / BLOCK;
        for row_y in 0..=row_x {
            let count = if row_y < row_x {
                BLOCK
            } else {
                (x_index % BLOCK) + 1
            };
            covariance_row(d, data, cov, means, stride, (x_index, row_y, count));
        }
    }
}

/// `compute_independent_term()`: the block-grid samples at every offset.
pub fn compute_independent_term(d: &Dims, data: &[f32], out: &mut [f32], stride: usize) {
    for start_i in 0..BLOCK {
        for start_j in 0..BLOCK {
            let out_row = start_i * BLOCK + start_j;
            for i in (start_i..d.trunc_h).step_by(BLOCK) {
                for j in (start_j..d.trunc_w).step_by(BLOCK) {
                    let out_col = ((i - start_i) / BLOCK) * d.blocks_h + (j - start_j) / BLOCK;
                    out[out_row * d.num_blocks + out_col] = data[i * stride + j];
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn plane(w: usize, h: usize) -> Vec<f32> {
        (0..w * h).map(|v| ((v * 7919) % 251) as f32).collect()
    }

    #[test]
    fn covariance_is_symmetric_with_variance_on_the_diagonal() {
        let d = Dims::new(160, 160, 1.0).ok();
        let Some(d) = d else { return };
        // The operating plane of a 160 px frame is 10 px: use it directly.
        let (w, h) = (d.trunc_w, d.trunc_h);
        let data = plane(w, h);
        let mut cov = vec![0.0_f32; ELEMS * ELEMS];
        let mut means = [0.0_f32; ELEMS];
        compute_covariance(&d, &data, &mut cov, &mut means, w);
        for i in 0..ELEMS {
            assert!(cov[i * ELEMS + i] >= 0.0);
            for j in 0..ELEMS {
                assert_eq!(cov[i * ELEMS + j], cov[j * ELEMS + i]);
            }
        }
    }

    #[test]
    fn independent_term_picks_block_samples() {
        let Some(d) = Dims::new(160, 160, 1.0).ok() else {
            return;
        };
        let data: Vec<f32> = (0..d.trunc_w * d.trunc_h).map(|v| v as f32).collect();
        let mut out = vec![0.0_f32; ELEMS * d.num_blocks];
        compute_independent_term(&d, &data, &mut out, d.trunc_w);
        assert_eq!(out[0], 0.0);
        assert_eq!(out[1], 5.0);
        assert_eq!(out[d.num_blocks], 1.0);
    }
}
