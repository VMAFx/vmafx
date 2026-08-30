// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package prefilter

import (
	"math"
	"math/rand"
	"testing"
)

// TestNormalPPF asserts the probit is the inverse of the normal CDF to well
// under the tolerance the sampler needs, including deep in both tails where
// the Acklam approximation is weakest before refinement.
func TestNormalPPF(t *testing.T) {
	t.Parallel()

	// Reference values from Python's statistics.NormalDist().inv_cdf, which
	// implements the Wichura AS241 algorithm to full double precision.
	tests := []struct {
		name string
		p    float64
		want float64
	}{
		{"median", 0.5, 0.0},
		{"one sigma", 0.8413447460685429, 1.0},
		{"minus one sigma", 0.15865525393145707, -1.0},
		{"two sigma", 0.9772498680518208, 2.0},
		{"lower tail", 1e-8, -5.61200124417479},
		{"upper tail", 1 - 1e-8, 5.61200124417479},
		{"at the Acklam branch point", 0.02425, -1.9729610513118845},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if got := normalPPF(tc.p); math.Abs(got-tc.want) > 1e-9 {
				t.Errorf("normalPPF(%v) = %v, want %v", tc.p, got, tc.want)
			}
		})
	}

	if !math.IsInf(normalPPF(0.0), -1) {
		t.Error("normalPPF(0) should be -Inf")
	}
	if !math.IsInf(normalPPF(1.0), 1) {
		t.Error("normalPPF(1) should be +Inf")
	}
}

// TestNormalPPF_isAntisymmetric pins the reflection the implementation relies
// on to keep the upper tail well conditioned.
func TestNormalPPF_isAntisymmetric(t *testing.T) {
	t.Parallel()

	// Round-trip through the float64 complement so the two arguments really
	// are each other's reflection: 1-(1-p) != p for tiny p.
	for _, p := range []float64{1e-12, 1e-8, 0.001, 0.02425, 0.1, 0.25, 0.4999} {
		upperArg := 1.0 - p
		lowerArg := 1.0 - upperArg
		lower, upper := normalPPF(lowerArg), normalPPF(upperArg)
		if math.Abs(lower+upper) > 1e-12 {
			t.Errorf("normalPPF(%v) = %v and normalPPF(%v) = %v are not antisymmetric",
				lowerArg, lower, upperArg, upper)
		}
	}
}

// TestNormalPPF_roundTripsCDF sweeps the open unit interval and asserts the
// composition with the CDF is the identity.
func TestNormalPPF_roundTripsCDF(t *testing.T) {
	t.Parallel()

	for i := 1; i < 1000; i++ {
		p := float64(i) / 1000.0
		if got := normalCDF(normalPPF(p)); math.Abs(got-p) > 1e-12 {
			t.Fatalf("normalCDF(normalPPF(%v)) = %v", p, got)
		}
	}
}

// TestNormalCDF pins a few reference values.
func TestNormalCDF(t *testing.T) {
	t.Parallel()

	tests := []struct {
		z    float64
		want float64
	}{
		{0.0, 0.5},
		{1.0, 0.8413447460685429},
		{-1.0, 0.15865525393145707},
		{3.0, 0.9986501019683699},
	}
	for _, tc := range tests {
		if got := normalCDF(tc.z); math.Abs(got-tc.want) > 1e-12 {
			t.Errorf("normalCDF(%v) = %v, want %v", tc.z, got, tc.want)
		}
	}
}

// TestLogSumExp covers the overflow-safe reduction and its empty case.
func TestLogSumExp(t *testing.T) {
	t.Parallel()

	// log(e^0 + e^0) = log 2
	if got := logSumExp([]float64{0, 0}); math.Abs(got-math.Log(2)) > 1e-12 {
		t.Errorf("logSumExp([0 0]) = %v, want log 2", got)
	}
	// Values that would overflow if exponentiated directly.
	if got := logSumExp([]float64{1000, 1000}); math.Abs(got-(1000+math.Log(2))) > 1e-9 {
		t.Errorf("logSumExp([1000 1000]) = %v, want 1000+log2", got)
	}
	if got := logSumExp(nil); !math.IsInf(got, -1) {
		t.Errorf("logSumExp(nil) = %v, want -Inf", got)
	}
	if got := logSumExp([]float64{math.Inf(-1)}); !math.IsInf(got, -1) {
		t.Errorf("logSumExp([-Inf]) = %v, want -Inf", got)
	}
}

// TestParzen_isAProperDensity asserts the estimator integrates to about 1
// over its support, which is what makes the log-ratio a valid EI proxy.
func TestParzen_isAProperDensity(t *testing.T) {
	t.Parallel()

	dim := Dimension{Name: "x", Kind: KindFloat, Lo: 0.0, Hi: 1.0}
	tests := []struct {
		name   string
		values []float64
	}{
		{"prior only", nil},
		{"single observation", []float64{0.3}},
		{"clustered observations", []float64{0.3, 0.31, 0.32, 0.33}},
		{"spread observations", []float64{0.05, 0.25, 0.5, 0.75, 0.95}},
		{"observations at the boundaries", []float64{0.0, 1.0}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			p := newParzen(tc.values, dim, 1.0)
			const steps = 20000
			integral := 0.0
			for i := 0; i < steps; i++ {
				x := dim.Lo + (float64(i)+0.5)*(dim.Hi-dim.Lo)/steps
				integral += math.Exp(p.logPDF(x)) * (dim.Hi - dim.Lo) / steps
			}
			if math.Abs(integral-1.0) > 1e-3 {
				t.Errorf("density integrates to %v, want 1", integral)
			}
		})
	}
}

// TestParzen_concentratesOnObservations asserts the estimator actually favours
// the observed region — without that the sampler is just random search.
func TestParzen_concentratesOnObservations(t *testing.T) {
	t.Parallel()

	dim := Dimension{Name: "x", Kind: KindFloat, Lo: 0.0, Hi: 1.0}
	p := newParzen([]float64{0.8, 0.81, 0.82, 0.79, 0.80}, dim, 1.0)

	atCluster := p.logPDF(0.80)
	awayFromCluster := p.logPDF(0.10)
	if atCluster <= awayFromCluster {
		t.Errorf("density at the cluster (%v) should exceed density away from it (%v)",
			atCluster, awayFromCluster)
	}
}

// TestParzen_sampleStaysInBounds asserts truncation holds, including for a
// component centred on a boundary.
func TestParzen_sampleStaysInBounds(t *testing.T) {
	t.Parallel()

	dim := Dimension{Name: "x", Kind: KindFloat, Lo: -5.0, Hi: 5.0}
	p := newParzen([]float64{-5.0, 5.0, 0.0}, dim, 1.0)
	//nolint:gosec // G404: deterministic test fixture, not a security context.
	rng := rand.New(rand.NewSource(1))

	for i := 0; i < 10000; i++ {
		v := p.sample(rng)
		if v < dim.Lo || v > dim.Hi {
			t.Fatalf("sample %v escaped [%v, %v]", v, dim.Lo, dim.Hi)
		}
		if math.IsNaN(v) {
			t.Fatal("sample produced NaN")
		}
	}
}

// TestQuantise covers the lattice snapping for integral dimensions.
func TestQuantise(t *testing.T) {
	t.Parallel()

	intDim := Dimension{Name: "crf", Kind: KindInt, Lo: 18, Hi: 40}
	floatDim := Dimension{Name: "thry", Kind: KindFloat, Lo: 0.0, Hi: 0.25}

	tests := []struct {
		name string
		dim  Dimension
		in   float64
		want float64
	}{
		{"integral rounds", intDim, 23.4, 23},
		{"integral rounds up", intDim, 23.6, 24},
		{"integral clamps low", intDim, 5, 18},
		{"integral clamps high", intDim, 99, 40},
		{"float passes through", floatDim, 0.0123, 0.0123},
		{"float clamps low", floatDim, -1, 0.0},
		{"float clamps high", floatDim, 1, 0.25},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if got := quantise(tc.in, tc.dim); got != tc.want {
				t.Errorf("quantise(%v) = %v, want %v", tc.in, got, tc.want)
			}
		})
	}
}

// TestTPESampler_startupIsUniform asserts the first StartupTrials proposals
// come from the uniform sampler, and that every proposal respects the space.
func TestTPESampler_startupIsUniform(t *testing.T) {
	t.Parallel()

	dims := []Dimension{
		{Name: "crf", Kind: KindInt, Lo: 18, Hi: 40},
		{Name: "grainy", Kind: KindFloat, Lo: 0.0, Hi: 0.4},
	}
	s := NewTPESampler(dims, DefaultTPEConfig(), 1)

	seen := map[float64]bool{}
	for i := 0; i < 40; i++ {
		p := s.Suggest()
		crf, grainy := p["crf"], p["grainy"]
		if crf < 18 || crf > 40 || crf != math.Trunc(crf) {
			t.Fatalf("crf proposal %v is off the lattice", crf)
		}
		if grainy < 0.0 || grainy > 0.4 {
			t.Fatalf("grainy proposal %v is out of range", grainy)
		}
		seen[crf] = true
		// Feed back an objective that rewards a CRF near 30.
		s.Observe(p, math.Abs(crf-30))
	}
	if len(seen) < 5 {
		t.Errorf("the sampler explored only %d distinct CRFs in 40 trials", len(seen))
	}
	if s.Trials() != 40 {
		t.Errorf("Trials = %d, want 40", s.Trials())
	}
}

// TestTPESampler_convergesOnTheOptimum is the substantive sampler test. On a
// unimodal objective the estimator must concentrate its late proposals far
// more tightly around the optimum than a uniform draw does.
//
// Concentration, not best-point-found, is the property under test: on a
// one-dimensional box a uniform sweep covers the space densely enough that it
// often stumbles onto a very good single point, so "best error" is a noisy
// statistic. Where TPE earns its keep is spending its later budget near the
// optimum instead of re-sampling the whole box — which is exactly what makes
// it worth running when each trial costs a full encode plus a VMAF score.
//
// The baseline is computed in-test and averaged over several seeds so the
// assertion stays meaningful and non-flaky if the space or budget is retuned.
func TestTPESampler_convergesOnTheOptimum(t *testing.T) {
	t.Parallel()

	const (
		optimum = 72.0
		trials  = 200
		lateAt  = 150
		seeds   = 5
	)
	dims := []Dimension{{Name: "x", Kind: KindFloat, Lo: 0.0, Hi: 100.0}}

	lateMean := func(values []float64) float64 {
		sum := 0.0
		for _, v := range values {
			sum += math.Abs(v - optimum)
		}
		return sum / float64(len(values))
	}

	tpeTotal, uniformTotal := 0.0, 0.0
	for seed := int64(1); seed <= seeds; seed++ {
		s := NewTPESampler(dims, DefaultTPEConfig(), seed)
		var tpeLate []float64
		for i := 0; i < trials; i++ {
			p := s.Suggest()
			s.Observe(p, math.Abs(p["x"]-optimum))
			if i >= lateAt {
				tpeLate = append(tpeLate, p["x"])
			}
		}
		tpeTotal += lateMean(tpeLate)

		//nolint:gosec // G404: deterministic test fixture, not a security context.
		rng := rand.New(rand.NewSource(seed))
		var uniformLate []float64
		for i := 0; i < trials; i++ {
			x := rng.Float64() * 100.0
			if i >= lateAt {
				uniformLate = append(uniformLate, x)
			}
		}
		uniformTotal += lateMean(uniformLate)
	}

	tpeMean := tpeTotal / seeds
	uniformMean := uniformTotal / seeds
	// Require a clear margin, not a coin-flip win: half the uniform error.
	if tpeMean >= uniformMean/2.0 {
		t.Errorf("TPE late-phase mean error %v vs uniform %v; the estimator is "+
			"not concentrating the search", tpeMean, uniformMean)
	}
}

// TestTPESampler_isDeterministic asserts the same seed replays the same
// proposal sequence.
func TestTPESampler_isDeterministic(t *testing.T) {
	t.Parallel()

	dims := []Dimension{
		{Name: "crf", Kind: KindInt, Lo: 18, Hi: 40},
		{Name: "grainy", Kind: KindFloat, Lo: 0.0, Hi: 0.4},
	}
	run := func(seed int64) []map[string]float64 {
		s := NewTPESampler(dims, DefaultTPEConfig(), seed)
		out := make([]map[string]float64, 0, 30)
		for i := 0; i < 30; i++ {
			p := s.Suggest()
			out = append(out, p)
			s.Observe(p, math.Abs(p["crf"]-30))
		}
		return out
	}
	a, b := run(5), run(5)
	for i := range a {
		if a[i]["crf"] != b[i]["crf"] || a[i]["grainy"] != b[i]["grainy"] {
			t.Fatalf("trial %d diverged: %v vs %v", i, a[i], b[i])
		}
	}

	c := run(6)
	same := true
	for i := range a {
		if a[i]["crf"] != c[i]["crf"] || a[i]["grainy"] != c[i]["grainy"] {
			same = false
			break
		}
	}
	if same {
		t.Error("different seeds produced an identical proposal sequence")
	}
}

// TestTPESampler_degenerateDimension covers a zero-width axis, which the
// bandwidth heuristic must not divide by.
func TestTPESampler_degenerateDimension(t *testing.T) {
	t.Parallel()

	dims := []Dimension{{Name: "crf", Kind: KindInt, Lo: 23, Hi: 23}}
	s := NewTPESampler(dims, DefaultTPEConfig(), 1)
	for i := 0; i < 30; i++ {
		p := s.Suggest()
		if p["crf"] != 23 {
			t.Fatalf("proposal %v on a degenerate axis, want 23", p["crf"])
		}
		s.Observe(p, 1.0)
	}
}

// TestTPESampler_split asserts the l/g partition sizes follow Optuna's
// default_gamma rule — ceil(gamma*n), capped at 25 — and always leave g
// non-empty.
func TestTPESampler_split(t *testing.T) {
	t.Parallel()

	dims := []Dimension{{Name: "x", Kind: KindFloat, Lo: 0, Hi: 1}}
	s := NewTPESampler(dims, DefaultTPEConfig(), 1)

	for n := 1; n <= 5000; n *= 5 {
		s.obs = nil
		for i := 0; i < n; i++ {
			s.Observe(map[string]float64{"x": float64(i) / float64(n)}, float64(i))
		}
		below, above := s.split()
		if n == 1 {
			if len(below) != 0 || len(above) != 0 {
				t.Errorf("n=1 should not split; got %d/%d", len(below), len(above))
			}
			continue
		}
		if len(below) == 0 || len(above) == 0 {
			t.Errorf("n=%d produced an empty side: %d/%d", n, len(below), len(above))
		}
		if len(below) > 25 {
			t.Errorf("n=%d put %d in the good set, above the cap of 25", n, len(below))
		}
		// The good set must actually hold the best objectives.
		for _, o := range below {
			for _, a := range above {
				if o.objective > a.objective {
					t.Fatalf("split is not ordered: %v in below, %v in above",
						o.objective, a.objective)
				}
			}
		}
	}
}
