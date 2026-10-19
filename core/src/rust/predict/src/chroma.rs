// Copyright 2016-2026 Netflix, Inc.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Mirrors `post_process_feature_from_another()`, `scan_feature()`,
// `scan_match_feature()` and `float_values_equal()` of `core/src/predict.c` /
// `core/src/predict_internal.h`. The guiding feature is "adm3", the guided one
// "speed_chroma", the sentinel 0.0 (the only values the C passes).

#![forbid(unsafe_code)]

use crate::model::{Model, SvNode};
use crate::predict::PredictError;

/// `float_values_equal`: NaN equals nothing, both zeros are equal (signed
/// zero included), everything else by bit pattern.
pub(crate) fn float_values_equal(a: f64, b: f64) -> bool {
    if a.is_nan() || b.is_nan() {
        return false;
    }
    if a == 0.0 && b == 0.0 {
        return true;
    }
    a.to_bits() == b.to_bits()
}

#[derive(Default)]
struct Scan {
    guiding: Option<f64>,
    guided: Option<f64>,
    guided_idx: usize,
}

enum Step {
    Continue,
    /// The guided feature does not carry the sentinel: no correction, `Ok`.
    NoCorrection,
}

impl Model {
    /// `denormalize_feature`.
    fn denormalize_feature(&self, slope: f64, intercept: f64, score: f64) -> f64 {
        match self.norm {
            crate::model::Norm::None => score,
            crate::model::Norm::LinearRescale => (score - intercept) / slope,
        }
    }

    /// `normalize`.
    pub(crate) fn normalize(&self, slope: f64, intercept: f64, score: f64) -> f64 {
        match self.norm {
            crate::model::Norm::None => score,
            crate::model::Norm::LinearRescale => slope * score + intercept,
        }
    }

    /// `scan_feature` for feature `i`.
    fn scan_feature(
        &self,
        nodes: &[SvNode],
        i: usize,
        scan: &mut Scan,
    ) -> Result<Step, PredictError> {
        let f = self.features[i];
        if f.guiding {
            if scan.guiding.is_some() {
                return Err(PredictError::Invalid);
            }
            scan.guiding = Some(self.denormalize_feature(f.slope, f.intercept, nodes[i].value));
        }
        if f.guided {
            if scan.guided.is_some() {
                return Err(PredictError::Invalid);
            }
            let guided = self.denormalize_feature(f.slope, f.intercept, nodes[i].value);
            scan.guided = Some(guided);
            if !float_values_equal(guided, 0.0) {
                return Ok(Step::NoCorrection);
            }
            scan.guided_idx = i;
        }
        Ok(Step::Continue)
    }

    /// `post_process_feature_from_another(model, node, parameter, 0.0,
    /// "adm3", "speed_chroma")`.
    pub(crate) fn chroma_correct(
        &self,
        nodes: &mut [SvNode],
        parameter: f64,
    ) -> Result<(), PredictError> {
        let mut scan = Scan::default();
        for i in 0..self.features.len() {
            if let Step::NoCorrection = self.scan_feature(nodes, i, &mut scan)? {
                return Ok(());
            }
        }
        let (Some(guiding), Some(_)) = (scan.guiding, scan.guided) else {
            return Ok(());
        };
        let corrected = (-parameter * guiding) + parameter;
        let f = self.features[scan.guided_idx];
        nodes[scan.guided_idx].value = self.normalize(f.slope, f.intercept, corrected);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn equality_is_bitwise_with_signed_zero_and_nan_rules() {
        assert!(float_values_equal(0.0, -0.0));
        assert!(!float_values_equal(f64::NAN, f64::NAN));
        assert!(float_values_equal(1.5, 1.5));
        assert!(!float_values_equal(1.0, 1.0 + f64::EPSILON));
    }
}
