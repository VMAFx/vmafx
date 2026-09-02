// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/score.go — Go port of vmaftune.score.
//
// Spawns the libvmaf CLI against a (reference YUV, distorted encode) pair and
// parses the pooled VMAF score plus the canonical-6 per-feature aggregates from
// the JSON output. The subprocess boundary is the integration seam.

package corpus

import (
	"context"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/VMAFx/vmafx/pkg/pyjson"
)

// canonicalToPooledKey maps the canonical-6 bare names to the integer_*
// pooled_metrics keys modern libvmaf emits. Any canonical name absent here is
// looked up by its bare name, which covers non-integer features such as cambi.
var canonicalToPooledKey = map[string]string{
	"adm2":       "integer_adm2",
	"vif_scale0": "integer_vif_scale0",
	"vif_scale1": "integer_vif_scale1",
	"vif_scale2": "integer_vif_scale2",
	"vif_scale3": "integer_vif_scale3",
	"motion2":    "integer_motion2",
}

// ScoreRequest is a (reference, distorted) pair to score.
//
// FrameSkipRef / FrameCnt mirror the libvmaf CLI flags. Sample-clip mode
// (ADR-0301) sets these so VMAF compares the same time window of the reference
// that was fed to the encoder, instead of slicing the reference YUV on disk.
//
// DurationS gates the container -> raw YUV decode MaybeDecodeDistorted performs
// before handing a path to the vmaf CLI: bounding the decode with ffmpeg's -t
// keeps a 10-second probe against a 634-second source from materialising tens
// of gigabytes of raw YUV (ADR-0498, BBB e2e v2 Bug #v2-A).
type ScoreRequest struct {
	Reference    string
	Distorted    string
	Width        int
	Height       int
	PixFmt       string
	Model        string
	FrameSkipRef int
	FrameCnt     int
	DurationS    float64
}

// ScoreResult is the outcome of one scoring call.
//
// FeatureMeans / FeatureStds carry the canonical-6 libvmaf aggregates parsed
// out of pooled_metrics.<feature>. A feature libvmaf does not emit for the run
// (e.g. adm2 under a cambi-only model) is absent from the map; the row writer
// fills the missing column with NaN rather than inventing a zero (ADR-0366).
type ScoreResult struct {
	Request           ScoreRequest
	VMAFScore         float64
	ScoreTimeMS       float64
	VMAFBinaryVersion string
	ExitStatus        int
	StderrTail        string
	FeatureMeans      map[string]float64
	FeatureStds       map[string]float64
}

var vmafVersionRe = regexp.MustCompile(`VMAF version[: ]+(\S+)`)

// BuildVMAFCommand composes the libvmaf CLI argv. Pure function for test
// pinning.
//
// backend (when non-empty) is forwarded as the CLI's "--backend NAME" selector
// — cpu / cuda / sycl / hip per ADR-0127 / ADR-0175 / ADR-0299 / ADR-0726. An
// empty backend omits the flag so the binary picks its own default.
func BuildVMAFCommand(req ScoreRequest, jsonOutput, vmafBin, backend string) []string {
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
	if backend != "" {
		cmd = append(cmd, "--backend", backend)
	}
	// Sample-clip mode (ADR-0301): align the reference window with the
	// encoded slice so VMAF compares matching frames. The distorted leg is
	// already a clip-length encode, so no --frame_skip_dist is needed.
	if req.FrameSkipRef > 0 {
		cmd = append(cmd, "--frame_skip_ref", strconv.Itoa(req.FrameSkipRef))
	}
	if req.FrameCnt > 0 {
		cmd = append(cmd, "--frame_cnt", strconv.Itoa(req.FrameCnt))
	}
	return cmd
}

// modelArg formats the --model argument. A bare version identifier is wrapped
// as "version=..."; a pre-formatted "key=value" string (e.g. the HDR model's
// "path=/abs/model.json") passes through unchanged.
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

// bitdepthFor derives the --bitdepth value from an ffmpeg pix_fmt.
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

// ParseVMAFJSON pulls the pooled VMAF score from libvmaf's JSON payload.
//
// It tries the modern pooled_metrics.vmaf.mean shape first, then the older
// top-level "VMAF score". ok is false when neither is present.
func ParseVMAFJSON(payload map[string]any) (float64, bool) {
	if pooled, ok := payload["pooled_metrics"].(map[string]any); ok {
		if vmaf, ok := pooled["vmaf"].(map[string]any); ok {
			if mean, ok := toFloat(vmaf["mean"]); ok {
				return mean, true
			}
		}
	}
	if v, ok := toFloat(payload["VMAF score"]); ok {
		return v, true
	}
	return math.NaN(), false
}

// toFloat coerces a decoded JSON number to float64.
func toFloat(v any) (float64, bool) {
	switch t := v.(type) {
	case float64:
		return t, true
	case int:
		return float64(t), true
	case json.Number:
		f, err := t.Float64()
		return f, err == nil
	default:
		return 0, false
	}
}

// ParseFeatureAggregates pulls per-feature mean / stddev aggregates from a
// libvmaf JSON payload.
//
// Modern libvmaf emits pooled_metrics.<key> = {min, max, mean, harmonic_mean}
// for every registered feature extractor, where <key> is the integer_-prefixed
// pipeline name for the canonical-6 features. Both the prefixed and the bare
// key are tried so synthetic fixtures and non-integer features (cambi, ...)
// still resolve.
//
// stddev is absent from real integer-pipeline blocks (libvmaf emits
// harmonic_mean instead) but is present in some older / synthetic fixtures, so
// the lookup is guarded. Features missing from pooled_metrics are simply absent
// from the returned maps; the row writer translates absence into NaN.
func ParseFeatureAggregates(
	payload map[string]any, featureNames []string,
) (map[string]float64, map[string]float64) {
	means := map[string]float64{}
	stds := map[string]float64{}
	pooled, ok := payload["pooled_metrics"].(map[string]any)
	if !ok {
		return means, stds
	}
	for _, name := range featureNames {
		pooledKey := name
		if mapped, ok := canonicalToPooledKey[name]; ok {
			pooledKey = mapped
		}
		block, ok := pooled[pooledKey].(map[string]any)
		if !ok {
			// Also try the bare name for synthetic payloads that do not
			// use the integer_* prefix.
			block, ok = pooled[name].(map[string]any)
			if !ok {
				continue
			}
		}
		if v, ok := toFloat(block["mean"]); ok {
			means[name] = v
		}
		if v, ok := toFloat(block["stddev"]); ok {
			stds[name] = v
		}
	}
	return means, stds
}

// vmafRawSuffixes are the suffixes the vmaf CLI accepts as raw YUV without a
// prior ffmpeg decode.
//
// ADR-0499 / BBB e2e v3 Bug #V3-B: ".y4m" was previously listed here on the
// assumption the CLI auto-detects Y4M from the extension. It does not —
// vmaf-tune always passes --width / --height / --pixel_format / --bitdepth,
// which flips the CLI's use_yuv flag (core/tools/cli_parse.cpp) and routes both
// inputs through raw_input_open; Y4M files then trip the file-size-mismatch
// guard. The empty-suffix entry is kept for fixture trees that name raw YUV
// without a ".yuv" extension — geometry is already pinned by the flags.
var vmafRawSuffixes = map[string]bool{".yuv": true, "": true}

// IsRawYUVPath reports whether path can be handed to the vmaf CLI directly.
func IsRawYUVPath(path string) bool {
	return vmafRawSuffixes[strings.ToLower(filepath.Ext(path))]
}

// decodeToRawYUV decodes a container (mp4/mkv/...) to a raw planar YUV file.
//
// durationS, when positive, bounds the decoded output with ffmpeg's -t so a
// 10-second probe against a long source does not materialise tens of gigabytes
// of raw YUV that the score step never reads.
func decodeToRawYUV(
	ctx context.Context, src, dst, pixFmt, ffmpegBin string, durationS float64, run Runner,
) int {
	cmd := []string{
		ffmpegBinOrDefault(ffmpegBin),
		"-y", "-hide_banner", "-loglevel", "error",
		"-i", src,
		"-f", "rawvideo",
		"-pix_fmt", pixFmt,
	}
	if durationS > 0.0 {
		// -t after -i clamps the output to the first N seconds.
		cmd = append(cmd, "-t", pyjson.FloatRepr(durationS))
	}
	cmd = append(cmd, dst)
	return runnerOrExec(run)(ctx, cmd).ReturnCode
}

// MaybeDecodeDistorted decodes req.Distorted to raw YUV when it is a container.
//
// The libvmaf binary only accepts raw .yuv; without this step it interprets the
// container bytes as raw planar samples and aborts with "file too small for
// declared geometry" (BBB end-to-end Bug #3, 2026-05-17).
//
// It returns (updatedRequest, returncode):
//   - rc == 0 with a request pointing at a freshly-written raw YUV when the
//     input was a container and the decode succeeded;
//   - rc == 0 with the original request when the input was already raw;
//   - rc != 0 with the original request on decode failure — callers should
//     treat the score step as failed rather than invoking the vmaf binary on an
//     undecodable file.
func MaybeDecodeDistorted(
	ctx context.Context, req ScoreRequest, workdir, ffmpegBin string, run Runner,
) (ScoreRequest, int) {
	if IsRawYUVPath(req.Distorted) {
		return req, 0
	}
	if err := os.MkdirAll(workdir, 0o750); err != nil {
		return req, 1
	}
	stem := strings.TrimSuffix(filepath.Base(req.Distorted), filepath.Ext(req.Distorted))
	decoded := filepath.Join(workdir, stem+".decoded.yuv")

	decodeDuration := 0.0
	if req.DurationS > 0.0 {
		decodeDuration = req.DurationS
	}
	rc := decodeToRawYUV(ctx, req.Distorted, decoded, req.PixFmt, ffmpegBin, decodeDuration, run)
	if rc != 0 {
		return req, rc
	}
	if _, err := os.Stat(decoded); err != nil {
		return req, 1
	}
	out := req
	out.Distorted = decoded
	return out, 0
}

// RunScore drives the vmaf CLI for a single (ref, dist) pair.
//
// backend is forwarded to BuildVMAFCommand; an empty backend emits no
// --backend flag. workdir holds the JSON sidecar; an empty workdir uses a fresh
// temp directory that is removed on return.
func RunScore(
	ctx context.Context, req ScoreRequest, vmafBin string, run Runner, workdir, backend string,
) ScoreResult {
	run = runnerOrExec(run)

	ownWorkdir := workdir == ""
	if ownWorkdir {
		dir, err := os.MkdirTemp("", "vmaftune-score-")
		if err != nil {
			return ScoreResult{
				Request:           req,
				VMAFScore:         math.NaN(),
				VMAFBinaryVersion: "unknown",
				ExitStatus:        2,
				StderrTail:        err.Error(),
				FeatureMeans:      map[string]float64{},
				FeatureStds:       map[string]float64{},
			}
		}
		workdir = dir
		defer func() { _ = os.RemoveAll(workdir) }()
	} else if err := os.MkdirAll(workdir, 0o750); err != nil {
		return ScoreResult{
			Request:           req,
			VMAFScore:         math.NaN(),
			VMAFBinaryVersion: "unknown",
			ExitStatus:        2,
			StderrTail:        err.Error(),
			FeatureMeans:      map[string]float64{},
			FeatureStds:       map[string]float64{},
		}
	}

	jsonPath := filepath.Join(workdir, "vmaf.json")
	cmd := BuildVMAFCommand(req, jsonPath, vmafBin, backend)

	started := time.Now()
	res := run(ctx, cmd)
	elapsedMS := float64(time.Since(started).Nanoseconds()) / 1e6

	rc := res.ReturnCode
	score := math.NaN()
	featureMeans := map[string]float64{}
	featureStds := map[string]float64{}

	if rc == 0 {
		if data, err := os.ReadFile(jsonPath); err == nil { // #nosec G304 -- driver-generated sidecar path.
			var payload map[string]any
			if uErr := json.Unmarshal(data, &payload); uErr != nil {
				// vmaf exited 0 but wrote corrupt / partial JSON (e.g.
				// killed mid-write). Treat this as a scoring error so
				// the corpus row records NaN and a non-zero status
				// rather than crashing the run.
				rc = 65
			} else {
				var ok bool
				score, ok = ParseVMAFJSON(payload)
				if !ok && rc == 0 {
					rc = 65
				}
				// Per-feature aggregates are best-effort — a cambi-only
				// model will not expose adm2 etc.
				featureMeans, featureStds = ParseFeatureAggregates(payload, Canonical6Features)
			}
		}
	}

	version := firstSubmatch(vmafVersionRe, res.Stderr)
	if version == "" {
		version = "unknown"
	}

	return ScoreResult{
		Request:           req,
		VMAFScore:         score,
		ScoreTimeMS:       elapsedMS,
		VMAFBinaryVersion: version,
		ExitStatus:        rc,
		StderrTail:        tail(res.Stderr, 2048),
		FeatureMeans:      featureMeans,
		FeatureStds:       featureStds,
	}
}
