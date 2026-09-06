// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package fast

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/VMAFx/vmafx/pkg/corpus"
	"github.com/VMAFx/vmafx/pkg/encoder"
	"github.com/VMAFx/vmafx/pkg/model"
)

// PipelineConfig carries the ffmpeg / libvmaf plumbing the production
// fast path needs. It mirrors the `vmaf-tune fast` CLI flags one-for-one.
type PipelineConfig struct {
	// Src is the source video: raw YUV (Width/Height/Framerate/PixFmt
	// describe it) or any ffmpeg-readable container.
	Src string
	// Width / Height are the raw-YUV reference geometry. Required when Src
	// is a raw YUV.
	Width  int
	Height int
	// PixFmt is the ffmpeg pixel format (default yuv420p).
	PixFmt string
	// Framerate is the reference framerate.
	Framerate float64
	// Encoder is the ffmpeg codec name.
	Encoder string
	// Preset is the encoder preset for probe and verify encodes.
	Preset string
	// CRFLo / CRFHi bound the TPE search; they also define the crf_norm
	// axis the proxy's codec block carries.
	CRFLo int
	CRFHi int
	// SampleChunkSeconds is the probe-encode slice length per TPE trial.
	SampleChunkSeconds float64
	// FFmpegBin / VMAFBin are the tool paths.
	FFmpegBin string
	VMAFBin   string
	// VMAFModel is the libvmaf --model argument (bare version string or a
	// pre-formatted key=value).
	VMAFModel string
	// ScoreBackend is the libvmaf --backend selector for the verify pass.
	// "cpu" and "" omit the flag, matching the Python
	// `backend if backend != "cpu" else None`.
	ScoreBackend string
	// EncodeDir is the scratch root for probe and verify encodes.
	EncodeDir string
	// Proxy scores each probe's canonical-6 vector.
	Proxy Proxy
}

// pixFmtOrDefault returns the configured pixel format, defaulting to yuv420p.
func (c PipelineConfig) pixFmtOrDefault() string {
	if c.PixFmt == "" {
		return "yuv420p"
	}
	return c.PixFmt
}

// vmafModelOrDefault returns the configured model, defaulting to vmaf_v0.6.1.
func (c PipelineConfig) vmafModelOrDefault() string {
	if c.VMAFModel == "" {
		return model.DefaultVersion
	}
	return c.VMAFModel
}

// sourceIsContainer reports whether Src should be handed to ffmpeg without
// explicit rawvideo geometry. Mirrors the Python
// `src.suffix.lower() not in {".yuv", ".y4m", ""}` test.
func (c PipelineConfig) sourceIsContainer() bool {
	switch strings.ToLower(filepath.Ext(c.Src)) {
	case ".yuv", ".y4m", "":
		return false
	default:
		return true
	}
}

// inputArgs builds the ffmpeg input-side options for a clip of clipSeconds
// starting at startSeconds. clipSeconds <= 0 encodes the whole source.
//
// Mirrors encode.build_ffmpeg_command: raw YUV needs
// -f rawvideo -pix_fmt -s -r, and -ss / -t must sit on the input side so
// ffmpeg fast-seeks instead of decoding and discarding.
func (c PipelineConfig) inputArgs(startSeconds, clipSeconds float64) []string {
	var args []string
	if !c.sourceIsContainer() {
		args = append(args,
			"-f", "rawvideo",
			"-pix_fmt", c.pixFmtOrDefault(),
			"-s", fmt.Sprintf("%dx%d", c.Width, c.Height),
			"-r", formatFFmpegFloat(c.Framerate),
		)
	}
	if clipSeconds > 0 {
		args = append(args,
			"-ss", formatFFmpegFloat(startSeconds),
			"-t", formatFFmpegFloat(clipSeconds),
		)
	}
	return args
}

// formatFFmpegFloat renders a float the way Python's f"{value}" does for the
// ffmpeg argv, so the two implementations emit the same command line.
func formatFFmpegFloat(v float64) string {
	return strconv.FormatFloat(v, 'g', -1, 64)
}

// presetArgs returns the encoder-preset output options. Empty preset emits
// nothing so the encoder's own default applies.
func (c PipelineConfig) presetArgs() []string {
	if c.Preset == "" {
		return nil
	}
	return []string{"-preset", c.Preset}
}

// crfNorm maps a CRF onto the [0, 1] axis the proxy's codec block carries.
func (c PipelineConfig) crfNorm(crf int) float64 {
	span := c.CRFHi - c.CRFLo
	if span < 1 {
		span = 1
	}
	return float64(crf-c.CRFLo) / float64(span)
}

// presetNorm is the preset axis the v2 training contract defaults to.
// It collapses to 0.5 (neutral) until a caller threads a real preset scale
// through — the same constant fast._build_prod_predictor uses
// (Research-0076 §2).
const presetNorm = 0.5

// BuildVMAFCommand composes the libvmaf CLI argv for one score.
//
// Pure function so tests can pin the exact command line, mirroring
// score.build_vmaf_command. backend is forwarded as --backend NAME when
// non-empty; frameSkipRef / frameCnt mirror the sample-clip window so VMAF
// compares the same frames the encoder saw (ADR-0301).
func BuildVMAFCommand(
	cfg PipelineConfig,
	distorted, jsonOutput string,
	backend string,
	frameSkipRef, frameCnt int,
) []string {
	bin := cfg.VMAFBin
	if bin == "" {
		bin = "vmaf"
	}
	cmd := []string{
		bin,
		"--reference", cfg.Src,
		"--distorted", distorted,
		"--width", strconv.Itoa(cfg.Width),
		"--height", strconv.Itoa(cfg.Height),
		"--pixel_format", pixFmtToVMAF(cfg.pixFmtOrDefault()),
		"--bitdepth", strconv.Itoa(bitDepthFor(cfg.pixFmtOrDefault())),
		"--model", modelArg(cfg.vmafModelOrDefault()),
		"--json",
		"--output", jsonOutput,
	}
	if !corpus.ModelRequestsVIF(cfg.vmafModelOrDefault()) {
		cmd = append(cmd, "--feature", "vif")
	}
	if backend != "" {
		cmd = append(cmd, "--backend", backend)
	}
	if frameSkipRef > 0 {
		cmd = append(cmd, "--frame_skip_ref", strconv.Itoa(frameSkipRef))
	}
	if frameCnt > 0 {
		cmd = append(cmd, "--frame_cnt", strconv.Itoa(frameCnt))
	}
	return cmd
}

// modelArg formats the --model argument. A bare version identifier is wrapped
// as version=...; a pre-formatted key=value string passes through.
func modelArg(model string) string {
	if strings.Contains(model, "=") {
		return model
	}
	return "version=" + model
}

// pixFmtToVMAF maps an ffmpeg pix_fmt to libvmaf's --pixel_format vocabulary.
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

// bitDepthFor derives the libvmaf --bitdepth from an ffmpeg pix_fmt.
func bitDepthFor(pixFmt string) int {
	switch {
	case strings.Contains(pixFmt, "10le"), strings.Contains(pixFmt, "p10"):
		return 10
	case strings.Contains(pixFmt, "12le"), strings.Contains(pixFmt, "p12"):
		return 12
	default:
		return 8
	}
}

// vmafPayload is the libvmaf --json output shape the fast path reads.
type vmafPayload struct {
	Frames []struct {
		Metrics map[string]float64 `json:"metrics"`
	} `json:"frames"`
	PooledMetrics map[string]map[string]float64 `json:"pooled_metrics"`
}

// ParseCanonical6Means pulls the canonical-6 per-feature means out of a
// libvmaf --json payload, in Canonical6Features order.
//
// Resolution order per feature, matching score.parse_feature_aggregates:
//
//  1. pooled_metrics["integer_<name>"].mean — what modern libvmaf emits for
//     the integer pipeline;
//  2. pooled_metrics["<name>"].mean — bare key, for non-integer features and
//     synthetic fixtures;
//  3. the mean of frames[].metrics["integer_<name>"] then
//     frames[].metrics["<name>"].
//
// Missing features fill 0.0 — the fr_regressor_v2 proxy sees a zero feature
// rather than NaN, which is in-distribution for content where libvmaf's model
// omits a metric (the Python cli._parse_canonical6_means contract).
//
// # Divergence from the Python fast path
//
// cli._parse_canonical6_means looks up the BARE names only ("adm2",
// "vif_scale0", ...) in both pooled_metrics and the per-frame metrics.
// Modern libvmaf emits the integer-pipeline keys ("integer_adm2",
// "integer_vif_scale0", ...) — score.py knows this and carries a
// _CANONICAL_TO_POOLED_KEY map for exactly that reason, but the fast path
// does not use it. The result is that every probe returns [0, 0, 0, 0, 0, 0]
// against a real libvmaf run. This port tries the integer_ key first and
// falls back to the bare one, so it works against both shapes.
func ParseCanonical6Means(raw []byte) ([]float64, error) {
	var payload vmafPayload
	if err := json.Unmarshal(raw, &payload); err != nil {
		return nil, fmt.Errorf("fast: parse libvmaf JSON: %w", err)
	}

	out := make([]float64, len(canonical6))
	for i, name := range canonical6 {
		if v, ok := pooledMean(payload, "integer_"+name); ok {
			out[i] = v
			continue
		}
		if v, ok := pooledMean(payload, name); ok {
			out[i] = v
			continue
		}
		if v, ok := frameMean(payload, "integer_"+name); ok {
			out[i] = v
			continue
		}
		if v, ok := frameMean(payload, name); ok {
			out[i] = v
			continue
		}
		out[i] = 0.0
	}
	return out, nil
}

// pooledMean reads pooled_metrics[key].mean.
func pooledMean(payload vmafPayload, key string) (float64, bool) {
	if block, ok := payload.PooledMetrics[key]; ok {
		v, ok := block["mean"]
		return v, ok
	}
	pref := key + "_"
	for k, block := range payload.PooledMetrics {
		if strings.HasPrefix(k, pref) {
			if v, ok := block["mean"]; ok {
				return v, true
			}
		}
	}
	return 0, false
}

// frameMean averages frames[].metrics[key].
func frameMean(payload vmafPayload, key string) (float64, bool) {
	sum, n := 0.0, 0
	pref := key + "_"
	for _, fr := range payload.Frames {
		if v, ok := fr.Metrics[key]; ok {
			sum += v
			n++
			continue
		}
		for k, v := range fr.Metrics {
			if strings.HasPrefix(k, pref) {
				sum += v
				n++
				break
			}
		}
	}
	if n == 0 {
		return 0, false
	}
	return sum / float64(n), true
}

// ParseVMAFScore pulls the pooled VMAF score out of a libvmaf --json payload.
func ParseVMAFScore(raw []byte) (float64, error) {
	var payload struct {
		PooledMetrics map[string]map[string]float64 `json:"pooled_metrics"`
		Legacy        float64                       `json:"VMAF score"`
	}
	if err := json.Unmarshal(raw, &payload); err != nil {
		return 0, fmt.Errorf("fast: parse libvmaf JSON: %w", err)
	}
	if block, ok := payload.PooledMetrics["vmaf"]; ok {
		if v, ok := block["mean"]; ok {
			return v, nil
		}
	}
	if payload.Legacy != 0 {
		return payload.Legacy, nil
	}
	return 0, fmt.Errorf("fast: libvmaf JSON missing pooled_metrics.vmaf.mean")
}

// BitrateKbps converts an encode size to kilobits per second over a clip
// duration. A non-positive size or duration yields 0, matching
// encode.bitrate_kbps guarded by the Python fast-path callers.
func BitrateKbps(sizeBytes int64, durationSeconds float64) float64 {
	if sizeBytes <= 0 || durationSeconds <= 0 {
		return 0
	}
	return float64(sizeBytes) * 8.0 / 1000.0 / durationSeconds
}

// RawClipDurationSeconds derives the playing time of a raw-YUV source from its
// file size and frame geometry. This is exact and, unlike wall-clock encode
// time, independent of machine speed — the same correction cli.py's
// _build_fast_encode_runner carries. Returns 0 when the geometry is unknown or
// the file cannot be stat'ed.
//
// The fast payload carries no verify-pass bitrate (fast._gpu_verify computes
// one and discards it: `_kbps, vmaf = encode_runner(...)`), so nothing in this
// package calls this on the hot path today. It is exported and tested because
// it is the correct denominator for any caller that does want that number —
// notably a future payload field, or the corpus writer.
func RawClipDurationSeconds(src, pixFmt string, width, height int, framerate float64) float64 {
	frameBytes := RawFrameBytes(pixFmt, width, height)
	if frameBytes <= 0 || framerate <= 0 {
		return 0
	}
	info, err := os.Stat(src)
	if err != nil || info.Size() <= 0 {
		return 0
	}
	frames := info.Size() / frameBytes
	if frames <= 0 {
		return 0
	}
	return float64(frames) / framerate
}

// RawFrameBytes returns the byte size of one raw frame for a 4:2:0 pix_fmt at
// the given geometry, mirroring the cli.py fast-path computation.
func RawFrameBytes(pixFmt string, width, height int) int64 {
	if width <= 0 || height <= 0 {
		return 0
	}
	bytesPerSample := int64(1)
	if strings.HasSuffix(pixFmt, "10le") ||
		strings.HasSuffix(pixFmt, "12le") ||
		strings.HasSuffix(pixFmt, "16le") {
		bytesPerSample = 2
	}
	luma := int64(width) * int64(height) * bytesPerSample
	chroma := int64(width/2) * int64(height/2) * 2 * bytesPerSample
	return luma + chroma
}

// NewSamplePredictor returns a Predictor that, for each candidate CRF,
//
//  1. encodes a SampleChunkSeconds slice of the source at that CRF,
//  2. scores the slice with libvmaf and parses the canonical-6 pooled means,
//  3. runs the proxy over (features, codec block) to predict VMAF,
//
// and reports the observed probe bitrate alongside. This is the Go equivalent
// of cli._build_fast_sample_extractor + fast._build_prod_predictor.
//
// A failed probe encode or score yields zeroed features and a zero bitrate
// rather than aborting the study, matching the Python seam's
// `return ([0.0] * 6, 0.0)` behaviour; a proxy failure is fatal, because a
// broken proxy makes every remaining trial meaningless.
func NewSamplePredictor(ctx context.Context, cfg PipelineConfig) (Predictor, error) {
	if cfg.Proxy == nil {
		return nil, fmt.Errorf("fast: sample predictor requires a proxy")
	}
	enc, err := encoder.NewExtended(cfg.Encoder)
	if err != nil {
		return nil, fmt.Errorf("fast: encoder %q: %w", cfg.Encoder, err)
	}
	probeDir := filepath.Join(cfg.EncodeDir, "probes")

	chunk := cfg.SampleChunkSeconds
	if chunk <= 0 {
		chunk = SampleChunkSeconds
	}

	return func(crf int) (TrialSample, error) {
		features, kbps := probeOnce(ctx, cfg, enc, probeDir, crf, chunk)
		score, scoreErr := cfg.Proxy.Score(ctx, features, cfg.Encoder, presetNorm, cfg.crfNorm(crf))
		if scoreErr != nil {
			return TrialSample{}, scoreErr
		}
		return TrialSample{CRF: crf, PredictedVMAF: score, PredictedKbps: kbps}, nil
	}, nil
}

// probeOnce encodes and scores one probe slice, returning
// (canonical-6 means, observed kbps). Encode / score failures degrade to
// zeros so a single bad CRF does not abort the study.
func probeOnce(
	ctx context.Context,
	cfg PipelineConfig,
	enc encoder.Encoder,
	probeDir string,
	crf int,
	chunkSeconds float64,
) ([]float64, float64) {
	zeros := make([]float64, len(canonical6))

	out := filepath.Join(probeDir, fmt.Sprintf("probe_%s_crf%d.mp4", cfg.Encoder, crf))
	encResult, encErr := enc.Encode(cfg.Src, encoder.EncodeParams{
		CRF:        crf,
		FFmpegBin:  cfg.FFmpegBin,
		OutputDir:  probeDir,
		OutputPath: out,
		InputArgs:  cfg.inputArgs(0, chunkSeconds),
		ExtraArgs:  cfg.presetArgs(),
	})
	if encErr != nil {
		return zeros, 0
	}
	kbps := BitrateKbps(encResult.OutputSizeBytes, chunkSeconds)

	// The probe scores on the same window the encoder saw. The probe encode
	// starts at offset 0, so no --frame_skip_ref is needed; --frame_cnt
	// bounds the reference to the slice length.
	frameCnt := int(chunkSeconds * cfg.Framerate)
	raw, scoreErr := runVMAF(ctx, cfg, encResult.OutputPath, "", 0, frameCnt)
	if scoreErr != nil {
		return zeros, kbps
	}
	features, parseErr := ParseCanonical6Means(raw)
	if parseErr != nil {
		return zeros, kbps
	}
	return features, kbps
}

// NewVerifier returns the mandatory single real encode + libvmaf score pass at
// the recommended CRF (ADR-0304: the proxy alone never wins). It is the Go
// equivalent of cli._build_fast_encode_runner.
func NewVerifier(cfg PipelineConfig) (VerifyFunc, error) {
	enc, err := encoder.NewExtended(cfg.Encoder)
	if err != nil {
		return nil, fmt.Errorf("fast: encoder %q: %w", cfg.Encoder, err)
	}
	verifyDir := filepath.Join(cfg.EncodeDir, "verify")

	return func(ctx context.Context, encoderName string, crf int) (float64, error) {
		out := filepath.Join(verifyDir, fmt.Sprintf("verify_%s_crf%d.mp4", encoderName, crf))
		encResult, encErr := enc.Encode(cfg.Src, encoder.EncodeParams{
			CRF:        crf,
			FFmpegBin:  cfg.FFmpegBin,
			OutputDir:  verifyDir,
			OutputPath: out,
			InputArgs:  cfg.inputArgs(0, 0),
			ExtraArgs:  cfg.presetArgs(),
		})
		if encErr != nil {
			return math.NaN(), fmt.Errorf("verify encode at CRF %d: %w", crf, encErr)
		}

		// "cpu" and "" both omit --backend so libvmaf picks its own default,
		// matching `backend if backend != "cpu" else None`.
		backend := cfg.ScoreBackend
		if backend == "cpu" {
			backend = ""
		}
		raw, scoreErr := runVMAF(ctx, cfg, encResult.OutputPath, backend, 0, 0)
		if scoreErr != nil {
			return math.NaN(), fmt.Errorf("verify score at CRF %d: %w", crf, scoreErr)
		}
		return ParseVMAFScore(raw)
	}, nil
}

// vmafRawSuffixes are the file suffixes the libvmaf CLI accepts as raw YUV
// without a prior ffmpeg decode.
//
// ".y4m" is deliberately absent: the fast path always passes --width /
// --height / --pixel_format / --bitdepth, which flips the libvmaf CLI's
// use_yuv flag (core/tools/cli_parse) and routes both inputs through
// raw_input_open, where a Y4M header trips the file-size mismatch guard
// (ADR-0499 / BBB e2e v3 Bug #V3-B). The empty suffix is kept for fixture
// trees that name raw YUV without an extension — geometry is already pinned
// by the flags. Mirrors score.VMAF_RAW_SUFFIXES.
var vmafRawSuffixes = map[string]bool{".yuv": true, "": true}

// needsRawDecode reports whether path must be decoded before the libvmaf CLI
// can read it.
func needsRawDecode(path string) bool {
	return !vmafRawSuffixes[strings.ToLower(filepath.Ext(path))]
}

// decodeToRawYUV decodes a container (mp4/mkv/…) to a raw planar YUV file for
// the libvmaf CLI, which only accepts raw input. durationSeconds > 0 clamps
// the decode with ffmpeg's -t so a short probe does not materialise the whole
// source as raw YUV. Mirrors score._decode_to_raw_yuv.
func decodeToRawYUV(ctx context.Context, cfg PipelineConfig, src, dst string, durationSeconds float64) error {
	bin := cfg.FFmpegBin
	if bin == "" {
		bin = "ffmpeg"
	}
	argv := []string{
		bin, "-y", "-hide_banner", "-loglevel", "error",
		"-i", src,
		"-f", "rawvideo",
		"-pix_fmt", cfg.pixFmtOrDefault(),
	}
	if durationSeconds > 0 {
		// -t after -i clamps the output to the first N seconds.
		argv = append(argv, "-t", formatFFmpegFloat(durationSeconds))
	}
	argv = append(argv, dst)
	return runCommand(ctx, argv)
}

// runVMAF invokes the libvmaf CLI and returns its JSON output bytes.
//
// Container-shaped distorted files are decoded to raw YUV first: without that
// step the libvmaf binary reads the container bytes as raw planar samples and
// aborts with "Error reading YUV frame data". The Python fast path only does
// this on the verify leg (it goes through score.run_score); its probe leg
// calls score.build_vmaf_command directly on the .mp4, so every probe score
// fails and the canonical-6 vector silently degrades to zeros. This port
// decodes on both legs.
func runVMAF(
	ctx context.Context,
	cfg PipelineConfig,
	distorted, backend string,
	frameSkipRef, frameCnt int,
) ([]byte, error) {
	tmp, err := os.CreateTemp("", "vmafx-tune-fast-*.json")
	if err != nil {
		return nil, fmt.Errorf("fast: create score temp: %w", err)
	}
	jsonPath := tmp.Name()
	if closeErr := tmp.Close(); closeErr != nil {
		return nil, fmt.Errorf("fast: close score temp: %w", closeErr)
	}
	defer func() { _ = os.Remove(jsonPath) }()

	if needsRawDecode(distorted) {
		decoded := strings.TrimSuffix(distorted, filepath.Ext(distorted)) + ".decoded.yuv"
		clamp := 0.0
		if frameCnt > 0 && cfg.Framerate > 0 {
			clamp = float64(frameCnt) / cfg.Framerate
		}
		if decodeErr := decodeToRawYUV(ctx, cfg, distorted, decoded, clamp); decodeErr != nil {
			return nil, fmt.Errorf("fast: decode %q to raw YUV: %w", distorted, decodeErr)
		}
		defer func() { _ = os.Remove(decoded) }()
		distorted = decoded
	}

	argv := BuildVMAFCommand(cfg, distorted, jsonPath, backend, frameSkipRef, frameCnt)
	if err := runCommand(ctx, argv); err != nil {
		return nil, err
	}

	// #nosec G304 -- jsonPath is this function's own os.CreateTemp output.
	raw, readErr := os.ReadFile(jsonPath)
	if readErr != nil {
		return nil, fmt.Errorf("fast: read libvmaf output: %w", readErr)
	}
	return raw, nil
}
