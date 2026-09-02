// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package prefilter

import (
	"context"
	"errors"
	"fmt"
	"math"
	"os/exec"
	"sort"
	"strings"
	"time"
)

// Default CRF search window. The codec adapter's own quality range overrides
// this in production; these constants give the search a sane default and are
// reused by the CLI's --crf-min / --crf-max help text. Matches the fast-path
// window (ADR-0276).
const (
	DefaultCRFLo = 18
	DefaultCRFHi = 40
)

// DefaultNTrials is the live-loop TPE budget. The joint space is eleven
// dimensional (ten deband knobs plus CRF), wider than the fast path's single
// CRF axis, so the default count is higher; TPE still converges well inside
// the soft time budget at realistic probe-encode cost.
const DefaultNTrials = 60

// SmokeNTrials exercises the joint search wiring end to end against the
// synthetic surface — no ffmpeg, no Vulkan, no GPU.
const SmokeNTrials = 40

// bitrateWeight is the objective's bitrate tie-breaker, small relative to the
// quality term so the search primarily hits the VMAF target and only breaks
// ties toward lower bitrate. Mirrors the fast path's weight.
const bitrateWeight = 1.0e-4

// ErrFilterUnavailable gates the live path: the Pelorus deband filter is a
// Vulkan ffmpeg filter that must be compiled into the ffmpeg build. vmafx
// only emits the -vf string, so without the filter the loop cannot run, and
// this error makes that actionable instead of an opaque "No such filter"
// stderr dump.
var ErrFilterUnavailable = errors.New(
	"the Pelorus deband filter is not available in this ffmpeg build")

// ProbeResult is the outcome of one deband -> encode -> score probe.
type ProbeResult struct {
	// VMAF is the achieved pooled VMAF for the (deband, crf) proposal.
	VMAF float64
	// Kbps is the observed output bitrate.
	Kbps float64
	// VFFragment is the exact -vf string the probe ran, kept for the
	// per-probe audit trail.
	VFFragment string
}

// ProbeFn is the production seam: one deband -> encode -> score round trip.
// The CLI builds it from ffmpeg plus libvmaf; tests inject a deterministic
// fake.
type ProbeFn func(ctx context.Context, deband map[string]float64, crf int) (ProbeResult, error)

// ProbeRecord is one recorded trial, emitted in the per-probe report.
type ProbeRecord struct {
	Trial        int                `json:"trial"`
	CRF          int                `json:"crf"`
	DebandParams map[string]float64 `json:"deband_params"`
	VFFragment   string             `json:"vf_fragment"`
	VMAF         float64            `json:"vmaf"`
	Kbps         float64            `json:"kbps"`
	Objective    float64            `json:"objective"`
}

// Result is the joint deband + CRF recommendation.
//
// The JSON tags reproduce the Python PrefilterResult.to_dict() key names and
// ordering, so the emitted payload is byte-compatible with the Python
// subcommand's output.
type Result struct {
	FilterName        string             `json:"filter_name"`
	Encoder           string             `json:"encoder"`
	TargetVMAF        float64            `json:"target_vmaf"`
	RecommendedCRF    int                `json:"recommended_crf"`
	RecommendedDeband map[string]float64 `json:"recommended_deband"`
	RecommendedVF     string             `json:"recommended_vf"`
	AchievedVMAF      float64            `json:"achieved_vmaf"`
	AchievedKbps      float64            `json:"achieved_kbps"`
	NTrials           int                `json:"n_trials"`
	Smoke             bool               `json:"smoke"`
	Probes            []ProbeRecord      `json:"probes"`
	Notes             string             `json:"notes"`
}

// Options configures RecommendPrefilter.
type Options struct {
	// Src is the source video. Empty is valid only in smoke mode.
	Src string
	// TargetVMAF is the quality target on the standard [0, 100] scale.
	TargetVMAF float64
	// Encoder is the codec adapter name, recorded in the result and
	// forwarded to the probe.
	Encoder string
	// FilterName selects the registered filter adapter.
	FilterName string
	// CRFRange is the inclusive (lo, hi) search window.
	CRFRange [2]int
	// SweepKnobs restricts the swept deband dimensions; empty sweeps all ten
	// contract knobs and leaves the rest at the filter default.
	SweepKnobs []string
	// NTrials is the TPE budget. 0 selects DefaultNTrials, or SmokeNTrials
	// in smoke mode.
	NTrials int
	// TimeBudget is a soft wall-clock cap on the loop. 0 disables it.
	TimeBudget time.Duration
	// Smoke selects the synthetic surface (no ffmpeg / Vulkan / GPU).
	Smoke bool
	// Probe is the production seam, required when Smoke is false.
	Probe ProbeFn
	// Seed makes the search reproducible.
	Seed int64
}

// BuildSearchSpace constructs the joint deband + CRF dimensions.
//
// The deband axes come straight from the adapter's frozen knob table, so they
// can never drift from the contract; CRF is appended as an ordinal integer
// axis so one study optimises both simultaneously.
//
// knobs optionally restricts the swept deband dimensions; an unknown name is
// an error rather than a silent no-op.
func BuildSearchSpace(adapter Adapter, crfRange [2]int, knobs []string) ([]Dimension, error) {
	crfLo, crfHi := crfRange[0], crfRange[1]
	if crfLo < 0 || crfHi < crfLo {
		return nil, fmt.Errorf(
			"invalid crf range [%d, %d]: need 0 <= lo <= hi", crfLo, crfHi)
	}

	var selected []Knob
	if len(knobs) == 0 {
		selected = adapter.Knobs()
	} else {
		for _, name := range knobs {
			k, err := adapter.Knob(name)
			if err != nil {
				return nil, err
			}
			selected = append(selected, k)
		}
	}

	dims := make([]Dimension, 0, len(selected)+1)
	for _, k := range selected {
		dims = append(dims, Dimension{Name: k.Name, Kind: k.Kind, Lo: k.Lo, Hi: k.Hi})
	}
	dims = append(dims, Dimension{
		Name: "crf", Kind: KindInt, Lo: float64(crfLo), Hi: float64(crfHi),
	})
	return dims, nil
}

// FilterAvailable reports whether ffmpeg lists the deband filter.
//
// Any failure (ffmpeg missing, non-zero exit) returns false rather than an
// error; the caller turns that into ErrFilterUnavailable with context.
func FilterAvailable(ctx context.Context, ffmpegBin, filterName string) bool {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	if filterName == "" {
		filterName = FilterName
	}
	// #nosec G204 -- ffmpegBin is an operator-configured CLI flag and the
	// remaining argv is fixed.
	out, err := exec.CommandContext(ctx, ffmpegBin, "-hide_banner", "-filters").Output()
	if err != nil {
		return false
	}
	return strings.Contains(string(out), filterName)
}

// SmokeProbe is the deterministic synthetic deband + CRF surface.
//
// CRF dominates: VMAF runs from about 99 at CRF 18 down to about 78 at CRF 40
// on a smooth taper. Moderate luma grain (~0.006) recovers a little
// perceptual quality on banded content; too much hurts. The surface is smooth
// with a clear optimum, so the joint search demonstrably converges without
// any ffmpeg, Vulkan or GPU.
func SmokeProbe(adapter Adapter) ProbeFn {
	return func(_ context.Context, deband map[string]float64, crf int) (ProbeResult, error) {
		crfNorm := (float64(crf) - 18.0) / 22.0
		vmaf := 99.0 - 21.0*math.Max(0.0, crfNorm)

		grainy := 0.006
		if v, ok := deband["grainy"]; ok {
			grainy = v
		}
		vmaf += 1.5*math.Exp(-math.Pow(grainy-0.006, 2)/(2*math.Pow(0.01, 2))) - 0.75

		kbps := 9000.0*math.Exp(-2.8*math.Max(0.0, crfNorm)) + 120.0

		fragment, err := adapter.VFFragment(deband, false)
		if err != nil {
			return ProbeResult{}, err
		}
		return ProbeResult{VMAF: vmaf, Kbps: kbps, VFFragment: fragment}, nil
	}
}

// Objective is the search objective: |achieved - target| + lambda * kbps.
//
// Minimising it converges on the lowest-bitrate deband + CRF combination that
// hits the target VMAF.
func Objective(result ProbeResult, targetVMAF float64) float64 {
	return math.Abs(result.VMAF-targetVMAF) + bitrateWeight*result.Kbps
}

// RecommendPrefilter runs the joint deband + CRF search and returns the
// recommendation.
//
// Production flow (Smoke false): the caller must inject Probe — this package
// never runs ffmpeg itself, and the live loop requires the Pelorus Vulkan
// filter in the ffmpeg build (the CLI enforces that with FilterAvailable
// before calling in).
//
// Smoke flow: the synthetic surface drives the whole loop with no external
// process.
func RecommendPrefilter(ctx context.Context, opts Options) (Result, error) {
	filterName := opts.FilterName
	if filterName == "" {
		filterName = "pelorus_deband"
	}
	adapter, err := GetAdapter(filterName)
	if err != nil {
		return Result{}, err
	}
	if opts.CRFRange == [2]int{0, 0} {
		opts.CRFRange = [2]int{DefaultCRFLo, DefaultCRFHi}
	}
	dims, spaceErr := BuildSearchSpace(adapter, opts.CRFRange, opts.SweepKnobs)
	if spaceErr != nil {
		return Result{}, spaceErr
	}
	if opts.TimeBudget < 0 {
		return Result{}, fmt.Errorf(
			"time budget must be > 0 when set; got %v", opts.TimeBudget)
	}

	nTrials := opts.NTrials
	if nTrials <= 0 {
		nTrials = DefaultNTrials
		if opts.Smoke {
			nTrials = SmokeNTrials
		}
	}

	probe := opts.Probe
	if opts.Smoke {
		if probe == nil {
			probe = SmokeProbe(adapter)
		}
	} else {
		if probe == nil {
			return Result{}, errors.New(
				"prefilter production mode requires an injected probe callable " +
					"(deband, crf) -> ProbeResult. The live deband->encode->score " +
					"loop is built by the CLI handler from ffmpeg + libvmaf and " +
					"gated on FilterAvailable()")
		}
		if opts.Src == "" {
			return Result{}, errors.New(
				"prefilter production mode requires a source path " +
					"(an empty src is only valid in smoke mode)")
		}
	}

	sampler := NewTPESampler(dims, DefaultTPEConfig(), opts.Seed)
	records := make([]ProbeRecord, 0, nTrials)

	bestObjective := math.Inf(1)
	bestCRF := opts.CRFRange[0]
	bestDeband := map[string]float64{}
	bestProbe := ProbeResult{VMAF: math.NaN(), Kbps: math.NaN()}

	deadline := time.Time{}
	if opts.TimeBudget > 0 {
		deadline = time.Now().Add(opts.TimeBudget)
	}

	for trial := 0; trial < nTrials; trial++ {
		if ctxErr := ctx.Err(); ctxErr != nil {
			break
		}
		if !deadline.IsZero() && time.Now().After(deadline) {
			break
		}

		proposal := sampler.Suggest()
		deband := make(map[string]float64, len(proposal))
		crf := 0
		for name, value := range proposal {
			if name == "crf" {
				crf = int(value)
				continue
			}
			deband[name] = value
		}

		result, probeErr := probe(ctx, deband, crf)
		if probeErr != nil {
			return Result{}, fmt.Errorf("prefilter trial %d: %w", trial, probeErr)
		}
		objective := Objective(result, opts.TargetVMAF)
		sampler.Observe(proposal, objective)

		records = append(records, ProbeRecord{
			Trial:        trial,
			CRF:          crf,
			DebandParams: deband,
			VFFragment:   result.VFFragment,
			VMAF:         result.VMAF,
			Kbps:         result.Kbps,
			Objective:    objective,
		})
		if objective < bestObjective {
			bestObjective, bestCRF, bestDeband, bestProbe = objective, crf, deband, result
		}
	}

	if len(records) == 0 {
		return Result{}, errors.New("prefilter: no trials completed")
	}

	bestVF, vfErr := adapter.VFFragment(bestDeband, false)
	if vfErr != nil {
		return Result{}, vfErr
	}

	swept := make([]string, 0, len(dims))
	for _, d := range dims {
		if d.Name != "crf" {
			swept = append(swept, d.Name)
		}
	}
	sort.Strings(swept)

	notes := fmt.Sprintf(
		"production: joint TPE over %d deband knobs + CRF, %d probes against "+
			"the %s filter (ADR-0110 contract). VMAF is the oracle; "+
			"lowest-bitrate hit wins.",
		len(swept), len(records), FilterName)
	if opts.Smoke {
		notes = fmt.Sprintf(
			"smoke mode — synthetic deband+CRF surface; no ffmpeg / Vulkan / GPU. "+
				"Joint TPE over deband knobs + CRF (ADR-1116 / ADR-0106). "+
				"Swept knobs: %s.", strings.Join(swept, ", "))
	}

	return Result{
		FilterName:        adapter.FilterName,
		Encoder:           opts.Encoder,
		TargetVMAF:        opts.TargetVMAF,
		RecommendedCRF:    bestCRF,
		RecommendedDeband: bestDeband,
		RecommendedVF:     bestVF,
		AchievedVMAF:      bestProbe.VMAF,
		AchievedKbps:      bestProbe.Kbps,
		NTrials:           len(records),
		Smoke:             opts.Smoke,
		Probes:            records,
		Notes:             notes,
	}, nil
}
