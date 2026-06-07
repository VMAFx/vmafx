// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// Score types and per-frame iteration.

use vmafx_sys::{
    VmafPoolingMethod_VMAF_POOL_METHOD_HARMONIC_MEAN, VmafPoolingMethod_VMAF_POOL_METHOD_MAX,
    VmafPoolingMethod_VMAF_POOL_METHOD_MEAN, VmafPoolingMethod_VMAF_POOL_METHOD_MIN,
};

/// Pooling method used by [`crate::Context::score_pooled`].
///
/// The variants mirror libvmaf's `VmafPoolingMethod` enum; we expose only the
/// stable subset documented in the C public header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
pub enum PoolingMethod {
    /// Arithmetic mean across frames (default in most CLI flows).
    Mean,
    /// Minimum score across frames.
    Min,
    /// Maximum score across frames.
    Max,
    /// Harmonic mean across frames.
    HarmonicMean,
}

impl PoolingMethod {
    pub(crate) const fn to_raw(self) -> u32 {
        match self {
            Self::Mean => VmafPoolingMethod_VMAF_POOL_METHOD_MEAN,
            Self::Min => VmafPoolingMethod_VMAF_POOL_METHOD_MIN,
            Self::Max => VmafPoolingMethod_VMAF_POOL_METHOD_MAX,
            Self::HarmonicMean => VmafPoolingMethod_VMAF_POOL_METHOD_HARMONIC_MEAN,
        }
    }
}

/// A single scoring result.
///
/// Phase 1 ships this as a thin pair (frame index + score) so callers can
/// build collections without losing the per-frame association. Per-feature
/// score iteration is deferred; raw FFI remains available for it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Score {
    /// The frame index this score refers to (matches the `index` passed to
    /// [`crate::Context::read_pictures`]).
    pub index: u32,
    /// The VMAF score in libvmaf's native units (0.0–100.0 for the
    /// canonical models).
    pub value: f64,
}

impl Score {
    /// Construct a new `Score`.
    #[must_use]
    pub fn new(index: u32, value: f64) -> Self {
        Self { index, value }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pooling_method_round_trip() {
        // We can't easily compare against the raw enum without re-importing
        // it, but we can confirm each variant maps to a distinct value.
        let raws = [
            PoolingMethod::Mean.to_raw(),
            PoolingMethod::Min.to_raw(),
            PoolingMethod::Max.to_raw(),
            PoolingMethod::HarmonicMean.to_raw(),
        ];
        let mut sorted = raws.to_vec();
        sorted.sort_unstable();
        sorted.dedup();
        assert_eq!(
            sorted.len(),
            raws.len(),
            "pooling raw values must be distinct"
        );
    }

    #[test]
    fn score_constructor() {
        let s = Score::new(42, 95.5);
        assert_eq!(s.index, 42);
        assert!((s.value - 95.5).abs() < f64::EPSILON);
    }
}
