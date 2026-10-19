// Copyright 2016-2025 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Linear solve K X = Y of SpEED by Householder QR. Mirrors
// `matrix_qr_decomposition()`, `solve_triangular_system()`,
// `solve_linear_system()` and `speed_matmul_scalar()` of
// core/src/feature/speed.c; ported statement by statement from Netflix code.
// The matrix product keeps the C's accumulation order over `k` and never
// fuses a multiply with an add (speed_matmul.h).

use crate::dims::ELEMS;
use crate::eigen::{EIGENVALUE_EPS, sign};

const N: usize = ELEMS;

/// The covariance matrix cannot be inverted (the C's `-EINVAL`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Singular;

/// Workspace of one solve (the `tmp_buffer` partition of the C, minus the
/// rectangular block which the caller owns).
pub struct QrWork {
    a: [f32; N * N],
    q: [f32; N * N],
    r: [f32; N * N],
    tmp_q: [f32; N * N],
    tmp_z: [f32; N * N],
    tmp_mul: [f32; N * N],
    vec: [f32; N],
}

impl QrWork {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            a: [0.0; N * N],
            q: [0.0; N * N],
            r: [0.0; N * N],
            tmp_q: [0.0; N * N],
            tmp_z: [0.0; N * N],
            tmp_mul: [0.0; N * N],
            vec: [0.0; N],
        }
    }
}

impl Default for QrWork {
    fn default() -> Self {
        Self::new()
    }
}

/// `speed_matmul_scalar()` with the strides the C passes: `x` is `rows` x
/// `inner`, `y` is `inner` x `cols`, `dst` is `rows` x `cols`.
fn matmul(dst: &mut [f32], x: &[f32], y: &[f32], rows: usize, inner: usize, cols: usize) {
    for i in 0..rows {
        let drow = &mut dst[i * cols..(i + 1) * cols];
        drow.fill(0.0);
        for k in 0..inner {
            let xv = x[i * inner + k];
            for (d, &yv) in drow.iter_mut().zip(&y[k * cols..(k + 1) * cols]) {
                *d += xv * yv;
            }
        }
    }
}

/// `matrix_transpose()` of a square matrix.
fn transpose(m: &mut [f32; N * N]) {
    for i in 0..N {
        for j in 0..i {
            m.swap(i * N + j, j * N + i);
        }
    }
}

/// `matrix_identity()`.
fn identity(m: &mut [f32; N * N]) {
    for i in 0..N {
        for j in 0..N {
            m[i * N + j] = if i == j { 1.0 } else { 0.0 };
        }
    }
}

/// `matrix_minor()`.
fn minor(m: &mut [f32; N * N], d: usize) {
    for i in 0..N {
        for j in 0..N {
            if i < d || j < d {
                m[i * N + j] = if i == j { 1.0 } else { 0.0 };
            }
        }
    }
}

/// `vector_norm()`.
fn vector_norm(x: &[f32; N]) -> f32 {
    let mut sum = 0.0_f32;
    for v in x {
        sum += v * v;
    }
    sum.sqrt()
}

/// `matrix_identity_minus_v_vt()`: `dst = I - v v^T`.
fn identity_minus_v_vt(dst: &mut [f32; N * N], v: &[f32; N]) {
    for i in 0..N {
        for j in 0..N {
            dst[i * N + j] = -2.0 * v[i] * v[j];
        }
    }
    for i in 0..N {
        dst[i * N + i] += 1.0;
    }
}

/// One reflection of `matrix_qr_decomposition()`; an all-zero column skips it
/// (`I - v v^T` with `v == 0` is the identity).
fn reflect(w: &mut QrWork, k: usize) {
    minor(&mut w.tmp_z, k);
    for i in 0..N {
        w.vec[i] = w.tmp_z[i * N + k];
    }
    let norm = vector_norm(&w.vec);
    // The sign is taken from the original matrix, not from the deflated one.
    w.vec[k] += sign(w.a[k * N + k]) as f32 * norm;
    let vn = vector_norm(&w.vec);
    if vn == 0.0 {
        return;
    }
    for v in &mut w.vec {
        *v /= vn;
    }
    identity_minus_v_vt(&mut w.tmp_q, &w.vec);
    matmul(&mut w.tmp_mul, &w.tmp_q, &w.tmp_z, N, N, N);
    w.tmp_z.copy_from_slice(&w.tmp_mul);
    matmul(&mut w.tmp_mul, &w.tmp_q, &w.q, N, N, N);
    w.q.copy_from_slice(&w.tmp_mul);
}

/// `matrix_qr_decomposition()`: `a` -> `q` (transposed, as the C leaves it) and `r`.
fn qr_decomposition(w: &mut QrWork) {
    w.tmp_z.copy_from_slice(&w.a);
    identity(&mut w.q);
    for k in 0..N - 1 {
        reflect(w, k);
    }
    matmul(&mut w.r, &w.q, &w.a, N, N, N);
    transpose(&mut w.q);
}

/// `solve_triangular_system()`: `R X = B`; `Err` when a pivot is below the
/// eigenvalue epsilon (the caller zeroes `x`).
fn solve_triangular(r: &[f32], x: &mut [f32], b: &[f32], cols: usize) -> Result<(), Singular> {
    for i in (0..N).rev() {
        let denominator = r[i * N + i];
        if f64::from(denominator.abs()) < EIGENVALUE_EPS {
            return Err(Singular);
        }
        for j in 0..cols {
            let mut independent_term = b[i * cols + j];
            for k in i + 1..N {
                independent_term -= x[k * cols + j] * r[i * N + k];
            }
            x[i * cols + j] = independent_term / denominator;
        }
    }
    Ok(())
}

/// `solve_linear_system()`: solve `K X = B` for `K` (25x25), `B` (25 x
/// `cols`). `rect` is scratch of 25 x `cols`. `Err(Singular)` is the C's `-EINVAL`.
pub fn solve(
    w: &mut QrWork,
    k_mat: &[f32],
    b: &[f32],
    cols: usize,
    out: &mut [f32],
    rect: &mut [f32],
) -> Result<(), Singular> {
    w.a.copy_from_slice(&k_mat[..N * N]);
    qr_decomposition(w);
    transpose(&mut w.q);
    matmul(rect, &w.q, b, N, N, cols);
    solve_triangular(&w.r, out, rect, cols)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn solves_a_diagonal_system() {
        let mut k = vec![0.0_f32; N * N];
        for i in 0..N {
            k[i * N + i] = 2.0;
        }
        let cols = 3;
        let b: Vec<f32> = (0..N * cols).map(|v| v as f32).collect();
        let mut out = vec![0.0_f32; N * cols];
        let mut rect = vec![0.0_f32; N * cols];
        let mut w = QrWork::new();
        assert!(solve(&mut w, &k, &b, cols, &mut out, &mut rect).is_ok());
        for (o, v) in out.iter().zip(&b) {
            assert!((o - v / 2.0).abs() < 1e-4);
        }
    }

    #[test]
    fn zero_matrix_is_singular() {
        let k = vec![0.0_f32; N * N];
        let b = vec![1.0_f32; N];
        let mut out = vec![0.0_f32; N];
        let mut rect = vec![0.0_f32; N];
        let mut w = QrWork::new();
        assert!(solve(&mut w, &k, &b, 1, &mut out, &mut rect).is_err());
    }
}
