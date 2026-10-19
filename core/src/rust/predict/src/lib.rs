// Copyright 2016-2026 Netflix, Inc.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
//! Rust model prediction (RC4 lane P, ADR-1713).
//!
//! Port, statement by statement, of `core/src/predict.c`
//! (`vmaf_predict_score_at_index`), the single-decision branch of
//! `core/src/svm.cpp` (`svm_predict`) and `core/src/predict_internal.h`.
//! The C routines are the ground truth: every arithmetic step keeps the C's
//! type, order and rounding (contract section 6).
//!
//! The C side keeps model loading, feature-name resolution, the score gather
//! from the feature collector and the append of the prediction. This crate
//! takes the raw feature scores of one frame and returns the prediction. The
//! only `unsafe` of the crate is the C ABI in `abi.rs`.

#![deny(unsafe_code)]

#[allow(unsafe_code)]
pub mod abi;
mod chroma;
mod model;
mod predict;
mod svm;
mod transform;

pub use model::{
    Chroma, Feature, Kernel, Model, ModelError, Norm, Point, Stage, SvNode, SvmView, TransformView,
};
pub use predict::PredictError;
