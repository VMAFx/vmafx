// Copyright 2016-2026 Netflix, Inc.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Mirrors `predict_build_svm_nodes()` and `vmaf_predict_score_at_index()` of
// `core/src/predict.c` after the score gather (which stays in C).

#![forbid(unsafe_code)]

use crate::model::{Model, Stage, SvNode};
use crate::transform::validate_finite;

/// `VMAF_MODEL_FLAG_DISABLE_CLIP`.
pub(crate) const DISABLE_CLIP: u32 = 1 << 0;
/// `VMAF_MODEL_FLAG_DISABLE_TRANSFORM`.
pub(crate) const DISABLE_TRANSFORM: u32 = 1 << 2;

/// Failure of one prediction; the C returns `-EINVAL` for every member.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum PredictError {
    /// Wrong argument, ambiguous guiding / guided feature, malformed knots.
    Invalid,
    /// `predict_validate_finite` failed after `stage`.
    NonFinite { stage: Stage, value: f64 },
}

impl Model {
    /// Fills the query nodes `1..=n` from the raw scores (`normalize`), then
    /// applies the chroma correction. `nodes` is the caller's scratch of at
    /// least `n + 1` entries; entry `n` is the `-1` terminator.
    fn build_nodes(&self, raw: &[f64], nodes: &mut [SvNode]) -> Result<(), PredictError> {
        for (i, f) in self.features.iter().enumerate() {
            let index = i32::try_from(i + 1).map_err(|_| PredictError::Invalid)?;
            nodes[i] = SvNode {
                index,
                value: self.normalize(f.slope, f.intercept, raw[i]),
            };
        }
        if let Some(c) = self.chroma {
            self.chroma_correct(nodes, c.parameter)?;
        }
        nodes[self.features.len()] = SvNode {
            index: -1,
            value: 0.0,
        };
        Ok(())
    }

    /// `denormalize` of the whole prediction.
    fn denormalize(&self, prediction: f64) -> f64 {
        match self.norm {
            crate::model::Norm::None => prediction,
            crate::model::Norm::LinearRescale => (prediction - self.intercept) / self.slope,
        }
    }

    /// One frame: `raw` are the feature scores in model order (what
    /// `vmaf_feature_collector_get_score` returned). Does not allocate.
    pub fn predict(
        &self,
        raw: &[f64],
        flags: u32,
        nodes: &mut [SvNode],
    ) -> Result<f64, PredictError> {
        let n = self.features.len();
        if raw.len() != n || nodes.len() < n + 1 {
            return Err(PredictError::Invalid);
        }
        self.build_nodes(raw, nodes)?;
        let mut prediction = self.svm.predict(&nodes[..n]);
        prediction = self.denormalize(prediction);
        validate_finite(prediction, Stage::ModelScore)?;
        prediction = self.apply_transform(prediction, flags)?;
        Ok(self.apply_clip(prediction, flags))
    }
}
