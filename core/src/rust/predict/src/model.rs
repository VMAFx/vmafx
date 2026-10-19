// Copyright 2016-2026 Netflix, Inc.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Owned, validated copy of the parts of `struct VmafModel` / `struct svm_model`
// (`core/src/model.h`, `core/src/svm.h`) that prediction reads.

use std::collections::TryReserveError;

/// `enum VmafModelNormalizationType` without the `UNKNOWN` member: C returns
/// `-EINVAL` for it on every use, so the view builder refuses it up front.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Norm {
    None,
    LinearRescale,
}

/// `struct VmafModelFeature` plus the two `strstr()` results of
/// `post_process_feature_from_another()` (the names stay in C).
#[derive(Clone, Copy, Debug)]
pub struct Feature {
    pub slope: f64,
    pub intercept: f64,
    /// `strstr(name, "adm3") != NULL`
    pub guiding: bool,
    /// `strstr(name, "speed_chroma") != NULL`
    pub guided: bool,
}

/// `model->chroma_from_luma`, already reduced to the C's condition
/// `enabled && chroma_correction_parameter` (non-zero).
#[derive(Clone, Copy, Debug)]
pub struct Chroma {
    pub parameter: f64,
}

/// `struct VmafPoint`.
#[derive(Clone, Copy, Debug)]
pub struct Point {
    pub x: f64,
    pub y: f64,
}

/// `model->score_transform` when `enabled`.
#[derive(Clone, Debug, Default)]
pub struct TransformView {
    pub p0: Option<f64>,
    pub p1: Option<f64>,
    pub p2: Option<f64>,
    /// `knots.enabled` -> `Some(list)`.
    pub knots: Option<Vec<Point>>,
    pub out_lte_in: bool,
    pub out_gte_in: bool,
}

/// `svm_parameter.kernel_type` for the kernels the Rust predictor evaluates
/// (`PRECOMPUTED` needs the query to carry the kernel row, `SIGMOID` needs
/// `tanh`: both are refused and run on the C predictor).
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Kernel {
    Linear,
    Poly { gamma: f64, coef0: f64, degree: i32 },
    Rbf { gamma: f64 },
}

/// One support vector node, `struct svm_node`; also the query node.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct SvNode {
    pub index: i32,
    pub value: f64,
}

/// `struct svm_model` of an EPSILON_SVR / NU_SVR model.
#[derive(Clone, Debug)]
pub struct SvmView {
    pub kernel: Kernel,
    /// `model->rho[0]`
    pub rho: f64,
    /// `model->sv_coef[0][0..l]`
    pub coef: Vec<f64>,
    /// `l + 1` offsets into `nodes`; support vector `i` is
    /// `nodes[start[i]..start[i + 1]]` without its `index == -1` terminator.
    pub start: Vec<usize>,
    pub nodes: Vec<SvNode>,
}

/// Why a model view was refused (the caller then runs the C predictor).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ModelError {
    InvalidArgument(&'static str),
    Unsupported(&'static str),
    OutOfMemory,
}

impl From<TryReserveError> for ModelError {
    fn from(_: TryReserveError) -> Self {
        ModelError::OutOfMemory
    }
}

/// Stage whose result failed the finite check (`predict_validate_finite`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    ModelScore,
    ScoreTransform,
    PiecewiseScore,
}

/// Everything `vmaf_predict_score_at_index()` reads from the model.
#[derive(Clone, Debug)]
pub struct Model {
    pub(crate) norm: Norm,
    pub(crate) slope: f64,
    pub(crate) intercept: f64,
    pub(crate) features: Vec<Feature>,
    pub(crate) chroma: Option<Chroma>,
    pub(crate) transform: Option<TransformView>,
    pub(crate) clip: Option<(f64, f64)>,
    pub(crate) svm: SvmView,
}

impl Model {
    /// Validates the invariants the prediction relies on (support vector
    /// offsets, one coefficient per support vector, at least one feature).
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        norm: Norm,
        slope: f64,
        intercept: f64,
        features: Vec<Feature>,
        chroma: Option<Chroma>,
        transform: Option<TransformView>,
        clip: Option<(f64, f64)>,
        svm: SvmView,
    ) -> Result<Self, ModelError> {
        if features.is_empty() {
            return Err(ModelError::InvalidArgument("model has no features"));
        }
        check_svm(&svm)?;
        Ok(Self {
            norm,
            slope,
            intercept,
            features,
            chroma,
            transform,
            clip,
            svm,
        })
    }

    /// Number of features, i.e. of raw scores one prediction consumes.
    pub fn n_features(&self) -> usize {
        self.features.len()
    }
}

fn check_svm(svm: &SvmView) -> Result<(), ModelError> {
    let l = svm.coef.len();
    if svm.start.len() != l + 1 || svm.start.first() != Some(&0) {
        return Err(ModelError::InvalidArgument("support vector offsets"));
    }
    if svm.start.windows(2).any(|w| w[0] > w[1]) || svm.start.last() != Some(&svm.nodes.len()) {
        return Err(ModelError::InvalidArgument("support vector offsets"));
    }
    Ok(())
}
