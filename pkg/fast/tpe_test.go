// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/fast/tpe_test.go — table-driven tests for the goptuna-backed TPE search
// that replaces Optuna in the Go port of vmaftune/fast.py.

package fast

import (
	"context"
	"errors"
	"math"
	"sync"
	"testing"
)

// TestRunTPEValidation covers the argument contract.
func TestRunTPEValidation(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		params  TPEParams
		wantErr string
	}{
		{
			name:    "nil predictor rejected",
			params:  TPEParams{CRFLo: 10, CRFHi: 51, NTrials: 5},
			wantErr: "requires a predictor",
		},
		{
			name:    "inverted CRF range rejected",
			params:  TPEParams{Predict: SmokePredictor, CRFLo: 51, CRFHi: 10, NTrials: 5},
			wantErr: "invalid CRF range",
		},
		{
			name:    "zero trial budget rejected",
			params:  TPEParams{Predict: SmokePredictor, CRFLo: 10, CRFHi: 51},
			wantErr: "n_trials must be > 0",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			_, err := RunTPE(context.Background(), tc.params)
			if err == nil {
				t.Fatalf("want error containing %q, got nil", tc.wantErr)
			}
			if !containsStr(err.Error(), tc.wantErr) {
				t.Errorf("error %q does not contain %q", err, tc.wantErr)
			}
		})
	}
}

// TestRunTPEConvergesOnSmokeCurve drives the search against the deterministic
// synthetic curve and asserts it reaches the brute-force global optimum of
// |vmaf(crf) - target| + 1e-4*kbps(crf) over the integer CRF axis.
//
// The reference optima were computed exhaustively from the *Python* smoke
// predictor, so these numbers cross-check the Go curve against fast.py:
//
//	PYTHONPATH=tools/vmaf-tune/src python3 -c "
//	from vmaftune.fast import _smoke_predictor, DEFAULT_CRF_LO, DEFAULT_CRF_HI
//	for target in (95.0, 90.0, 88.0, 75.0):
//	    best = min(((abs((s:=_smoke_predictor(c)).predicted_vmaf-target)
//	                 + 1e-4*s.predicted_kbps), c)
//	               for c in range(DEFAULT_CRF_LO, DEFAULT_CRF_HI+1))
//	    print(target, best)"
//
//	95.0 -> crf 15, vmaf 95.23711249329942, kbps 5300.59966363287
//	90.0 -> crf 20, vmaf 90.35515462202830, kbps 3486.83260599045
//	88.0 -> crf 22, vmaf 88.24093019754717, kbps 2952.12248362229
//	75.0 -> crf 33, vmaf 75.51283314339115, kbps 1203.02608958883
//
// This test and its subtests deliberately do NOT call t.Parallel(): goptuna
// v0.9.0 draws part of its randomness from the process-global math/rand
// source (see the caveat at the top of tpe.go), so concurrent studies perturb
// each other's trial sequences and widen the spread well past the ±1 band
// measured for sequential runs.
func TestRunTPEConvergesOnSmokeCurve(t *testing.T) {
	// convergedBudget is a trial budget at which the search reaches the exact
	// optimum reliably. Measured over 150 sequential repeats per target at
	// both 150 and 300 trials, every run returned the brute-force optimum
	// (CRF 15 / 20 / 22 / 33 respectively, 150 out of 150 each). The default
	// budgets converge to within ±1 but not always exactly, which is what
	// TestRunTPEHitsTheBandAtDefaultBudgets covers.
	const convergedBudget = 150

	tests := []struct {
		name      string
		target    float64
		wantCRF   int
		wantVMAF  float64
		wantKbps  float64
		wantValue float64
	}{
		{
			name: "target 95", target: 95.0, wantCRF: 15,
			wantVMAF: 95.23711249329942, wantKbps: 5300.59966363287,
			wantValue: 0.7671724596627076,
		},
		{
			name: "target 90", target: 90.0, wantCRF: 20,
			wantVMAF: 90.3551546220283, wantKbps: 3486.832605990455,
			wantValue: 0.7038378826273399,
		},
		{
			name: "target 88", target: 88.0, wantCRF: 22,
			wantVMAF: 88.24093019754717, wantKbps: 2952.122483622292,
			wantValue: 0.5361424459094013,
		},
		{
			name: "target 75", target: 75.0, wantCRF: 33,
			wantVMAF: 75.51283314339115, wantKbps: 1203.0260895888348,
			wantValue: 0.6331357523500318,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, err := RunTPE(context.Background(), TPEParams{
				TargetVMAF: tc.target,
				Predict:    SmokePredictor,
				CRFLo:      DefaultCRFLo,
				CRFHi:      DefaultCRFHi,
				NTrials:    convergedBudget,
			})
			if err != nil {
				t.Fatalf("RunTPE: %v", err)
			}
			if got.CompletedTrials != convergedBudget {
				t.Errorf("CompletedTrials = %d, want %d", got.CompletedTrials, convergedBudget)
			}
			if got.Sample.CRF != tc.wantCRF {
				t.Errorf("recommended CRF = %d, want the brute-force optimum %d "+
					"(predicted VMAF %.6f, objective %.6f)",
					got.Sample.CRF, tc.wantCRF, got.Sample.PredictedVMAF, got.BestValue)
			}
			// The reported values must be the Python curve's own numbers,
			// recovered from the trial user attributes without loss.
			if math.Abs(got.Sample.PredictedVMAF-tc.wantVMAF) > 1e-12 {
				t.Errorf("PredictedVMAF = %.17g, want %.17g", got.Sample.PredictedVMAF, tc.wantVMAF)
			}
			if math.Abs(got.Sample.PredictedKbps-tc.wantKbps) > 1e-9 {
				t.Errorf("PredictedKbps = %.17g, want %.17g", got.Sample.PredictedKbps, tc.wantKbps)
			}
			if math.Abs(got.BestValue-tc.wantValue) > 1e-9 {
				t.Errorf("BestValue = %.17g, want %.17g", got.BestValue, tc.wantValue)
			}
		})
	}
}

// TestRunTPEHitsTheBandAtDefaultBudgets is the weaker guard that applies at
// the budgets the CLI actually ships (ProdNTrials / SmokeNTrials). goptuna's
// partially-seeded sampler means the search can settle on a CRF adjacent to
// the optimum there, and rarely two or three steps away, so a single draw is
// not a sound assertion.
//
// The test therefore takes `repeats` draws per case and requires a majority to
// land within ±1 of the brute-force optimum. Measured over 200 sequential
// repeats per cell the within-±1 rate was ~99-100 %, so a majority of 5 is a
// guard that essentially never fires by chance yet still fails loudly if the
// search regresses. Every individual draw must additionally never beat the
// brute-force optimum — that would mean the objective no longer matches
// fast.py — and must report self-consistent numbers.
//
// Not parallel, for the reason given on TestRunTPEConvergesOnSmokeCurve.
func TestRunTPEHitsTheBandAtDefaultBudgets(t *testing.T) {
	const (
		crfTolerance = 1
		repeats      = 5
		wantWithin   = 3 // majority of repeats
	)

	tests := []struct {
		name      string
		target    float64
		nTrials   int
		wantCRF   int
		wantValue float64
	}{
		{
			name: "target 95 at the production budget", target: 95.0, nTrials: ProdNTrials,
			wantCRF: 15, wantValue: 0.7671724596627076,
		},
		{
			name: "target 90 at the smoke budget", target: 90.0, nTrials: SmokeNTrials,
			wantCRF: 20, wantValue: 0.7038378826273399,
		},
		{
			name: "target 88 at the smoke budget", target: 88.0, nTrials: SmokeNTrials,
			wantCRF: 22, wantValue: 0.5361424459094013,
		},
		{
			name: "target 75 at the smoke budget", target: 75.0, nTrials: SmokeNTrials,
			wantCRF: 33, wantValue: 0.6331357523500318,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			within := 0
			seen := make([]int, 0, repeats)

			for i := 0; i < repeats; i++ {
				got, err := RunTPE(context.Background(), TPEParams{
					TargetVMAF: tc.target,
					Predict:    SmokePredictor,
					CRFLo:      DefaultCRFLo,
					CRFHi:      DefaultCRFHi,
					NTrials:    tc.nTrials,
				})
				if err != nil {
					t.Fatalf("RunTPE: %v", err)
				}
				if got.CompletedTrials != tc.nTrials {
					t.Errorf("CompletedTrials = %d, want %d", got.CompletedTrials, tc.nTrials)
				}
				seen = append(seen, got.Sample.CRF)
				if delta := got.Sample.CRF - tc.wantCRF; delta >= -crfTolerance && delta <= crfTolerance {
					within++
				}
				if got.BestValue < tc.wantValue-1e-9 {
					t.Errorf("BestValue = %.17g beats the brute-force optimum %.17g — "+
						"the objective no longer matches fast.py", got.BestValue, tc.wantValue)
				}
				// Whatever CRF wins, the reported numbers must be the
				// predictor's own, recovered from the user attributes without
				// loss, and the objective must be the one this port computes
				// for that sample.
				want, _ := SmokePredictor(got.Sample.CRF)
				if got.Sample.PredictedVMAF != want.PredictedVMAF {
					t.Errorf("user-attr round-trip lost precision: %.17g vs %.17g",
						got.Sample.PredictedVMAF, want.PredictedVMAF)
				}
				if got.Sample.PredictedKbps != want.PredictedKbps {
					t.Errorf("user-attr round-trip lost precision: %.17g vs %.17g",
						got.Sample.PredictedKbps, want.PredictedKbps)
				}
				if recomputed := objectiveValue(want, tc.target); math.Abs(got.BestValue-recomputed) > 1e-12 {
					t.Errorf("BestValue %.17g does not match objectiveValue %.17g",
						got.BestValue, recomputed)
				}
			}

			if within < wantWithin {
				t.Errorf("only %d of %d runs landed within ±%d of the optimum %d; got %v",
					within, repeats, crfTolerance, tc.wantCRF, seen)
			}
		})
	}
}

// TestRunTPERepeatRunsAgree is the stability guard for the seeding caveat
// documented at the top of tpe.go: goptuna v0.9.0 leaks part of its randomness
// to the process-global math/rand source, so repeat runs are NOT bit-identical
// the way Optuna's TPESampler(seed=0) guarantees.
//
// What must hold is that, given a budget large enough for the search to
// converge, repeat runs still agree exactly. Measured over 150 sequential
// repeats at 150 trials, every run returned the same CRF for every target, so
// the seeding leak costs reproducibility only where the search has not fully
// converged — it does not make the tuner erratic.
//
// Not parallel, for the reason given on TestRunTPEConvergesOnSmokeCurve.
func TestRunTPERepeatRunsAgree(t *testing.T) {
	const (
		convergedBudget = 150
		repeats         = 8
	)

	run := func() TPEResult {
		got, err := RunTPE(context.Background(), TPEParams{
			TargetVMAF: 88.0,
			Predict:    SmokePredictor,
			CRFLo:      DefaultCRFLo,
			CRFHi:      DefaultCRFHi,
			NTrials:    convergedBudget,
		})
		if err != nil {
			t.Fatalf("RunTPE: %v", err)
		}
		return got
	}

	first := run()
	for i := 1; i < repeats; i++ {
		got := run()
		if got.Sample != first.Sample || got.BestValue != first.BestValue {
			t.Errorf("run %d disagreed with the first at a converged budget: %+v vs %+v",
				i, got.Sample, first.Sample)
		}
	}
}

// TestRunTPESingletonRange checks a degenerate one-value search space.
func TestRunTPESingletonRange(t *testing.T) {
	t.Parallel()

	got, err := RunTPE(context.Background(), TPEParams{
		TargetVMAF: 90.0,
		Predict:    SmokePredictor,
		CRFLo:      23,
		CRFHi:      23,
		NTrials:    3,
	})
	if err != nil {
		t.Fatalf("RunTPE: %v", err)
	}
	if got.Sample.CRF != 23 {
		t.Errorf("CRF = %d, want 23", got.Sample.CRF)
	}
}

// TestRunTPEPredictorErrorAborts verifies a failing probe encode surfaces
// rather than silently degrading the recommendation.
func TestRunTPEPredictorErrorAborts(t *testing.T) {
	t.Parallel()

	sentinel := errors.New("probe encode failed")
	_, err := RunTPE(context.Background(), TPEParams{
		TargetVMAF: 90.0,
		Predict: func(int) (TrialSample, error) {
			return TrialSample{}, sentinel
		},
		CRFLo:   10,
		CRFHi:   51,
		NTrials: 5,
	})
	if err == nil {
		t.Fatal("want an error when every trial fails, got nil")
	}
	if !containsStr(err.Error(), "probe encode failed") {
		t.Errorf("error %q should carry the predictor failure", err)
	}
}

// TestRunTPECancelledContext verifies that an already-cancelled context stops
// the study before any trial runs, and that the "no completed trial" case is
// reported rather than silently returning a zero recommendation.
func TestRunTPECancelledContext(t *testing.T) {
	t.Parallel()

	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	var calls int
	_, err := RunTPE(ctx, TPEParams{
		TargetVMAF: 90.0,
		Predict: func(crf int) (TrialSample, error) {
			calls++
			return SmokePredictor(crf)
		},
		CRFLo:   10,
		CRFHi:   51,
		NTrials: 10,
	})
	if calls != 0 {
		t.Errorf("predictor ran %d times under a cancelled context, want 0", calls)
	}
	if err == nil {
		t.Fatal("want an error when the study ran no trials, got nil")
	}
}

// TestRunTPETimeBudgetStopsScheduling verifies the soft wall-clock cap ends
// the search without failing it: goptuna checks the deadline before each
// trial, so an in-flight probe is never interrupted midway.
func TestRunTPETimeBudgetStopsScheduling(t *testing.T) {
	t.Parallel()

	var (
		mu    sync.Mutex
		calls int
	)
	got, err := RunTPE(context.Background(), TPEParams{
		TargetVMAF: 90.0,
		Predict: func(crf int) (TrialSample, error) {
			mu.Lock()
			calls++
			mu.Unlock()
			return SmokePredictor(crf)
		},
		CRFLo: 10,
		CRFHi: 51,
		// A budget generous enough that all 5 trials complete: the point of
		// the test is that setting a budget does not turn a normal finish
		// into an error.
		NTrials:           5,
		TimeBudgetSeconds: 60,
	})
	if err != nil {
		t.Fatalf("RunTPE with a time budget: %v", err)
	}
	if calls != 5 {
		t.Errorf("predictor ran %d times, want 5", calls)
	}
	if got.CompletedTrials != 5 {
		t.Errorf("CompletedTrials = %d, want 5", got.CompletedTrials)
	}
}

// TestFormatParseAttrRoundTrip pins the user-attribute encoding, including
// the NaN fallback that mirrors Python's user_attrs.get(..., float("nan")).
func TestFormatParseAttrRoundTrip(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		value   float64
		wantNaN bool
	}{
		{name: "integral", value: 90},
		{name: "fractional", value: 88.123456789012345},
		{name: "tiny", value: 1e-300},
		{name: "huge", value: 1e300},
		{name: "negative", value: -12.5},
		{name: "zero", value: 0},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := parseAttr(formatAttr(tc.value)); got != tc.value {
				t.Errorf("round-trip %v -> %q -> %v", tc.value, formatAttr(tc.value), got)
			}
		})
	}

	if !math.IsNaN(parseAttr("")) {
		t.Error("missing attribute should parse to NaN")
	}
	if !math.IsNaN(parseAttr("not-a-number-at-all")) {
		t.Error("malformed attribute should parse to NaN")
	}
}

// containsStr is a tiny substring helper so the tests stay dependency-free.
func containsStr(haystack, needle string) bool {
	if needle == "" {
		return true
	}
	for i := 0; i+len(needle) <= len(haystack); i++ {
		if haystack[i:i+len(needle)] == needle {
			return true
		}
	}
	return false
}
