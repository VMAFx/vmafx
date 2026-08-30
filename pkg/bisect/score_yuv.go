// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// pkg/bisect/score_yuv.go — raw-YUV VMAF ScoreFunc.
//
// Go port of tools/vmaf-tune/src/vmaftune/score.py (build_vmaf_command,
// maybe_decode_distorted, parse_vmaf_json) plus the container-decode step
// bisect.py performs inside _encode_and_score.
//
// VMAFScoreFunc (bisect.go) hands the reference and the encoded artefact
// straight to the vmaf binary with no geometry flags, which only works for
// a Y4M pair. The per-shot tuner scores a headerless raw-YUV reference
// against an encoded Matroska segment, and libvmaf accepts neither: without
// --width/--height it demands Y4M, and with them it routes both inputs
// through raw_input_open, where a container's bytes are mis-read as planar
// samples and the run aborts with "file too small for declared geometry".
// YUVScoreFunc closes both gaps: it decodes any container distorted file to
// raw YUV first, then invokes vmaf with the full geometry + model + backend
// flag set.

package bisect

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

// YUVScoreParams configures YUVScoreFunc.
type YUVScoreParams struct {
	// Width / Height are the raw reference geometry. Both are required —
	// a headerless YUV file does not describe itself.
	Width, Height int

	// PixFmt is the ffmpeg pixel-format name (e.g. "yuv420p",
	// "yuv420p10le"). It selects both the libvmaf --pixel_format / --bitdepth
	// pair and the -pix_fmt of the distorted decode.
	PixFmt string

	// Model is the libvmaf model selector. A bare version identifier
	// ("vmaf_v0.6.1") is wrapped as "version=..."; a pre-formatted
	// "key=value" string ("path=/abs/model.json") passes through untouched.
	Model string

	// Backend is the libvmaf --backend selector ("cpu", "cuda", "sycl",
	// "hip"). Empty omits the flag so the binary picks its own default.
	Backend string

	// VMAFBin is the vmaf binary path. Defaults to "vmaf".
	VMAFBin string

	// FFmpegBin is the ffmpeg binary used for the distorted container
	// decode. Defaults to "ffmpeg".
	FFmpegBin string

	// WorkDir holds the decoded distorted YUV and the vmaf JSON output.
	// Defaults to os.TempDir().
	WorkDir string

	// DurationS, when positive, clamps the distorted decode with ffmpeg's
	// "-t" so scoring a short window of a long source does not materialise
	// tens of gigabytes of raw YUV (ADR-0498).
	DurationS float64

	// DecodeSem, when non-nil, gates concurrent decode operations. Send to
	// acquire, receive to release. Mirrors the Python decode semaphore
	// (ADR-0577), which serialises raw-YUV decodes so a disk-space-limited
	// work volume is not asked to hold N of them at once.
	DecodeSem chan struct{}
}

// rawYUVSuffixes are the extensions the vmaf CLI reads as raw planar YUV
// without a prior ffmpeg decode. Mirrors score.VMAF_RAW_SUFFIXES — note that
// ".y4m" is deliberately absent: vmaf-tune always passes explicit geometry
// flags, which flips libvmaf's use_yuv path, and a Y4M file then trips the
// file-size guard inside raw_input_open (ADR-0499). The empty entry covers
// fixture trees that name raw YUV without an extension.
var rawYUVSuffixes = map[string]bool{".yuv": true, "": true}

// pixFmtToVMAF maps an ffmpeg pix_fmt onto libvmaf's --pixel_format
// vocabulary. Mirrors score._pixfmt_to_vmaf, including its 420 fallback.
func pixFmtToVMAF(pixFmt string) string {
	if strings.HasPrefix(pixFmt, "yuv422") {
		return "422"
	}
	if strings.HasPrefix(pixFmt, "yuv444") {
		return "444"
	}
	return "420"
}

// bitdepthFor derives the sample bit depth from an ffmpeg pix_fmt name.
// Mirrors score._bitdepth_for.
func bitdepthFor(pixFmt string) int {
	if strings.Contains(pixFmt, "10le") || strings.Contains(pixFmt, "p10") {
		return 10
	}
	if strings.Contains(pixFmt, "12le") || strings.Contains(pixFmt, "p12") {
		return 12
	}
	return 8
}

// modelArg formats the libvmaf --model argument. Mirrors score._model_arg:
// a bare version identifier is wrapped as "version=..."; anything already
// carrying "=" is a path/version override and passes through.
func modelArg(model string) string {
	if model == "" {
		model = "vmaf_v0.6.1"
	}
	if strings.Contains(model, "=") {
		return model
	}
	return "version=" + model
}

// BuildVMAFCommand composes the libvmaf CLI argv for one raw-YUV scoring
// call. Exported as a pure function so tests can pin the argv without
// running a subprocess, mirroring score.build_vmaf_command's role.
func BuildVMAFCommand(ref, distorted, jsonOutput string, p YUVScoreParams) []string {
	bin := p.VMAFBin
	if bin == "" {
		bin = "vmaf"
	}
	argv := []string{
		bin,
		"--reference", ref,
		"--distorted", distorted,
		"--width", strconv.Itoa(p.Width),
		"--height", strconv.Itoa(p.Height),
		"--pixel_format", pixFmtToVMAF(p.PixFmt),
		"--bitdepth", strconv.Itoa(bitdepthFor(p.PixFmt)),
		"--model", modelArg(p.Model),
		"--json",
		"--output", jsonOutput,
	}
	if p.Backend != "" {
		argv = append(argv, "--backend", p.Backend)
	}
	return argv
}

// DecodeToRawYUV decodes a container file to headerless planar YUV.
//
// Mirrors score._decode_to_raw_yuv, including the post-input "-t" clamp:
// placed after "-i" it bounds the *output*, so a short analysis window of a
// long source does not decode the whole file.
func DecodeToRawYUV(src, dst, pixFmt, ffmpegBin string, durationS float64) error {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	argv := decodeArgv(src, dst, pixFmt, durationS)

	ctx := context.Background()
	cancel := func() {}
	if to := scoreTimeout(); to > 0 {
		ctx, cancel = context.WithTimeout(ctx, to)
	}
	defer cancel()

	// #nosec G204 -- ffmpegBin is operator-configured; the remaining argv is
	// fixed flags plus the caller's src/dst paths. ctx bounds the runtime.
	out, err := exec.CommandContext(ctx, ffmpegBin, argv...).CombinedOutput()
	if err != nil {
		return fmt.Errorf("decode %q to raw YUV: %w\n%s", src, err, string(out))
	}
	if _, statErr := os.Stat(dst); statErr != nil {
		return fmt.Errorf("decode %q to raw YUV produced no output: %w", src, statErr)
	}
	return nil
}

// decodeArgv builds the ffmpeg argv (excluding argv[0]) for the container to
// raw-YUV decode. Split out as a pure function so the flag order can be
// pinned by a test without launching a subprocess — the same seam the Python
// keeps in score._decode_to_raw_yuv.
//
// A non-positive or non-finite durationS omits "-t" entirely, so a NaN
// duration degrades to a full decode rather than an unparseable flag.
func decodeArgv(src, dst, pixFmt string, durationS float64) []string {
	argv := []string{
		"-y", "-hide_banner", "-loglevel", "error",
		"-i", src,
		"-f", "rawvideo",
		"-pix_fmt", pixFmt,
	}
	if durationS > 0 {
		argv = append(argv, "-t", strconv.FormatFloat(durationS, 'f', -1, 64))
	}
	return append(argv, dst)
}

// ParseVMAFJSON pulls the pooled mean VMAF out of libvmaf's JSON output.
//
// Tries the modern pooled_metrics.vmaf.mean shape first, then the legacy
// top-level "VMAF score" key. Mirrors score.parse_vmaf_json, with the
// additional non-finite rejection VMAFScoreFunc already applies to the XML
// path: a NaN score must never reach a report emitter, where it would crash
// json.Marshal and break the Python/Go parser-parity invariant
// (cmd/vmafx-tune/AGENTS.md #2).
func ParseVMAFJSON(data []byte) (float64, error) {
	var payload struct {
		PooledMetrics struct {
			VMAF struct {
				Mean *float64 `json:"mean"`
			} `json:"vmaf"`
		} `json:"pooled_metrics"`
		LegacyScore *float64 `json:"VMAF score"`
	}
	if err := json.Unmarshal(data, &payload); err != nil {
		return 0, fmt.Errorf("parse vmaf JSON: %w", err)
	}
	var score float64
	switch {
	case payload.PooledMetrics.VMAF.Mean != nil:
		score = *payload.PooledMetrics.VMAF.Mean
	case payload.LegacyScore != nil:
		score = *payload.LegacyScore
	default:
		return 0, errors.New("vmaf JSON missing pooled_metrics.vmaf.mean")
	}
	if math.IsNaN(score) || math.IsInf(score, 0) {
		return 0, fmt.Errorf(
			"vmaf mean is non-finite (%v); refusing to propagate corrupt score", score)
	}
	return score, nil
}

// YUVScoreFunc returns a ScoreFunc that scores a raw-YUV reference against a
// possibly-containerised distorted file.
//
// The returned closure, per call:
//
//  1. decodes the distorted file to raw YUV when its extension is not one
//     the vmaf CLI reads directly (gated by DecodeSem when set),
//  2. invokes the vmaf binary with full geometry, model and backend flags,
//  3. parses the pooled mean out of the JSON output,
//  4. removes both the decoded sidecar and the JSON output.
//
// Every invocation is bounded by scoreTimeout() (30 min default,
// VMAFX_TUNE_SCORE_TIMEOUT override), matching VMAFScoreFunc.
func YUVScoreFunc(p YUVScoreParams) ScoreFunc {
	return func(ref, distorted string) (float64, error) {
		workDir := p.WorkDir
		if workDir == "" {
			workDir = os.TempDir()
		}
		// G301: 0o750 keeps the scratch tree owner+group readable only.
		if err := os.MkdirAll(workDir, 0o750); err != nil {
			return 0, fmt.Errorf("create score workdir: %w", err)
		}

		scoreTarget := distorted
		var decoded string
		if !rawYUVSuffixes[strings.ToLower(filepath.Ext(distorted))] {
			decoded = filepath.Join(workDir,
				strings.TrimSuffix(filepath.Base(distorted),
					filepath.Ext(distorted))+".decoded.yuv")
			release := acquire(p.DecodeSem)
			err := DecodeToRawYUV(distorted, decoded, p.PixFmt, p.FFmpegBin, p.DurationS)
			release()
			if err != nil {
				return 0, err
			}
			scoreTarget = decoded
		}
		defer func() {
			if decoded == "" {
				return
			}
			if rmErr := os.Remove(decoded); rmErr != nil && !errors.Is(rmErr, os.ErrNotExist) {
				slog.Warn("bisect: remove decoded distorted YUV",
					"error", rmErr, "path", decoded)
			}
		}()

		jsonOut, err := os.CreateTemp(workDir, "vmafx-tune-score-*.json")
		if err != nil {
			return 0, fmt.Errorf("create score temp: %w", err)
		}
		jsonPath := jsonOut.Name()
		if closeErr := jsonOut.Close(); closeErr != nil {
			return 0, fmt.Errorf("close score temp: %w", closeErr)
		}
		defer func() {
			if rmErr := os.Remove(jsonPath); rmErr != nil && !errors.Is(rmErr, os.ErrNotExist) {
				slog.Warn("bisect: remove score temp", "error", rmErr, "path", jsonPath)
			}
		}()

		argv := BuildVMAFCommand(ref, scoreTarget, jsonPath, p)

		ctx := context.Background()
		cancel := func() {}
		if to := scoreTimeout(); to > 0 {
			ctx, cancel = context.WithTimeout(ctx, to)
		}
		defer cancel()

		// #nosec G204 -- argv[0] is the operator-configured vmaf binary; the
		// rest is fixed flags plus caller paths. ctx enforces scoreTimeout().
		cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
		out, runErr := cmd.CombinedOutput()
		if runErr != nil {
			if ctx.Err() != nil && errors.Is(ctx.Err(), context.DeadlineExceeded) {
				return 0, fmt.Errorf("vmaf score timed out after %s: %w\n%s",
					scoreTimeout(), runErr, string(out))
			}
			return 0, fmt.Errorf("vmaf score failed: %w\n%s", runErr, string(out))
		}

		// #nosec G304 -- jsonPath is this function's own os.CreateTemp output.
		data, readErr := os.ReadFile(jsonPath)
		if readErr != nil {
			return 0, fmt.Errorf("read vmaf output %q: %w", jsonPath, readErr)
		}
		return ParseVMAFJSON(data)
	}
}

// acquire takes a slot on sem (when non-nil) and returns the release func.
// A nil channel yields a no-op pair, so callers need no nil checks.
func acquire(sem chan struct{}) func() {
	if sem == nil {
		return func() {}
	}
	sem <- struct{}{}
	return func() { <-sem }
}
