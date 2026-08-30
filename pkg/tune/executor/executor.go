// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package executor is the Go port of vmaftune.executor.run_plan — Phase F
// execute mode (ADR-0454).
//
// RunPlan walks the selected cell(s) of an auto plan, runs FFmpeg per cell,
// scores each output with the libvmaf CLI, and appends one row per cell to
// out_dir/tune_results.jsonl.
//
// Design notes carried over from the Python:
//   - Zero new dependencies: results are JSONL, matching the corpus path.
//   - The subprocess boundary is the seam. EncodeRunner / ScoreRunner accept
//     stubs so the executor is fully testable without ffmpeg or vmaf.
//   - Only the selected cell runs by default; ExecuteAll runs every cell,
//     which is useful for a post-hoc A/B comparison.
//   - An encode failure is recorded in the row and scoring is skipped; the
//     JSONL is always written, even on partial failure.
package executor

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
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/VMAFx/vmafx/pkg/tune/auto"
	"github.com/VMAFx/vmafx/pkg/tune/codec"
	"github.com/VMAFx/vmafx/pkg/tune/pyjson"
)

// ResultsFilename is the JSONL log RunPlan appends to inside the output dir.
const ResultsFilename = "tune_results.jsonl"

// Canonical6Features are the libvmaf per-feature pooled aggregates the corpus
// schema carries. Order is load-bearing — downstream readers index the
// derived _mean / _std column tuples positionally.
var Canonical6Features = []string{
	"adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2",
}

// canonicalToPooledKey resolves each bare feature name to the integer_*
// pooled key modern libvmaf emits.
var canonicalToPooledKey = map[string]string{
	"adm2":       "integer_adm2",
	"vif_scale0": "integer_vif_scale0",
	"vif_scale1": "integer_vif_scale1",
	"vif_scale2": "integer_vif_scale2",
	"vif_scale3": "integer_vif_scale3",
	"motion2":    "integer_motion2",
}

// CommandResult is what a Runner reports back: the captured output streams
// plus the exit status. err is reserved for spawn failures.
//
// Stderr carries the encoder / libvmaf diagnostics every parser here reads.
// Stdout is populated too because `ffmpeg -version` prints its configure
// summary there, and the encoder-version fallback needs it.
type CommandResult struct {
	Stdout   string
	Stderr   string
	ExitCode int
}

// Runner is the subprocess seam. Production callers pass nil (ExecRunner is
// used); tests pass a stub that fabricates stderr and an exit code.
type Runner func(ctx context.Context, argv []string) (CommandResult, error)

// ExecRunner runs argv and captures both output streams. A non-zero exit is
// reported through ExitCode, not err, so callers can tell "the tool said no"
// from "the tool is not installed".
func ExecRunner(ctx context.Context, argv []string) (CommandResult, error) {
	if len(argv) == 0 {
		return CommandResult{ExitCode: 1}, fmt.Errorf("executor: empty argv")
	}
	// #nosec G204 -- argv[0] is an operator-configured binary name and the
	// tail is a fixed flag shape plus CLI-supplied paths; vmaf-tune is a
	// dev-time tool, not an RPC surface.
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	var stdout, stderr strings.Builder
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr

	err := cmd.Run()
	result := CommandResult{Stdout: stdout.String(), Stderr: stderr.String()}
	if err == nil {
		return result, nil
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		result.ExitCode = exitCodeOf(exitErr.ProcessState)
		return result, nil
	}
	result.ExitCode = 1
	return result, err
}

// EncodeRequest is one ffmpeg invocation.
type EncodeRequest struct {
	Source            string
	Width             int
	Height            int
	PixFmt            string
	Framerate         float64
	Encoder           string
	Preset            string
	CRF               int
	Output            string
	ExtraParams       []string
	SampleClipSeconds float64
	SampleClipStartS  float64
	SourceIsContainer bool
	DurationS         float64
}

// EncodeResult is the outcome of one encode.
type EncodeResult struct {
	Request        EncodeRequest
	SizeBytes      int64
	TimeMS         float64
	EncoderVersion string
	FFmpegVersion  string
	ExitStatus     int
	StderrTail     string
}

// ScoreResult is the outcome of one libvmaf scoring call.
type ScoreResult struct {
	VMAFScore     float64
	TimeMS        float64
	BinaryVersion string
	ExitStatus    int
	StderrTail    string
	FeatureMeans  map[string]float64
	FeatureStds   map[string]float64
}

// ExecuteResult pairs one plan cell with its encode and score outcomes plus
// the flat row written to the JSONL log.
type ExecuteResult struct {
	Cell   map[string]any
	Encode *EncodeResult
	Score  *ScoreResult
	Row    map[string]any
}

// BuildFFmpegCommand composes the ffmpeg argv for a single encode. Pure — no
// I/O — so tests can pin the exact command line.
//
// When SampleClipSeconds > 0, -ss / -t are inserted as *input-side* options
// (before -i) so ffmpeg fast-seeks. Output-side seeking would still decode
// the full source, defeating the speedup. When the caller did not opt into
// sample-clip mode but did bind DurationS, that becomes an input-side -t so
// the encode is bounded to the analysed window rather than the full source.
func BuildFFmpegCommand(req EncodeRequest, ffmpegBin string) []string {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	cmd := []string{ffmpegBin, "-y", "-hide_banner", "-loglevel", "info"}

	fallbackDuration := 0.0
	if req.SampleClipSeconds <= 0.0 && req.DurationS > 0.0 {
		fallbackDuration = req.DurationS
	}

	if !req.SourceIsContainer {
		// Raw YUV: ffmpeg needs the format spelled out.
		cmd = append(cmd,
			"-f", "rawvideo",
			"-pix_fmt", req.PixFmt,
			"-s", fmt.Sprintf("%dx%d", req.Width, req.Height),
			"-r", pyjson.FormatFloat(req.Framerate),
		)
	}
	switch {
	case req.SampleClipSeconds > 0.0:
		cmd = append(cmd,
			"-ss", pyjson.FormatFloat(req.SampleClipStartS),
			"-t", pyjson.FormatFloat(req.SampleClipSeconds))
	case fallbackDuration > 0.0:
		cmd = append(cmd, "-t", pyjson.FormatFloat(fallbackDuration))
	}
	cmd = append(cmd, "-i", req.Source)
	cmd = append(cmd, resolveCodecArgs(req)...)
	cmd = append(cmd, req.ExtraParams...)
	cmd = append(cmd, req.Output)
	return cmd
}

// resolveCodecArgs routes through the codec-adapter registry (ADR-0237 /
// ADR-0326): every adapter renders the -c:v slice in the codec-correct shape.
// An unregistered encoder falls back to the historic libx264 shape so
// existing callers keep working.
//
// The adapter's ExtraParams tail is appended after the codec slice, exactly
// as the Python _resolve_codec_args does. For the AMF family that repeats the
// -quality / -rc / -qp_i / -qp_p flags the codec slice already carries; ffmpeg
// takes the last occurrence, so the duplication is inert. It is reproduced
// here deliberately, so a Go-vs-Python argv diff stays empty.
func resolveCodecArgs(req EncodeRequest) []string {
	adapter, err := codec.Get(req.Encoder)
	if err != nil {
		return legacyCodecArgs(req.Encoder, req.Preset, req.CRF)
	}
	args, err := adapter.FFmpegCodecArgs(req.Preset, req.CRF)
	if err != nil {
		return legacyCodecArgs(req.Encoder, req.Preset, req.CRF)
	}
	return append(args, adapter.ExtraParams(req.Preset, req.CRF)...)
}

func legacyCodecArgs(encoder, preset string, crf int) []string {
	return []string{"-c:v", encoder, "-preset", preset, "-crf", strconv.Itoa(crf)}
}

// ---------------------------------------------------------------------------
// Version parsing.
// ---------------------------------------------------------------------------

var (
	ffmpegVersionRE   = regexp.MustCompile(`ffmpeg version (\S+)`)
	x264VersionRE     = regexp.MustCompile(`x264\s*-?\s*core\s+(\d+)`)
	x265VersionRE     = regexp.MustCompile(`x265 \[info\]: HEVC encoder version (\S+)`)
	libvpxVersionRE   = regexp.MustCompile(`\[libvpx-vp9 @ [^\]]+\]\s+v(\S+)`)
	svtAV1VersionRE   = regexp.MustCompile(`(?i)SVT-AV1 Encoder(?:\s+Lib)?\s+v(\S+)`)
	libaomVersionRE   = regexp.MustCompile(`(?i)\[libaom(?:-av1)?\s*@\s*[^\]]+\]\s+(?:libaom-av1\s+v|AOM version:\s*)(\S+)`)
	libvvencVersionRE = regexp.MustCompile(`(?i)\[libvvenc\s*@\s*[^\]]+\]\s+(?:Fraunhofer\s+VVC/H\.266\s+Encoder\s+)?VVenC\s+v(\S+)`)
	vmafVersionRE     = regexp.MustCompile(`VMAF version[: ]+(\S+)`)
)

var hardwareEncoderTokens = []string{"_nvenc", "_amf", "_qsv", "_videotoolbox"}

// ParseVersions extracts (ffmpegVersion, encoderVersion) from an ffmpeg
// stderr capture. Missing matches return "unknown" rather than an error — the
// results row records what we can detect and moves on.
//
// Hardware encoders advertise no version in stderr, so the encoder token is
// returned verbatim to keep the row identifier stable. libaom and libvvenc
// fall back to their adapter name for quiet builds.
func ParseVersions(stderr, encoder string) (string, string) {
	ffmpegVersion := "unknown"
	if m := ffmpegVersionRE.FindStringSubmatch(stderr); m != nil {
		ffmpegVersion = m[1]
	}
	return ffmpegVersion, parseEncoderVersion(stderr, encoder)
}

func parseEncoderVersion(stderr, encoder string) string {
	switch encoder {
	case "", "libx264":
		// Auto-detect: the x264 banner appears first in multi-codec logs.
		if m := x264VersionRE.FindStringSubmatch(stderr); m != nil {
			return "libx264-" + m[1]
		}
		if m := x265VersionRE.FindStringSubmatch(stderr); m != nil {
			return "libx265-" + m[1]
		}
		if m := svtAV1VersionRE.FindStringSubmatch(stderr); m != nil {
			return "libsvtav1-" + m[1]
		}
		return "unknown"
	case "libx265":
		return prefixedOr(x265VersionRE, stderr, "libx265-", "unknown")
	case "libsvtav1", "libsvtav1-vbr":
		return prefixedOr(svtAV1VersionRE, stderr, "libsvtav1-", "unknown")
	case "libvpx-vp9":
		return prefixedOr(libvpxVersionRE, stderr, "libvpx-vp9-", "unknown")
	case "libaom-av1":
		return prefixedOr(libaomVersionRE, stderr, "libaom-av1-", "libaom-av1")
	case "libvvenc":
		return prefixedOr(libvvencVersionRE, stderr, "libvvenc-", "libvvenc")
	}
	for _, token := range hardwareEncoderTokens {
		if strings.Contains(encoder, token) {
			return encoder
		}
	}
	return "unknown"
}

// versionProbePatterns maps an encoder onto the `--enable-<codec>` token its
// presence shows up as in `ffmpeg -version`'s configure summary. That summary
// carries no per-encoder version, so the best available answer is an
// "<encoder>-enabled" marker confirming the codec was compiled in. Encoders
// absent from this table have no fallback and stay "unknown".
var versionProbePatterns = map[string]*regexp.Regexp{
	"libx264":    regexp.MustCompile(`--enable-libx264`),
	"libsvtav1":  regexp.MustCompile(`--enable-libsvtav1`),
	"libx265":    regexp.MustCompile(`--enable-libx265`),
	"libvpx-vp9": regexp.MustCompile(`--enable-libvpx`),
	"libaom-av1": regexp.MustCompile(`--enable-libaom`),
	"libvvenc":   regexp.MustCompile(`--enable-libvvenc`),
}

// probeCache memoises the `ffmpeg -version` probe. The answer cannot change
// within a process, and one auto run can execute many cells against the same
// binary. Keyed by (binary, encoder) so a test that swaps the binary path
// still gets a fresh probe.
var (
	probeCacheMu sync.Mutex
	probeCache   = map[string]string{}
)

// ProbeEncoderVersion returns a best-effort "<encoder>-enabled" label, or ""
// when nothing is parseable — which leaves the caller's "unknown" in place.
//
// Modern ffmpeg suppresses the per-encoder banner under -hide_banner, which
// the encode argv sets, so a perfectly good encode would otherwise record
// "unknown" for its encoder. This probe recovers a stable identifier from the
// configure summary instead.
func ProbeEncoderVersion(ctx context.Context, ffmpegBin, encoder string, run Runner) string {
	pattern, ok := versionProbePatterns[encoder]
	if !ok {
		return ""
	}
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	if run == nil {
		run = ExecRunner
	}
	key := ffmpegBin + "\x00" + encoder

	probeCacheMu.Lock()
	cached, hit := probeCache[key]
	probeCacheMu.Unlock()
	if hit {
		return cached
	}

	label := ""
	// `ffmpeg -version` prints the configure line on stdout, but older builds
	// use stderr; match against both, as the Python probe does.
	if res, err := run(ctx, []string{ffmpegBin, "-version"}); err == nil {
		if pattern.MatchString(res.Stdout) || pattern.MatchString(res.Stderr) {
			label = encoder + "-enabled"
		}
	}

	probeCacheMu.Lock()
	probeCache[key] = label
	probeCacheMu.Unlock()
	return label
}

func prefixedOr(re *regexp.Regexp, stderr, prefix, fallback string) string {
	if m := re.FindStringSubmatch(stderr); m != nil {
		return prefix + m[1]
	}
	return fallback
}

// ---------------------------------------------------------------------------
// Encode + score drivers.
// ---------------------------------------------------------------------------

// RunEncode drives ffmpeg to produce req.Output.
func RunEncode(ctx context.Context, req EncodeRequest, ffmpegBin string, run Runner) EncodeResult {
	if run == nil {
		run = ExecRunner
	}
	argv := BuildFFmpegCommand(req, ffmpegBin)
	started := time.Now()
	res, err := run(ctx, argv)
	elapsed := float64(time.Since(started).Nanoseconds()) / 1e6
	if err != nil {
		res.ExitCode = 1
		if res.Stderr == "" {
			res.Stderr = err.Error()
		}
	}

	var size int64
	if res.ExitCode == 0 {
		if info, statErr := os.Stat(req.Output); statErr == nil {
			size = info.Size()
		}
	}
	ffmpegVersion, encoderVersion := ParseVersions(res.Stderr, req.Encoder)
	// A successful encode whose stderr carried no per-encoder banner still
	// deserves a stable identifier in the results row; recover one from the
	// configure summary. Cached, so this costs one subprocess per run at most.
	if res.ExitCode == 0 && encoderVersion == "unknown" {
		if probed := ProbeEncoderVersion(ctx, ffmpegBin, req.Encoder, run); probed != "" {
			encoderVersion = probed
		}
	}
	return EncodeResult{
		Request:        req,
		SizeBytes:      size,
		TimeMS:         elapsed,
		EncoderVersion: encoderVersion,
		FFmpegVersion:  ffmpegVersion,
		ExitStatus:     res.ExitCode,
		StderrTail:     tail(res.Stderr, 2048),
	}
}

// ScoreRequest is one libvmaf CLI invocation.
type ScoreRequest struct {
	Reference    string
	Distorted    string
	Width        int
	Height       int
	PixFmt       string
	Model        string
	FrameSkipRef int
	FrameCnt     int
	Backend      string
}

// BuildVMAFCommand composes the libvmaf CLI argv. Pure, for test pinning.
func BuildVMAFCommand(req ScoreRequest, jsonOutput, vmafBin string) []string {
	if vmafBin == "" {
		vmafBin = "vmaf"
	}
	model := req.Model
	if model == "" {
		model = "vmaf_v0.6.1"
	}
	if !strings.Contains(model, "=") {
		model = "version=" + model
	}
	cmd := []string{
		vmafBin,
		"--reference", req.Reference,
		"--distorted", req.Distorted,
		"--width", strconv.Itoa(req.Width),
		"--height", strconv.Itoa(req.Height),
		"--pixel_format", pixFmtToVMAF(req.PixFmt),
		"--bitdepth", strconv.Itoa(bitdepthFor(req.PixFmt)),
		"--model", model,
		"--json",
		"--output", jsonOutput,
	}
	if req.Backend != "" {
		cmd = append(cmd, "--backend", req.Backend)
	}
	// Sample-clip mode (ADR-0301): align the reference window with the
	// encoded slice so VMAF compares matching frames. The distorted file is
	// already a clip-length encode, so no --frame_skip_dist is needed.
	if req.FrameSkipRef > 0 {
		cmd = append(cmd, "--frame_skip_ref", strconv.Itoa(req.FrameSkipRef))
	}
	if req.FrameCnt > 0 {
		cmd = append(cmd, "--frame_cnt", strconv.Itoa(req.FrameCnt))
	}
	return cmd
}

// pixFmtToVMAF maps an ffmpeg pix_fmt onto libvmaf's --pixel_format
// vocabulary, defaulting to 4:2:0.
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

func bitdepthFor(pixFmt string) int {
	switch {
	case strings.Contains(pixFmt, "10le"), strings.Contains(pixFmt, "p10"):
		return 10
	case strings.Contains(pixFmt, "12le"), strings.Contains(pixFmt, "p12"):
		return 12
	default:
		return 8
	}
}

// RunScore drives the libvmaf CLI for a single (ref, distorted) pair.
//
// The vmaf CLI only accepts raw .yuv / .y4m inputs; a caller passing a
// container path must decode it first. A zero exit with corrupt JSON is
// treated as a scoring error (exit 65) so the row records NaN rather than
// crashing the run.
func RunScore(ctx context.Context, req ScoreRequest, vmafBin, workDir string, run Runner) ScoreResult {
	if run == nil {
		run = ExecRunner
	}
	jsonPath := filepath.Join(workDir, "vmaf.json")
	argv := BuildVMAFCommand(req, jsonPath, vmafBin)

	started := time.Now()
	res, err := run(ctx, argv)
	elapsed := float64(time.Since(started).Nanoseconds()) / 1e6
	if err != nil {
		res.ExitCode = 1
		if res.Stderr == "" {
			res.Stderr = err.Error()
		}
	}

	out := ScoreResult{
		VMAFScore:     math.NaN(),
		TimeMS:        elapsed,
		BinaryVersion: "unknown",
		ExitStatus:    res.ExitCode,
		StderrTail:    tail(res.Stderr, 2048),
		FeatureMeans:  map[string]float64{},
		FeatureStds:   map[string]float64{},
	}
	if m := vmafVersionRE.FindStringSubmatch(res.Stderr); m != nil {
		out.BinaryVersion = m[1]
	}
	if res.ExitCode != 0 {
		return out
	}
	data, readErr := os.ReadFile(jsonPath) // #nosec G304 -- workDir is created by RunPlan
	if readErr != nil {
		return out
	}
	var payload map[string]any
	if err := json.Unmarshal(data, &payload); err != nil {
		out.ExitStatus = 65
		return out
	}
	score, ok := ParseVMAFScore(payload)
	if !ok {
		out.ExitStatus = 65
	} else {
		out.VMAFScore = score
	}
	out.FeatureMeans, out.FeatureStds = ParseFeatureAggregates(payload, Canonical6Features)
	return out
}

// ParseVMAFScore pulls the pooled VMAF score out of libvmaf's JSON, trying
// the modern pooled_metrics.vmaf.mean shape first and falling back to the
// legacy top-level "VMAF score".
func ParseVMAFScore(payload map[string]any) (float64, bool) {
	if pooled, ok := payload["pooled_metrics"].(map[string]any); ok {
		if vmaf, ok := pooled["vmaf"].(map[string]any); ok {
			if mean, ok := vmaf["mean"].(float64); ok {
				return mean, true
			}
		}
	}
	if legacy, ok := payload["VMAF score"].(float64); ok {
		return legacy, true
	}
	return math.NaN(), false
}

// ParseFeatureAggregates pulls the per-feature mean / stddev aggregates.
//
// Modern libvmaf emits pooled_metrics.<integer_key>; the bare name is tried
// as a fallback so features without a prefix mapping (cambi) and synthetic
// test fixtures still resolve. Features absent from the payload are simply
// absent from the returned maps — the row writer turns absence into NaN
// rather than inventing a zero (ADR-0366).
func ParseFeatureAggregates(
	payload map[string]any,
	featureNames []string,
) (map[string]float64, map[string]float64) {
	means := map[string]float64{}
	stds := map[string]float64{}
	pooled, ok := payload["pooled_metrics"].(map[string]any)
	if !ok {
		return means, stds
	}
	for _, name := range featureNames {
		block, ok := pooled[canonicalToPooledKey[name]].(map[string]any)
		if !ok {
			block, ok = pooled[name].(map[string]any)
			if !ok {
				continue
			}
		}
		if mean, ok := block["mean"].(float64); ok {
			means[name] = mean
		}
		if std, ok := block["stddev"].(float64); ok {
			stds[name] = std
		}
	}
	return means, stds
}

// ---------------------------------------------------------------------------
// The plan driver.
// ---------------------------------------------------------------------------

// Params configures RunPlan.
type Params struct {
	// OutDir receives the encoded files and tune_results.jsonl.
	OutDir string
	// PixFmt is stored on the request and forwarded to the score driver.
	PixFmt string
	// Width / Height / Framerate default from the plan's source_meta when
	// left at the Python defaults (1920x1080 / 25 fps).
	Width     int
	Height    int
	Framerate float64
	// SourceIsContainer omits the raw-YUV input flags so ffmpeg detects the
	// format from the container.
	SourceIsContainer bool
	// ExecuteAll runs every cell instead of only the selected one.
	ExecuteAll bool
	// VMAFModel is forwarded to the score request.
	VMAFModel string
	// VMAFBin / FFmpegBin are the binary names or paths.
	VMAFBin   string
	FFmpegBin string
	// EncodeRunner / ScoreRunner are the subprocess seams.
	EncodeRunner Runner
	ScoreRunner  Runner
	// Log receives per-cell diagnostics.
	Log *slog.Logger
}

// DefaultParams mirrors the Python run_plan keyword defaults.
func DefaultParams(outDir string) Params {
	return Params{
		OutDir:            outDir,
		PixFmt:            "yuv420p",
		Width:             1920,
		Height:            1080,
		Framerate:         25.0,
		SourceIsContainer: true,
		VMAFModel:         "vmaf_v0.6.1",
		VMAFBin:           "vmaf",
		FFmpegBin:         "ffmpeg",
	}
}

// RunPlan realises an auto plan by running real encodes and scores.
//
// Returns one entry per executed cell, in plan order. The JSONL log is always
// written, even on partial failure.
func RunPlan(ctx context.Context, plan auto.Plan, src string, params Params) ([]ExecuteResult, error) {
	log := params.Log
	if log == nil {
		log = slog.Default()
	}
	// G301: 0o750 keeps the run directory owner-and-group readable only.
	if err := os.MkdirAll(params.OutDir, 0o750); err != nil {
		return nil, fmt.Errorf("create runs dir: %w", err)
	}
	resultsPath := filepath.Join(params.OutDir, ResultsFilename)
	// G302/G304: 0o600 — the results log carries source path strings.
	fh, err := os.OpenFile(resultsPath, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0o600) // #nosec G304
	if err != nil {
		return nil, fmt.Errorf("open results log: %w", err)
	}
	defer func() {
		if closeErr := fh.Close(); closeErr != nil {
			log.Warn("executor: close results log", "error", closeErr, "path", resultsPath)
		}
	}()

	width, height := effectiveGeometry(plan, params)

	results := make([]ExecuteResult, 0, len(plan.Cells))
	for _, cell := range plan.Cells {
		selected, _ := cell["selected"].(bool)
		if !params.ExecuteAll && !selected {
			continue
		}
		req := cellToEncodeRequest(cell, src, params, width, height)

		var enc *EncodeResult
		var score *ScoreResult

		encoded := RunEncode(ctx, req, params.FFmpegBin, params.EncodeRunner)
		enc = &encoded
		if encoded.ExitStatus != 0 {
			log.Warn("executor: encode failed",
				"cell_index", cell["cell_index"], "exit_status", encoded.ExitStatus)
		} else {
			workDir, tmpErr := os.MkdirTemp("", "vmafx-tune-score-")
			if tmpErr != nil {
				log.Warn("executor: score temp dir", "error", tmpErr)
			} else {
				scored := RunScore(ctx, ScoreRequest{
					Reference: src,
					Distorted: req.Output,
					Width:     width,
					Height:    height,
					PixFmt:    params.PixFmt,
					Model:     params.VMAFModel,
				}, params.VMAFBin, workDir, params.ScoreRunner)
				score = &scored
				if rmErr := os.RemoveAll(workDir); rmErr != nil {
					log.Warn("executor: remove score temp dir", "error", rmErr)
				}
			}
		}

		row := makeRow(cell, enc, score)
		line, marshalErr := pyjson.MarshalStrict(row, 0)
		if marshalErr != nil {
			return results, fmt.Errorf("render results row: %w", marshalErr)
		}
		if _, writeErr := fh.WriteString(line + "\n"); writeErr != nil {
			return results, fmt.Errorf("append results row: %w", writeErr)
		}
		results = append(results, ExecuteResult{Cell: cell, Encode: enc, Score: score, Row: row})
	}
	return results, nil
}

// effectiveGeometry pulls the frame geometry from the plan's source_meta
// unless the caller overrode the Python defaults.
func effectiveGeometry(plan auto.Plan, params Params) (int, int) {
	width, height := params.Width, params.Height
	meta, ok := plan.Metadata["source_meta"].(map[string]any)
	if !ok {
		return width, height
	}
	if width == 1920 {
		if v, ok := meta["width"].(int); ok {
			width = v
		}
	}
	if height == 1080 {
		if v, ok := meta["height"].(int); ok {
			height = v
		}
	}
	return width, height
}

// cellToEncodeRequest builds an EncodeRequest from a plan cell.
//
// Plan cells carry no cell_index or preset key (the planner does not emit
// them), so the defaults below — index 0 and preset "medium" — are what the
// Python executor also uses. The output filename keeps the Python
// encode_%03d_<codec>_<preset>_crf<n>.mkv shape so a mixed Go/Python run
// directory stays navigable.
func cellToEncodeRequest(
	cell map[string]any,
	src string,
	params Params,
	width, height int,
) EncodeRequest {
	codecName := stringField(cell, "codec", "libx264")
	preset := stringField(cell, "preset", "medium")
	crf := intField(cell, "crf", 23)
	cellIndex := intField(cell, "cell_index", 0)
	output := filepath.Join(params.OutDir,
		fmt.Sprintf("encode_%03d_%s_%s_crf%d.mkv", cellIndex, codecName, preset, crf))
	return EncodeRequest{
		Source:            src,
		Width:             width,
		Height:            height,
		PixFmt:            params.PixFmt,
		Framerate:         params.Framerate,
		Encoder:           codecName,
		Preset:            preset,
		CRF:               crf,
		Output:            output,
		SourceIsContainer: params.SourceIsContainer,
	}
}

// makeRow flattens cell + encode + score into the JSONL results row.
func makeRow(cell map[string]any, enc *EncodeResult, score *ScoreResult) map[string]any {
	row := map[string]any{
		"cell_index":             cell["cell_index"],
		"codec":                  cell["codec"],
		"preset":                 cell["preset"],
		"crf":                    cell["crf"],
		"selected":               boolField(cell, "selected"),
		"estimated_vmaf":         cell["estimated_vmaf"],
		"estimated_bitrate_kbps": cell["estimated_bitrate_kbps"],
		"prediction_source":      cell["prediction_source"],
		"encode_size_bytes":      nil,
		"encode_time_ms":         nil,
		"encode_exit_status":     nil,
		"ffmpeg_version":         nil,
		"encoder_version":        nil,
		"encode_path":            nil,
		"vmaf_score":             nil,
		"score_time_ms":          nil,
		"score_exit_status":      nil,
		"vmaf_binary_version":    nil,
	}
	if enc != nil {
		row["encode_size_bytes"] = int(enc.SizeBytes)
		row["encode_time_ms"] = enc.TimeMS
		row["encode_exit_status"] = enc.ExitStatus
		row["ffmpeg_version"] = enc.FFmpegVersion
		row["encoder_version"] = enc.EncoderVersion
		row["encode_path"] = enc.Request.Output
	}
	if score != nil {
		row["vmaf_score"] = score.VMAFScore
		row["score_time_ms"] = score.TimeMS
		row["score_exit_status"] = score.ExitStatus
		row["vmaf_binary_version"] = score.BinaryVersion
		for feature, value := range score.FeatureMeans {
			row["feature_"+feature+"_mean"] = value
		}
		for feature, value := range score.FeatureStds {
			row["feature_"+feature+"_std"] = value
		}
	}
	return row
}

func stringField(cell map[string]any, key, fallback string) string {
	if v, ok := cell[key].(string); ok && v != "" {
		return v
	}
	return fallback
}

func intField(cell map[string]any, key string, fallback int) int {
	switch v := cell[key].(type) {
	case int:
		return v
	case float64:
		return int(v)
	default:
		return fallback
	}
}

func boolField(cell map[string]any, key string) bool {
	v, _ := cell[key].(bool)
	return v
}

func tail(text string, n int) string {
	if len(text) <= n {
		return text
	}
	return text[len(text)-n:]
}
