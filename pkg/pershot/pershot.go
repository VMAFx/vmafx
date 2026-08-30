// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

// Package pershot implements per-shot CRF tuning: the "Netflix per-shot
// encoding" table-stakes feature of vmafx-tune.
//
// Go port of tools/vmaf-tune/src/vmaftune/per_shot.py (Phase D). The
// pipeline is:
//
//  1. DetectShots cuts the source into shot ranges by shelling out to the
//     fork's C-side vmaf-perShot binary (ADR-0222), which wraps TransNet V2
//     (ADR-0223). A missing or failing binary degrades to a single shot.
//  2. SplitLongShots slices any shot longer than a uniform time window into
//     equal sub-shots, so an under-cutting detector still yields a
//     non-degenerate timeline (ADR-0513).
//  3. Tune drives a pluggable PredicateFn per shot to pick a CRF. The CLI
//     wires the CRF bisect (pkg/bisect); tests and custom operators inject
//     deterministic selectors.
//  4. Merge collapses the recommendations into an EncodingPlan: one ffmpeg
//     command per segment plus a concat-demuxer command that stitches them.
//
// Like the Python, the planner stops short of *running* the segment
// encodes — operators inspect or execute the emitted command list. Native
// per-codec zone/qpfile emission remains a later optimisation over the
// segment-and-concat path.
//
// Not ported from per_shot.py's CLI surface (see the package tests and
// docs/usage/vmafx-tune-go.md for the operator-facing statement):
//
//   - --predicate-module MODULE:CALLABLE. The Python hook imports an
//     arbitrary Python callable at runtime. Go has no runtime import; the
//     equivalent seam is PredicateFn, available to Go callers only.
//   - --fast-nr NR early-elimination. It runs the nr_metric_v1 ONNX model
//     through onnxruntime for every bisect midpoint; there is no ONNX
//     runtime binding in this module's dependency set.
package pershot

import (
	"context"
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/VMAFx/vmafx/pkg/encoder"
)

// DefaultPerShotBin is the shot-detector binary looked up on PATH.
const DefaultPerShotBin = "vmaf-perShot"

// detectTimeout bounds the vmaf-perShot invocation. TransNet inference over
// a long source is not instant, but an unbounded wait would let a wedged
// detector hang the whole tuning run.
const detectTimeout = 30 * time.Minute

// Shot is a half-open frame range [StartFrame, EndFrame).
//
// StartFrame is inclusive, EndFrame exclusive — Python-slice convention.
// vmaf-perShot's own output uses an *inclusive* end frame; the parsers in
// this package normalise into the half-open form.
type Shot struct {
	StartFrame int `json:"start_frame"`
	EndFrame   int `json:"end_frame"`
}

// NewShot builds a Shot, rejecting inverted or negative ranges the same way
// the Python dataclass __post_init__ does.
func NewShot(start, end int) (Shot, error) {
	if start < 0 || end <= start {
		return Shot{}, fmt.Errorf("invalid shot range: [%d, %d)", start, end)
	}
	return Shot{StartFrame: start, EndFrame: end}, nil
}

// Length returns the shot length in frames.
func (s Shot) Length() int { return s.EndFrame - s.StartFrame }

// Recommendation is one shot's CRF pick.
//
// BitratekBps carries the encoded segment bitrate when the predicate
// measured one (the bisect predicate does). It is NaN for dry-run or
// synthetic predicates that never encode, and serialises as JSON null
// (ADR-0531).
type Recommendation struct {
	Shot          Shot
	CRF           int
	PredictedVMAF float64
	BitratekBps   float64
}

// EncodingPlan is the segment list plus the ffmpeg commands that realise the
// per-shot encode.
//
// The plan is split into per-shot single-encode commands plus a final
// concat-demuxer command. Segment files are independent, so callers may run
// the segment commands sequentially or in parallel.
type EncodingPlan struct {
	Recommendations []Recommendation
	Encoder         string
	Framerate       float64
	SegmentCommands [][]string
	ConcatCommand   []string
	ConcatListing   string
	// SegmentDir is the directory the segment commands write into. Exposed
	// so callers can create it (and drop the concat listing there) without
	// re-deriving the same defaulting rules.
	SegmentDir string
}

// ---------------------------------------------------------------------------
// Shot detection
// ---------------------------------------------------------------------------

// DetectOptions configures DetectShots.
type DetectOptions struct {
	// Width / Height / PixFmt / Bitdepth describe the source for the
	// detector's raw-YUV reader.
	Width, Height int
	PixFmt        string
	Bitdepth      int

	// TotalFrames feeds the single-shot fallback used when the detector is
	// unavailable. Zero or negative means "unknown".
	TotalFrames int

	// Bin is the detector binary. Empty uses DefaultPerShotBin.
	Bin string

	// DiffThreshold overrides the detector's --diff-threshold (the
	// mean-absolute-luma-delta cutoff for cut classification; lower yields
	// more shots). Nil keeps the binary's compiled-in default of 12.0 on
	// 8-bit content (ADR-0513).
	DiffThreshold *float64

	// Framerate is required when MaxShotDurationSec is set, to convert the
	// window from seconds to frames.
	Framerate float64

	// MaxShotDurationSec enables the uniform-time-window splitter. Zero or
	// negative disables it.
	MaxShotDurationSec float64

	// Runner is the subprocess seam. It runs the detector and returns
	// whether the process exited zero. Tests inject a stub; production
	// callers leave it nil.
	Runner func(ctx context.Context, name string, args ...string) (ok bool)
}

func (o DetectOptions) bin() string {
	if o.Bin == "" {
		return DefaultPerShotBin
	}
	return o.Bin
}

// pixFmtToDetector maps an ffmpeg pix_fmt name onto vmaf-perShot's
// --pixel_format vocabulary. Mirrors per_shot._bitdepth_aware_pix, including
// its substring (not prefix) matching and 420 fallback.
func pixFmtToDetector(pixFmt string) string {
	if strings.Contains(pixFmt, "422") {
		return "422"
	}
	if strings.Contains(pixFmt, "444") {
		return "444"
	}
	return "420"
}

// singleShotFallback returns the shot list used when detection is
// unavailable. Mirrors per_shot._single_shot_fallback: a sentinel [0, 1)
// range when the frame count is unknown, otherwise one shot spanning the
// whole clip.
func singleShotFallback(totalFrames int) []Shot {
	if totalFrames <= 0 {
		return []Shot{{StartFrame: 0, EndFrame: 1}}
	}
	return []Shot{{StartFrame: 0, EndFrame: totalFrames}}
}

// DetectShots returns the shot boundary list for videoPath.
//
// Calls the fork's C-side vmaf-perShot binary and falls back to a single
// shot spanning the clip when the binary is missing or fails. When
// MaxShotDurationSec and Framerate are both set, SplitLongShots is applied
// on top of the detector output.
func DetectShots(ctx context.Context, videoPath string, opts DetectOptions) []Shot {
	shots, _ := DetectShotsStatus(ctx, videoPath, opts)
	if opts.MaxShotDurationSec > 0 && opts.Framerate > 0 {
		shots = SplitLongShots(shots, opts.MaxShotDurationSec, opts.Framerate)
	}
	return shots
}

// DetectShotsStatus is DetectShots without the splitter, additionally
// reporting whether the detector actually ran and produced shot data.
//
// Mirrors per_shot._detect_shots_with_status: callers that summarise a
// corpus need to tell a genuine one-shot source apart from a fallback, which
// the list-only return shape cannot carry.
func DetectShotsStatus(ctx context.Context, videoPath string, opts DetectOptions) ([]Shot, bool) {
	bin := opts.bin()
	if opts.Runner == nil {
		if _, err := exec.LookPath(bin); err != nil {
			return singleShotFallback(opts.TotalFrames), false
		}
	}

	// vmaf-perShot writes "vmaf-perShot: wrote N shot(s) to PATH" to stdout
	// regardless of the --output value, including "--output -" (which lands
	// the JSON in a file literally named "-"). Always use a real temp file so
	// stdout stays progress-only and the JSON is read back from the file the
	// binary actually wrote.
	tmp, err := os.CreateTemp("", "vmaf_pershot_*.json")
	if err != nil {
		return singleShotFallback(opts.TotalFrames), false
	}
	tmpPath := tmp.Name()
	if closeErr := tmp.Close(); closeErr != nil {
		return singleShotFallback(opts.TotalFrames), false
	}
	defer func() {
		// Best-effort cleanup; an unlink failure is non-fatal.
		_ = os.Remove(tmpPath)
	}()

	bitdepth := opts.Bitdepth
	if bitdepth == 0 {
		bitdepth = 8
	}
	args := []string{
		"--reference", videoPath,
		"--width", strconv.Itoa(opts.Width),
		"--height", strconv.Itoa(opts.Height),
		"--pixel_format", pixFmtToDetector(opts.PixFmt),
		"--bitdepth", strconv.Itoa(bitdepth),
		"--output", tmpPath,
		"--format", "json",
	}
	// ADR-0513: thread the user-tunable cut threshold through so operators
	// can dial sensitivity per content class without rebuilding the binary.
	if opts.DiffThreshold != nil {
		args = append(args, "--diff-threshold",
			strconv.FormatFloat(*opts.DiffThreshold, 'f', 6, 64))
	}

	if !runDetector(ctx, opts, bin, args) {
		return singleShotFallback(opts.TotalFrames), false
	}

	// #nosec G304 -- tmpPath is this function's own os.CreateTemp output.
	payload, readErr := os.ReadFile(tmpPath)
	if readErr != nil || len(strings.TrimSpace(string(payload))) == 0 {
		return singleShotFallback(opts.TotalFrames), false
	}
	shots, parseErr := ParseJSON(payload)
	if parseErr != nil {
		return singleShotFallback(opts.TotalFrames), false
	}
	return shots, true
}

// runDetector executes the detector, honouring the injected Runner seam.
func runDetector(ctx context.Context, opts DetectOptions, bin string, args []string) bool {
	if opts.Runner != nil {
		return opts.Runner(ctx, bin, args...)
	}
	dctx, cancel := context.WithTimeout(ctx, detectTimeout)
	defer cancel()
	// #nosec G204 -- bin is the operator-configured --per-shot-bin; args are
	// fixed flags plus the caller's source path and this call's temp output.
	cmd := exec.CommandContext(dctx, bin, args...)
	cmd.WaitDelay = time.Second
	return cmd.Run() == nil
}

// perShotPayload is the JSON document vmaf-perShot writes. Only the frame
// range is consumed here; the per-shot complexity / motion / predicted_crf
// columns are the C tool's own encoder hint, not an input to this planner.
type perShotPayload struct {
	Shots []struct {
		StartFrame int `json:"start_frame"`
		EndFrame   int `json:"end_frame"`
	} `json:"shots"`
}

// ParseJSON parses vmaf-perShot's JSON output into a shot list.
//
// The source schema uses an *inclusive* end_frame; this normalises to the
// half-open form by adding one. An empty shot array degrades to the [0, 1)
// sentinel, matching per_shot._parse_per_shot_json.
func ParseJSON(payload []byte) ([]Shot, error) {
	var doc perShotPayload
	if err := json.Unmarshal(payload, &doc); err != nil {
		return nil, fmt.Errorf("parse vmaf-perShot JSON: %w", err)
	}
	out := make([]Shot, 0, len(doc.Shots))
	for _, entry := range doc.Shots {
		shot, err := NewShot(entry.StartFrame, entry.EndFrame+1)
		if err != nil {
			return nil, err
		}
		out = append(out, shot)
	}
	if len(out) == 0 {
		return []Shot{{StartFrame: 0, EndFrame: 1}}, nil
	}
	return out, nil
}

// ParseCSV parses vmaf-perShot's CSV sidecar into a shot list, for callers
// that already have the CSV from a prior detector run. Mirrors
// per_shot.parse_per_shot_csv, including the inclusive-to-half-open
// conversion.
func ParseCSV(payload string) ([]Shot, error) {
	reader := csv.NewReader(strings.NewReader(payload))
	header, err := reader.Read()
	if err != nil {
		if errors.Is(err, io.EOF) {
			return nil, nil
		}
		return nil, fmt.Errorf("read vmaf-perShot CSV header: %w", err)
	}
	startIdx, endIdx := -1, -1
	for i, name := range header {
		switch strings.TrimSpace(name) {
		case "start_frame":
			startIdx = i
		case "end_frame":
			endIdx = i
		}
	}
	if startIdx < 0 || endIdx < 0 {
		return nil, errors.New("vmaf-perShot CSV missing start_frame / end_frame columns")
	}

	var out []Shot
	for {
		rec, readErr := reader.Read()
		if errors.Is(readErr, io.EOF) {
			break
		}
		if readErr != nil {
			return nil, fmt.Errorf("read vmaf-perShot CSV row: %w", readErr)
		}
		if startIdx >= len(rec) || endIdx >= len(rec) {
			return nil, fmt.Errorf("vmaf-perShot CSV row too short: %v", rec)
		}
		start, sErr := strconv.Atoi(strings.TrimSpace(rec[startIdx]))
		end, eErr := strconv.Atoi(strings.TrimSpace(rec[endIdx]))
		if sErr != nil || eErr != nil {
			return nil, fmt.Errorf("vmaf-perShot CSV row has non-integer frames: %v", rec)
		}
		shot, shotErr := NewShot(start, end+1)
		if shotErr != nil {
			return nil, shotErr
		}
		out = append(out, shot)
	}
	return out, nil
}

// SplitLongShots slices any shot longer than maxDurationSec into uniform
// sub-shots.
//
// Guards downstream tuning against an under-cutting detector: when
// vmaf-perShot's mean-absolute-luma-delta heuristic cannot see a cut (fades,
// short clips, content the empirical threshold under-fits), the splitter
// still partitions the timeline into ceil(L/W) pieces, where L is the shot
// length in seconds and W the window. The remainder is distributed so
// partitions differ by at most one frame. ADR-0513.
//
// A non-positive window or a non-finite / non-positive framerate is a no-op:
// the caller has disabled the splitter or has no usable conversion factor.
func SplitLongShots(shots []Shot, maxDurationSec, framerate float64) []Shot {
	if len(shots) == 0 {
		return shots
	}
	if math.IsNaN(framerate) || math.IsInf(framerate, 0) || framerate <= 0 {
		return shots
	}
	if maxDurationSec <= 0 {
		return shots
	}
	maxFrames := int(math.Round(maxDurationSec * framerate))
	if maxFrames < 1 {
		maxFrames = 1
	}

	out := make([]Shot, 0, len(shots))
	for _, shot := range shots {
		length := shot.Length()
		if length <= maxFrames {
			out = append(out, shot)
			continue
		}
		nParts := (length + maxFrames - 1) / maxFrames
		base := length / nParts
		extra := length - base*nParts
		cursor := shot.StartFrame
		for idx := range nParts {
			partLen := base
			if idx < extra {
				partLen++
			}
			out = append(out, Shot{StartFrame: cursor, EndFrame: cursor + partLen})
			cursor += partLen
		}
	}
	return out
}

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

// PredicateFn picks a CRF for one shot at a target VMAF.
//
// It returns the chosen CRF and the measured (or predicted) VMAF. This is
// the integration seam the CLI binds to the CRF bisect; tests and advanced
// Go callers inject deterministic selectors. Mirrors per_shot.PredicateFn.
type PredicateFn func(shot Shot, targetVMAF float64, enc string) (int, float64, error)

// TuneParams configures Tune.
type TuneParams struct {
	// TargetVMAF is the quality floor handed to the predicate.
	TargetVMAF float64

	// Encoder is the codec adapter name. Empty defaults to libx264.
	Encoder string

	// Predicate is the per-shot selector. Nil uses DefaultPredicate.
	Predicate PredicateFn
}

// DefaultPredicate is the trivial fallback used when no real bisect is
// wired: it returns the codec's default quality alongside the requested
// target, so library dry runs stay deterministic without launching encodes.
// Mirrors per_shot._default_predicate.
func DefaultPredicate(a encoder.Adapter) PredicateFn {
	return func(_ Shot, targetVMAF float64, _ string) (int, float64, error) {
		return a.QualityDefault, targetVMAF, nil
	}
}

// Tune picks a per-shot CRF for every shot.
//
// Each predicate result is clamped into the codec's informative quality
// window, mirroring per_shot.tune_per_shot. BitratekBps is left NaN; the
// caller attaches measured bitrates afterwards (see WithBitrates).
func Tune(shots []Shot, params TuneParams) ([]Recommendation, error) {
	if len(shots) == 0 {
		return nil, errors.New("per-shot tuning requires at least one shot")
	}
	codec := params.Encoder
	if codec == "" {
		codec = "libx264"
	}
	adapter, err := encoder.GetAdapter(codec)
	if err != nil {
		return nil, err
	}
	pred := params.Predicate
	if pred == nil {
		pred = DefaultPredicate(adapter)
	}

	recs := make([]Recommendation, 0, len(shots))
	for _, shot := range shots {
		crf, predicted, predErr := pred(shot, params.TargetVMAF, codec)
		if predErr != nil {
			return nil, fmt.Errorf("shot [%d, %d): %w",
				shot.StartFrame, shot.EndFrame, predErr)
		}
		recs = append(recs, Recommendation{
			Shot:          shot,
			CRF:           adapter.ClampQuality(crf),
			PredictedVMAF: predicted,
			BitratekBps:   math.NaN(),
		})
	}
	return recs, nil
}

// WithBitrates returns a copy of recs with each recommendation's BitratekBps
// replaced by the sidecar entry for its shot, when one exists.
//
// Mirrors the dataclasses.replace pass in cli._run_tune_per_shot: the bisect
// predicate records measured bitrates in a side table keyed by frame range
// rather than widening the PredicateFn return type (ADR-0536). Shots absent
// from the sidecar keep their NaN default.
func WithBitrates(recs []Recommendation, sidecar map[Shot]float64) []Recommendation {
	out := make([]Recommendation, len(recs))
	copy(out, recs)
	for i := range out {
		if br, ok := sidecar[out[i].Shot]; ok {
			out[i].BitratekBps = br
		}
	}
	return out
}

// ---------------------------------------------------------------------------
// Plan construction
// ---------------------------------------------------------------------------

// MergeParams configures Merge.
type MergeParams struct {
	// Source is the reference the segment encodes read from.
	Source string

	// Output is the final concatenated encode destination.
	Output string

	// Framerate converts shot frame offsets to the "-ss" seek time.
	Framerate float64

	// Encoder is the codec adapter name. Empty defaults to libx264.
	Encoder string

	// SegmentDir holds the per-shot segment files. Empty defaults to
	// <Output dir>/segments.
	SegmentDir string

	// FFmpegBin is the ffmpeg binary named in the emitted commands.
	// Empty defaults to "ffmpeg".
	FFmpegBin string
}

// Merge collapses per-shot recommendations into an EncodingPlan.
//
// One ffmpeg invocation per segment (input-seek "-ss" plus "-frames:v"
// derived from the half-open range) plus a final concat-demuxer command.
// The per-segment codec argv is delegated to the codec adapter (HP-1 /
// ADR-0297) so non-x264 codecs get their codec-correct flags rather than a
// hardcoded "-preset ... -crf ..." pair.
func Merge(recs []Recommendation, params MergeParams) (EncodingPlan, error) {
	if len(recs) == 0 {
		return EncodingPlan{}, errors.New("per-shot merge requires at least one recommendation")
	}
	codec := params.Encoder
	if codec == "" {
		codec = "libx264"
	}
	adapter, err := encoder.GetAdapter(codec)
	if err != nil {
		return EncodingPlan{}, err
	}
	ffmpegBin := params.FFmpegBin
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	if params.Framerate <= 0 || math.IsNaN(params.Framerate) || math.IsInf(params.Framerate, 0) {
		return EncodingPlan{}, fmt.Errorf(
			"framerate must be positive and finite, got %v", params.Framerate)
	}
	segDir := SegmentDirFor(params.SegmentDir, params.Output)

	// The plan carries a CRF only; the preset comes from the adapter. Note
	// this is deliberately independent of the bisect's --preset: the Python
	// merge_shots never forwards it either, so the emitted plan is
	// reproducible from the plan JSON alone.
	preset := adapter.SegmentPreset()

	segmentCmds := make([][]string, 0, len(recs))
	listing := make([]string, 0, len(recs))
	for idx, rec := range recs {
		segPath := filepath.Join(segDir, fmt.Sprintf("shot_%04d.mp4", idx))
		codecArgs, argErr := adapter.CodecArgs(preset, rec.CRF)
		if argErr != nil {
			return EncodingPlan{}, argErr
		}
		startSeconds := float64(rec.Shot.StartFrame) / params.Framerate
		cmd := []string{
			ffmpegBin,
			"-y",
			"-hide_banner",
			"-ss", strconv.FormatFloat(startSeconds, 'f', 6, 64),
			"-i", params.Source,
			"-frames:v", strconv.Itoa(rec.Shot.Length()),
		}
		cmd = append(cmd, codecArgs...)
		cmd = append(cmd, segPath)
		segmentCmds = append(segmentCmds, cmd)
		// The concat demuxer wants POSIX-style paths in its listing.
		listing = append(listing, "file '"+filepath.ToSlash(segPath)+"'")
	}

	concatCmd := []string{
		ffmpegBin,
		"-y",
		"-hide_banner",
		"-f", "concat",
		"-safe", "0",
		"-i", filepath.ToSlash(filepath.Join(segDir, "concat.txt")),
		"-c", "copy",
		params.Output,
	}

	out := make([]Recommendation, len(recs))
	copy(out, recs)
	return EncodingPlan{
		Recommendations: out,
		Encoder:         adapter.Name,
		Framerate:       params.Framerate,
		SegmentCommands: segmentCmds,
		ConcatCommand:   concatCmd,
		ConcatListing:   strings.Join(listing, "\n") + "\n",
		SegmentDir:      segDir,
	}, nil
}

// SegmentDirFor resolves the segment directory: the explicit override when
// set, otherwise a "segments" child of the output file's directory.
func SegmentDirFor(segmentDir, output string) string {
	if segmentDir != "" {
		return segmentDir
	}
	return filepath.Join(filepath.Dir(output), "segments")
}

// WriteConcatListing persists the concat-demuxer listing to listingPath,
// creating parent directories as needed.
//
// Kept separate from Merge so a plan can be inspected and tested without
// filesystem side effects.
func WriteConcatListing(plan EncodingPlan, listingPath string) error {
	// G301: 0o750 keeps the segments tree owner+group accessible only.
	if err := os.MkdirAll(filepath.Dir(listingPath), 0o750); err != nil {
		return fmt.Errorf("create segments dir: %w", err)
	}
	// G306: 0o600 — the listing embeds workspace paths.
	if err := os.WriteFile(listingPath, []byte(plan.ConcatListing), 0o600); err != nil {
		return fmt.Errorf("write concat listing: %w", err)
	}
	return nil
}

// PlanToShellScript renders a plan as a copy-paste shell script.
//
// Diagnostics only, exactly as in the Python: the argv is constructed
// in-process and joined with plain spaces, so it is human-readable rather
// than safe for shell evaluation of adversarial paths.
func PlanToShellScript(plan EncodingPlan) string {
	var sb strings.Builder
	sb.WriteString("#!/bin/sh\nset -eu\n")
	for _, cmd := range plan.SegmentCommands {
		sb.WriteString(strings.Join(cmd, " "))
		sb.WriteByte('\n')
	}
	sb.WriteString(strings.Join(plan.ConcatCommand, " "))
	sb.WriteByte('\n')
	return sb.String()
}

// ---------------------------------------------------------------------------
// Shot-metadata aggregation
// ---------------------------------------------------------------------------

// Metadata aggregates shot statistics for one source. All fields are zero
// when shot detection was unavailable; Count > 0 with AvgDurationSec > 0 is
// the contract for "real shot data was captured".
type Metadata struct {
	Count          int     `json:"count"`
	AvgDurationSec float64 `json:"avg_duration_sec"`
	DurationStdSec float64 `json:"duration_std_sec"`
}

// isFallbackShotList detects the single-shot fallback: DetectShots emits
// either the sentinel [0, 1) when no frame count is known, or one shot
// spanning the clip when the binary failed. Both mean "detection was not
// real", and the caller should treat the metadata as missing rather than as
// one giant shot.
func isFallbackShotList(shots []Shot) bool {
	if len(shots) != 1 {
		return false
	}
	return shots[0].StartFrame == 0 && shots[0].Length() <= 1
}

// Summarise computes the count, mean and standard deviation of shot lengths
// in seconds.
//
// Returns the all-zero sentinel for a fallback shot list or a non-finite /
// non-positive framerate. Uses the *population* standard deviation so the
// result is well-defined for a single shot (the sample std would be NaN and
// force every caller to special-case the singleton). Mirrors
// per_shot.summarise_shots.
func Summarise(shots []Shot, framerate float64) Metadata {
	if len(shots) == 0 {
		return Metadata{}
	}
	if math.IsNaN(framerate) || math.IsInf(framerate, 0) || framerate <= 0 {
		return Metadata{}
	}
	if isFallbackShotList(shots) {
		return Metadata{}
	}

	durations := make([]float64, len(shots))
	sum := 0.0
	for i, shot := range shots {
		durations[i] = float64(shot.Length()) / framerate
		sum += durations[i]
	}
	mean := sum / float64(len(durations))

	std := 0.0
	if len(durations) > 1 {
		variance := 0.0
		for _, d := range durations {
			variance += (d - mean) * (d - mean)
		}
		std = math.Sqrt(variance / float64(len(durations)))
	}
	return Metadata{
		Count:          len(shots),
		AvgDurationSec: mean,
		DurationStdSec: std,
	}
}
