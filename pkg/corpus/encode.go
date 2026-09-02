// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/encode.go — the corpus sweep's encode drivers.
//
// The single-encode argv (BuildFFmpegCommand) and the version parser are
// pkg/ffencode's — the one Go port of vmaftune.encode.build_ffmpeg_command /
// parse_versions (ADR-1137) — re-exported here under the names the corpus
// package has always used. What stays local is the corpus-specific driver
// layer: the stats-capturing pass-1 command, the 2-pass orchestration, and the
// RunResult-shaped Runner seam the rest of the package shares.

package corpus

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/ffencode"
)

// EncodeRequest is a single (preset, crf) request against one source. It is
// ffencode.Request — the one Go mirror of vmaftune.encode.EncodeRequest —
// under the name this package's callers and tests use.
type EncodeRequest = ffencode.Request

// EncodeResult is the outcome of one encode call.
type EncodeResult struct {
	Request         EncodeRequest
	EncodeSizeBytes int64
	EncodeTimeMS    float64
	EncoderVersion  string
	FFmpegVersion   string
	ExitStatus      int
	StderrTail      string

	// EncoderStats carries per-frame x264 / x265 stats records when the
	// request was driven through RunEncodeWithStats and the adapter
	// declares SupportsEncoderStats. Empty otherwise.
	EncoderStats []PerFrameStats
}

// BuildFFmpegCommand composes the ffmpeg argv for a single encode.
//
// It is ffencode.BuildFFmpegCommand under the corpus package's name: the
// input-side -ss / -t placement, the clip precedence (sample-clip, then a
// bound DurationS, then nothing — ADR-0506 Bug #V6-1 / ADR-0508 Bug #V8-A),
// the codec-adapter argv and the 2-pass splice all live there, and
// encode_test.go's argv table pins them through this name.
func BuildFFmpegCommand(req EncodeRequest, ffmpegBin string) ([]string, error) {
	return ffencode.BuildFFmpegCommand(req, ffmpegBin)
}

// firstSubmatch returns the first capture group or "" when re does not match.
func firstSubmatch(re *regexp.Regexp, s string) string {
	m := re.FindStringSubmatch(s)
	if len(m) < 2 {
		return ""
	}
	return m[1]
}

// ParseVersions extracts (ffmpegVersion, encoderVersion) from ffmpeg stderr.
//
// It is ffencode.ParseVersions — the one port of vmaftune.encode.parse_versions
// — under the corpus package's name. Hardware encoders do not advertise a
// version, so the encoder token is returned verbatim; missing matches yield
// "unknown" rather than an error, and corpus rows record what can be detected.
func ParseVersions(stderr, encoder string) (string, string) {
	return ffencode.ParseVersions(stderr, encoder)
}

// probeCacheKey keys the fallback encoder-version probe by (binary, encoder) so
// a test that swaps the binary path still gets a fresh probe.
type probeCacheKey struct{ bin, encoder string }

var (
	probeCacheMu sync.Mutex
	probeCache   = map[probeCacheKey]string{}
)

// ResetEncoderVersionProbeCache clears the memoised "ffmpeg -version" probe.
// Test-only seam, mirroring the module-level dict in encode.py.
func ResetEncoderVersionProbeCache() {
	probeCacheMu.Lock()
	defer probeCacheMu.Unlock()
	probeCache = map[probeCacheKey]string{}
}

// probeEncoderVersionFromFFmpeg returns a best-effort "<encoder>-enabled"
// label, or "" when nothing parseable is found.
//
// Modern ffmpeg builds suppress the per-encoder banner under -hide_banner, so
// the corpus row would otherwise record "unknown" even with the encoder
// running. Falling back to the "configuration:" line of "ffmpeg -version"
// recovers a stable marker.
func probeEncoderVersionFromFFmpeg(
	ctx context.Context, ffmpegBin, encoder string, run Runner,
) string {
	pattern, ok := ffencode.ProbePattern(encoder)
	if !ok {
		return ""
	}
	key := probeCacheKey{bin: ffmpegBin, encoder: encoder}
	probeCacheMu.Lock()
	cached, hit := probeCache[key]
	probeCacheMu.Unlock()
	if hit {
		return cached
	}
	res := run(ctx, []string{ffmpegBin, "-version"})
	label := ""
	if strings.Contains(res.Stdout+res.Stderr, pattern) {
		label = encoder + "-enabled"
	}
	probeCacheMu.Lock()
	probeCache[key] = label
	probeCacheMu.Unlock()
	return label
}

// tail returns the final n bytes of text.
func tail(text string, n int) string {
	if len(text) <= n {
		return text
	}
	return text[len(text)-n:]
}

// RunEncode drives ffmpeg to produce req.Output.
func RunEncode(ctx context.Context, req EncodeRequest, ffmpegBin string, run Runner) EncodeResult {
	run = runnerOrExec(run)
	cmd, err := BuildFFmpegCommand(req, ffmpegBin)
	if err != nil {
		return EncodeResult{
			Request:        req,
			EncoderVersion: "unknown",
			FFmpegVersion:  "unknown",
			ExitStatus:     2,
			StderrTail:     err.Error(),
		}
	}

	started := time.Now()
	res := run(ctx, cmd)
	elapsedMS := float64(time.Since(started).Nanoseconds()) / 1e6

	var size int64
	// Pass 1 of a 2-pass encode writes only the stats file; req.Output is
	// not produced. Skip the size probe so a spurious zero does not read as
	// a failure to callers.
	if res.ReturnCode == 0 && req.PassNumber != 1 {
		if info, statErr := os.Stat(req.Output); statErr == nil {
			size = info.Size()
		}
	}

	ffmpegV, encoderV := ParseVersions(res.Stderr, req.Encoder)
	if res.ReturnCode == 0 && encoderV == "unknown" {
		if probed := probeEncoderVersionFromFFmpeg(ctx, ffmpegBinOrDefault(ffmpegBin),
			req.Encoder, run); probed != "" {
			encoderV = probed
		}
	}
	return EncodeResult{
		Request:         req,
		EncodeSizeBytes: size,
		EncodeTimeMS:    elapsedMS,
		EncoderVersion:  encoderV,
		FFmpegVersion:   ffmpegV,
		ExitStatus:      res.ReturnCode,
		StderrTail:      tail(res.Stderr, 2048),
	}
}

func ffmpegBinOrDefault(bin string) string {
	if bin == "" {
		return "ffmpeg"
	}
	return bin
}

// BuildPass1StatsCommand composes the ffmpeg argv for a stats-only pass-1
// invocation.
//
// It mirrors BuildFFmpegCommand but appends "-pass 1 -passlogfile <prefix>" and
// writes the bitstream to "-f null /dev/null": the encoder still runs the full
// RD loop (and thus emits the stats file) while the muxing is skipped. The
// stats file lands at "<prefix>-0.log" (plus an mbtree sidecar we ignore).
//
// The clip precedence matches BuildFFmpegCommand (ADR-0508 Bug #V8-A): without
// it, "ladder --duration 5" against a 9-minute source would run the pass-1
// sweep over the whole source.
func BuildPass1StatsCommand(req EncodeRequest, statsPrefix, ffmpegBin string) []string {
	cmd := []string{ffmpegBinOrDefault(ffmpegBin), "-y", "-hide_banner", "-loglevel", "info"}
	cmd = append(cmd, ffencode.InputArgs(req)...)
	cmd = append(cmd, "-i", req.Source)
	cmd = append(cmd, "-c:v", req.Encoder, "-preset", req.Preset, "-crf", strconv.Itoa(req.CRF))
	cmd = append(cmd, req.ExtraParams...)
	cmd = append(cmd, "-pass", "1", "-passlogfile", statsPrefix, "-f", "null", os.DevNull)
	return cmd
}

// statsFileFor is the path FFmpeg writes the stats file to under -passlogfile:
// it appends "-0.log" for the first (and in our case only) video stream.
func statsFileFor(prefix string) string {
	return filepath.Join(filepath.Dir(prefix), filepath.Base(prefix)+"-0.log")
}

// RunEncodeWithStats encodes req and captures the x264 / x265 pass-1 stats.
//
// It runs ffmpeg twice: a stats-only pass-1 whose "<tmp>-0.log" is parsed into
// PerFrameStats records, then the regular CRF encode that produces the
// bitstream the corpus scores. Per ADR-0332 this doubles the per-encode wall
// clock — the documented trade-off for closing the loop on the encoder's own
// rate-control ledger.
func RunEncodeWithStats(
	ctx context.Context, req EncodeRequest, ffmpegBin string, run Runner, statsDir string,
) EncodeResult {
	run = runnerOrExec(run)
	baseDir := statsDir
	if baseDir == "" {
		baseDir = os.TempDir()
	}
	if err := os.MkdirAll(baseDir, 0o750); err != nil {
		// Cannot stage the stats file; fall back to a plain encode rather
		// than failing the cell.
		return RunEncode(ctx, req, ffmpegBin, run)
	}
	// Deterministic prefix so test fixtures can pre-seed a stats file at
	// the expected path.
	stem := strings.TrimSuffix(filepath.Base(req.Output), filepath.Ext(req.Output))
	prefix := filepath.Join(baseDir, fmt.Sprintf("vmaftune_stats_%d_%s", os.Getpid(), stem))

	statsPath := statsFileFor(prefix)
	defer func() {
		_ = os.Remove(statsPath)
		_ = os.Remove(statsPath + ".mbtree")
	}()

	run(ctx, BuildPass1StatsCommand(req, prefix, ffmpegBin))
	frames := ParseStatsFile(statsPath)

	result := RunEncode(ctx, req, ffmpegBin, run)
	result.EncoderStats = frames
	return result
}

// twoPassCleanupCandidates lists the files known encoders create for a
// two-pass stats prefix.
func twoPassCleanupCandidates(statsPath string) []string {
	streamLog := statsFileFor(statsPath)
	return []string{
		statsPath,
		statsPath + ".cutree",
		streamLog,
		streamLog + ".mbtree",
	}
}

// RunTwoPassEncode drives a 2-pass ffmpeg encode (ADR-0333).
//
// Pass 1 redirects to "-f null -"; pass 2 writes req.Output. The combined
// result reports the pass-2 output size, the summed wall time, the pass-2
// version banners, and the first non-zero exit status.
//
// When the request's encoder declares SupportsTwoPass == false the driver logs
// a one-line warning to stderr and runs a single-pass encode, mirroring the
// saliency "unsupported ROI encoder" fallback precedent.
func RunTwoPassEncode(
	ctx context.Context, req EncodeRequest, ffmpegBin string, run Runner, scratchDir string,
) EncodeResult {
	run = runnerOrExec(run)
	adapter, err := codecadapter.Get(req.Encoder)
	if err != nil || !adapter.SupportsTwoPass {
		fmt.Fprintf(os.Stderr,
			"vmaf-tune: encoder %q does not support 2-pass encoding; falling back to single-pass.\n",
			req.Encoder)
		return RunEncode(ctx, req, ffmpegBin, run)
	}

	ownScratch := scratchDir == ""
	if ownScratch {
		dir, mkErr := os.MkdirTemp("", "vmaftune-2pass-")
		if mkErr != nil {
			return RunEncode(ctx, req, ffmpegBin, run)
		}
		scratchDir = dir
	}

	statsPath := statsPathFor(req, scratchDir)
	defer func() {
		for _, candidate := range twoPassCleanupCandidates(statsPath) {
			_ = os.Remove(candidate)
		}
		if ownScratch {
			_ = os.RemoveAll(scratchDir)
		}
	}()

	pass1Req := req
	pass1Req.PassNumber = 1
	pass1Req.StatsPath = statsPath
	pass1 := RunEncode(ctx, pass1Req, ffmpegBin, run)
	if pass1.ExitStatus != 0 {
		// Skip pass 2; surface the pass-1 failure with a clarifying tail
		// so the caller can disambiguate it from a pass-2 fault.
		pass1.Request = req
		pass1.StderrTail = "[pass 1 failed]\n" + pass1.StderrTail
		return pass1
	}

	pass2Req := req
	pass2Req.PassNumber = 2
	pass2Req.StatsPath = statsPath
	pass2 := RunEncode(ctx, pass2Req, ffmpegBin, run)

	return EncodeResult{
		Request:         req,
		EncodeSizeBytes: pass2.EncodeSizeBytes,
		EncodeTimeMS:    pass1.EncodeTimeMS + pass2.EncodeTimeMS,
		EncoderVersion:  pass2.EncoderVersion,
		FFmpegVersion:   pass2.FFmpegVersion,
		ExitStatus:      pass2.ExitStatus,
		StderrTail:      pass2.StderrTail,
	}
}

// statsPathFor builds a per-encode unique stats-file path under scratchDir.
// The name embeds the source stem, encoder, preset and CRF so a debug session
// can correlate it back to the encode that produced it; a short random suffix
// prevents collisions when the same cell runs more than once in parallel.
func statsPathFor(req EncodeRequest, scratchDir string) string {
	srcStem := strings.TrimSuffix(filepath.Base(req.Source), filepath.Ext(req.Source))
	stem := fmt.Sprintf("%s__%s__%s__crf%d__%s",
		srcStem, req.Encoder, req.Preset, req.CRF, shortToken())
	return filepath.Join(scratchDir, stem+".stats")
}

// BitrateKbps returns the file-size-derived bitrate, or 0 when the duration is
// non-positive.
func BitrateKbps(sizeBytes int64, durationS float64) float64 {
	if durationS <= 0 {
		return 0.0
	}
	return (float64(sizeBytes) * 8.0 / 1000.0) / durationS
}

// Cell is one (preset, quality) grid coordinate.
type Cell struct {
	Preset string
	CRF    int
}

// IterGrid returns the Cartesian product of presets x crfs as a deterministic
// slice.
func IterGrid(presets []string, crfs []int) []Cell {
	out := make([]Cell, 0, len(presets)*len(crfs))
	for _, p := range presets {
		for _, c := range crfs {
			out = append(out, Cell{Preset: p, CRF: c})
		}
	}
	return out
}
