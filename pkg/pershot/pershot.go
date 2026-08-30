// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package pershot ports the shot-detection half of
// tools/vmaf-tune/src/vmaftune/per_shot.py — the part the `predict`
// subcommand needs.
//
// Shot boundaries come from the fork's C-side vmaf-perShot binary (ADR-0222),
// which wraps TransNet V2 (ADR-0223). When that binary is missing or fails,
// detection degrades to a single shot spanning the whole clip so the caller
// always gets a usable timeline.
//
// Scope note: the encoding-plan half of per_shot.py (tune_per_shot,
// merge_shots, EncodingPlan, the segment-and-concat ffmpeg emitter) belongs to
// the `tune-per-shot` subcommand and is NOT ported here — that subcommand is
// another group's work.
package pershot

import (
	"context"
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Shot is a half-open frame range [StartFrame, EndFrame).
//
// vmaf-perShot's JSON and CSV outputs use an INCLUSIVE end_frame; the parsers
// here normalise to half-open, matching the Python original.
type Shot struct {
	StartFrame int
	EndFrame   int
}

// Length returns the shot's frame count.
func (s Shot) Length() int { return s.EndFrame - s.StartFrame }

// Validate rejects the degenerate ranges the Python dataclass rejects in
// __post_init__.
func (s Shot) Validate() error {
	if s.StartFrame < 0 || s.EndFrame <= s.StartFrame {
		return fmt.Errorf("invalid shot range: [%d, %d)", s.StartFrame, s.EndFrame)
	}
	return nil
}

// Options configures DetectShots.
type Options struct {
	Width  int
	Height int
	PixFmt string
	// Bitdepth is forwarded to vmaf-perShot as --bitdepth.
	Bitdepth int
	// TotalFrames drives the single-shot fallback. 0 emits the sentinel
	// [0, 1) range downstream code pattern-matches on.
	TotalFrames int
	// PerShotBin is the detector binary; "" means "vmaf-perShot on PATH".
	PerShotBin string
	// DiffThreshold overrides the C-side --diff-threshold (luma
	// mean-absolute-delta cutoff). nil keeps the binary's compiled-in
	// default (12.0 in the 8-bit domain). ADR-0512.
	DiffThreshold *float64
	// MaxShotDurationSec, with Framerate, slices any shot longer than the
	// window into uniform sub-shots. 0 disables the splitter.
	MaxShotDurationSec float64
	Framerate          float64
}

// Runner is the subprocess seam. Tests inject a stub.
type Runner func(ctx context.Context, argv []string) (exitStatus int, err error)

// defaultDetectTimeout bounds one vmaf-perShot invocation. TransNet V2 over a
// feature-length source is slow but not unbounded; a wedged detector must not
// hang the predict run forever.
const defaultDetectTimeout = 30 * time.Minute

// defaultRunner runs argv with a timeout and reports its exit status.
func defaultRunner(ctx context.Context, argv []string) (int, error) {
	cancel := func() {}
	if _, hasDeadline := ctx.Deadline(); !hasDeadline {
		ctx, cancel = context.WithTimeout(ctx, defaultDetectTimeout)
	}
	defer cancel()

	// #nosec G204 -- argv[0] is the operator-configured detector binary and
	// argv[1:] is assembled by BuildCommand from validated CLI flags.
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	cmd.WaitDelay = 2 * time.Second
	if err := cmd.Run(); err != nil {
		var exitErr *exec.ExitError
		if errors.As(err, &exitErr) {
			return exitErr.ExitCode(), nil
		}
		return 1, err
	}
	return 0, nil
}

// bitdepthAwarePixFmt maps an ffmpeg pix_fmt name onto vmaf-perShot's
// --pixel_format vocabulary.
func bitdepthAwarePixFmt(pixFmt string) string {
	switch {
	case strings.Contains(pixFmt, "422"):
		return "422"
	case strings.Contains(pixFmt, "444"):
		return "444"
	default:
		return "420"
	}
}

// BuildCommand composes the vmaf-perShot argv. Pure function for test pinning.
func BuildCommand(videoPath, outputPath string, opts Options) []string {
	bin := opts.PerShotBin
	if bin == "" {
		bin = "vmaf-perShot"
	}
	pixFmt := opts.PixFmt
	if pixFmt == "" {
		pixFmt = "yuv420p"
	}
	bitdepth := opts.Bitdepth
	if bitdepth == 0 {
		bitdepth = 8
	}
	cmd := []string{
		bin,
		"--reference", videoPath,
		"--width", strconv.Itoa(opts.Width),
		"--height", strconv.Itoa(opts.Height),
		"--pixel_format", bitdepthAwarePixFmt(pixFmt),
		"--bitdepth", strconv.Itoa(bitdepth),
		"--output", outputPath,
		"--format", "json",
	}
	if opts.DiffThreshold != nil {
		cmd = append(cmd, "--diff-threshold",
			strconv.FormatFloat(*opts.DiffThreshold, 'f', 6, 64))
	}
	return cmd
}

// SingleShotFallback returns the one-shot timeline used when detection is
// unavailable. totalFrames <= 0 emits the [0, 1) sentinel range that
// downstream code pattern-matches on: an end above start keeps Shot valid
// without lying about the real length.
func SingleShotFallback(totalFrames int) []Shot {
	if totalFrames <= 0 {
		return []Shot{{StartFrame: 0, EndFrame: 1}}
	}
	return []Shot{{StartFrame: 0, EndFrame: totalFrames}}
}

// Detect returns the shot list for videoPath plus whether real detection
// succeeded. ok=false means the caller is looking at the fallback, which
// matters to the corpus summariser (a genuine one-shot source and a failed
// detector produce the same list).
func Detect(ctx context.Context, videoPath string, opts Options, runner Runner) ([]Shot, bool) {
	shots, ok := detect(ctx, videoPath, opts, runner)
	if opts.MaxShotDurationSec > 0 && opts.Framerate > 0 {
		shots = SplitLongShots(shots, opts.MaxShotDurationSec, opts.Framerate)
	}
	return shots, ok
}

// detect runs the detector and parses its output, falling back on any failure.
func detect(ctx context.Context, videoPath string, opts Options, runner Runner) ([]Shot, bool) {
	bin := opts.PerShotBin
	if bin == "" {
		bin = "vmaf-perShot"
	}
	if runner == nil {
		if _, err := exec.LookPath(bin); err != nil {
			return SingleShotFallback(opts.TotalFrames), false
		}
		runner = defaultRunner
	}

	// vmaf-perShot always writes a progress line to stdout regardless of
	// --output (including "--output -"), so the JSON must go to a real file:
	// parsing stdout picks up the progress string instead.
	tmp, err := os.CreateTemp("", "vmaf_pershot_*.json")
	if err != nil {
		return SingleShotFallback(opts.TotalFrames), false
	}
	tmpPath := tmp.Name()
	if closeErr := tmp.Close(); closeErr != nil {
		return SingleShotFallback(opts.TotalFrames), false
	}
	defer func() {
		if rmErr := os.Remove(tmpPath); rmErr != nil {
			_ = rmErr // best-effort cleanup
		}
	}()

	argv := BuildCommand(videoPath, tmpPath, opts)
	exitStatus, runErr := runner(ctx, argv)
	if runErr != nil || exitStatus != 0 {
		return SingleShotFallback(opts.TotalFrames), false
	}

	// #nosec G304 -- tmpPath is this function's own CreateTemp output.
	payload, readErr := os.ReadFile(tmpPath)
	if readErr != nil || len(strings.TrimSpace(string(payload))) == 0 {
		return SingleShotFallback(opts.TotalFrames), false
	}
	shots, parseErr := ParseJSON(payload)
	if parseErr != nil {
		return SingleShotFallback(opts.TotalFrames), false
	}
	return shots, true
}

// perShotPayload is vmaf-perShot's JSON schema (docs/usage/vmaf-perShot.md).
type perShotPayload struct {
	Shots []struct {
		StartFrame int `json:"start_frame"`
		EndFrame   int `json:"end_frame"`
	} `json:"shots"`
}

// ParseJSON parses vmaf-perShot's JSON output, converting the inclusive
// end_frame to the half-open convention. An empty shot list degrades to the
// [0, 1) sentinel rather than an empty timeline.
func ParseJSON(payload []byte) ([]Shot, error) {
	var doc perShotPayload
	if err := json.Unmarshal(payload, &doc); err != nil {
		return nil, fmt.Errorf("pershot: parse JSON: %w", err)
	}
	out := make([]Shot, 0, len(doc.Shots))
	for _, entry := range doc.Shots {
		out = append(out, Shot{StartFrame: entry.StartFrame, EndFrame: entry.EndFrame + 1})
	}
	if len(out) == 0 {
		return []Shot{{StartFrame: 0, EndFrame: 1}}, nil
	}
	return out, nil
}

// ParseCSV is the CSV variant, for callers that already hold the sidecar from
// a prior vmaf-perShot run.
func ParseCSV(payload string) ([]Shot, error) {
	reader := csv.NewReader(strings.NewReader(payload))
	records, err := reader.ReadAll()
	if err != nil {
		return nil, fmt.Errorf("pershot: parse CSV: %w", err)
	}
	if len(records) == 0 {
		return nil, nil
	}
	header := records[0]
	startCol, endCol := -1, -1
	for i, name := range header {
		switch strings.TrimSpace(name) {
		case "start_frame":
			startCol = i
		case "end_frame":
			endCol = i
		}
	}
	if startCol < 0 || endCol < 0 {
		return nil, fmt.Errorf(
			"pershot: CSV header missing start_frame / end_frame; got %v", header)
	}
	out := make([]Shot, 0, len(records)-1)
	for _, rec := range records[1:] {
		if startCol >= len(rec) || endCol >= len(rec) {
			continue
		}
		start, startErr := strconv.Atoi(strings.TrimSpace(rec[startCol]))
		end, endErr := strconv.Atoi(strings.TrimSpace(rec[endCol]))
		if startErr != nil || endErr != nil {
			return nil, fmt.Errorf("pershot: CSV row %v has non-integer frames", rec)
		}
		out = append(out, Shot{StartFrame: start, EndFrame: end + 1})
	}
	return out, nil
}

// SplitLongShots slices any shot longer than maxDurationSec into uniform
// sub-shots, guarding downstream per-shot tuning against an under-cutting
// detector (low-contrast fades, short clips, content the empirical threshold
// under-fits). ADR-0512.
//
// A non-positive window or a non-finite / non-positive framerate is a no-op.
// Partitions differ by at most one frame.
func SplitLongShots(shots []Shot, maxDurationSec, framerate float64) []Shot {
	if len(shots) == 0 {
		return shots
	}
	if math.IsNaN(framerate) || math.IsInf(framerate, 0) || framerate <= 0.0 {
		return shots
	}
	if maxDurationSec <= 0.0 {
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
		for idx := 0; idx < nParts; idx++ {
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

// IsFallback reports whether shots is the sentinel one-shot timeline emitted
// when detection was unavailable.
func IsFallback(shots []Shot) bool {
	return len(shots) == 1 && shots[0].StartFrame == 0 && shots[0].EndFrame == 1
}

// LookPath reports whether the detector binary is resolvable, so callers can
// warn before starting a long run. Kept here so the CLI does not need to
// import os/exec directly.
func LookPath(bin string) (string, bool) {
	if bin == "" {
		bin = "vmaf-perShot"
	}
	path, err := exec.LookPath(bin)
	if err != nil {
		return "", false
	}
	return filepath.Clean(path), true
}
