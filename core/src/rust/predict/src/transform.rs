// Copyright 2016-2026 Netflix, Inc.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Mirrors `transform()` and `clip()` of `core/src/predict.c` and
// `find_linear_function_parameters()`, `piecewise_segment_apply()`,
// `piecewise_linear_mapping()` of `core/src/predict_internal.h`.

#![forbid(unsafe_code)]

use crate::model::{Model, Point, Stage, TransformView};
use crate::predict::{DISABLE_CLIP, DISABLE_TRANSFORM, PredictError};

/// `predict_validate_finite`.
pub(crate) fn validate_finite(value: f64, stage: Stage) -> Result<(), PredictError> {
    if value.is_finite() {
        Ok(())
    } else {
        Err(PredictError::NonFinite { stage, value })
    }
}

/// `find_linear_function_parameters`; `None` is the C's `-EINVAL`.
fn linear_parameters(p1: Point, p2: Point) -> Option<(f64, f64)> {
    if !(p1.x <= p2.x && p1.y <= p2.y) {
        return None;
    }
    if p2.x - p1.x == 0.0 || p2.y - p1.y == 0.0 {
        if !(p1.x == p2.x && p1.y == p2.y) {
            return None;
        }
        return Some((1.0, 0.0));
    }
    if p1.x == 0.0 {
        let beta = p1.y;
        let alpha = (p2.y - beta) / p2.x;
        return Some((alpha, beta));
    }
    let alpha = (p2.y - p1.y) / (p2.x - p1.x);
    let beta = p1.y - (p1.x * alpha);
    Some((alpha, beta))
}

/// Which of the three `if`s of `piecewise_segment_apply` assign `*y`
/// (they are independent, not an else-if chain).
fn segment_hits(knots: &[Point], idx: usize, x: f64) -> [bool; 3] {
    let n_seg = knots.len() - 1;
    let (lo, hi) = (knots[idx], knots[idx + 1]);
    [
        lo.x <= x && x <= hi.x,
        idx == 0 && x < lo.x,
        idx == n_seg - 1 && x > hi.x,
    ]
}

/// `piecewise_segment_apply`; `None` is the C's `-EINVAL`.
fn segment_apply(knots: &[Point], idx: usize, x: f64, y: &mut f64) -> Option<()> {
    let (lo, hi) = (knots[idx], knots[idx + 1]);
    if !(lo.x < hi.x && lo.y <= hi.y) {
        return None;
    }
    let hits = segment_hits(knots, idx, x);
    if lo.y == hi.y {
        if hits.iter().any(|h| *h) {
            *y = lo.y;
        }
        return Some(());
    }
    let (slope, offset) = linear_parameters(lo, hi)?;
    if hits.iter().any(|h| *h) {
        *y = slope * x + offset;
    }
    Some(())
}

/// `piecewise_linear_mapping`; `None` is the C's `-EINVAL`.
fn piecewise(knots: &[Point], x: f64) -> Option<f64> {
    if knots.len() <= 1 || !x.is_finite() {
        return None;
    }
    let mut y = 0.0_f64;
    for idx in 0..knots.len() - 1 {
        segment_apply(knots, idx, x, &mut y)?;
    }
    Some(y)
}

/// Polynomial step of `transform()`: `y_out = 0; += p0; += p1 * y;
/// += p2 * y * y`, each term only when enabled; no term enabled keeps `y`.
fn polynomial(t: &TransformView, y_stage: f64) -> f64 {
    if t.p0.is_none() && t.p1.is_none() && t.p2.is_none() {
        return y_stage;
    }
    let mut y_out = 0.0_f64;
    if let Some(p0) = t.p0 {
        y_out += p0;
    }
    if let Some(p1) = t.p1 {
        y_out += p1 * y_stage;
    }
    if let Some(p2) = t.p2 {
        y_out += p2 * y_stage * y_stage;
    }
    y_out
}

/// `transform()`: polynomial, piecewise-linear, rectification, in that order.
fn transform(t: &TransformView, y_in: f64) -> Result<f64, PredictError> {
    let mut y_out = polynomial(t, y_in);
    validate_finite(y_out, Stage::ScoreTransform)?;
    if let Some(knots) = &t.knots {
        y_out = piecewise(knots, y_out).ok_or(PredictError::Invalid)?;
        validate_finite(y_out, Stage::PiecewiseScore)?;
    }
    if t.out_lte_in {
        y_out = if y_out > y_in { y_in } else { y_out };
    }
    if t.out_gte_in {
        y_out = if y_out < y_in { y_in } else { y_out };
    }
    Ok(y_out)
}

impl Model {
    /// `transform()` of `predict.c` including its enable / flag checks.
    pub(crate) fn apply_transform(&self, y_in: f64, flags: u32) -> Result<f64, PredictError> {
        match &self.transform {
            Some(t) if flags & DISABLE_TRANSFORM == 0 => transform(t, y_in),
            _ => Ok(y_in),
        }
    }

    /// `clip()` of `predict.c`.
    pub(crate) fn apply_clip(&self, prediction: f64, flags: u32) -> f64 {
        let Some((min, max)) = self.clip else {
            return prediction;
        };
        if flags & DISABLE_CLIP != 0 {
            return prediction;
        }
        let low = if prediction < min { min } else { prediction };
        if low > max { max } else { low }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pt(x: f64, y: f64) -> Point {
        Point { x, y }
    }

    #[test]
    fn piecewise_interpolates_and_extrapolates() {
        let k = [pt(0.0, 0.0), pt(50.0, 40.0), pt(100.0, 100.0)];
        assert_eq!(piecewise(&k, 25.0), Some(20.0));
        assert_eq!(
            piecewise(&k, 75.0),
            Some(40.0 + (75.0 - 50.0) * ((100.0 - 40.0) / 50.0))
        );
        assert_eq!(piecewise(&k, -10.0), Some(-10.0 * 0.8 + 0.0));
        assert_eq!(piecewise(&k, f64::NAN), None);
        assert_eq!(piecewise(&k[..1], 1.0), None);
    }

    #[test]
    fn piecewise_rejects_unordered_segment() {
        let k = [pt(1.0, 1.0), pt(0.5, 2.0)];
        assert_eq!(piecewise(&k, 0.75), None);
    }

    #[test]
    fn horizontal_segment_assigns_its_level() {
        let k = [pt(0.0, 5.0), pt(10.0, 5.0)];
        assert_eq!(piecewise(&k, 3.0), Some(5.0));
        assert_eq!(piecewise(&k, 30.0), Some(5.0));
    }

    #[test]
    fn polynomial_orders_the_square_as_the_c_does() {
        let t = TransformView {
            p0: Some(5.30624188),
            p1: Some(0.980791305),
            p2: Some(-0.000490758318),
            ..TransformView::default()
        };
        let y = 71.123_f64;
        let mut want = 0.0_f64;
        want += 5.30624188;
        want += 0.980791305 * y;
        want += -0.000490758318 * y * y;
        assert_eq!(polynomial(&t, y), want);
    }

    #[test]
    fn rectification_uses_the_input() {
        let t = TransformView {
            p0: Some(10.0),
            out_lte_in: true,
            ..TransformView::default()
        };
        assert_eq!(transform(&t, 3.0), Ok(3.0));
        let t = TransformView {
            p0: Some(1.0),
            out_gte_in: true,
            ..TransformView::default()
        };
        assert_eq!(transform(&t, 3.0), Ok(3.0));
    }
}
