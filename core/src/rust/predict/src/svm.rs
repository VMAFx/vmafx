// Copyright 2016-2026 Netflix, Inc.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Mirrors `Kernel::dot`, `Kernel::k_function`, `powi`, `svm_predict_values`
// (single-decision branch: EPSILON_SVR / NU_SVR) of `core/src/svm.cpp`.

#![forbid(unsafe_code)]

use crate::model::{Kernel, SvNode, SvmView};
use vmafx_fex::libm;

/// `Kernel::dot`: sparse dot product, merged on the node index.
fn dot(x: &[SvNode], y: &[SvNode]) -> f64 {
    let mut sum = 0.0_f64;
    let (mut i, mut j) = (0_usize, 0_usize);
    while i < x.len() && j < y.len() {
        if x[i].index == y[j].index {
            sum += x[i].value * y[j].value;
            i += 1;
            j += 1;
        } else if x[i].index > y[j].index {
            j += 1;
        } else {
            i += 1;
        }
    }
    sum
}

/// The RBF branch of `Kernel::k_function` up to `exp()`: the squared
/// distance, merged on the node index, tails added in order.
fn squared_distance(x: &[SvNode], y: &[SvNode]) -> f64 {
    let mut sum = 0.0_f64;
    let (mut i, mut j) = (0_usize, 0_usize);
    while i < x.len() && j < y.len() {
        if x[i].index == y[j].index {
            let d = x[i].value - y[j].value;
            sum += d * d;
            i += 1;
            j += 1;
        } else if x[i].index > y[j].index {
            sum += y[j].value * y[j].value;
            j += 1;
        } else {
            sum += x[i].value * x[i].value;
            i += 1;
        }
    }
    for n in &x[i..] {
        sum += n.value * n.value;
    }
    for n in &y[j..] {
        sum += n.value * n.value;
    }
    sum
}

/// `powi`: the C's square-and-multiply loop, which also squares after the
/// last bit (the extra product is never used, but keeps the C's overflow
/// behaviour out of the result: it is a double, so no trap).
fn powi(base: f64, times: i32) -> f64 {
    let mut tmp = base;
    let mut ret = 1.0_f64;
    let mut t = times;
    // `t` halves each round: at most 31 rounds for an `i32`.
    for _ in 0..31 {
        if t <= 0 {
            break;
        }
        if t % 2 != 0 {
            ret *= tmp;
        }
        tmp *= tmp;
        t /= 2;
    }
    ret
}

/// `Kernel::k_function` (the SIGMOID kernel is refused when the view is built:
/// `vmafx_fex::libm` has no `tanh` and no v1 model uses it).
fn k_function(kernel: Kernel, x: &[SvNode], y: &[SvNode]) -> f64 {
    match kernel {
        Kernel::Linear => dot(x, y),
        Kernel::Poly {
            gamma,
            coef0,
            degree,
        } => powi(gamma * dot(x, y) + coef0, degree),
        Kernel::Rbf { gamma } => libm::exp(-gamma * squared_distance(x, y)),
    }
}

impl SvmView {
    /// `svm_predict` for EPSILON_SVR / NU_SVR: `sum += coef[i] * k(x, SV[i])`
    /// in support vector order, then `sum -= rho[0]`. `x` is the query without
    /// its terminator.
    pub fn predict(&self, x: &[SvNode]) -> f64 {
        let mut sum = 0.0_f64;
        for (i, coef) in self.coef.iter().enumerate() {
            let sv = &self.nodes[self.start[i]..self.start[i + 1]];
            sum += *coef * k_function(self.kernel, x, sv);
        }
        sum -= self.rho;
        sum
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn n(index: i32, value: f64) -> SvNode {
        SvNode { index, value }
    }

    #[test]
    fn dot_merges_on_index() {
        let x = [n(1, 2.0), n(2, 3.0), n(4, 5.0)];
        let y = [n(2, 7.0), n(3, 1.0), n(4, 0.5)];
        assert_eq!(dot(&x, &y), 3.0 * 7.0 + 5.0 * 0.5);
    }

    #[test]
    fn squared_distance_adds_tails_in_order() {
        let x = [n(1, 1.0), n(3, 2.0)];
        let y = [n(1, 4.0), n(2, 3.0)];
        let want = (1.0 - 4.0) * (1.0 - 4.0) + 3.0 * 3.0 + 2.0 * 2.0;
        assert_eq!(squared_distance(&x, &y), want);
    }

    #[test]
    fn powi_matches_the_c_loop() {
        assert_eq!(powi(3.0, 0), 1.0);
        assert_eq!(powi(3.0, 1), 3.0);
        assert_eq!(powi(3.0, 5), 243.0);
        assert_eq!(powi(2.0, -3), 1.0);
    }

    #[test]
    fn empty_model_is_minus_rho() {
        let svm = SvmView {
            kernel: Kernel::Rbf { gamma: 0.5 },
            rho: 1.25,
            coef: vec![],
            start: vec![0],
            nodes: vec![],
        };
        assert_eq!(svm.predict(&[n(1, 0.5)]), -1.25);
    }
}
