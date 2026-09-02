// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package auto is the Go port of vmaftune.auto — the Phase F adaptive
// recipe-aware tuning entry point (ADR-0364 / ADR-0325 / ADR-0454).
//
// Phase F composes the per-phase subcommands (corpus, recommend, fast,
// predict, tune-per-shot, recommend-saliency, ladder, compare) plus the
// orthogonal modes (HDR, sample-clip, resolution-aware) into a single
// deterministic decision tree:
//
//	auto(src, target_vmaf, max_budget_kbps, allow_codecs):
//	    meta = probe(src); is_hdr = detect_hdr(meta)          # ADR-0300
//	    rungs = [meta.resolution] if meta.height < 2160        # ADR-0289
//	            else ladder.candidate_rungs(meta)
//	    codecs = (allow_codecs if len==1
//	              else [user_pin] if user_pinned_codec
//	              else compare.shortlist(allow_codecs, meta))
//	    plan = []
//	    for rung, codec in (rungs x codecs):
//	        v = predict.crf_for_target(rung, codec, target_vmaf, meta)
//	        if v.verdict == FALL_BACK:
//	            v = recommend.coarse_to_fine(rung, codec, target_vmaf)
//	        plan.append((rung, codec, v))
//	    winner = pick_pareto(plan, target_vmaf, max_budget_kbps)
//	    return realise(winner, hdr=is_hdr)
//
// The ten short-circuit predicates live here as standalone functions so each
// is unit-testable in isolation. The driver records the firing predicate
// names in plan.metadata.short_circuits for post-hoc speedup analysis.
//
// Non-smoke runs probe source geometry, duration, and HDR signalling through
// ffprobe and use the predictor path for per-cell CRF, VMAF, and bitrate
// estimates; Smoke keeps the same planner deterministic without ffmpeg or
// ONNX.
package auto

import (
	"context"
	"encoding/json"
	"log/slog"
	"math"
	"sort"
	"strconv"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/hdr"
	"github.com/VMAFx/vmafx/pkg/predictor"
	"github.com/VMAFx/vmafx/pkg/pyjson"
	"github.com/VMAFx/vmafx/pkg/pymath"
)

// Phase D gate thresholds (short-circuit #7). The 5-minute / 0.15-shot-
// variance pair is a placeholder pending an F.3 corpus fit; until then the
// gate stays conservative — short low-variance content skips per-shot tuning,
// everything else gets it.
const (
	PhaseDDurationGateS    = 300.0
	PhaseDShotVarianceGate = 0.15
)

// LadderMultiRungHeight is the resolution above which the ladder is
// multi-rung (ADR-0289 / ADR-0295). Below it the auto driver evaluates only
// the source rung.
const LadderMultiRungHeight = 2160

// LowComplexityProbeBitrateThresholdKbps is the probe-encode bitrate below
// which a source is "low complexity" and the recommend / ladder stages add no
// information (short-circuit #8). Placeholder pending an F.3 corpus fit.
const LowComplexityProbeBitrateThresholdKbps = 200.0

// Predictor verdict vocabulary. The driver never produces LIKELY itself; it
// arrives through the CellIntervals production-wiring seam.
const (
	VerdictGospel   = "GOSPEL"
	VerdictLikely   = "LIKELY"
	VerdictFallBack = "FALL_BACK"
	VerdictUnknown  = "UNKNOWN"
)

// saliencyContentClasses are the classes that benefit from saliency-aware ROI
// tuning (ADR-0293). Photographic / live-action does not: VMAF's perceptual
// model already gives centre-frame foveal weighting.
var saliencyContentClasses = map[string]bool{
	"animation":      true,
	"screen_content": true,
}

// ShortCircuit names the ten predicates, in canonical evaluation order. The
// string values are the identifiers recorded in plan.metadata.short_circuits.
// Adding a short-circuit means appending — never reordering.
type ShortCircuit string

const (
	SCLadderSingleRung    ShortCircuit = "ladder-single-rung"
	SCCodecPinned         ShortCircuit = "codec-pinned"
	SCPredictorGospel     ShortCircuit = "predictor-gospel"
	SCSkipSaliency        ShortCircuit = "skip-saliency"
	SCSDRSkip             ShortCircuit = "sdr-skip"
	SCSampleClipPropagate ShortCircuit = "sample-clip-propagate"
	SCSkipPerShot         ShortCircuit = "skip-per-shot"
	SCLowComplexity       ShortCircuit = "low-complexity"
	SCBaselineMeetsTarget ShortCircuit = "baseline-meets-target"
	SCNoTwoPass           ShortCircuit = "no-two-pass"
)

// SourceMeta is the source metadata the decision tree consumes.
//
// Height comes from ffprobe (ADR-0289); IsHDR from hdr.Detect (ADR-0300);
// ContentClass from the F.4 classifier (defaults to "live_action" so the
// saliency gate stays a no-op on unknown content); DurationS and ShotVariance
// from the per-shot detector.
//
// ComplexityScore is the probe-encode bitrate at the adapter's probe knobs;
// 0.0 or NaN means the probe has not run and short-circuit #8 stays dormant.
// BaselineVMAF is the pooled-mean VMAF at the codec's default CRF; 0.0 or NaN
// means no baseline was scored and short-circuit #9 stays dormant.
type SourceMeta struct {
	Height            int
	Width             int
	IsHDR             bool
	ContentClass      string
	DurationS         float64
	ShotVariance      float64
	SampleClipSeconds float64
	ComplexityScore   float64
	BaselineVMAF      float64
}

// DefaultSourceMeta is the synthetic 1080p SDR live-action metadata the smoke
// path uses so a smoke run is deterministic without touching ffprobe.
func DefaultSourceMeta(sampleClipSeconds float64) SourceMeta {
	return SourceMeta{
		Height:            1080,
		Width:             1920,
		IsHDR:             false,
		ContentClass:      "live_action",
		DurationS:         120.0,
		ShotVariance:      0.05,
		SampleClipSeconds: sampleClipSeconds,
	}
}

// asMap renders SourceMeta the way dataclasses.asdict(meta) does, for the
// plan's metadata.source_meta block.
func (m SourceMeta) asMap() map[string]any {
	return map[string]any{
		"height":              m.Height,
		"width":               m.Width,
		"is_hdr":              m.IsHDR,
		"content_class":       m.ContentClass,
		"duration_s":          m.DurationS,
		"shot_variance":       m.ShotVariance,
		"sample_clip_seconds": m.SampleClipSeconds,
		"complexity_score":    m.ComplexityScore,
		"baseline_vmaf":       m.BaselineVMAF,
	}
}

// PlanState is the mutable state threaded through the decision tree. Each
// stage may append to ShortCircuits, set PredictorVerdict, and so on; the
// driver returns the final state in the plan's metadata so post-hoc analysis
// can measure which short-circuits fired.
type PlanState struct {
	TargetVMAF       float64
	MaxBudgetKbps    float64
	AllowCodecs      []string
	UserPinnedCodec  string
	PredictorVerdict string
	ShortCircuits    []string
	// AdapterSupportsTwoPass is nil until the codec adapter is resolved;
	// predicate #10 stays dormant while it is nil so an evaluation-order bug
	// cannot silently suppress two-pass encodes.
	AdapterSupportsTwoPass *bool
}

// Fired records that sc fired. Idempotent on repeats.
func (s *PlanState) Fired(sc ShortCircuit) {
	for _, existing := range s.ShortCircuits {
		if existing == string(sc) {
			return
		}
	}
	s.ShortCircuits = append(s.ShortCircuits, string(sc))
}

// ---------------------------------------------------------------------------
// The ten short-circuit predicates. Each returns true when the stage it
// guards can be skipped. Every predicate is a pure function of (meta, state)
// so tests can assert a branch fires without invoking the full driver.
// ---------------------------------------------------------------------------

// ShouldShortCircuitSingleRungLadder is #1 — sub-4K sources don't need a
// multi-rung ABR ladder evaluation; the source rung is the only candidate.
func ShouldShortCircuitSingleRungLadder(meta SourceMeta, _ *PlanState) bool {
	return meta.Height < LadderMultiRungHeight
}

// ShouldShortCircuitCodecPinned is #2 — when --allow-codecs resolves to one
// codec (or --codec pins it) the compare-shortlist stage adds no information.
func ShouldShortCircuitCodecPinned(_ SourceMeta, state *PlanState) bool {
	if state.UserPinnedCodec != "" {
		return true
	}
	return len(state.AllowCodecs) == 1
}

// ShouldShortCircuitPredictorGospel is #3 — a GOSPEL verdict means residuals
// are within threshold across the validation sample, so trust the predictor's
// CRF pick and skip the coarse-to-fine fallback for that cell.
func ShouldShortCircuitPredictorGospel(_ SourceMeta, state *PlanState) bool {
	return state.PredictorVerdict == VerdictGospel
}

// ShouldShortCircuitSkipSaliency is #4 — saliency-aware ROI tuning is gated
// on content class (ADR-0293).
func ShouldShortCircuitSkipSaliency(meta SourceMeta, _ *PlanState) bool {
	return !saliencyContentClasses[meta.ContentClass]
}

// ShouldShortCircuitSDRSkip is #5 — an SDR source skips the HDR resolution +
// model-selection branch.
func ShouldShortCircuitSDRSkip(meta SourceMeta, _ *PlanState) bool {
	return !meta.IsHDR
}

// ShouldShortCircuitSampleClipPropagate is #6 — propagate the operator's
// --sample-clip-seconds to the internal sweeps rather than re-deciding clip
// length per stage (ADR-0301). A propagation short-circuit, not a stage skip.
func ShouldShortCircuitSampleClipPropagate(meta SourceMeta, _ *PlanState) bool {
	return meta.SampleClipSeconds > 0.0
}

// ShouldShortCircuitSkipPerShot is #7 — skip per-shot refinement when the
// source is both short (< 5 min) AND low-variance (< 0.15). Either condition
// alone is not enough: a short high-variance trailer benefits from per-shot,
// and so does a long low-variance lecture capture.
func ShouldShortCircuitSkipPerShot(meta SourceMeta, _ *PlanState) bool {
	short := meta.DurationS < PhaseDDurationGateS
	lowVariance := meta.ShotVariance < PhaseDShotVarianceGate
	return short && lowVariance
}

// ShouldShortCircuitLowComplexity is #8 — a low-complexity source's point
// estimate is already tight enough that coarse-to-fine adds nothing. A 0.0 or
// NaN complexity score means the probe has not run; the predicate does not
// fire, so smoke runs are never gated.
func ShouldShortCircuitLowComplexity(meta SourceMeta, _ *PlanState) bool {
	score := meta.ComplexityScore
	if math.IsNaN(score) || score <= 0.0 {
		return false
	}
	return score < LowComplexityProbeBitrateThresholdKbps
}

// ShouldShortCircuitBaselineMeetsTarget is #9 — when a default-CRF encode
// already meets target, the predictor sweep and coarse-to-fine are redundant.
// A 0.0 or NaN baseline means none was scored; the predicate does not fire.
func ShouldShortCircuitBaselineMeetsTarget(meta SourceMeta, state *PlanState) bool {
	baseline := meta.BaselineVMAF
	if math.IsNaN(baseline) || baseline <= 0.0 {
		return false
	}
	return baseline >= state.TargetVMAF
}

// ShouldShortCircuitNoTwoPass is #10 — an adapter without two-pass support
// makes the two-pass calibration stage a no-op. Unresolved (nil) does not
// fire.
func ShouldShortCircuitNoTwoPass(_ SourceMeta, state *PlanState) bool {
	if state.AdapterSupportsTwoPass == nil {
		return false
	}
	return !*state.AdapterSupportsTwoPass
}

// Predicate pairs a short-circuit name with its predicate.
type Predicate struct {
	Name ShortCircuit
	Fn   func(SourceMeta, *PlanState) bool
}

// ShortCircuitPredicates is the canonical evaluation order and part of the
// public contract: tests assert that an earlier-firing predicate does not
// shadow a later one. Append, never reorder.
var ShortCircuitPredicates = []Predicate{
	{SCLadderSingleRung, ShouldShortCircuitSingleRungLadder},
	{SCCodecPinned, ShouldShortCircuitCodecPinned},
	{SCPredictorGospel, ShouldShortCircuitPredictorGospel},
	{SCSkipSaliency, ShouldShortCircuitSkipSaliency},
	{SCSDRSkip, ShouldShortCircuitSDRSkip},
	{SCSampleClipPropagate, ShouldShortCircuitSampleClipPropagate},
	{SCSkipPerShot, ShouldShortCircuitSkipPerShot},
	{SCLowComplexity, ShouldShortCircuitLowComplexity},
	{SCBaselineMeetsTarget, ShouldShortCircuitBaselineMeetsTarget},
	{SCNoTwoPass, ShouldShortCircuitNoTwoPass},
}

// EvaluateShortCircuits runs every predicate in declaration order, recording
// the firers on state, and returns the accumulated list.
func EvaluateShortCircuits(meta SourceMeta, state *PlanState) []string {
	for _, p := range ShortCircuitPredicates {
		if p.Fn(meta, state) {
			state.Fired(p.Name)
		}
	}
	out := make([]string, len(state.ShortCircuits))
	copy(out, state.ShortCircuits)
	return out
}

// ---------------------------------------------------------------------------
// Source probing.
// ---------------------------------------------------------------------------

// ProbeSourceMeta probes the metadata the non-smoke planner needs: geometry
// and duration from ffprobe, HDR signalling from hdr.Detect. Every failure
// degrades to the same conservative defaults (1920x1080, duration 0, SDR) the
// planner used before the probe path existed.
func ProbeSourceMeta(
	ctx context.Context,
	src string,
	sampleClipSeconds float64,
	ffprobeBin string,
	run hdr.Runner,
	log *slog.Logger,
) (SourceMeta, *hdr.Info) {
	if log == nil {
		log = slog.Default()
	}
	if ffprobeBin == "" {
		ffprobeBin = "ffprobe"
	}
	if run == nil {
		run = hdr.CommandRunner
	}
	width, height := probeVideoGeometry(ctx, src, ffprobeBin, run)
	info := hdr.Detect(ctx, src, ffprobeBin, run, log)
	duration := ProbeSourceDuration(ctx, src, ffprobeBin, run)

	if height == 0 {
		height = 1080
	}
	if width == 0 {
		width = 1920
	}
	return SourceMeta{
		Height:            height,
		Width:             width,
		IsHDR:             info != nil,
		ContentClass:      "live_action",
		DurationS:         duration,
		SampleClipSeconds: sampleClipSeconds,
	}, info
}

// probeVideoGeometry reads width and height from ffprobe, returning (0, 0) on
// any failure so the caller can apply its own defaults.
func probeVideoGeometry(ctx context.Context, src, ffprobeBin string, run hdr.Runner) (int, int) {
	argv := []string{
		ffprobeBin,
		"-v", "error",
		"-select_streams", "v:0",
		"-show_entries", "stream=width,height,r_frame_rate",
		"-of", "json",
		src,
	}
	stdout, code, err := run(ctx, argv)
	if err != nil || code != 0 {
		return 0, 0
	}
	var doc struct {
		Streams []struct {
			Width  int `json:"width"`
			Height int `json:"height"`
		} `json:"streams"`
	}
	if err := json.Unmarshal([]byte(stdout), &doc); err != nil || len(doc.Streams) == 0 {
		return 0, 0
	}
	return doc.Streams[0].Width, doc.Streams[0].Height
}

// ProbeSourceDuration returns the source duration in seconds, or 0.0 when
// probing fails.
func ProbeSourceDuration(ctx context.Context, src, ffprobeBin string, run hdr.Runner) float64 {
	if ffprobeBin == "" {
		ffprobeBin = "ffprobe"
	}
	if run == nil {
		run = hdr.CommandRunner
	}
	argv := []string{
		ffprobeBin,
		"-v", "error",
		"-show_entries", "format=duration",
		"-of", "json",
		src,
	}
	stdout, code, err := run(ctx, argv)
	if err != nil || code != 0 {
		return 0.0
	}
	// ffprobe emits duration as a JSON *string* ("12.520000"); json.Number
	// accepts both that and a bare number without committing to either.
	var doc struct {
		Format struct {
			Duration any `json:"duration"`
		} `json:"format"`
	}
	if err := json.Unmarshal([]byte(stdout), &doc); err != nil {
		return 0.0
	}
	switch v := doc.Format.Duration.(type) {
	case float64:
		return v
	case string:
		f, err := strconv.ParseFloat(v, 64)
		if err != nil {
			return 0.0
		}
		return f
	default:
		return 0.0
	}
}

// ---------------------------------------------------------------------------
// Per-cell estimation.
// ---------------------------------------------------------------------------

// predictorFeaturesFromMeta builds predictor features from metadata-only auto
// inputs. Features that need per-frame probe logs stay at zero until the
// Phase F probe-encode capture lands; the predictor contract treats those as
// unavailable signals.
func predictorFeaturesFromMeta(meta SourceMeta) predictor.ShotFeatures {
	const fps = 30.0
	duration := math.Max(meta.DurationS, 0.0)
	shotFrames := int(fps)
	if duration > 0.0 {
		shotFrames = int(math.Round(duration * fps))
	}
	pixels := float64(maxInt(meta.Width, 1) * maxInt(meta.Height, 1))

	// Complexity score is the probe bitrate when available. Without it, seed
	// a resolution-proportional neutral bitrate so metadata-only runs still
	// produce a codec-specific CRF rather than a fixed placeholder.
	probeBitrate := meta.ComplexityScore
	if math.IsNaN(probeBitrate) || probeBitrate <= 0.0 {
		probeBitrate = math.Max(500.0, pixels/900.0)
	}
	return predictor.ShotFeatures{
		ProbeBitrateKbps: probeBitrate,
		FrameDiffMean:    math.Max(meta.ShotVariance, 0.0),
		ShotLengthFrames: maxInt(shotFrames, 1),
		FPS:              fps,
		Width:            maxInt(meta.Width, 1),
		Height:           maxInt(meta.Height, 1),
	}
}

// estimateCellBitrateKbps estimates a cell's bitrate from the probe bitrate
// and CRF. Explicitly a predictor estimate, not a measured encode: it follows
// the common encoder rule of thumb that six CRF/QP points roughly halve or
// double bitrate, anchored at the adapter's probe quality. Downstream
// planners get a monotone estimate until the realise step lands.
func estimateCellBitrateKbps(features predictor.ShotFeatures, codecName string, crf int) float64 {
	probeQuality := crf
	if adapter, err := codecadapter.Get(codecName); err == nil {
		probeQuality = adapter.ProbeQuality
	}
	// pymath.Exp2 rather than math.Pow: the result lands in a
	// user-discoverable JSON field that must match the Python emitter to the
	// last mantissa bit, and the stdlib kernel does not (see pkg/pymath).
	scale := pymath.Exp2((float64(probeQuality) - float64(crf)) / 6.0)
	return math.Max(1.0, features.ProbeBitrateKbps*scale)
}

// ---------------------------------------------------------------------------
// Winner selection.
// ---------------------------------------------------------------------------

// Winner-selection status values recorded under metadata.winner.status.
const (
	StatusBudgetAndQualityMet      = "budget_and_quality_met"
	StatusQualityMetBudgetExceeded = "quality_met_budget_exceeded"
	StatusTargetUnmet              = "target_unmet"
	StatusNoEligibleCells          = "no_eligible_cells"
)

// scoredCell is one plan cell with its finite estimates extracted.
type scoredCell struct {
	index   int
	cell    map[string]any
	vmaf    float64
	bitrate float64
}

// PickAutoWinner picks the realised cell from the estimated plan rows.
//
// The selector is deliberately conservative:
//   - prefer cells that satisfy both the quality target and the bitrate
//     budget;
//   - if no cell is inside budget, keep the quality gate and minimise budget
//     overage;
//   - if no cell meets quality, return the closest quality miss so the caller
//     gets a concrete next encode instead of an empty plan.
//
// Ties favour lower bitrate, then higher VMAF, then a higher rung, then a
// stable codec/index ordering — the same total order the Python min/max keys
// impose, so both implementations pick the same cell.
func PickAutoWinner(cells []map[string]any, targetVMAF, maxBudgetKbps float64) map[string]any {
	scored := make([]scoredCell, 0, len(cells))
	for i, cell := range cells {
		vmaf, vmafOK := finiteFloat(cell["estimated_vmaf"])
		bitrate, bitrateOK := finiteFloat(cell["estimated_bitrate_kbps"])
		if !vmafOK || !bitrateOK {
			continue
		}
		scored = append(scored, scoredCell{index: i, cell: cell, vmaf: vmaf, bitrate: bitrate})
	}
	if len(scored) == 0 {
		return map[string]any{
			"status": StatusNoEligibleCells,
			"reason": "no cell carried finite estimated_vmaf and estimated_bitrate_kbps",
		}
	}

	var passing, qualityOnly []scoredCell
	for _, item := range scored {
		if item.vmaf >= targetVMAF {
			qualityOnly = append(qualityOnly, item)
			if item.bitrate <= maxBudgetKbps {
				passing = append(passing, item)
			}
		}
	}

	var status string
	var selected scoredCell
	switch {
	case len(passing) > 0:
		status = StatusBudgetAndQualityMet
		selected = minBy(passing, func(a, b scoredCell) bool {
			return lessTuple(
				[]float64{a.bitrate, -a.vmaf, -float64(cellRung(a.cell))},
				[]float64{b.bitrate, -b.vmaf, -float64(cellRung(b.cell))},
				cellCodec(a.cell), cellCodec(b.cell),
				float64(a.index), float64(b.index),
			)
		})
	case len(qualityOnly) > 0:
		status = StatusQualityMetBudgetExceeded
		selected = minBy(qualityOnly, func(a, b scoredCell) bool {
			return lessTuple(
				[]float64{a.bitrate - maxBudgetKbps, a.bitrate, -a.vmaf, -float64(cellRung(a.cell))},
				[]float64{b.bitrate - maxBudgetKbps, b.bitrate, -b.vmaf, -float64(cellRung(b.cell))},
				cellCodec(a.cell), cellCodec(b.cell),
				float64(a.index), float64(b.index),
			)
		})
	default:
		status = StatusTargetUnmet
		// Python's max() over (vmaf, -bitrate, rung, codec, -index): invert
		// every component to reuse the same "strictly less" comparator.
		selected = minBy(scored, func(a, b scoredCell) bool {
			return lessTuple(
				[]float64{-a.vmaf, a.bitrate, -float64(cellRung(a.cell))},
				[]float64{-b.vmaf, b.bitrate, -float64(cellRung(b.cell))},
				invertString(cellCodec(a.cell)), invertString(cellCodec(b.cell)),
				float64(a.index), float64(b.index),
			)
		})
	}

	return map[string]any{
		"status":                 status,
		"cell_index":             selected.index,
		"rung":                   cellRung(selected.cell),
		"codec":                  cellCodec(selected.cell),
		"crf":                    cellCRF(selected.cell),
		"estimated_vmaf":         selected.vmaf,
		"estimated_bitrate_kbps": selected.bitrate,
		"quality_margin":         selected.vmaf - targetVMAF,
		"budget_margin_kbps":     maxBudgetKbps - selected.bitrate,
	}
}

// invertString is a sentinel marker: the target_unmet branch maximises over a
// tuple whose codec component sorts ascending (Python's max picks the
// lexicographically largest codec), so the reused "less" comparator has to
// see the codec order flipped. Go cannot negate a string, so the comparator
// checks this wrapper and reverses the comparison.
type invertedString string

func invertString(s string) any { return invertedString(s) }

// lessTuple compares two cells by a numeric prefix, then a codec component,
// then the plan index — the shape of every Python selection key in
// pick_auto_winner.
func lessTuple(a, b []float64, codecA, codecB any, indexA, indexB float64) bool {
	for i := range a {
		if a[i] != b[i] {
			return a[i] < b[i]
		}
	}
	sa, aInverted := codecA.(invertedString)
	sb, bInverted := codecB.(invertedString)
	if aInverted && bInverted {
		if sa != sb {
			return sa > sb
		}
	} else {
		ca, _ := codecA.(string)
		cb, _ := codecB.(string)
		if ca != cb {
			return ca < cb
		}
	}
	return indexA < indexB
}

func minBy(items []scoredCell, less func(a, b scoredCell) bool) scoredCell {
	best := items[0]
	for _, item := range items[1:] {
		if less(item, best) {
			best = item
		}
	}
	return best
}

// markSelectedCell annotates cells in place with the winner's index.
func markSelectedCell(cells []map[string]any, winner map[string]any) {
	selectedIndex, ok := winner["cell_index"].(int)
	for i, cell := range cells {
		cell["selected"] = ok && i == selectedIndex
	}
}

func cellRung(cell map[string]any) int {
	if v, ok := cell["rung"].(int); ok {
		return v
	}
	return 0
}

func cellCodec(cell map[string]any) string {
	if v, ok := cell["codec"].(string); ok {
		return v
	}
	return ""
}

func cellCRF(cell map[string]any) int {
	if v, ok := cell["crf"].(int); ok {
		return v
	}
	return 0
}

// finiteFloat returns value as a finite float, reporting false when it is
// absent, non-numeric, NaN, or infinite.
func finiteFloat(value any) (float64, bool) {
	var f float64
	switch v := value.(type) {
	case float64:
		f = v
	case float32:
		f = float64(v)
	case int:
		f = float64(v)
	case int64:
		f = float64(v)
	default:
		return 0, false
	}
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return 0, false
	}
	return f, true
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// ---------------------------------------------------------------------------
// The driver.
// ---------------------------------------------------------------------------

// CellInterval is one entry of the F.3 production-wiring seam: the native
// verdict and conformal interval width for a (rung, codec) cell.
type CellInterval struct {
	Rung          int
	Codec         string
	Verdict       string
	IntervalWidth float64
}

// Options configures a RunAuto call.
type Options struct {
	// Src is the reference video path. Recorded in metadata.src and probed
	// when Smoke is false and MetaOverride is nil.
	Src string
	// TargetVMAF is the pooled-mean VMAF the plan aims for.
	TargetVMAF float64
	// MaxBudgetKbps bounds the picked rendition's bitrate.
	MaxBudgetKbps float64
	// AllowCodecs is the codec list the tree may pick from.
	AllowCodecs []string
	// UserPinnedCodec overrides the allow-list ranking when non-empty.
	UserPinnedCodec string
	// SampleClipSeconds propagates a clip length to internal sweeps.
	SampleClipSeconds float64
	// Smoke exercises the composition without ffprobe or ONNX.
	Smoke bool

	// MetaOverride injects a pre-built SourceMeta, skipping the probe.
	MetaOverride *SourceMeta
	// ConfidenceThresholds carries the F.3 width gates; the zero value
	// resolves to the documented defaults.
	ConfidenceThresholds *ConfidenceThresholds
	// CellIntervals is the F.3 production-wiring seam. When non-nil, any
	// (rung, codec) cell missing from the slice falls back to a NaN interval
	// so the gate defers to the native verdict instead of silently using a
	// synthetic tight width.
	CellIntervals []CellInterval

	// Predictor supplies the per-cell CRF / VMAF estimates in non-smoke
	// mode. nil builds an analytical-fallback predictor.
	Predictor *predictor.Predictor
	// Recipes is the F.4 override table. nil loads it from disk.
	Recipes *RecipeTable
	// FFprobeBin and ProbeRunner are the source-probe seams.
	FFprobeBin  string
	ProbeRunner hdr.Runner
	// Log receives the driver's diagnostics.
	Log *slog.Logger
}

// Plan is the result of an auto run. Cells is one entry per (rung, codec)
// cell; the schema is stable from F.1 onwards, and smoke entries are
// placeholders carrying the same keys as production ones.
type Plan struct {
	Cells    []map[string]any
	Metadata map[string]any
}

// RunAuto drives the F.1 + F.2 + F.3 + F.4 decision tree.
//
// stage order is the public contract (plan.metadata.short_circuits records the
// firing order). Splitting it into per-stage helpers would thread eight
// mutable locals through eight signatures and make the Python-parity diff
// unreadable; the stage banners below carry the structure instead.
//
//nolint:funlen,gocyclo // The driver is a linear ten-stage decision tree whose
func RunAuto(ctx context.Context, opts Options) (Plan, error) {
	log := opts.Log
	if log == nil {
		log = slog.Default()
	}

	// ------------------------------------------------------------------
	// Stage -1 — source metadata.
	// ------------------------------------------------------------------
	var detectedHDR *hdr.Info
	meta := opts.MetaOverride
	if !opts.Smoke && meta == nil {
		probed, info := ProbeSourceMeta(
			ctx, opts.Src, opts.SampleClipSeconds, opts.FFprobeBin, opts.ProbeRunner, log)
		meta = &probed
		detectedHDR = info
	}
	if meta == nil {
		synthetic := DefaultSourceMeta(opts.SampleClipSeconds)
		meta = &synthetic
	}

	hdrInfo := detectedHDR
	if !meta.IsHDR {
		hdrInfo = nil
	} else if hdrInfo == nil {
		hdrInfo = hdr.DefaultForMetadataOnly()
	}

	planState := &PlanState{
		TargetVMAF:      opts.TargetVMAF,
		MaxBudgetKbps:   opts.MaxBudgetKbps,
		AllowCodecs:     append([]string(nil), opts.AllowCodecs...),
		UserPinnedCodec: opts.UserPinnedCodec,
	}

	// ------------------------------------------------------------------
	// Stage 0 — F.4 per-content-type recipe override.
	//
	// Fires *before* the F.2 short-circuits so a recipe can flip
	// force_single_rung and have the ladder stage honour it. The predictor's
	// effective target VMAF is offset by target_vmaf_offset, but the
	// production-flip gate that ships models is NOT shifted by that value.
	// ------------------------------------------------------------------
	baseThresholds := DefaultConfidenceThresholds()
	if opts.ConfidenceThresholds != nil {
		baseThresholds = *opts.ConfidenceThresholds
	}
	recipes := opts.Recipes
	if recipes == nil {
		recipes = NewRecipeTable(".", log)
	}
	recipeClass := ResolveRecipeClass(*meta)
	recipe := recipes.ForClass(recipeClass)
	thresholds := applyRecipeThresholds(recipe, recipeClass, baseThresholds)

	targetVMAFOffset := recipeFloat(recipe, RecipeKeyTargetVMAFOffset, 0.0)
	effectivePredictorTarget := opts.TargetVMAF + targetVMAFOffset
	forceSingleRung := recipeBool(recipe, RecipeKeyForceSingleRung, false)
	saliencyIntensity := recipeString(recipe, RecipeKeySaliencyIntensity, "default")

	// ------------------------------------------------------------------
	// Stage 1 — ladder rung selection (short-circuit #1).
	// ------------------------------------------------------------------
	var rungs []int
	if ShouldShortCircuitSingleRungLadder(*meta, planState) || forceSingleRung {
		planState.Fired(SCLadderSingleRung)
		rungs = []int{meta.Height}
	} else {
		// Multi-rung path — production wiring delegates to pkg/ladder.
		rungs = []int{2160, 1440, 1080, 720, 540}
	}

	// ------------------------------------------------------------------
	// Stage 2 — codec shortlist (short-circuit #2).
	// ------------------------------------------------------------------
	var codecs []string
	if ShouldShortCircuitCodecPinned(*meta, planState) {
		planState.Fired(SCCodecPinned)
		if opts.UserPinnedCodec != "" {
			codecs = []string{opts.UserPinnedCodec}
		} else {
			codecs = append([]string(nil), opts.AllowCodecs...)
		}
	} else {
		// Production wiring delegates to compare.shortlist; the smoke path
		// keeps the full allow-list.
		codecs = append([]string(nil), opts.AllowCodecs...)
	}

	// ------------------------------------------------------------------
	// Stage 3 — HDR pipeline (short-circuit #5).
	// ------------------------------------------------------------------
	if ShouldShortCircuitSDRSkip(*meta, planState) {
		planState.Fired(SCSDRSkip)
		hdrInfo = nil
	}

	// ------------------------------------------------------------------
	// Stage 4 — sample-clip propagation (short-circuit #6).
	// ------------------------------------------------------------------
	propagatedClip := 0.0
	if ShouldShortCircuitSampleClipPropagate(*meta, planState) {
		planState.Fired(SCSampleClipPropagate)
		propagatedClip = meta.SampleClipSeconds
	}

	// ------------------------------------------------------------------
	// Stage 5 — per-cell predictor + escalation (short-circuit #3 plus the
	// F.3 confidence-aware override).
	//
	// In smoke mode we synthesise a GOSPEL verdict so the F.2 gate fires in
	// the unit smoke run; production wiring sets the verdict from the
	// validation report and the width from the conformal interval.
	// ------------------------------------------------------------------
	if opts.Smoke {
		planState.PredictorVerdict = VerdictGospel
	}

	intervalLookup := map[string]CellInterval{}
	for _, ci := range opts.CellIntervals {
		intervalLookup[cellKey(ci.Rung, ci.Codec)] = ci
	}

	var pred *predictor.Predictor
	var features predictor.ShotFeatures
	if !opts.Smoke {
		pred = opts.Predictor
		if pred == nil {
			// The analytical fallback: what the Python auto driver builds
			// with Predictor() and no model path.
			pred = predictor.New()
		}
		features = predictorFeaturesFromMeta(*meta)
	}

	escalations := make([]any, 0, len(rungs)*len(codecs))
	cells := make([]map[string]any, 0, len(rungs)*len(codecs))

	for _, rung := range rungs {
		for _, codecName := range codecs {
			// Per-cell state: the GOSPEL firing is recorded on the cell and
			// carried back up so the metadata block records that it fired at
			// least once.
			cellState := *planState
			cellState.ShortCircuits = append([]string(nil), planState.ShortCircuits...)
			if ShouldShortCircuitPredictorGospel(*meta, &cellState) {
				cellState.Fired(SCPredictorGospel)
				planState.Fired(SCPredictorGospel)
			}

			cellVerdict := planState.PredictorVerdict
			cellWidth := math.NaN()
			if ci, ok := intervalLookup[cellKey(rung, codecName)]; ok {
				cellVerdict = ci.Verdict
				cellWidth = ci.IntervalWidth
			} else if opts.CellIntervals == nil && opts.Smoke {
				// Synthetic smoke default: a tight interval below the gate so
				// the F.3 branch is exercised deterministically without ONNX.
				cellWidth = 1.0
			}

			decision, err := ConfidenceAwareEscalation(cellVerdict, cellWidth, thresholds)
			if err != nil {
				return Plan{}, err
			}

			cellHDRArgs := []any{}
			if hdrInfo != nil {
				for _, arg := range hdr.CodecArgs(codecName, hdrInfo, log) {
					cellHDRArgs = append(cellHDRArgs, arg)
				}
			}

			escalations = append(escalations, map[string]any{
				"rung":           rung,
				"codec":          codecName,
				"verdict":        orUnknown(cellVerdict),
				"interval_width": cellWidth,
				"decision":       string(decision),
			})

			crf := 23
			estimatedVMAF := opts.TargetVMAF
			estimatedBitrate := opts.MaxBudgetKbps
			predictionSource := "smoke-placeholder"
			if pred != nil {
				picked, err := pred.PickCRF(features, effectivePredictorTarget, codecName)
				if err != nil {
					return Plan{}, err
				}
				crf = picked
				estimatedVMAF = pred.PredictVMAF(features, crf, codecName)
				estimatedBitrate = estimateCellBitrateKbps(features, codecName, crf)
				predictionSource = "predictor"
			}

			cells = append(cells, map[string]any{
				"rung":                            rung,
				"codec":                           codecName,
				"verdict":                         orUnknown(firstNonEmpty(cellVerdict, planState.PredictorVerdict)),
				"crf":                             crf,
				"estimated_vmaf":                  estimatedVMAF,
				"estimated_bitrate_kbps":          estimatedBitrate,
				"hdr_args":                        cellHDRArgs,
				"sample_clip_seconds":             propagatedClip,
				"confidence_decision":             string(decision),
				"interval_width":                  cellWidth,
				"effective_predictor_target_vmaf": effectivePredictorTarget,
				"prediction_source":               predictionSource,
				"saliency_intensity":              saliencyIntensity,
			})
		}
	}

	// ------------------------------------------------------------------
	// Stage 6 — saliency gate (short-circuit #4). Otherwise production
	// wiring would apply the saliency stage to every cell.
	// ------------------------------------------------------------------
	if ShouldShortCircuitSkipSaliency(*meta, planState) {
		planState.Fired(SCSkipSaliency)
	}

	// ------------------------------------------------------------------
	// Stage 7 — per-shot refinement gate (short-circuit #7).
	// ------------------------------------------------------------------
	if ShouldShortCircuitSkipPerShot(*meta, planState) {
		planState.Fired(SCSkipPerShot)
	}

	// ------------------------------------------------------------------
	// Stage 8 — low-complexity source (short-circuit #8). Dormant when
	// complexity_score is 0.0 / NaN (no probe yet).
	// ------------------------------------------------------------------
	if ShouldShortCircuitLowComplexity(*meta, planState) {
		planState.Fired(SCLowComplexity)
	}

	// ------------------------------------------------------------------
	// Stage 9 — baseline already meets target (short-circuit #9). Dormant
	// when baseline_vmaf is 0.0 / NaN (no baseline yet).
	// ------------------------------------------------------------------
	if ShouldShortCircuitBaselineMeetsTarget(*meta, planState) {
		planState.Fired(SCBaselineMeetsTarget)
	}

	// ------------------------------------------------------------------
	// Stage 10 — per-cell no-two-pass gate (short-circuit #10). The flag is
	// resolved from the first codec in the list; an unknown codec resolves
	// to false, matching the Python KeyError branch.
	// ------------------------------------------------------------------
	if len(codecs) > 0 {
		supportsTwoPass := false
		if adapter, err := codecadapter.Get(codecs[0]); err == nil {
			supportsTwoPass = adapter.SupportsTwoPass
		}
		planState.AdapterSupportsTwoPass = &supportsTwoPass
		if ShouldShortCircuitNoTwoPass(*meta, planState) {
			planState.Fired(SCNoTwoPass)
		}
	}

	winner := PickAutoWinner(cells, opts.TargetVMAF, opts.MaxBudgetKbps)
	markSelectedCell(cells, winner)

	allowCodecs := make([]any, len(opts.AllowCodecs))
	for i, c := range opts.AllowCodecs {
		allowCodecs[i] = c
	}
	var pinned any
	if opts.UserPinnedCodec != "" {
		pinned = opts.UserPinnedCodec
	}
	shortCircuits := make([]any, len(planState.ShortCircuits))
	for i, sc := range planState.ShortCircuits {
		shortCircuits[i] = sc
	}

	metadata := map[string]any{
		"src":                          opts.Src,
		"target_vmaf":                  opts.TargetVMAF,
		"max_budget_kbps":              opts.MaxBudgetKbps,
		"allow_codecs":                 allowCodecs,
		"user_pinned_codec":            pinned,
		"smoke":                        opts.Smoke,
		"source_meta":                  meta.asMap(),
		"short_circuits":               shortCircuits,
		"confidence_aware_escalations": escalations,
		"confidence_thresholds": map[string]any{
			"tight_interval_max_width": thresholds.TightIntervalMaxWidth,
			"wide_interval_min_width":  thresholds.WideIntervalMinWidth,
			"source":                   thresholds.Source,
		},
		"recipe_applied":                  recipeClass,
		"recipe_overrides":                recipe,
		"effective_predictor_target_vmaf": effectivePredictorTarget,
		"winner":                          winner,
	}

	return Plan{Cells: cells, Metadata: metadata}, nil
}

// applyRecipeThresholds folds the recipe's tight_interval_max_width override
// into the F.3 gates. wide_interval_min_width is preserved verbatim — F.4
// only tightens or loosens the predictor-confidence gate, never the hard
// "force escalation" wall — and a recipe asking for a tight wider than the
// corpus-fit wide is silently capped so the tight <= wide invariant holds.
func applyRecipeThresholds(
	recipe map[string]any,
	recipeClass string,
	base ConfidenceThresholds,
) ConfidenceThresholds {
	raw, ok := recipe[RecipeKeyTightIntervalMaxWidth]
	if !ok {
		return base
	}
	tight, ok := toFloat(raw)
	if !ok {
		return base
	}
	if tight > base.WideIntervalMinWidth {
		tight = base.WideIntervalMinWidth
	}
	return ConfidenceThresholds{
		TightIntervalMaxWidth: tight,
		WideIntervalMinWidth:  base.WideIntervalMinWidth,
		Source:                "recipe:" + recipeClass + "/" + base.Source,
	}
}

func recipeFloat(recipe map[string]any, key string, fallback float64) float64 {
	if v, ok := recipe[key]; ok {
		if f, ok := toFloat(v); ok {
			return f
		}
	}
	return fallback
}

func recipeBool(recipe map[string]any, key string, fallback bool) bool {
	if v, ok := recipe[key]; ok {
		if b, ok := v.(bool); ok {
			return b
		}
	}
	return fallback
}

func recipeString(recipe map[string]any, key, fallback string) string {
	if v, ok := recipe[key]; ok {
		if s, ok := v.(string); ok {
			return s
		}
	}
	return fallback
}

func toFloat(v any) (float64, bool) {
	switch value := v.(type) {
	case float64:
		return value, true
	case float32:
		return float64(value), true
	case int:
		return float64(value), true
	case int64:
		return float64(value), true
	case json.Number:
		f, err := value.Float64()
		return f, err == nil
	default:
		return 0, false
	}
}

func cellKey(rung int, codecName string) string {
	return strconv.Itoa(rung) + "\x00" + codecName
}

func orUnknown(verdict string) string {
	if verdict == "" {
		return VerdictUnknown
	}
	return verdict
}

func firstNonEmpty(values ...string) string {
	for _, v := range values {
		if v != "" {
			return v
		}
	}
	return ""
}

// EmitPlanJSON serialises plan to a stable JSON string.
//
// The schema is the public contract for downstream consumers (the MCP server,
// the CI corpus collector, post-hoc speedup analysis). Keys are sorted so the
// output is reproducible across runs, and the bytes match CPython's
// json.dumps(payload, indent=2, sort_keys=True) — including the bare NaN
// token an uncalibrated conformal interval_width produces.
func EmitPlanJSON(plan Plan) (string, error) {
	cells := make([]any, len(plan.Cells))
	for i, cell := range plan.Cells {
		cells[i] = cell
	}
	payload := map[string]any{
		"cells":    cells,
		"metadata": plan.Metadata,
	}
	return pyjson.MarshalIndentSorted(payload, 2)
}

// SortedShortCircuitNames returns every declared short-circuit identifier,
// sorted. Handy for CLI help text and for tests that assert the vocabulary
// has not drifted from the Python enum.
func SortedShortCircuitNames() []string {
	out := make([]string, 0, len(ShortCircuitPredicates))
	for _, p := range ShortCircuitPredicates {
		out = append(out, string(p.Name))
	}
	sort.Strings(out)
	return out
}
