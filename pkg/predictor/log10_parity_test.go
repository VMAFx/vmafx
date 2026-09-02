// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

package predictor

import (
	"math"
	"math/rand"
	"testing"

	"github.com/VMAFx/vmafx/pkg/pymath"
)

// TestAnalyticalCurveUsesPythonLog10 pins that the analytical VMAF curve routes
// its log10 through pymath, not Go's math.Log10.
//
// Go implements Log10 as Log2*Ln2/Ln10 and lands one ULP off CPython on roughly
// 27% of realistic probe bitrates. predicted_vmaf is a user-discoverable JSON
// field, and before ADR-1137 folded the two predictors the pkg/tune/predictor
// copy already made this distinction, so `predict` and `auto` disagreed on
// the same input.
func TestAnalyticalCurveUsesPythonLog10(t *testing.T) {
	t.Parallel()

	// Guard the premise: if Go's Log10 ever becomes bit-identical to pymath's,
	// this test is vacuous and should be revisited rather than silently passing.
	rng := rand.New(rand.NewSource(20260830))
	divergent := 0
	for i := 0; i < 20000; i++ {
		b := 1.0 + rng.Float64()*49999.0
		if math.Log10(b) != pymath.Log10(b) {
			divergent++
		}
	}
	if divergent == 0 {
		t.Skip("math.Log10 and pymath.Log10 agree on every sample; premise no longer holds")
	}

	// The curve must track pymath on inputs where the two disagree.
	p := New()
	for i := 0; i < 5000; i++ {
		b := 1.0 + rng.Float64()*49999.0
		if math.Log10(b) == pymath.Log10(b) {
			continue
		}
		feat := ShotFeatures{ProbeBitrateKbps: b}
		got := p.PredictAnalytical(feat, 23, "libx264")

		c := p.coeffsFor("libx264")
		delta := float64(23) - c.CRFRef
		wantPy := clamp(c.A-c.B*delta-c.C*delta*delta+c.D*pymath.Log10(b), 0, 100)
		wantGo := clamp(c.A-c.B*delta-c.C*delta*delta+c.D*math.Log10(b), 0, 100)
		if got != wantPy && wantPy != wantGo {
			t.Fatalf("bitrate %v: got %v, want %v (pymath); Go's math.Log10 would give %v",
				b, got, wantPy, wantGo)
		}
	}
}
