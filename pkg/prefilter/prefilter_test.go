// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package prefilter_test

import (
	"context"
	"encoding/json"
	"errors"
	"math"
	"slices"
	"strings"
	"testing"
	"time"

	"github.com/VMAFx/vmafx/pkg/prefilter"
)

// TestVFFragment_matchesPythonGolden pins the emitted -vf strings against the
// Python PelorusDebandAdapter.vf_fragment for the same parameter sets.
//
// The fragment is what actually reaches ffmpeg, so its knob ordering, its
// number formatting (0.0 collapsing to "0", 0.012 staying "0.012") and its
// bare-filter fallback all have to match exactly.
func TestVFFragment_matchesPythonGolden(t *testing.T) {
	t.Parallel()

	adapter := prefilter.PelorusDeband()

	tests := []struct {
		name            string
		params          map[string]float64
		includeDefaults bool
		want            string
	}{
		{
			name:   "no knobs emits the bare filter name",
			params: map[string]float64{},
			want:   "pelorus_deband_vulkan",
		},
		{
			name: "partial set in contract order",
			params: map[string]float64{
				"grainy": 0.006, "range": 15, "thry": 0.012,
			},
			want: "pelorus_deband_vulkan=range=15:thry=0.012:grainy=0.006",
		},
		{
			name: "zero float collapses and integrals stay bare",
			params: map[string]float64{
				"dynamic": 1, "grainy": 0.0, "dither": 2,
			},
			want: "pelorus_deband_vulkan=grainy=0:dither=2:dynamic=1",
		},
		{
			name:   "include-defaults pins every contract knob",
			params: map[string]float64{}, includeDefaults: true,
			want: "pelorus_deband_vulkan=range=15:thry=0.012:thrc=0.012:" +
				"grainy=0.006:grainc=0:softness=0.5:detail=0.06:dither=2:" +
				"dynamic=1:protect=1",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := adapter.VFFragment(tc.params, tc.includeDefaults)
			if err != nil {
				t.Fatalf("VFFragment: %v", err)
			}
			if got != tc.want {
				t.Errorf("VFFragment = %q, want %q", got, tc.want)
			}
		})
	}
}

// TestVFArgs wraps the fragment in the argv slice the encode driver splices.
func TestVFArgs(t *testing.T) {
	t.Parallel()

	got, err := prefilter.PelorusDeband().VFArgs(
		map[string]float64{"range": 15}, false)
	if err != nil {
		t.Fatalf("VFArgs: %v", err)
	}
	want := []string{"-vf", "pelorus_deband_vulkan=range=15"}
	if !slices.Equal(got, want) {
		t.Errorf("VFArgs = %v, want %v", got, want)
	}
}

// TestValidate enforces the frozen contract: unknown knobs (including the
// deliberately out-of-contract pipeline switches), out-of-range values, NaN,
// and fractional values on integral knobs.
func TestValidate(t *testing.T) {
	t.Parallel()

	adapter := prefilter.PelorusDeband()

	tests := []struct {
		name    string
		params  map[string]float64
		wantErr bool
	}{
		{"empty is valid", map[string]float64{}, false},
		{"contract defaults", adapter.Defaults(), false},
		{"range at the floor", map[string]float64{"range": 1}, false},
		{"range at the ceiling", map[string]float64{"range": 31}, false},
		{"range below the floor", map[string]float64{"range": 0}, true},
		{"range above the ceiling", map[string]float64{"range": 32}, true},
		{"fractional range", map[string]float64{"range": 15.5}, true},
		{"fractional dither", map[string]float64{"dither": 1.5}, true},
		{"float knob accepts fractions", map[string]float64{"thry": 0.0123}, false},
		{"thry above the ceiling", map[string]float64{"thry": 0.26}, true},
		{"NaN", map[string]float64{"thry": math.NaN()}, true},
		{"out-of-contract sample", map[string]float64{"sample": 1}, true},
		{"out-of-contract blur", map[string]float64{"blur": 1}, true},
		{"out-of-contract planes", map[string]float64{"planes": 1}, true},
		{"out-of-contract meta", map[string]float64{"meta": 1}, true},
		{"typo", map[string]float64{"grainyy": 0.01}, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if err := adapter.Validate(tc.params); (err != nil) != tc.wantErr {
				t.Errorf("Validate error = %v, wantErr %v", err, tc.wantErr)
			}
		})
	}
}

// TestValidate_outOfContractMessage asserts the rejection explains WHY the
// pipeline switches are absent, which is the whole point of the message.
func TestValidate_outOfContractMessage(t *testing.T) {
	t.Parallel()

	err := prefilter.PelorusDeband().Validate(map[string]float64{"sample": 1})
	if err == nil {
		t.Fatal("expected an error")
	}
	if !strings.Contains(err.Error(), "ADR-0110") ||
		!strings.Contains(err.Error(), "out-of-contract") {
		t.Errorf("message should cite the contract and explain the exclusion; got %q", err)
	}
}

// TestClamp bounds out-of-range numbers and rounds integral knobs, but still
// rejects unknown names.
func TestClamp(t *testing.T) {
	t.Parallel()

	adapter := prefilter.PelorusDeband()

	got, err := adapter.Clamp(map[string]float64{
		"range": 99, "thry": -1.0, "dither": 1.4, "softness": 0.5,
	})
	if err != nil {
		t.Fatalf("Clamp: %v", err)
	}
	want := map[string]float64{
		"range": 31, "thry": 0.0, "dither": 1, "softness": 0.5,
	}
	for k, v := range want {
		if got[k] != v {
			t.Errorf("Clamp[%s] = %v, want %v", k, got[k], v)
		}
	}
	if _, unknownErr := adapter.Clamp(map[string]float64{"nope": 1}); unknownErr == nil {
		t.Error("Clamp should still reject unknown knobs")
	}
}

// TestKnobTable_isTheFrozenContract guards the ten-knob table against silent
// drift: a rename, retype or range change here is a coordinated two-repo
// break, so it must fail loudly.
func TestKnobTable_isTheFrozenContract(t *testing.T) {
	t.Parallel()

	want := []struct {
		name   string
		kind   prefilter.KnobKind
		lo, hi float64
		defVal float64
	}{
		{"range", prefilter.KindInt, 1.0, 31.0, 15.0},
		{"thry", prefilter.KindFloat, 0.0, 0.25, 0.012},
		{"thrc", prefilter.KindFloat, 0.0, 0.25, 0.012},
		{"grainy", prefilter.KindFloat, 0.0, 0.4, 0.006},
		{"grainc", prefilter.KindFloat, 0.0, 0.4, 0.0},
		{"softness", prefilter.KindFloat, 0.0, 1.0, 0.5},
		{"detail", prefilter.KindFloat, 0.0, 0.25, 0.06},
		{"dither", prefilter.KindEnum, 0.0, 2.0, 2.0},
		{"dynamic", prefilter.KindBool, 0.0, 1.0, 1.0},
		{"protect", prefilter.KindBool, 0.0, 1.0, 1.0},
	}
	knobs := prefilter.PelorusDeband().Knobs()
	if len(knobs) != len(want) {
		t.Fatalf("knob count = %d, want %d", len(knobs), len(want))
	}
	for i, w := range want {
		k := knobs[i]
		if k.Name != w.name || k.Kind != w.kind ||
			k.Lo != w.lo || k.Hi != w.hi || k.Default != w.defVal {
			t.Errorf("knob %d = %+v, want {%s %s [%g %g] default %g}",
				i, k, w.name, w.kind, w.lo, w.hi, w.defVal)
		}
	}
}

// TestBuildSearchSpace covers the full sweep, the knob subset, the CRF axis
// and the range validation.
func TestBuildSearchSpace(t *testing.T) {
	t.Parallel()

	adapter := prefilter.PelorusDeband()

	tests := []struct {
		name     string
		crfRange [2]int
		knobs    []string
		wantDims int
		wantErr  bool
	}{
		{"full sweep is ten knobs plus CRF", [2]int{18, 40}, nil, 11, false},
		{"knob subset", [2]int{18, 40}, []string{"grainy", "thry"}, 3, false},
		{"single knob", [2]int{18, 40}, []string{"range"}, 2, false},
		{"degenerate CRF range is allowed", [2]int{23, 23}, nil, 11, false},
		{"inverted CRF range", [2]int{40, 18}, nil, 0, true},
		{"negative CRF floor", [2]int{-1, 40}, nil, 0, true},
		{"unknown knob", [2]int{18, 40}, []string{"grainyy"}, 0, true},
		{"out-of-contract knob", [2]int{18, 40}, []string{"sample"}, 0, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			dims, err := prefilter.BuildSearchSpace(adapter, tc.crfRange, tc.knobs)
			if (err != nil) != tc.wantErr {
				t.Fatalf("BuildSearchSpace error = %v, wantErr %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if len(dims) != tc.wantDims {
				t.Errorf("dimension count = %d, want %d", len(dims), tc.wantDims)
			}
			last := dims[len(dims)-1]
			if last.Name != "crf" || last.Kind != prefilter.KindInt {
				t.Errorf("last dimension = %+v, want the integral crf axis", last)
			}
			if last.Lo != float64(tc.crfRange[0]) || last.Hi != float64(tc.crfRange[1]) {
				t.Errorf("crf axis = [%g, %g], want [%d, %d]",
					last.Lo, last.Hi, tc.crfRange[0], tc.crfRange[1])
			}
		})
	}
}

// TestGetAdapter covers the registry lookup.
func TestGetAdapter(t *testing.T) {
	t.Parallel()

	if _, err := prefilter.GetAdapter("pelorus_deband"); err != nil {
		t.Errorf("GetAdapter(pelorus_deband): %v", err)
	}
	if _, err := prefilter.GetAdapter("unsharp"); err == nil {
		t.Error("expected an error for an unregistered filter")
	}
	if got := prefilter.KnownFilters(); !slices.Equal(got, []string{"pelorus_deband"}) {
		t.Errorf("KnownFilters = %v, want [pelorus_deband]", got)
	}
}

// TestObjective pins the |achieved - target| + lambda*kbps form and the
// bitrate tie-break.
func TestObjective(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		result prefilter.ProbeResult
		target float64
		want   float64
	}{
		{
			name:   "exact hit costs only the bitrate term",
			result: prefilter.ProbeResult{VMAF: 93.0, Kbps: 5000},
			target: 93.0, want: 0.5,
		},
		{
			name:   "quality gap dominates",
			result: prefilter.ProbeResult{VMAF: 88.0, Kbps: 5000},
			target: 93.0, want: 5.5,
		},
		{
			name:   "overshoot is penalised symmetrically",
			result: prefilter.ProbeResult{VMAF: 98.0, Kbps: 5000},
			target: 93.0, want: 5.5,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if got := prefilter.Objective(tc.result, tc.target); math.Abs(got-tc.want) > 1e-9 {
				t.Errorf("Objective = %v, want %v", got, tc.want)
			}
		})
	}

	// The bitrate term must be small enough that it only breaks ties: a
	// 1 VMAF quality gap must outweigh any plausible bitrate difference.
	cheapButOff := prefilter.Objective(
		prefilter.ProbeResult{VMAF: 92.0, Kbps: 100}, 93.0)
	expensiveButExact := prefilter.Objective(
		prefilter.ProbeResult{VMAF: 93.0, Kbps: 9000}, 93.0)
	if expensiveButExact >= cheapButOff {
		t.Error("the bitrate term should only break ties, not outrank quality")
	}
}

// TestSmokeProbe pins the synthetic surface's shape: monotone in CRF, peaked
// at the documented grain level.
func TestSmokeProbe(t *testing.T) {
	t.Parallel()

	probe := prefilter.SmokeProbe(prefilter.PelorusDeband())
	ctx := context.Background()

	prev := math.Inf(1)
	for crf := 18; crf <= 40; crf++ {
		got, err := probe(ctx, map[string]float64{"grainy": 0.006}, crf)
		if err != nil {
			t.Fatalf("SmokeProbe: %v", err)
		}
		if got.VMAF > prev {
			t.Fatalf("VMAF rose at crf=%d: %v > %v", crf, got.VMAF, prev)
		}
		prev = got.VMAF
		if got.Kbps <= 0 {
			t.Fatalf("kbps should stay positive; got %v at crf=%d", got.Kbps, crf)
		}
	}

	// Moderate grain beats both extremes.
	atOptimum, _ := probe(ctx, map[string]float64{"grainy": 0.006}, 25)
	atZero, _ := probe(ctx, map[string]float64{"grainy": 0.0}, 25)
	atMax, _ := probe(ctx, map[string]float64{"grainy": 0.4}, 25)
	if atOptimum.VMAF <= atZero.VMAF || atOptimum.VMAF <= atMax.VMAF {
		t.Errorf("the grain optimum should beat both extremes: %v vs %v / %v",
			atOptimum.VMAF, atZero.VMAF, atMax.VMAF)
	}
}

// TestRecommendPrefilter_smokeConverges asserts the joint search actually
// finds a near-target point on the synthetic surface, not merely that it
// runs. The surface hits VMAF 93 around CRF 25, so a converged search should
// land close.
func TestRecommendPrefilter_smokeConverges(t *testing.T) {
	t.Parallel()

	got, err := prefilter.RecommendPrefilter(context.Background(), prefilter.Options{
		TargetVMAF: 93.0,
		Encoder:    "libx264",
		CRFRange:   [2]int{18, 40},
		NTrials:    120,
		Smoke:      true,
		Seed:       7,
	})
	if err != nil {
		t.Fatalf("RecommendPrefilter: %v", err)
	}
	if got.NTrials != 120 {
		t.Errorf("NTrials = %d, want 120", got.NTrials)
	}
	if math.Abs(got.AchievedVMAF-93.0) > 1.0 {
		t.Errorf("achieved VMAF = %v, want within 1.0 of the 93.0 target",
			got.AchievedVMAF)
	}
	if got.RecommendedCRF < 18 || got.RecommendedCRF > 40 {
		t.Errorf("recommended CRF %d is outside the search window",
			got.RecommendedCRF)
	}
	if !strings.HasPrefix(got.RecommendedVF, "pelorus_deband_vulkan") {
		t.Errorf("RecommendedVF = %q, want a pelorus_deband_vulkan fragment",
			got.RecommendedVF)
	}
	if !strings.Contains(got.Notes, "smoke mode") {
		t.Errorf("smoke notes = %q", got.Notes)
	}
}

// TestRecommendPrefilter_beatsRandomSearch is the substantive check that the
// sampler is doing something: TPE with the same budget must beat a purely
// random sweep of the same space on the same surface.
func TestRecommendPrefilter_beatsRandomSearch(t *testing.T) {
	t.Parallel()

	const budget = 60
	tpe, err := prefilter.RecommendPrefilter(context.Background(), prefilter.Options{
		TargetVMAF: 93.0, Encoder: "libx264", CRFRange: [2]int{18, 40},
		NTrials: budget, Smoke: true, Seed: 3,
	})
	if err != nil {
		t.Fatalf("RecommendPrefilter: %v", err)
	}

	// A random search is the same loop with the estimator never engaging,
	// which is what a huge startup-trial count gives us. Approximate it by
	// scoring the best of the first ten (uniform) trials of the TPE run.
	uniformBest := math.Inf(1)
	for i, p := range tpe.Probes {
		if i >= 10 {
			break
		}
		if p.Objective < uniformBest {
			uniformBest = p.Objective
		}
	}
	finalBest := math.Inf(1)
	for _, p := range tpe.Probes {
		if p.Objective < finalBest {
			finalBest = p.Objective
		}
	}
	if finalBest > uniformBest {
		t.Errorf("the estimator made the search worse: best %v vs uniform-phase %v",
			finalBest, uniformBest)
	}
}

// TestRecommendPrefilter_isDeterministic pins reproducibility: the same seed
// must reproduce the same recommendation, which is what --seed promises.
func TestRecommendPrefilter_isDeterministic(t *testing.T) {
	t.Parallel()

	run := func() prefilter.Result {
		got, err := prefilter.RecommendPrefilter(context.Background(), prefilter.Options{
			TargetVMAF: 93.0, Encoder: "libx264", CRFRange: [2]int{18, 40},
			NTrials: 40, Smoke: true, Seed: 42,
		})
		if err != nil {
			t.Fatalf("RecommendPrefilter: %v", err)
		}
		return got
	}
	a, b := run(), run()
	if a.RecommendedCRF != b.RecommendedCRF ||
		a.RecommendedVF != b.RecommendedVF ||
		a.AchievedVMAF != b.AchievedVMAF {
		t.Errorf("the same seed produced different results:\n%+v\n%+v", a, b)
	}

	different, err := prefilter.RecommendPrefilter(context.Background(), prefilter.Options{
		TargetVMAF: 93.0, Encoder: "libx264", CRFRange: [2]int{18, 40},
		NTrials: 40, Smoke: true, Seed: 43,
	})
	if err != nil {
		t.Fatalf("RecommendPrefilter: %v", err)
	}
	if different.RecommendedVF == a.RecommendedVF &&
		different.RecommendedCRF == a.RecommendedCRF {
		t.Error("different seeds should explore differently")
	}
}

// TestRecommendPrefilter_sweepKnobsRestriction asserts an unswept knob never
// appears in the recommendation (it stays at the filter default instead).
func TestRecommendPrefilter_sweepKnobsRestriction(t *testing.T) {
	t.Parallel()

	got, err := prefilter.RecommendPrefilter(context.Background(), prefilter.Options{
		TargetVMAF: 93.0, Encoder: "libx264", CRFRange: [2]int{18, 40},
		SweepKnobs: []string{"grainy"}, NTrials: 20, Smoke: true, Seed: 1,
	})
	if err != nil {
		t.Fatalf("RecommendPrefilter: %v", err)
	}
	if len(got.RecommendedDeband) != 1 {
		t.Errorf("recommended deband = %v, want only the swept grainy knob",
			got.RecommendedDeband)
	}
	if _, ok := got.RecommendedDeband["grainy"]; !ok {
		t.Error("the swept knob should be present in the recommendation")
	}
	if strings.Contains(got.RecommendedVF, "range=") {
		t.Errorf("unswept knobs must stay off the fragment; got %q", got.RecommendedVF)
	}
}

// TestRecommendPrefilter_errors covers the production-mode guards.
func TestRecommendPrefilter_errors(t *testing.T) {
	t.Parallel()

	stubProbe := func(context.Context, map[string]float64, int) (prefilter.ProbeResult, error) {
		return prefilter.ProbeResult{VMAF: 90, Kbps: 4000, VFFragment: "x"}, nil
	}

	tests := []struct {
		name string
		opts prefilter.Options
	}{
		{
			name: "production mode needs a probe",
			opts: prefilter.Options{
				TargetVMAF: 93, Src: "/src/a.yuv", CRFRange: [2]int{18, 40}, NTrials: 1,
			},
		},
		{
			name: "production mode needs a source",
			opts: prefilter.Options{
				TargetVMAF: 93, CRFRange: [2]int{18, 40}, NTrials: 1, Probe: stubProbe,
			},
		},
		{
			name: "unknown filter",
			opts: prefilter.Options{
				TargetVMAF: 93, FilterName: "unsharp", Smoke: true, NTrials: 1,
			},
		},
		{
			name: "inverted CRF range",
			opts: prefilter.Options{
				TargetVMAF: 93, CRFRange: [2]int{40, 18}, Smoke: true, NTrials: 1,
			},
		},
		{
			name: "negative time budget",
			opts: prefilter.Options{
				TargetVMAF: 93, CRFRange: [2]int{18, 40}, Smoke: true,
				NTrials: 1, TimeBudget: -time.Second,
			},
		},
		{
			name: "unknown sweep knob",
			opts: prefilter.Options{
				TargetVMAF: 93, CRFRange: [2]int{18, 40}, Smoke: true,
				NTrials: 1, SweepKnobs: []string{"nope"},
			},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if _, err := prefilter.RecommendPrefilter(
				context.Background(), tc.opts); err == nil {
				t.Error("expected an error")
			}
		})
	}
}

// TestRecommendPrefilter_probeFailureAborts asserts a probe error stops the
// sweep rather than silently recording garbage.
func TestRecommendPrefilter_probeFailureAborts(t *testing.T) {
	t.Parallel()

	calls := 0
	probe := func(context.Context, map[string]float64, int) (prefilter.ProbeResult, error) {
		calls++
		if calls == 3 {
			return prefilter.ProbeResult{}, errors.New("ffmpeg died")
		}
		return prefilter.ProbeResult{VMAF: 90, Kbps: 4000, VFFragment: "x"}, nil
	}
	_, err := prefilter.RecommendPrefilter(context.Background(), prefilter.Options{
		TargetVMAF: 93, Src: "/src/a.yuv", Encoder: "libx264",
		CRFRange: [2]int{18, 40}, NTrials: 10, Probe: probe, Seed: 1,
	})
	if err == nil {
		t.Fatal("expected the probe failure to abort the sweep")
	}
	if !strings.Contains(err.Error(), "trial 2") {
		t.Errorf("error should name the failing trial; got %q", err)
	}
}

// TestRecommendPrefilter_timeBudget asserts the soft cap truncates the sweep.
func TestRecommendPrefilter_timeBudget(t *testing.T) {
	t.Parallel()

	probe := func(context.Context, map[string]float64, int) (prefilter.ProbeResult, error) {
		time.Sleep(5 * time.Millisecond)
		return prefilter.ProbeResult{VMAF: 90, Kbps: 4000, VFFragment: "x"}, nil
	}
	got, err := prefilter.RecommendPrefilter(context.Background(), prefilter.Options{
		TargetVMAF: 93, Src: "/src/a.yuv", Encoder: "libx264",
		CRFRange: [2]int{18, 40}, NTrials: 1000, Probe: probe,
		TimeBudget: 60 * time.Millisecond, Seed: 1,
	})
	if err != nil {
		t.Fatalf("RecommendPrefilter: %v", err)
	}
	if got.NTrials >= 1000 {
		t.Errorf("the time budget should have truncated the sweep; ran %d trials",
			got.NTrials)
	}
	if got.NTrials == 0 {
		t.Error("the budget should still allow at least one trial")
	}
}

// TestRecommendPrefilter_cancellation asserts a cancelled context stops the
// loop without losing the trials already run.
func TestRecommendPrefilter_cancellation(t *testing.T) {
	t.Parallel()

	ctx, cancel := context.WithCancel(context.Background())
	calls := 0
	probe := func(context.Context, map[string]float64, int) (prefilter.ProbeResult, error) {
		calls++
		if calls == 5 {
			cancel()
		}
		return prefilter.ProbeResult{VMAF: 90, Kbps: 4000, VFFragment: "x"}, nil
	}
	got, err := prefilter.RecommendPrefilter(ctx, prefilter.Options{
		TargetVMAF: 93, Src: "/src/a.yuv", Encoder: "libx264",
		CRFRange: [2]int{18, 40}, NTrials: 100, Probe: probe, Seed: 1,
	})
	if err != nil {
		t.Fatalf("RecommendPrefilter: %v", err)
	}
	if got.NTrials != 5 {
		t.Errorf("NTrials = %d, want the 5 trials that completed before cancellation",
			got.NTrials)
	}
}

// TestResult_jsonSchema pins the emitted key names against the Python
// PrefilterResult.to_dict() shape, since the payload is a user-discoverable
// surface downstream tooling parses.
func TestResult_jsonSchema(t *testing.T) {
	t.Parallel()

	got, err := prefilter.RecommendPrefilter(context.Background(), prefilter.Options{
		TargetVMAF: 93.0, Encoder: "libx264", CRFRange: [2]int{18, 40},
		NTrials: 3, Smoke: true, Seed: 1,
	})
	if err != nil {
		t.Fatalf("RecommendPrefilter: %v", err)
	}
	blob, marshalErr := json.Marshal(got)
	if marshalErr != nil {
		t.Fatalf("marshal result: %v", marshalErr)
	}
	var decoded map[string]any
	if unmarshalErr := json.Unmarshal(blob, &decoded); unmarshalErr != nil {
		t.Fatalf("unmarshal result: %v", unmarshalErr)
	}

	wantKeys := []string{
		"filter_name", "encoder", "target_vmaf", "recommended_crf",
		"recommended_deband", "recommended_vf", "achieved_vmaf",
		"achieved_kbps", "n_trials", "smoke", "probes", "notes",
	}
	for _, key := range wantKeys {
		if _, ok := decoded[key]; !ok {
			t.Errorf("payload is missing the %q key", key)
		}
	}
	if len(decoded) != len(wantKeys) {
		t.Errorf("payload has %d keys, want exactly %d", len(decoded), len(wantKeys))
	}

	probes, ok := decoded["probes"].([]any)
	if !ok || len(probes) == 0 {
		t.Fatalf("probes = %v, want a non-empty array", decoded["probes"])
	}
	probe, ok := probes[0].(map[string]any)
	if !ok {
		t.Fatalf("probe entry = %v, want an object", probes[0])
	}
	for _, key := range []string{
		"trial", "crf", "deband_params", "vf_fragment", "vmaf", "kbps", "objective",
	} {
		if _, has := probe[key]; !has {
			t.Errorf("probe record is missing the %q key", key)
		}
	}
}
