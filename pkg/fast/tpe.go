// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package fast

import (
	"context"
	"errors"
	"fmt"
	"math"
	"strconv"
	"time"

	"github.com/c-bata/goptuna"
	"github.com/c-bata/goptuna/tpe"
)

// The Python fast path runs Optuna's TPESampler(seed=0) over a single
// suggest_int("crf", lo, hi) axis. github.com/c-bata/goptuna is a Go
// implementation of the same Tree-structured Parzen Estimator with the same
// Parzen-estimator defaults (consider_prior, prior_weight 1.0, magic clip,
// 24 EI candidates, 10 startup trials), so the search behaves the same way
// even though the two RNGs are different and the trial *sequences* therefore
// differ. What is guaranteed to match is the contract: the same objective,
// the same integer search space, the same "best trial wins" reduction, and a
// deterministic run for a fixed seed.
//
// Optuna's per-trial user attributes are mirrored through goptuna's
// FrozenTrial.UserAttrs (a map[string]string), so the best trial's predicted
// VMAF / kbps are recovered exactly the way _run_tpe reads
// best.user_attrs[...] rather than by re-invoking the predictor.
//
// # Reproducibility caveat (goptuna v0.9.0)
//
// Optuna's TPESampler(seed=0) makes a `vmaf-tune fast` run bit-reproducible.
// goptuna v0.9.0 does NOT fully honour its seed: tpe.SamplerOptionSeed seeds
// the sampler's own *rand.Rand and the startup RandomSampler, but
// tpe.Sampler.sampleFromGMM picks the active Parzen component through
// goptuna/internal/random.ArgMaxMultinomial, which draws from the *process-
// global* math/rand source (goptuna@v0.9.0/internal/random/random.go — a bare
// `rand.Float64()`). Go 1.20+ seeds that source randomly at startup and
// rand.Seed is a no-op since Go 1.24, so the leak cannot be closed from
// outside the library.
//
// Consequence: repeated runs explore slightly different trial sequences and
// may return a neighbouring CRF when two candidates score almost equally.
// The search still converges — on the ADR-0276 smoke curve it reaches the
// brute-force global optimum at the default budgets — but callers must not
// assume byte-identical recommendations across processes the way the Python
// contract allows. Closing the gap needs an upstream goptuna change threading
// the sampler rng into internal/random (or a vendored sampler); it is not a
// defect in this port.

// DefaultSeed is the sampler seed used when TPEParams.Seed is left at zero.
// The Python original hardcodes TPESampler(seed=0); this port keeps the same
// value, subject to the reproducibility caveat above.
const DefaultSeed int64 = 0

// userAttrVMAF / userAttrKbps are the per-trial attribute keys. The names
// match the Python trial.set_user_attr calls.
const (
	userAttrVMAF = "predicted_vmaf"
	userAttrKbps = "predicted_kbps"
)

// TPEParams configures RunTPE.
type TPEParams struct {
	// TargetVMAF is the quality target the objective minimises distance to.
	TargetVMAF float64
	// Predict maps a candidate CRF to its (vmaf, kbps) proposal.
	Predict Predictor
	// CRFLo / CRFHi bound the inclusive integer search space.
	CRFLo int
	CRFHi int
	// NTrials is the trial budget.
	NTrials int
	// TimeBudgetSeconds is a soft wall-clock cap. TPE stops scheduling new
	// trials once it elapses; the in-flight trial is allowed to finish.
	// 0 or negative disables the cap.
	TimeBudgetSeconds int
	// Seed is the TPE sampler seed. 0 selects DefaultSeed. See the
	// reproducibility caveat at the top of this file: goptuna v0.9.0 leaks
	// one draw per EI candidate to the process-global math/rand source, so
	// the seed constrains but does not fully determine the trial sequence.
	Seed int64
}

// seed resolves the sampler seed.
func (p TPEParams) seed() int64 {
	if p.Seed == 0 {
		return DefaultSeed
	}
	return p.Seed
}

// TPEResult is the outcome of a completed TPE search.
type TPEResult struct {
	// Sample is the best trial's (crf, predicted_vmaf, predicted_kbps).
	Sample TrialSample
	// CompletedTrials is the number of trials the study recorded — the value
	// the Python original reports as len(study.trials).
	CompletedTrials int
	// BestValue is the objective value at Sample.
	BestValue float64
}

// RunTPE runs the TPE search and returns the best (crf, vmaf, kbps) triple.
//
// The objective is |predicted_vmaf - target| + bitrateWeight*predicted_kbps,
// minimised. Errors raised by Predict abort the study, matching Optuna's
// default catch=() behaviour.
func RunTPE(ctx context.Context, params TPEParams) (TPEResult, error) {
	if params.Predict == nil {
		return TPEResult{}, errors.New("fast: RunTPE requires a predictor")
	}
	if params.CRFLo > params.CRFHi {
		return TPEResult{}, fmt.Errorf("fast: invalid CRF range [%d, %d]",
			params.CRFLo, params.CRFHi)
	}
	if params.NTrials <= 0 {
		return TPEResult{}, fmt.Errorf("fast: n_trials must be > 0; got %d", params.NTrials)
	}
	if ctx == nil {
		ctx = context.Background()
	}

	study, err := goptuna.CreateStudy(
		"vmafx-tune-fast",
		goptuna.StudyOptionDirection(goptuna.StudyDirectionMinimize),
		goptuna.StudyOptionSampler(tpe.NewSampler(tpe.SamplerOptionSeed(params.seed()))),
		// goptuna's default logger writes Debug-level chatter to stdout,
		// which would corrupt the JSON payload the CLI prints there. The
		// Python original silences Optuna the same way with
		// optuna.logging.set_verbosity(WARNING); the CLI is the right place
		// to surface progress.
		goptuna.StudyOptionLogger(nil),
	)
	if err != nil {
		return TPEResult{}, fmt.Errorf("fast: create TPE study: %w", err)
	}

	// The soft wall-clock budget is a deadline on the study context.
	// goptuna checks ctx.Done() *before* scheduling each trial, so an
	// in-flight probe encode is never interrupted midway — matching the
	// Optuna `timeout=` semantics the Python docstring describes.
	studyCtx := ctx
	cancel := context.CancelFunc(func() {})
	if params.TimeBudgetSeconds > 0 {
		studyCtx, cancel = context.WithTimeout(ctx,
			time.Duration(params.TimeBudgetSeconds)*time.Second)
	}
	defer cancel()
	study.WithContext(studyCtx)

	objective := func(trial goptuna.Trial) (float64, error) {
		crf, suggestErr := trial.SuggestInt("crf", params.CRFLo, params.CRFHi)
		if suggestErr != nil {
			return 0, fmt.Errorf("suggest crf: %w", suggestErr)
		}
		sample, predictErr := params.Predict(crf)
		if predictErr != nil {
			return 0, fmt.Errorf("predict at CRF %d: %w", crf, predictErr)
		}
		if attrErr := trial.SetUserAttr(userAttrVMAF, formatAttr(sample.PredictedVMAF)); attrErr != nil {
			return 0, fmt.Errorf("record predicted_vmaf: %w", attrErr)
		}
		if attrErr := trial.SetUserAttr(userAttrKbps, formatAttr(sample.PredictedKbps)); attrErr != nil {
			return 0, fmt.Errorf("record predicted_kbps: %w", attrErr)
		}
		return objectiveValue(sample, params.TargetVMAF), nil
	}

	optErr := study.Optimize(objective, params.NTrials)
	// A tripped time budget is a normal stop, not a failure: Optuna's
	// timeout= behaves the same way. Any other error aborts.
	if optErr != nil && !errors.Is(optErr, context.DeadlineExceeded) {
		return TPEResult{}, fmt.Errorf("fast: TPE search: %w", optErr)
	}

	trials, trialsErr := study.GetTrials()
	if trialsErr != nil {
		return TPEResult{}, fmt.Errorf("fast: read TPE trials: %w", trialsErr)
	}

	best, bestErr := study.Storage.GetBestTrial(study.ID)
	if bestErr != nil {
		return TPEResult{}, fmt.Errorf(
			"fast: TPE search produced no completed trial (budget %d trials, %ds): %w",
			params.NTrials, params.TimeBudgetSeconds, bestErr)
	}

	crf, crfErr := bestCRF(best)
	if crfErr != nil {
		return TPEResult{}, crfErr
	}

	return TPEResult{
		Sample: TrialSample{
			CRF:           crf,
			PredictedVMAF: parseAttr(best.UserAttrs[userAttrVMAF]),
			PredictedKbps: parseAttr(best.UserAttrs[userAttrKbps]),
		},
		CompletedTrials: len(trials),
		BestValue:       best.Value,
	}, nil
}

// bestCRF extracts the integer "crf" parameter from a frozen trial. goptuna
// stores suggest_int results as float64 in Params, so the value is rounded
// back to the integer the objective actually evaluated.
func bestCRF(trial goptuna.FrozenTrial) (int, error) {
	raw, ok := trial.Params["crf"]
	if !ok {
		return 0, errors.New("fast: best TPE trial carries no 'crf' parameter")
	}
	switch v := raw.(type) {
	case float64:
		return int(math.Round(v)), nil
	case int:
		return v, nil
	default:
		return 0, fmt.Errorf("fast: best TPE trial 'crf' has unexpected type %T", raw)
	}
}

// formatAttr renders a float for the string-valued user-attribute store with
// full round-trip precision.
func formatAttr(v float64) string {
	return strconv.FormatFloat(v, 'g', -1, 64)
}

// parseAttr reads back a formatAttr value. A missing or malformed attribute
// yields NaN, matching the Python
// best.user_attrs.get("predicted_vmaf", float("nan")) fallback.
func parseAttr(s string) float64 {
	if s == "" {
		return math.NaN()
	}
	v, err := strconv.ParseFloat(s, 64)
	if err != nil {
		return math.NaN()
	}
	return v
}
