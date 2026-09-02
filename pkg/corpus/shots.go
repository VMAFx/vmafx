// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/shots.go — shot detection + per-source shot metadata.
//
// Port of the vmaftune.per_shot subset the corpus sweep consumes: it drives the
// fork's C-side vmaf-perShot binary (ADR-0222, wrapping TransNet V2 per
// ADR-0223) once per source and reduces the boundary list to the
// (count, mean, std) triple the v3 corpus row carries.
//
// Failures are silent by design: a missing binary or a failed invocation yields
// the all-zero metadata, which downstream consumers read as "shot data
// unavailable for this source" (shot_count == 0).

package corpus

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"os/exec"
	"strconv"
	"strings"
)

// Shot is a half-open frame range describing one shot: StartFrame is
// inclusive, EndFrame exclusive (Python slice convention). vmaf-perShot's own
// output uses an inclusive end frame; the parser normalises to half-open.
type Shot struct {
	StartFrame int
	EndFrame   int
}

// Length is the shot's frame count.
func (s Shot) Length() int { return s.EndFrame - s.StartFrame }

// ShotMetadata is the aggregate shot statistics for one source. Every field is
// zero when shot detection is unavailable; count > 0 with avgDurationSec > 0 is
// the contract for "real shot data was captured".
type ShotMetadata struct {
	Count          int
	AvgDurationSec float64
	DurationStdSec float64
}

// bitdepthAwarePix maps an ffmpeg pix_fmt to vmaf-perShot's --pixel_format.
func bitdepthAwarePix(pixFmt string) string {
	switch {
	case strings.Contains(pixFmt, "422"):
		return "422"
	case strings.Contains(pixFmt, "444"):
		return "444"
	default:
		return "420"
	}
}

// singleShotFallback is one shot covering the whole clip, used when detection
// fails. With no frame count the sentinel Shot{0, 1} is emitted so downstream
// code can pattern-match it.
func singleShotFallback(totalFrames int) []Shot {
	if totalFrames <= 0 {
		return []Shot{{StartFrame: 0, EndFrame: 1}}
	}
	return []Shot{{StartFrame: 0, EndFrame: totalFrames}}
}

// DetectShotsOptions carries the vmaf-perShot invocation parameters.
type DetectShotsOptions struct {
	Width  int
	Height int
	PixFmt string
	// Bitdepth defaults to 8 when zero.
	Bitdepth int
	// TotalFrames drives the fallback path; 0 means "unknown".
	TotalFrames int
	// PerShotBin defaults to "vmaf-perShot".
	PerShotBin string
	// DiffThreshold overrides the C-side --diff-threshold (luma
	// mean-absolute-delta cutoff). Nil keeps the binary's compiled-in
	// default (ADR-0512).
	DiffThreshold *float64
}

// DetectShotsWithStatus returns the shot list plus ok == true iff the
// vmaf-perShot invocation succeeded and yielded shot data.
//
// The summarise step needs to distinguish "real one-shot source" from "fallback
// because the binary failed", which a list-only return cannot carry.
func DetectShotsWithStatus(
	ctx context.Context, videoPath string, opts DetectShotsOptions, run Runner,
) ([]Shot, bool) {
	perShotBin := opts.PerShotBin
	if perShotBin == "" {
		perShotBin = "vmaf-perShot"
	}
	bitdepth := opts.Bitdepth
	if bitdepth == 0 {
		bitdepth = 8
	}
	if run == nil {
		// Production path: refuse to spawn when the binary is not on PATH,
		// matching shutil.which()'s guard.
		if _, err := exec.LookPath(perShotBin); err != nil {
			return singleShotFallback(opts.TotalFrames), false
		}
		run = ExecRunner
	}

	// vmaf-perShot always writes "vmaf-perShot: wrote N shot(s) to PATH" to
	// stdout regardless of --output (including "--output -"), so the JSON
	// must be read from a real temp file rather than parsed off stdout.
	tmp, err := os.CreateTemp("", "vmaf_pershot_*.json")
	if err != nil {
		return singleShotFallback(opts.TotalFrames), false
	}
	tmpPath := tmp.Name()
	_ = tmp.Close()
	defer func() { _ = os.Remove(tmpPath) }()

	cmd := []string{
		perShotBin,
		"--reference", videoPath,
		"--width", strconv.Itoa(opts.Width),
		"--height", strconv.Itoa(opts.Height),
		"--pixel_format", bitdepthAwarePix(opts.PixFmt),
		"--bitdepth", strconv.Itoa(bitdepth),
		"--output", tmpPath,
		"--format", "json",
	}
	// ADR-0512: thread a user-tunable cut threshold to the C binary so
	// operators can dial sensitivity per content without rebuilding.
	if opts.DiffThreshold != nil {
		cmd = append(cmd, "--diff-threshold", strconv.FormatFloat(*opts.DiffThreshold, 'f', 6, 64))
	}

	if res := run(ctx, cmd); res.ReturnCode != 0 {
		return singleShotFallback(opts.TotalFrames), false
	}
	payload, readErr := os.ReadFile(tmpPath) // #nosec G304 -- driver-generated temp path.
	if readErr != nil || strings.TrimSpace(string(payload)) == "" {
		return singleShotFallback(opts.TotalFrames), false
	}
	shots, parseErr := ParsePerShotJSON(string(payload))
	if parseErr != nil {
		return singleShotFallback(opts.TotalFrames), false
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

// ParsePerShotJSON parses vmaf-perShot's JSON output into a shot list. The
// source schema's end_frame is inclusive; the half-open conversion adds 1.
func ParsePerShotJSON(payload string) ([]Shot, error) {
	var data perShotPayload
	if err := json.Unmarshal([]byte(payload), &data); err != nil {
		return nil, fmt.Errorf("parse vmaf-perShot json: %w", err)
	}
	out := make([]Shot, 0, len(data.Shots))
	for _, entry := range data.Shots {
		out = append(out, Shot{StartFrame: entry.StartFrame, EndFrame: entry.EndFrame + 1})
	}
	if len(out) == 0 {
		return []Shot{{StartFrame: 0, EndFrame: 1}}, nil
	}
	return out, nil
}

// isFallbackShotList detects the Shot{0, 1} sentinel / single-shot fallback.
//
// DetectShots emits either the sentinel (no frame count known) or a single shot
// spanning the whole clip (the binary failed). Both mean "shot detection was
// not real"; the caller should treat the metadata as missing rather than as one
// giant shot.
func isFallbackShotList(shots []Shot) bool {
	if len(shots) != 1 {
		return false
	}
	only := shots[0]
	return only.StartFrame == 0 && only.Length() <= 1
}

// SummariseShots computes (count, mean, std) of the shot lengths in seconds.
//
// It returns the all-zero sentinel for single-shot fallback lists and for any
// non-finite or non-positive framerate. The standard deviation is the
// population form, so the result is well-defined for count == 1 (the sample
// form would emit NaN and force the caller to special-case the singleton).
func SummariseShots(shots []Shot, framerate float64) ShotMetadata {
	var zero ShotMetadata
	if len(shots) == 0 {
		return zero
	}
	if math.IsNaN(framerate) || math.IsInf(framerate, 0) || framerate <= 0.0 {
		return zero
	}
	if isFallbackShotList(shots) {
		return zero
	}
	durations := make([]float64, len(shots))
	for i, s := range shots {
		durations[i] = float64(s.Length()) / framerate
	}
	// statistics.pstdev is the population form, so the result stays
	// well-defined for count == 1 (the sample form would emit NaN and force
	// the caller to special-case the singleton). pyPopulationStdev
	// reproduces its exact-rational variance and correctly-rounded square
	// root rather than approximating with a float accumulator.
	return ShotMetadata{
		Count:          len(shots),
		AvgDurationSec: mean(durations),
		DurationStdSec: pyPopulationStdev(durations),
	}
}
