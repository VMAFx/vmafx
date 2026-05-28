// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package conformal_test

import (
	"math"
	"testing"

	"github.com/VMAFx/vmafx/pkg/conformal"
)

// TestCompute_basicInterval verifies that the interval mean equals the sample
// mean and that lo ≤ mean ≤ hi.
func TestCompute_basicInterval(t *testing.T) {
	t.Parallel()
	samples := []float64{85.0, 86.0, 84.5, 85.5, 85.2, 84.8, 86.1, 85.3, 84.9, 85.7}
	iv, err := conformal.Compute(samples, 0.90)
	if err != nil {
		t.Fatalf("Compute: %v", err)
	}
	wantMean := 0.0
	for _, s := range samples {
		wantMean += s
	}
	wantMean /= float64(len(samples))

	if math.Abs(iv.Mean-wantMean) > 1e-9 {
		t.Errorf("Mean = %.6f, want %.6f", iv.Mean, wantMean)
	}
	if iv.Lo > iv.Mean {
		t.Errorf("Lo (%v) > Mean (%v)", iv.Lo, iv.Mean)
	}
	if iv.Hi < iv.Mean {
		t.Errorf("Hi (%v) < Mean (%v)", iv.Hi, iv.Mean)
	}
	if iv.N != len(samples) {
		t.Errorf("N = %d, want %d", iv.N, len(samples))
	}
}

// TestCompute_symmetry verifies that a uniform sample set yields a symmetric
// interval around the mean.
func TestCompute_symmetry(t *testing.T) {
	t.Parallel()
	// Symmetric around 85.0.
	samples := []float64{84.0, 86.0, 83.0, 87.0, 82.0, 88.0}
	iv, err := conformal.Compute(samples, 0.90)
	if err != nil {
		t.Fatalf("Compute: %v", err)
	}
	if math.Abs((iv.Hi-iv.Mean)-(iv.Mean-iv.Lo)) > 1e-9 {
		t.Errorf("interval is not symmetric: lo=%v, mean=%v, hi=%v", iv.Lo, iv.Mean, iv.Hi)
	}
}

// TestCompute_tooFewSamples verifies that < 2 samples returns an error.
func TestCompute_tooFewSamples(t *testing.T) {
	t.Parallel()
	_, err := conformal.Compute([]float64{85.0}, 0.90)
	if err == nil {
		t.Error("expected error for 1 sample, got nil")
	}
	_, err = conformal.Compute(nil, 0.90)
	if err == nil {
		t.Error("expected error for nil samples, got nil")
	}
}

// TestCompute_invalidCoverage verifies that coverage outside (0,1) errors.
func TestCompute_invalidCoverage(t *testing.T) {
	t.Parallel()
	samples := []float64{85, 86, 84, 87, 83}
	for _, cov := range []float64{0, 1, -0.1, 1.5} {
		_, err := conformal.Compute(samples, cov)
		if err == nil {
			t.Errorf("expected error for coverage=%v, got nil", cov)
		}
	}
}

// TestMeetsTarget verifies the pruning decision rule.
func TestMeetsTarget(t *testing.T) {
	t.Parallel()
	// Interval: [83.0, 87.0], mean = 85.0.
	samples := []float64{83, 87, 85, 85, 85, 85, 85, 85, 85, 85}
	iv, err := conformal.Compute(samples, 0.90)
	if err != nil {
		t.Fatalf("Compute: %v", err)
	}

	// Target 85 with 0 margin: lo must be >= 85 — depends on quantile.
	// Just test the boundary: MeetsTarget(iv.Lo, 0) should always be true.
	if !iv.MeetsTarget(iv.Lo, 0) {
		t.Errorf("MeetsTarget(lo, 0) = false, want true")
	}
	// MeetsTarget well above hi should be false.
	if iv.MeetsTarget(iv.Hi+10, 0) {
		t.Errorf("MeetsTarget(hi+10, 0) = true, want false")
	}
}

// TestRejectableWithHighConfidence verifies the rejection rule.
func TestRejectableWithHighConfidence(t *testing.T) {
	t.Parallel()
	samples := []float64{70, 71, 70.5, 71.5, 70.2, 70.8, 71.2, 70.6, 71.3, 70.9}
	iv, err := conformal.Compute(samples, 0.90)
	if err != nil {
		t.Fatalf("Compute: %v", err)
	}
	// Target 85 is way above the hi bound — should be rejectable.
	if !iv.RejectableWithHighConfidence(85) {
		t.Errorf("RejectableWithHighConfidence(85) = false for interval %v, want true", iv)
	}
	// Target equal to lo — should NOT be rejectable (hi >= lo >= target is possible).
	if iv.RejectableWithHighConfidence(iv.Lo - 1) {
		t.Errorf("RejectableWithHighConfidence(lo-1) = true, want false")
	}
}
