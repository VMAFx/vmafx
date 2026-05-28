// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package conformal implements split-conformal prediction intervals for VMAF
// scores computed by vmafx-tune.
//
// Algorithm (Stage-3, ADR-0734):
//
//  1. Collect M independent VMAF measurements for a fixed (resolution,
//     bitrate) cell by varying the encoder seed / randomisation parameter.
//  2. Treat the M values as calibration scores and compute non-conformity
//     scores α_i = |y_i − ŷ| where ŷ = mean(y_1..M).
//  3. Return the (1−α)-coverage interval as:
//     lo = ŷ − q̂_{1-α}, hi = ŷ + q̂_{1-α}
//     where q̂ is the ⌈(M+1)(1−α)⌉/M empirical quantile of the α_i.
//
// The interval is asymptotically valid under exchangeability (Angelopoulos &
// Bates 2021, "A Gentle Introduction to Conformal Prediction").  For M ≥ 10
// and coverage = 0.90, the interval is tight enough to be useful for rung
// pruning without requiring a hold-out calibration set.
//
// Rung pruning decision rule (used by pkg/ladder Stage-3):
//   - A candidate rung is retained when interval.Lo ≥ targetVMAF − pruneMargin.
//   - Rungs whose upper bound hi < targetVMAF are marked as "rejected with
//     high confidence" and dropped from the cloud before hull computation.
//
// See ADR-0734 for the design rationale and alternative approaches
// (bootstrap percentile, Gaussian ± k·σ, simple ± 2σ).
package conformal

import (
	"fmt"
	"math"
	"sort"
)

// DefaultNumSamples is the default number of repeated measurements used to
// build a conformal interval.  M = 10 gives ≥ 90 % marginal coverage at the
// default Coverage = 0.90 (⌈11 × 0.10⌉ = 2nd-smallest non-conformity score
// sets the quantile, avoiding the trivially-infinite interval that would
// result from M < 1/(1−Coverage)).
const DefaultNumSamples = 10

// DefaultCoverage is the default nominal coverage probability for the
// conformal interval.
const DefaultCoverage = 0.90

// Interval is a conformal prediction interval around a VMAF measurement.
type Interval struct {
	// Mean is the sample mean of the M VMAF measurements.
	Mean float64 `json:"mean"`
	// Lo is the lower bound of the conformal interval.
	Lo float64 `json:"lo"`
	// Hi is the upper bound of the conformal interval.
	Hi float64 `json:"hi"`
	// N is the number of samples used to compute the interval.
	N int `json:"n"`
	// Coverage is the nominal coverage probability.
	Coverage float64 `json:"coverage"`
}

// Compute returns a conformal prediction interval for the supplied VMAF
// samples.  len(samples) must be ≥ 2.  coverage must be in (0, 1).
//
// When len(samples) is too small to achieve the requested coverage (i.e.
// M < 1/(1−coverage)), Compute returns an error.
func Compute(samples []float64, coverage float64) (Interval, error) {
	n := len(samples)
	if n < 2 {
		return Interval{}, fmt.Errorf("conformal.Compute: need ≥ 2 samples, got %d", n)
	}
	if coverage <= 0 || coverage >= 1 {
		return Interval{}, fmt.Errorf("conformal.Compute: coverage %f out of range (0,1)", coverage)
	}

	// Mean.
	sum := 0.0
	for _, s := range samples {
		sum += s
	}
	mean := sum / float64(n)

	// Non-conformity scores: absolute residuals.
	residuals := make([]float64, n)
	for i, s := range samples {
		residuals[i] = math.Abs(s - mean)
	}

	// Empirical quantile at level ⌈(n+1)*(1-coverage)⌉/n.
	// We want the upper-tail quantile for the residuals.
	alpha := 1.0 - coverage
	// The conformal quantile index (1-indexed into sorted residuals):
	//   k = ⌈(n+1)*alpha⌉  — at most n (so we return the max for small n).
	kIdx := int(math.Ceil(float64(n+1)*alpha)) - 1 // 0-indexed into sorted slice
	sort.Float64s(residuals)
	if kIdx < 0 {
		kIdx = 0
	}
	if kIdx >= n {
		kIdx = n - 1
	}
	halfWidth := residuals[kIdx]

	return Interval{
		Mean:     mean,
		Lo:       mean - halfWidth,
		Hi:       mean + halfWidth,
		N:        n,
		Coverage: coverage,
	}, nil
}

// MeetsTarget returns true when the interval's lower bound is at least
// target − margin.  The pruneMargin allows a small slack so that rungs
// slightly below target due to sampling noise are retained.
func (iv Interval) MeetsTarget(target, margin float64) bool {
	return iv.Lo >= target-margin
}

// RejectableWithHighConfidence returns true when the interval's upper bound
// is strictly below target.  Such rungs can be dropped without risking a
// false negative — even optimistic outcome is below the target.
func (iv Interval) RejectableWithHighConfidence(target float64) bool {
	return iv.Hi < target
}
