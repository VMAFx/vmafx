// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package fast is the Go port of the Phase A.5 fast-path
// (tools/vmaf-tune/src/vmaftune/fast.py) — proxy + Bayesian + GPU-verify
// recommend, documented in ADR-0276 (fast path) and ADR-0304 (production
// wiring).
//
// The flow is:
//
//  1. TPE search over the integer CRF axis. The objective is
//     |predicted_vmaf - target| + lambda*predicted_kbps so ties break toward
//     lower bitrate. Default budget is 30 trials (production) or
//     SmokeNTrials (smoke). The Python original drives Optuna; this port
//     drives github.com/c-bata/goptuna, a Go implementation of the same TPE
//     sampler, seeded identically (seed 0).
//  2. Proxy scoring via the production fr_regressor_v2 ONNX session. Each TPE
//     trial encodes a short sample chunk, extracts the canonical-6 features,
//     and predicts VMAF in microseconds.
//  3. A single GPU verify pass at the end — one real ffmpeg encode + libvmaf
//     score at the recommended CRF. This is mandatory; the proxy alone never
//     wins (ADR-0304 invariant). The verify score is authoritative; the proxy
//     score is a diagnostic.
//
// Smoke mode keeps the synthetic CRF->VMAF curve from the ADR-0276 scaffold so
// CI on hosts without an ONNX runtime or a GPU still exercises the search-loop
// wiring end to end. The slow Phase A grid path stays canonical and untouched
// (ADR-0237 contract).
//
// # Port status
//
// Step 2's ONNX inference is NOT reachable from Go today: fr_regressor_v2 is a
// two-named-input graph ("features" [N,6] + "codec" [N,14]) and the only ONNX
// seam in the Go tree (pkg/ai.Registry.Infer -> vmafx-ort-runner) accepts a
// single flat input vector. See proxy.go for the full analysis; ErrProxyPortsUnsupported
// is what production mode fails with. Everything else — TPE, the probe-encode
// + canonical-6 extraction pipeline, the verify pass, the JSON schema and the
// exit-code contract — is ported and exercised by tests.
package fast

import (
	"context"
	"errors"
	"fmt"
	"math"
)

// DefaultCRFLo is the default lower bound of the x264 CRF search range.
// Other codecs override it through the adapter once the production loop wires
// the codec-adapter registry.
const DefaultCRFLo = 10

// DefaultCRFHi is the default upper bound of the x264 CRF search range.
const DefaultCRFHi = 51

// SampleChunkSeconds is the sample-chunk duration used for proxy-grade
// probe encodes in the production loop.
const SampleChunkSeconds = 5.0

// SmokeNTrials is the trial budget synthesised in smoke mode so the TPE
// wiring is exercised end to end. Matches the speedup-model entry in
// Research-0060.
const SmokeNTrials = 50

// ProdNTrials is the production default — TPE converges in 30-50 trials on a
// single integer CRF axis (Research-0076 §1).
const ProdNTrials = 30

// DefaultProxyTolerance is the default proxy/verify gap tolerance. When the
// GPU verify pass disagrees with the proxy by more than this many VMAF
// points, the recommendation is flagged out-of-distribution and the operator
// is expected to fall back to the slow Phase A grid (ADR-0276 fallback
// contract; Research-0076 §2).
const DefaultProxyTolerance = 1.5

// DefaultTimeBudgetSeconds is the soft wall-clock cap on the TPE loop.
const DefaultTimeBudgetSeconds = 300

// bitrateWeight scales the bitrate term of the TPE objective. It is small
// relative to the quality term so the optimiser primarily hits the target;
// ties (multiple CRFs at the target) break toward the lower-bitrate option.
const bitrateWeight = 1.0e-4

// TrialSample is one (crf, predicted_vmaf, predicted_kbps) proposal.
//
// Production: filled by encoding a short chunk, extracting canonical-6, and
// running fr_regressor_v2 over the feature vector + codec one-hot.
// Smoke mode: synthesised by a deterministic mock.
type TrialSample struct {
	CRF           int
	PredictedVMAF float64
	PredictedKbps float64
}

// Predictor maps a candidate CRF to a TrialSample. Production builds one from
// the probe-encode pipeline plus the ONNX proxy; smoke mode uses
// SmokePredictor; tests inject their own.
type Predictor func(crf int) (TrialSample, error)

// RecommendResult is the outcome of one Recommend call, and the JSON payload
// the `vmafx-tune-go fast` subcommand emits.
//
// The struct fields are declared in alphabetical order of their JSON tag so
// encoding/json reproduces the Python CLI's
// json.dumps(result, indent=2, sort_keys=True) byte-for-byte. Do not reorder
// them: the ordering is the schema.
//
// VerifyVMAF and ProxyVerifyGap are populated when the production loop runs
// the verify pass; smoke mode leaves them nil, which marshals to JSON null
// exactly like the Python None.
//
// ScoreBackend is set by the CLI in production mode only, mirroring the
// Python `result["score_backend"] = backend_for_payload` assignment; it is
// omitted in smoke mode.
type RecommendResult struct {
	Encoder        string   `json:"encoder"`
	NTrials        int      `json:"n_trials"`
	Notes          string   `json:"notes"`
	PredictedKbps  float64  `json:"predicted_kbps"`
	PredictedVMAF  float64  `json:"predicted_vmaf"`
	ProxyVerifyGap *float64 `json:"proxy_verify_gap"`
	RecommendedCRF int      `json:"recommended_crf"`
	ScoreBackend   string   `json:"score_backend,omitempty"`
	Smoke          bool     `json:"smoke"`
	TargetVMAF     float64  `json:"target_vmaf"`
	VerifyVMAF     *float64 `json:"verify_vmaf"`
}

// smokeNotes is the fixed note string smoke-mode results carry.
const smokeNotes = "smoke mode — synthetic predictor; no ffmpeg / ONNX / GPU. " +
	"See ADR-0276 + ADR-0304 + Research-0076 for the production path."

// ErrSrcRequired is returned when production mode is asked to run without a
// source path.
var ErrSrcRequired = errors.New(
	"vmafx-tune fast production mode requires a source path; use --smoke for the synthetic pipeline")

// SmokePredictor is a deterministic mock that mimics x264's monotone
// CRF->VMAF curve: higher CRF -> lower VMAF, lower bitrate. The shape is
// loosely calibrated against published x264 medium-preset behaviour on 1080p
// material: VMAF ~= 100 at CRF 10, VMAF ~= 50 at CRF 51, with a smooth taper.
// Smoke mode uses it so the optimiser has a sensible objective without real
// weights.
func SmokePredictor(crf int) (TrialSample, error) {
	span := DefaultCRFHi - DefaultCRFLo
	if span < 1 {
		span = 1
	}
	crfNorm := float64(crf-DefaultCRFLo) / float64(span)
	// VMAF curve: smooth taper from ~99 at CRF 10 to ~52 at CRF 51.
	vmaf := 99.0 - 47.0*math.Pow(crfNorm, 1.2)
	// Bitrate curve: exponential decay (typical for x264).
	kbps := 8000.0*math.Exp(-3.5*crfNorm) + 80.0
	return TrialSample{CRF: crf, PredictedVMAF: vmaf, PredictedKbps: kbps}, nil
}

// objectiveValue is the TPE objective: |vmaf - target| + lambda*kbps.
func objectiveValue(sample TrialSample, targetVMAF float64) float64 {
	return math.Abs(sample.PredictedVMAF-targetVMAF) + bitrateWeight*sample.PredictedKbps
}

// VerifyFunc runs ONE real encode + libvmaf score at the recommended CRF and
// returns the measured VMAF. The verify pass is mandatory in production —
// the proxy alone never wins (ADR-0304 invariant).
type VerifyFunc func(ctx context.Context, encoder string, crf int) (float64, error)

// Params configures one Recommend call.
type Params struct {
	// Src is the source video path. Only meaningful in production mode;
	// smoke mode ignores it.
	Src string

	// TargetVMAF is the quality target on the standard VMAF [0, 100] scale.
	TargetVMAF float64

	// Encoder is the codec adapter name. Production requires it to be
	// resolvable against the proxy model's encoder vocabulary.
	Encoder string

	// CRFLo / CRFHi bound the inclusive integer CRF search range.
	CRFLo int
	CRFHi int

	// NTrials is the TPE trial budget. 0 selects ProdNTrials in production
	// mode and SmokeNTrials in smoke mode.
	NTrials int

	// TimeBudgetSeconds is a soft wall-clock cap on the TPE loop. TPE stops
	// scheduling new trials after the budget elapses; an in-flight trial is
	// allowed to finish so probe encodes are not interrupted midway.
	// 0 selects DefaultTimeBudgetSeconds; negative is rejected.
	TimeBudgetSeconds int

	// Smoke selects the deterministic synthetic curve — no ffmpeg, no ONNX,
	// no GPU verify.
	Smoke bool

	// Predict overrides the crf -> TrialSample callable. Production callers
	// leave it nil and the caller-supplied pipeline builds one. The verify
	// pass still runs unless Smoke is set.
	Predict Predictor

	// Verify runs the mandatory single real encode + score pass. Required in
	// production mode.
	Verify VerifyFunc

	// ProxyTolerance is the VMAF gap above which the result is flagged
	// out-of-distribution. 0 selects DefaultProxyTolerance.
	ProxyTolerance float64
}

// effectiveNTrials resolves the trial budget.
func (p Params) effectiveNTrials() int {
	if p.NTrials > 0 {
		return p.NTrials
	}
	if p.Smoke {
		return SmokeNTrials
	}
	return ProdNTrials
}

// effectiveTolerance resolves the proxy/verify tolerance.
func (p Params) effectiveTolerance() float64 {
	if p.ProxyTolerance > 0 {
		return p.ProxyTolerance
	}
	return DefaultProxyTolerance
}

// effectiveTimeBudget resolves the soft wall-clock cap.
func (p Params) effectiveTimeBudget() int {
	if p.TimeBudgetSeconds == 0 {
		return DefaultTimeBudgetSeconds
	}
	return p.TimeBudgetSeconds
}

// effectiveCRFRange resolves the search window, defaulting either bound to
// the x264 range when left at zero.
func (p Params) effectiveCRFRange() (int, int) {
	lo, hi := p.CRFLo, p.CRFHi
	if lo == 0 && hi == 0 {
		return DefaultCRFLo, DefaultCRFHi
	}
	return lo, hi
}

// Recommend returns a fast-path CRF recommendation for Src at TargetVMAF.
//
// Production flow (Smoke == false):
//
//  1. Run the TPE search over Params.Predict (which the caller wires to the
//     probe-encode + proxy pipeline).
//  2. Run Params.Verify for a single real encode+score pass at the chosen CRF
//     (proxy alone never wins).
//  3. Report the proxy score, the verify score, and the absolute gap; flag
//     out-of-distribution when the gap exceeds the tolerance.
//
// Smoke flow (Smoke == true): synthetic CRF->VMAF curve, no proxy, no encode,
// no verify.
func Recommend(ctx context.Context, params Params) (RecommendResult, error) {
	crfLo, crfHi := params.effectiveCRFRange()
	if crfLo > crfHi {
		return RecommendResult{}, fmt.Errorf("invalid CRF range [%d, %d]", crfLo, crfHi)
	}
	if params.TimeBudgetSeconds < 0 {
		return RecommendResult{}, fmt.Errorf(
			"time budget must be > 0 when set; got %d", params.TimeBudgetSeconds)
	}
	nTrials := params.effectiveNTrials()

	if params.Smoke {
		predict := params.Predict
		if predict == nil {
			predict = SmokePredictor
		}
		best, err := RunTPE(ctx, TPEParams{
			TargetVMAF:        params.TargetVMAF,
			Predict:           predict,
			CRFLo:             crfLo,
			CRFHi:             crfHi,
			NTrials:           nTrials,
			TimeBudgetSeconds: params.effectiveTimeBudget(),
		})
		if err != nil {
			return RecommendResult{}, err
		}
		return RecommendResult{
			Encoder:        params.Encoder,
			NTrials:        best.CompletedTrials,
			Notes:          smokeNotes,
			PredictedKbps:  best.Sample.PredictedKbps,
			PredictedVMAF:  best.Sample.PredictedVMAF,
			ProxyVerifyGap: nil,
			RecommendedCRF: best.Sample.CRF,
			Smoke:          true,
			TargetVMAF:     params.TargetVMAF,
			VerifyVMAF:     nil,
		}, nil
	}

	// Production path.
	if params.Src == "" {
		return RecommendResult{}, ErrSrcRequired
	}
	if params.Predict == nil {
		return RecommendResult{}, errors.New(
			"fast: production mode requires a CRF predictor (proxy pipeline)")
	}
	if params.Verify == nil {
		return RecommendResult{}, errors.New(
			"fast: production mode requires a verify pass (ADR-0304: the proxy alone never wins)")
	}

	best, err := RunTPE(ctx, TPEParams{
		TargetVMAF:        params.TargetVMAF,
		Predict:           params.Predict,
		CRFLo:             crfLo,
		CRFHi:             crfHi,
		NTrials:           nTrials,
		TimeBudgetSeconds: params.effectiveTimeBudget(),
	})
	if err != nil {
		return RecommendResult{}, err
	}

	// Single verify pass — mandatory; proxy alone never wins.
	verifyVMAF, verifyErr := params.Verify(ctx, params.Encoder, best.Sample.CRF)
	if verifyErr != nil {
		return RecommendResult{}, fmt.Errorf("fast: verify pass at CRF %d: %w",
			best.Sample.CRF, verifyErr)
	}

	tolerance := params.effectiveTolerance()
	gap := math.Abs(best.Sample.PredictedVMAF - verifyVMAF)
	notes := fmt.Sprintf(
		"production: TPE over %d trials with v2 proxy; GPU verify gap = %.3f VMAF (tolerance %.2f).",
		nTrials, gap, tolerance)
	if gap > tolerance {
		notes += " FLAG: proxy/verify gap exceeds tolerance — consider falling " +
			"back to the slow Phase A grid (ADR-0276)."
	}

	verifyCopy := verifyVMAF
	gapCopy := gap
	return RecommendResult{
		Encoder:        params.Encoder,
		NTrials:        best.CompletedTrials,
		Notes:          notes,
		PredictedKbps:  best.Sample.PredictedKbps,
		PredictedVMAF:  best.Sample.PredictedVMAF,
		ProxyVerifyGap: &gapCopy,
		RecommendedCRF: best.Sample.CRF,
		Smoke:          false,
		TargetVMAF:     params.TargetVMAF,
		VerifyVMAF:     &verifyCopy,
	}, nil
}
