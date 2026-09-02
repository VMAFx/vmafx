// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/fast/fast_test.go — table-driven tests for the Recommend entry point
// and the smoke predictor, both ported from
// tools/vmaf-tune/src/vmaftune/fast.py.

package fast

import (
	"context"
	"errors"
	"math"
	"testing"
)

// TestSmokePredictor pins the synthetic CRF->VMAF curve against the Python
// _smoke_predictor. Reference values:
//
//	PYTHONPATH=tools/vmaf-tune/src python3 -c "
//	from vmaftune.fast import _smoke_predictor
//	for c in (10, 15, 20, 22, 33, 51): print(c, _smoke_predictor(c))"
func TestSmokePredictor(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name     string
		crf      int
		wantVMAF float64
		wantKbps float64
	}{
		{name: "range floor is ~99 VMAF", crf: 10, wantVMAF: 99.0, wantKbps: 8080.0},
		{name: "crf 15", crf: 15, wantVMAF: 95.23711249329942, wantKbps: 5300.59966363287},
		{name: "crf 20", crf: 20, wantVMAF: 90.3551546220283, wantKbps: 3486.832605990455},
		{name: "crf 22", crf: 22, wantVMAF: 88.24093019754717, wantKbps: 2952.122483622292},
		{name: "crf 33", crf: 33, wantVMAF: 75.51283314339115, wantKbps: 1203.0260895888348},
		{name: "range ceiling is ~52 VMAF", crf: 51, wantVMAF: 52.0, wantKbps: 321.579067378548},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := SmokePredictor(tc.crf)
			if err != nil {
				t.Fatalf("SmokePredictor(%d): %v", tc.crf, err)
			}
			if got.CRF != tc.crf {
				t.Errorf("CRF = %d, want %d", got.CRF, tc.crf)
			}
			if math.Abs(got.PredictedVMAF-tc.wantVMAF) > 1e-9 {
				t.Errorf("PredictedVMAF = %.17g, want %.17g", got.PredictedVMAF, tc.wantVMAF)
			}
			if math.Abs(got.PredictedKbps-tc.wantKbps) > 1e-9 {
				t.Errorf("PredictedKbps = %.17g, want %.17g", got.PredictedKbps, tc.wantKbps)
			}
		})
	}
}

// TestSmokePredictorIsMonotone guards the curve's defining property: higher
// CRF means lower VMAF and lower bitrate. A non-monotone curve would break
// the bisect-style reasoning the whole fast path rests on.
func TestSmokePredictorIsMonotone(t *testing.T) {
	t.Parallel()

	prev, err := SmokePredictor(DefaultCRFLo)
	if err != nil {
		t.Fatalf("SmokePredictor: %v", err)
	}
	for crf := DefaultCRFLo + 1; crf <= DefaultCRFHi; crf++ {
		got, err := SmokePredictor(crf)
		if err != nil {
			t.Fatalf("SmokePredictor(%d): %v", crf, err)
		}
		if got.PredictedVMAF >= prev.PredictedVMAF {
			t.Errorf("VMAF not decreasing at CRF %d: %v >= %v",
				crf, got.PredictedVMAF, prev.PredictedVMAF)
		}
		if got.PredictedKbps >= prev.PredictedKbps {
			t.Errorf("bitrate not decreasing at CRF %d: %v >= %v",
				crf, got.PredictedKbps, prev.PredictedKbps)
		}
		prev = got
	}
}

// TestObjectiveValue pins the TPE objective formula.
func TestObjectiveValue(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		sample TrialSample
		target float64
		want   float64
	}{
		{
			name:   "exact hit costs only the bitrate term",
			sample: TrialSample{PredictedVMAF: 90, PredictedKbps: 1000},
			target: 90,
			want:   0.1,
		},
		{
			name:   "overshoot is penalised symmetrically",
			sample: TrialSample{PredictedVMAF: 92, PredictedKbps: 0},
			target: 90,
			want:   2.0,
		},
		{
			name:   "undershoot is penalised symmetrically",
			sample: TrialSample{PredictedVMAF: 88, PredictedKbps: 0},
			target: 90,
			want:   2.0,
		},
		{
			name:   "ties break toward the lower bitrate",
			sample: TrialSample{PredictedVMAF: 90, PredictedKbps: 2000},
			target: 90,
			want:   0.2,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := objectiveValue(tc.sample, tc.target); math.Abs(got-tc.want) > 1e-12 {
				t.Errorf("objectiveValue = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestParamsDefaults covers the resolution of the zero-value knobs.
func TestParamsDefaults(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name          string
		params        Params
		wantNTrials   int
		wantTolerance float64
		wantBudget    int
		wantCRFLo     int
		wantCRFHi     int
	}{
		{
			name:          "production zero value",
			params:        Params{},
			wantNTrials:   ProdNTrials,
			wantTolerance: DefaultProxyTolerance,
			wantBudget:    DefaultTimeBudgetSeconds,
			wantCRFLo:     DefaultCRFLo,
			wantCRFHi:     DefaultCRFHi,
		},
		{
			name:          "smoke zero value picks the smoke budget",
			params:        Params{Smoke: true},
			wantNTrials:   SmokeNTrials,
			wantTolerance: DefaultProxyTolerance,
			wantBudget:    DefaultTimeBudgetSeconds,
			wantCRFLo:     DefaultCRFLo,
			wantCRFHi:     DefaultCRFHi,
		},
		{
			name: "explicit values win",
			params: Params{
				NTrials: 7, ProxyTolerance: 3.5, TimeBudgetSeconds: 42,
				CRFLo: 18, CRFHi: 30, Smoke: true,
			},
			wantNTrials:   7,
			wantTolerance: 3.5,
			wantBudget:    42,
			wantCRFLo:     18,
			wantCRFHi:     30,
		},
		{
			name:          "a single explicit bound disables the range default",
			params:        Params{CRFHi: 40},
			wantNTrials:   ProdNTrials,
			wantTolerance: DefaultProxyTolerance,
			wantBudget:    DefaultTimeBudgetSeconds,
			wantCRFLo:     0,
			wantCRFHi:     40,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := tc.params.effectiveNTrials(); got != tc.wantNTrials {
				t.Errorf("effectiveNTrials = %d, want %d", got, tc.wantNTrials)
			}
			if got := tc.params.effectiveTolerance(); got != tc.wantTolerance {
				t.Errorf("effectiveTolerance = %v, want %v", got, tc.wantTolerance)
			}
			if got := tc.params.effectiveTimeBudget(); got != tc.wantBudget {
				t.Errorf("effectiveTimeBudget = %d, want %d", got, tc.wantBudget)
			}
			lo, hi := tc.params.effectiveCRFRange()
			if lo != tc.wantCRFLo || hi != tc.wantCRFHi {
				t.Errorf("effectiveCRFRange = (%d, %d), want (%d, %d)",
					lo, hi, tc.wantCRFLo, tc.wantCRFHi)
			}
		})
	}
}

// TestRecommendSmoke drives the whole smoke flow.
//
// Not parallel: this test asserts on a TPE outcome, and goptuna v0.9.0 draws
// part of its randomness from the process-global math/rand source, so
// concurrent studies perturb each other (see the caveat at the top of tpe.go).
func TestRecommendSmoke(t *testing.T) {
	got, err := Recommend(context.Background(), Params{
		TargetVMAF: 90.0,
		Encoder:    "libx264",
		Smoke:      true,
	})
	if err != nil {
		t.Fatalf("Recommend: %v", err)
	}
	if !got.Smoke {
		t.Error("Smoke = false, want true")
	}
	if got.NTrials != SmokeNTrials {
		t.Errorf("NTrials = %d, want %d", got.NTrials, SmokeNTrials)
	}
	if got.Encoder != "libx264" {
		t.Errorf("Encoder = %q, want libx264", got.Encoder)
	}
	if got.TargetVMAF != 90.0 {
		t.Errorf("TargetVMAF = %v, want 90", got.TargetVMAF)
	}
	if got.VerifyVMAF != nil || got.ProxyVerifyGap != nil {
		t.Errorf("smoke mode must leave verify fields nil; got %v / %v",
			got.VerifyVMAF, got.ProxyVerifyGap)
	}
	if got.Notes != smokeNotes {
		t.Errorf("Notes = %q, want the fixed smoke note", got.Notes)
	}
	if got.ScoreBackend != "" {
		t.Errorf("smoke mode must not set score_backend; got %q", got.ScoreBackend)
	}
	// This test owns the payload contract, not the search quality — the
	// convergence guards live in tpe_test.go, where the tolerance is stated
	// against the measured distribution. Here it is enough that the
	// recommendation is a real point on the curve.
	if got.RecommendedCRF < DefaultCRFLo || got.RecommendedCRF > DefaultCRFHi {
		t.Errorf("RecommendedCRF = %d, outside the search range [%d, %d]",
			got.RecommendedCRF, DefaultCRFLo, DefaultCRFHi)
	}
	want, _ := SmokePredictor(got.RecommendedCRF)
	if got.PredictedVMAF != want.PredictedVMAF || got.PredictedKbps != want.PredictedKbps {
		t.Errorf("payload does not match the smoke curve at CRF %d: got (%v, %v), want (%v, %v)",
			got.RecommendedCRF, got.PredictedVMAF, got.PredictedKbps,
			want.PredictedVMAF, want.PredictedKbps)
	}
}

// TestRecommendValidation covers the argument contract on both paths.
func TestRecommendValidation(t *testing.T) {
	t.Parallel()

	okVerify := func(context.Context, string, int) (float64, error) { return 90.0, nil }

	tests := []struct {
		name    string
		params  Params
		wantErr string
		wantIs  error
	}{
		{
			name:    "inverted CRF range rejected",
			params:  Params{TargetVMAF: 90, Smoke: true, CRFLo: 40, CRFHi: 20},
			wantErr: "invalid CRF range",
		},
		{
			name:    "negative time budget rejected",
			params:  Params{TargetVMAF: 90, Smoke: true, TimeBudgetSeconds: -1},
			wantErr: "time budget must be > 0",
		},
		{
			name:    "production without a source rejected",
			params:  Params{TargetVMAF: 90, Predict: SmokePredictor, Verify: okVerify},
			wantErr: "requires a source path",
			wantIs:  ErrSrcRequired,
		},
		{
			name:    "production without a predictor rejected",
			params:  Params{TargetVMAF: 90, Src: "clip.yuv", Verify: okVerify},
			wantErr: "requires a CRF predictor",
		},
		{
			name:    "production without a verify pass rejected",
			params:  Params{TargetVMAF: 90, Src: "clip.yuv", Predict: SmokePredictor},
			wantErr: "requires a verify pass",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			_, err := Recommend(context.Background(), tc.params)
			if err == nil {
				t.Fatalf("want error containing %q, got nil", tc.wantErr)
			}
			if !containsStr(err.Error(), tc.wantErr) {
				t.Errorf("error %q does not contain %q", err, tc.wantErr)
			}
			if tc.wantIs != nil && !errors.Is(err, tc.wantIs) {
				t.Errorf("error %v is not %v", err, tc.wantIs)
			}
		})
	}
}

// TestRecommendProduction drives the production flow with injected seams and
// checks the verify-gap accounting, notes text and out-of-distribution flag.
func TestRecommendProduction(t *testing.T) {
	t.Parallel()

	// A flat predictor makes the TPE outcome irrelevant: every CRF returns
	// the same proposal, so the assertions below are about the gap
	// bookkeeping rather than the search.
	flat := func(crf int) (TrialSample, error) {
		return TrialSample{CRF: crf, PredictedVMAF: 90.0, PredictedKbps: 1000.0}, nil
	}

	tests := []struct {
		name          string
		verifyScore   float64
		tolerance     float64
		wantGap       float64
		wantOODInNote bool
	}{
		{
			name: "gap inside tolerance", verifyScore: 89.5, tolerance: 1.5,
			wantGap: 0.5, wantOODInNote: false,
		},
		{
			name: "gap exactly at tolerance is not flagged", verifyScore: 88.5, tolerance: 1.5,
			wantGap: 1.5, wantOODInNote: false,
		},
		{
			name: "gap beyond tolerance is flagged", verifyScore: 85.0, tolerance: 1.5,
			wantGap: 5.0, wantOODInNote: true,
		},
		{
			name: "verify above the proxy is an absolute gap", verifyScore: 95.0, tolerance: 1.5,
			wantGap: 5.0, wantOODInNote: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			var verifyCalls int
			var verifiedCRF int
			got, err := Recommend(context.Background(), Params{
				Src:            "clip.yuv",
				TargetVMAF:     90.0,
				Encoder:        "libx264",
				NTrials:        5,
				Predict:        flat,
				ProxyTolerance: tc.tolerance,
				Verify: func(_ context.Context, _ string, crf int) (float64, error) {
					verifyCalls++
					verifiedCRF = crf
					return tc.verifyScore, nil
				},
			})
			if err != nil {
				t.Fatalf("Recommend: %v", err)
			}

			// ADR-0304: exactly one verify pass, at the recommended CRF.
			if verifyCalls != 1 {
				t.Errorf("verify ran %d times, want exactly 1", verifyCalls)
			}
			if verifiedCRF != got.RecommendedCRF {
				t.Errorf("verify ran at CRF %d but the payload recommends %d",
					verifiedCRF, got.RecommendedCRF)
			}
			if got.Smoke {
				t.Error("Smoke = true in production mode")
			}
			if got.VerifyVMAF == nil || *got.VerifyVMAF != tc.verifyScore {
				t.Errorf("VerifyVMAF = %v, want %v", got.VerifyVMAF, tc.verifyScore)
			}
			if got.ProxyVerifyGap == nil || math.Abs(*got.ProxyVerifyGap-tc.wantGap) > 1e-9 {
				t.Errorf("ProxyVerifyGap = %v, want %v", got.ProxyVerifyGap, tc.wantGap)
			}
			if !containsStr(got.Notes, "production: TPE over 5 trials") {
				t.Errorf("Notes %q missing the production preamble", got.Notes)
			}
			flagged := containsStr(got.Notes, "FLAG: proxy/verify gap exceeds tolerance")
			if flagged != tc.wantOODInNote {
				t.Errorf("out-of-distribution flag in notes = %v, want %v (notes: %q)",
					flagged, tc.wantOODInNote, got.Notes)
			}
		})
	}
}

// TestRecommendVerifyFailurePropagates makes sure a broken verify pass is
// surfaced rather than downgraded to a proxy-only recommendation — the
// ADR-0304 invariant that the proxy alone never wins.
func TestRecommendVerifyFailurePropagates(t *testing.T) {
	t.Parallel()

	sentinel := errors.New("ffmpeg exploded")
	_, err := Recommend(context.Background(), Params{
		Src:        "clip.yuv",
		TargetVMAF: 90.0,
		Encoder:    "libx264",
		NTrials:    3,
		Predict:    SmokePredictor,
		Verify: func(context.Context, string, int) (float64, error) {
			return 0, sentinel
		},
	})
	if !errors.Is(err, sentinel) {
		t.Fatalf("want the verify error to propagate, got %v", err)
	}
}

// TestRecommendSmokeAcceptsPredictorOverride checks the smoke path honours an
// injected predictor, the seam the Python `predictor=` keyword provides.
//
// Not parallel, for the reason given on TestRecommendSmoke.
func TestRecommendSmokeAcceptsPredictorOverride(t *testing.T) {
	got, err := Recommend(context.Background(), Params{
		TargetVMAF: 50.0,
		Encoder:    "libx265",
		Smoke:      true,
		NTrials:    12,
		Predict: func(crf int) (TrialSample, error) {
			// A curve whose optimum sits at the top of the range.
			return TrialSample{
				CRF:           crf,
				PredictedVMAF: 50.0 + float64(crf-DefaultCRFHi),
				PredictedKbps: 10.0,
			}, nil
		},
	})
	if err != nil {
		t.Fatalf("Recommend: %v", err)
	}
	// The injected curve peaks at the top of the range, the opposite end from
	// the built-in smoke curve's optimum for this target — so a recommendation
	// up here proves the override was actually used. The ±2 band absorbs the
	// goptuna seeding caveat documented in tpe.go at this small budget.
	if got.RecommendedCRF < DefaultCRFHi-2 {
		t.Errorf("RecommendedCRF = %d, want the injected curve's optimum near %d",
			got.RecommendedCRF, DefaultCRFHi)
	}
	if got.NTrials != 12 {
		t.Errorf("NTrials = %d, want 12", got.NTrials)
	}
}
