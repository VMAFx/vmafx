// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package scorecli is the Go port of tools/vmaf-tune/src/vmaftune/score.py —
// the libvmaf CLI driver the tuning subcommands use as their quality oracle.
//
// It is distinct from pkg/score (a gRPC client for the vmafx scoring service)
// and from pkg/libvmaf (a cgo binding). This package shells out to the `vmaf`
// binary with explicit raw-YUV geometry, which is what the tuning search loops
// need: they score raw reference YUV against a just-produced encode, and the
// CLI's --width/--height/--pixel_format/--bitdepth flags are the only way to
// pin that geometry.
//
// pkg/bisect.VMAFScoreFunc covers the container-in, XML-out case for the
// compare subcommand; this package covers the raw-YUV, JSON-out case with the
// backend selector and the per-feature pooled aggregates.
package scorecli

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	vmafmodel "github.com/VMAFx/vmafx/pkg/model"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
)

// Canonical6Features are the six libvmaf feature extractors the fork's
// regressors consume. Mirrors vmaftune.CANONICAL6_FEATURES.
var Canonical6Features = []string{
	"adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2",
}

// canonicalToPooledKey maps a canonical bare feature name onto the
// integer_-prefixed key modern libvmaf emits under pooled_metrics.
var canonicalToPooledKey = map[string]string{
	"adm2":       "integer_adm2",
	"vif_scale0": "integer_vif_scale0",
	"vif_scale1": "integer_vif_scale1",
	"vif_scale2": "integer_vif_scale2",
	"vif_scale3": "integer_vif_scale3",
	"motion2":    "integer_motion2",
}

// rawSuffixes are the file suffixes the vmaf CLI accepts without a prior
// ffmpeg decode.
//
// ADR-0499 / BBB e2e v3 bug V3-B: ".y4m" is deliberately NOT here. vmaf-tune
// always passes --width/--height/--pixel_format/--bitdepth, which flips the
// CLI's use_yuv flag and routes both inputs through raw_input_open; a Y4M file
// then trips that function's file-size mismatch guard. The empty-suffix entry
// covers fixture trees that name raw YUV without a .yuv extension.
var rawSuffixes = map[string]bool{".yuv": true, "": true}

// Request mirrors vmaftune.score.ScoreRequest.
type Request struct {
	Reference string
	Distorted string
	Width     int
	Height    int
	PixFmt    string
	// Model accepts a bare version identifier ("vmaf_v0.6.1") or a
	// pre-formatted key=value string ("path=/abs/model.json").
	Model string
	// FrameSkipRef / FrameCnt align the reference window with a sample-clip
	// encode (ADR-0301). Both 0 scores the full source.
	FrameSkipRef int
	FrameCnt     int
	// DurationS bounds the container->raw-YUV decode of the distorted side.
	DurationS float64
}

// Result mirrors vmaftune.score.ScoreResult.
type Result struct {
	Request           Request
	VMAFScore         float64
	ScoreTimeMS       float64
	VMAFBinaryVersion string
	ExitStatus        int
	StderrTail        string
	FeatureMeans      map[string]float64
	FeatureStds       map[string]float64
}

// modelArg formats the --model argument. Bare identifiers are wrapped as
// "version=..."; pre-formatted key=value strings pass through.
func modelArg(model string) string {
	if model == "" {
		model = vmafmodel.DefaultVersion
	}
	if strings.Contains(model, "=") {
		return model
	}
	return "version=" + model
}

// pixFmtToVMAF maps an ffmpeg pix_fmt onto libvmaf's --pixel_format
// vocabulary. Anything unrecognised falls back to 420.
func pixFmtToVMAF(pixFmt string) string {
	switch {
	case strings.HasPrefix(pixFmt, "yuv422"):
		return "422"
	case strings.HasPrefix(pixFmt, "yuv444"):
		return "444"
	default:
		return "420"
	}
}

// bitdepthFor derives the sample bit depth from an ffmpeg pix_fmt name.
func bitdepthFor(pixFmt string) int {
	switch {
	case strings.Contains(pixFmt, "10le") || strings.Contains(pixFmt, "p10"):
		return 10
	case strings.Contains(pixFmt, "12le") || strings.Contains(pixFmt, "p12"):
		return 12
	default:
		return 8
	}
}

// BuildCommand composes the libvmaf CLI argv. Pure function for test pinning.
//
// backend, when non-empty, is forwarded as --backend NAME (cpu / cuda / sycl /
// hip per ADR-0127 / ADR-0175 / ADR-0299 / ADR-0726). Empty omits the flag so
// the binary picks its own default.
func BuildCommand(req Request, jsonOutput, vmafBin, backend string) []string {
	if vmafBin == "" {
		vmafBin = "vmaf"
	}
	cmd := []string{
		vmafBin,
		"--reference", req.Reference,
		"--distorted", req.Distorted,
		"--width", strconv.Itoa(req.Width),
		"--height", strconv.Itoa(req.Height),
		"--pixel_format", pixFmtToVMAF(req.PixFmt),
		"--bitdepth", strconv.Itoa(bitdepthFor(req.PixFmt)),
		"--model", modelArg(req.Model),
		"--json",
		"--output", jsonOutput,
	}
	// The v1 default omits VIF; request it so the canonical-6 vif_scale0..3
	// aggregates are populated (T-VMAFTUNE-VIF-NAN-UNDER-V1-2026-09-04).
	if !vmafmodel.RequestsVIF(req.Model) {
		cmd = append(cmd, "--feature", "vif")
	}
	if backend != "" {
		cmd = append(cmd, "--backend", backend)
	}
	if req.FrameSkipRef > 0 {
		cmd = append(cmd, "--frame_skip_ref", strconv.Itoa(req.FrameSkipRef))
	}
	if req.FrameCnt > 0 {
		cmd = append(cmd, "--frame_cnt", strconv.Itoa(req.FrameCnt))
	}
	return cmd
}

// pooledBlock is one pooled_metrics entry.
type pooledBlock struct {
	Mean   *float64 `json:"mean"`
	StdDev *float64 `json:"stddev"`
}

// vmafPayload is the subset of the libvmaf JSON output this driver reads.
type vmafPayload struct {
	PooledMetrics map[string]pooledBlock `json:"pooled_metrics"`
	LegacyScore   *float64               `json:"VMAF score"`
}

// ParseJSON pulls the pooled VMAF score out of a libvmaf JSON payload,
// preferring the modern pooled_metrics.vmaf.mean shape and falling back to
// the older top-level "VMAF score".
func ParseJSON(data []byte) (float64, error) {
	var payload vmafPayload
	if err := json.Unmarshal(data, &payload); err != nil {
		return math.NaN(), fmt.Errorf("parse vmaf JSON: %w", err)
	}
	if block, ok := payload.PooledMetrics["vmaf"]; ok && block.Mean != nil {
		return *block.Mean, nil
	}
	if payload.LegacyScore != nil {
		return *payload.LegacyScore, nil
	}
	return math.NaN(), errors.New("vmaf JSON missing pooled_metrics.vmaf.mean")
}

// ParseFeatureAggregates pulls per-feature mean / stddev out of
// pooled_metrics, resolving each canonical bare name to its integer_-prefixed
// pooled key and falling back to the bare name.
//
// Features the run did not emit (a cambi-only model has no adm2) are simply
// absent from the returned maps; the corpus row writer turns absence into NaN
// rather than inventing a zero (ADR-0366).
func ParseFeatureAggregates(data []byte, featureNames []string) (map[string]float64, map[string]float64) {
	means := map[string]float64{}
	stds := map[string]float64{}

	var payload vmafPayload
	if err := json.Unmarshal(data, &payload); err != nil {
		return means, stds
	}
	for _, name := range featureNames {
		pooledKey := canonicalToPooledKey[name]
		block, ok := payload.PooledMetrics[pooledKey]
		if !ok {
			block, ok = payload.PooledMetrics[name]
		}
		if !ok {
			// Options-suffixed keys (integer_adm2_csf_2_..., integer_motion2_mmxv_18)
			// are what the v1 default emits; match on the "<key>_" prefix.
			block, ok = pooledByPrefix(payload.PooledMetrics, pooledKey+"_", name+"_")
		}
		if !ok {
			continue
		}
		if block.Mean != nil {
			means[name] = *block.Mean
		}
		if block.StdDev != nil {
			stds[name] = *block.StdDev
		}
	}
	return means, stds
}

// pooledByPrefix returns the first pooled block whose key starts with one
// of the prefixes. Keys are visited in sorted order so the choice is
// deterministic when several option variants of one feature are present.
func pooledByPrefix(pooled map[string]pooledBlock, prefixes ...string) (pooledBlock, bool) {
	keys := make([]string, 0, len(pooled))
	for k := range pooled {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		for _, pref := range prefixes {
			if strings.HasPrefix(k, pref) {
				return pooled[k], true
			}
		}
	}
	return pooledBlock{}, false
}

var vmafVersionRE = regexp.MustCompile(`VMAF version[: ]+(\S+)`)

// Runner is the subprocess seam. Tests inject a stub.
type Runner func(ctx context.Context, argv []string) (stderr string, exitStatus int, err error)

// defaultScoreTimeout bounds a single vmaf subprocess. Override via
// VMAFX_TUNE_SCORE_TIMEOUT.
const defaultScoreTimeout = 30 * time.Minute

func scoreTimeout() time.Duration {
	if raw := os.Getenv("VMAFX_TUNE_SCORE_TIMEOUT"); raw != "" {
		if d, err := time.ParseDuration(raw); err == nil {
			return d
		}
	}
	return defaultScoreTimeout
}

// defaultRunner executes argv under a timeout.
func defaultRunner(ctx context.Context, argv []string) (string, int, error) {
	cancel := func() {}
	if _, hasDeadline := ctx.Deadline(); !hasDeadline {
		if to := scoreTimeout(); to > 0 {
			ctx, cancel = context.WithTimeout(ctx, to)
		}
	}
	defer cancel()

	// #nosec G204 -- argv[0] is the operator-configured vmaf binary and
	// argv[1:] is assembled by BuildCommand from validated CLI flags.
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	cmd.WaitDelay = 2 * time.Second
	out, runErr := cmd.CombinedOutput()
	if runErr != nil {
		var exitErr *exec.ExitError
		if errors.As(runErr, &exitErr) {
			return string(out), exitErr.ExitCode(), nil
		}
		return string(out), 1, runErr
	}
	return string(out), 0, nil
}

// Run drives the vmaf CLI for a single (reference, distorted) pair.
//
// A non-zero vmaf exit is reported through Result.ExitStatus with a NaN score
// rather than as a Go error, matching the Python driver. Exit status 65 is
// synthesised when vmaf exits 0 but writes JSON this driver cannot read —
// truncated output must not crash a sweep.
func Run(ctx context.Context, req Request, vmafBin, backend string, runner Runner) (Result, error) {
	if runner == nil {
		runner = defaultRunner
	}
	workdir, err := os.MkdirTemp("", "vmafx-tune-score-")
	if err != nil {
		return Result{}, fmt.Errorf("create score workdir: %w", err)
	}
	defer func() {
		if rmErr := os.RemoveAll(workdir); rmErr != nil {
			// Best effort: a leaked temp dir is far less bad than losing
			// the score we just computed.
			_ = rmErr
		}
	}()

	jsonPath := filepath.Join(workdir, "vmaf.json")
	argv := BuildCommand(req, jsonPath, vmafBin, backend)

	started := time.Now()
	stderr, exitStatus, runErr := runner(ctx, argv)
	elapsedMS := float64(time.Since(started).Nanoseconds()) / 1e6
	if runErr != nil {
		return Result{}, fmt.Errorf("run vmaf: %w", runErr)
	}

	score := math.NaN()
	means := map[string]float64{}
	stds := map[string]float64{}
	if exitStatus == 0 {
		// #nosec G304 -- jsonPath is this function's own MkdirTemp output.
		data, readErr := os.ReadFile(jsonPath)
		switch {
		case readErr != nil:
			exitStatus = 65
		default:
			parsed, parseErr := ParseJSON(data)
			if parseErr != nil {
				exitStatus = 65
			} else {
				score = parsed
			}
			means, stds = ParseFeatureAggregates(data, Canonical6Features)
		}
	}

	version := "unknown"
	if m := vmafVersionRE.FindStringSubmatch(stderr); m != nil {
		version = m[1]
	}

	return Result{
		Request:           req,
		VMAFScore:         score,
		ScoreTimeMS:       elapsedMS,
		VMAFBinaryVersion: version,
		ExitStatus:        exitStatus,
		StderrTail:        tail(stderr, 2048),
		FeatureMeans:      means,
		FeatureStds:       stds,
	}, nil
}

// tail returns the last n bytes of text.
func tail(text string, n int) string {
	if len(text) <= n {
		return text
	}
	return text[len(text)-n:]
}

// NeedsDecode reports whether path must be decoded to raw YUV before the vmaf
// CLI will accept it.
func NeedsDecode(path string) bool {
	return !rawSuffixes[strings.ToLower(filepath.Ext(path))]
}

// DecodeCommand composes the ffmpeg argv that decodes a container to raw
// planar YUV for the vmaf CLI. durationS > 0 clamps the output with -t so a
// 10-second probe against a 10-minute source does not materialise tens of
// gigabytes of raw YUV (ADR-0498 / BBB e2e v2 bug v2-A).
func DecodeCommand(src, dst, pixFmt, ffmpegBin string, durationS float64) []string {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	cmd := []string{
		ffmpegBin, "-y", "-hide_banner", "-loglevel", "error",
		"-i", src,
		"-f", "rawvideo",
		"-pix_fmt", pixFmt,
	}
	if durationS > 0.0 {
		cmd = append(cmd, "-t", strconv.FormatFloat(durationS, 'g', -1, 64))
	}
	return append(cmd, dst)
}
