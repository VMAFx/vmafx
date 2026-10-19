// Copyright 2016-2025 Netflix, Inc.
// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Eigenvalues of the 25x25 covariance matrix: Householder tridiagonalisation
// and implicit-shift QR iteration (adapted gsl_eigen_symm). Mirrors
// `compute_eigenvalues()` and its helpers in core/src/feature/speed.c; ported
// statement by statement from Netflix code. `EIGENVALUE_EPS` is a double, so
// every comparison against it is made in double.

use crate::dims::ELEMS;

/// `EIGENVALUE_EPS` (a double literal in the C).
pub const EIGENVALUE_EPS: f64 = 1e-6;
/// `EIGENVALUE_MAX_ITERS`.
const MAX_ITERS: usize = 500;
/// Side of the matrix.
const N: usize = ELEMS;

/// Scratch of one eigen solve (`float *buffer` of `compute_eigenvalues()`).
pub struct EigenWork {
    a: [f32; N * N],
    d: [f32; N],
    sd: [f32; N],
    v: [f32; N],
    x: [f32; N],
}

impl EigenWork {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            a: [0.0; N * N],
            d: [0.0; N],
            sd: [0.0; N],
            v: [0.0; N],
            x: [0.0; N],
        }
    }
}

impl Default for EigenWork {
    fn default() -> Self {
        Self::new()
    }
}

/// `get_sign()`.
pub fn sign(x: f32) -> i32 {
    if x >= 0.0 { 1 } else { -1 }
}

/// `pythagoras()`.
fn pythagoras(x: f32, y: f32) -> f32 {
    (x * x + y * y).sqrt()
}

/// `compute_column_norm()`.
fn column_norm(a: &[f32], col: usize, start_row: usize) -> f32 {
    let mut norm = 0.0_f32;
    for i in start_row..N {
        norm += a[i * N + col] * a[i * N + col];
    }
    norm.sqrt()
}

/// `compute_householder_transform()`.
fn householder(a: &mut [f32], col: usize, start_row: usize) -> f32 {
    if N - start_row == 1 {
        return 0.0;
    }
    let xnorm = column_norm(a, col, start_row + 1);
    if xnorm == 0.0 {
        return 0.0;
    }
    let alpha = a[start_row * N + col];
    let beta = -(sign(alpha) as f32) * pythagoras(alpha, xnorm);
    let tau = (beta - alpha) / beta;
    let s = alpha - beta;
    if s != 0.0 {
        for i in start_row..N {
            a[i * N + col] /= s;
        }
        a[start_row * N + col] = beta;
    }
    tau
}

/// `tridiagonal_multiply()`: `x = tau_i * A * v` over rows and columns `start..`.
fn tridiagonal_multiply(a: &[f32], v: &[f32], x: &mut [f32], tau_i: f32, start: usize) {
    for i in start..N {
        x[i] = 0.0;
        for j in start..N {
            x[i] += tau_i * a[i * N + j] * v[j];
        }
    }
}

/// `tridiagonal_dot_product()`.
fn tridiagonal_dot(x: &[f32], v: &[f32], start: usize) -> f32 {
    let mut res = 0.0_f32;
    for i in start..N {
        res += x[i] * v[i];
    }
    res
}

/// `tridiagonal_syr2()`: `A -= x * v' + v * x'`.
fn tridiagonal_syr2(a: &mut [f32], x: &[f32], v: &[f32], start: usize) {
    for i in start..N {
        for j in start..N {
            a[i * N + j] -= x[i] * v[j] + v[i] * x[j];
        }
    }
}

/// One Householder step of `convert_to_tridiagonal()` for column `i`.
fn tridiagonal_step(w: &mut EigenWork, i: usize) {
    let tau_i = householder(&mut w.a, i, i + 1);
    for j in i + 1..N {
        w.v[j] = w.a[j * N + i];
    }
    if tau_i == 0.0 {
        return;
    }
    // compute_householder_transform() sets the first element to 1 implicitly.
    w.a[(i + 1) * N + i] = w.v[i + 1];
    w.v[i + 1] = 1.0;
    tridiagonal_multiply(&w.a, &w.v, &mut w.x, tau_i, i + 1);
    let xv = tridiagonal_dot(&w.x, &w.v, i + 1);
    // `-0.5 * tau_i * xv` is a double expression narrowed to float.
    let alpha = (-0.5 * f64::from(tau_i) * f64::from(xv)) as f32;
    for r in i + 1..N {
        w.x[r] += alpha * w.v[r];
    }
    tridiagonal_syr2(&mut w.a, &w.x, &w.v, i + 1);
}

/// `convert_to_tridiagonal()`.
fn convert_to_tridiagonal(w: &mut EigenWork) {
    for i in 0..N - 2 {
        tridiagonal_step(w, i);
    }
    for i in 0..N {
        w.d[i] = w.a[i * N + i];
    }
    for i in 0..N - 1 {
        w.sd[i] = w.a[(i + 1) * N + i];
    }
}

/// `chop_small_elements()` on the first `n` entries.
fn chop_small_elements(d: &[f32], sd: &mut [f32], n: usize) {
    for i in 0..n - 1 {
        let bound = EIGENVALUE_EPS * f64::from(d[i].abs() + d[i + 1].abs());
        if f64::from(sd[i].abs()) < bound {
            sd[i] = 0.0;
        }
    }
}

/// `trailing_eigenvalue()`.
fn trailing_eigenvalue(d: &[f32], sd: &[f32], n: usize) -> f32 {
    let ta = d[n - 2];
    let tb = d[n - 1];
    let tab = sd[n - 2];
    let dt = (ta - tb) / 2.0;
    if dt > 0.0 {
        tb - tab * (tab / (dt + pythagoras(dt, tab)))
    } else if dt == 0.0 {
        tb - tab.abs()
    } else {
        tb + tab * (tab / (-dt + pythagoras(dt, tab)))
    }
}

/// `1.0 / sqrt(1 + t * t)` as Netflix writes it (ADR-1477): `t * t` and the
/// sum are float, the square root and the quotient double, rounded to float
/// once.
fn create_givens_scale(t: f32) -> f32 {
    (1.0 / f64::from(1.0_f32 + t * t).sqrt()) as f32
}

/// `create_givens()`: returns `(c, s)`.
fn create_givens(a: f32, b: f32) -> (f32, f32) {
    if b == 0.0 {
        (1.0, 0.0)
    } else if b.abs() > a.abs() {
        let t = -a / b;
        let s1 = create_givens_scale(t);
        (s1 * t, s1)
    } else {
        let t = -b / a;
        let c1 = create_givens_scale(t);
        (c1, c1 * t)
    }
}

/// `qr_step_size2()`.
fn qr_step_size2(d: &mut [f32], sd: &mut [f32], x: f32, z: f32) {
    let (ap, bp, aq) = (d[0], sd[0], d[1]);
    let (c, s) = create_givens(x, z);
    let ak = c * (c * ap - s * bp) + s * (s * aq - c * bp);
    let bk = c * (s * ap + c * bp) - s * (s * bp + c * aq);
    let ap_new = s * (s * ap + c * bp) + c * (s * bp + c * aq);
    d[0] = ak;
    sd[0] = bk;
    d[1] = ap_new;
}

/// `qr_step_general()` for `n >= 3`.
fn qr_step_general(d: &mut [f32], sd: &mut [f32], n: usize, x0: f32, z0: f32) {
    let (mut x, mut z) = (x0, z0);
    let (mut bk, mut zk) = (0.0_f32, 0.0_f32);
    let (mut ap, mut bp, mut aq, mut bq) = (d[0], sd[0], d[1], sd[1]);
    for k in 0..n - 1 {
        let (c, s) = create_givens(x, z);
        let bk1 = c * bk - s * zk;
        let ap1 = c * (c * ap - s * bp) + s * (s * aq - c * bp);
        let bp1 = c * (s * ap + c * bp) - s * (s * bp + c * aq);
        let zp1 = -s * bq;
        let aq1 = s * (s * ap + c * bp) + c * (s * bp + c * aq);
        let bq1 = c * bq;
        (bk, zk, ap, bp) = (bp1, zp1, aq1, bq1);
        if k < n - 2 {
            aq = d[k + 2];
        }
        if k + 3 < n {
            bq = sd[k + 2];
        }
        d[k] = ap1;
        if k > 0 {
            sd[k - 1] = bk1;
        }
        if k < n - 2 {
            sd[k + 1] = bp;
        }
        (x, z) = (bk, zk);
    }
    d[n - 1] = ap;
    sd[n - 2] = bk;
}

/// `qr_step()`.
fn qr_step(d: &mut [f32], sd: &mut [f32], n: usize) {
    let mut mu = trailing_eigenvalue(d, sd, n);
    if EIGENVALUE_EPS * f64::from(mu.abs()) > f64::from(d[0].abs() + sd[0].abs()) {
        mu = 0.0;
    }
    let x = d[0] - mu;
    let z = sd[0];
    if n == 2 {
        qr_step_size2(d, sd, x, z);
    } else {
        qr_step_general(d, sd, n, x, z);
    }
}

/// Start of the largest unreduced block ending at `b`.
fn block_start(sd: &[f32], b: usize) -> usize {
    let mut a = b - 1;
    while a > 0 && sd[a - 1] != 0.0 {
        a -= 1;
    }
    a
}

/// `compute_eigenvalues_tridiagonal()`; returns whether it hit the iteration
/// cap (the C logs a warning then).
fn eigenvalues_tridiagonal(d: &mut [f32], sd: &mut [f32], out: &mut [f32]) -> bool {
    chop_small_elements(d, sd, N);
    let mut b = N - 1;
    let mut iter = 0;
    while b > 0 && iter < MAX_ITERS {
        if sd[b - 1] == 0.0 {
            b -= 1;
            continue;
        }
        let a = block_start(sd, b);
        let n_block = b - a + 1;
        qr_step(&mut d[a..], &mut sd[a..], n_block);
        chop_small_elements(&d[a..], &mut sd[a..], n_block);
        iter += 1;
    }
    out[..N].copy_from_slice(&d[..N]);
    iter == MAX_ITERS
}

/// `compute_eigenvalues()` of the symmetric 25x25 matrix `a_in`; returns
/// whether the iteration cap was reached.
pub fn eigenvalues(a_in: &[f32], out: &mut [f32], w: &mut EigenWork) -> bool {
    w.a.copy_from_slice(&a_in[..N * N]);
    convert_to_tridiagonal(w);
    eigenvalues_tridiagonal(&mut w.d, &mut w.sd, out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn diagonal_matrix_returns_its_diagonal() {
        let mut a = [0.0_f32; N * N];
        for i in 0..N {
            a[i * N + i] = (i + 1) as f32;
        }
        let mut out = [0.0_f32; N];
        let mut w = EigenWork::new();
        assert!(!eigenvalues(&a, &mut out, &mut w));
        let mut sorted = out;
        sorted.sort_by(f32::total_cmp);
        for (i, v) in sorted.iter().enumerate() {
            assert!((v - (i + 1) as f32).abs() < 1e-4);
        }
    }

    #[test]
    fn trace_is_preserved() {
        let mut a = [0.0_f32; N * N];
        for i in 0..N {
            for j in 0..N {
                a[i * N + j] = 1.0 / (1 + i + j) as f32;
            }
        }
        let mut out = [0.0_f32; N];
        let mut w = EigenWork::new();
        eigenvalues(&a, &mut out, &mut w);
        let trace: f32 = (0..N).map(|i| a[i * N + i]).sum();
        let sum: f32 = out.iter().sum();
        assert!((trace - sum).abs() < 1e-3);
    }
}
