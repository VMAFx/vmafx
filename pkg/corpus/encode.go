// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/encode.go — Go port of vmaftune.encode.
//
// Builds the ffmpeg argv for a single (preset, crf) cell and drives the
// subprocess. BuildFFmpegCommand is a pure function so tests can pin the exact
// command line, which is what keeps the Go and Python sweeps producing
// identical encodes for the duration of the ADR-0703 / ADR-0704 migration.

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
	"github.com/VMAFx/vmafx/pkg/pyjson"
)

// EncodeRequest is a single (preset, crf) request against one source.
type EncodeRequest struct {
	Source    string
	Width     int
	Height    int
	PixFmt    string
	Framerate float64
	Encoder   string
	Preset    string
	CRF       int
	Output    string

	// ExtraParams are appended verbatim after the codec argv slice.
	ExtraParams []string

	// SampleClipSeconds opts the request into sample-clip mode (ADR-0297):
	// the ffmpeg input is sliced to the centre N-second window of the
	// reference. SampleClipStartS is the start offset, computed by the
	// caller so the score driver can mirror the same window via
	// --frame_skip_ref / --frame_cnt.
	SampleClipSeconds float64
	SampleClipStartS  float64

	// PassNumber is 0 (single-pass), 1 (analyse, write stats), or 2 (read
	// stats, encode). StatsPath is the per-encode stats file and is
	// required when PassNumber != 0 (ADR-0333).
	PassNumber int
	StatsPath  string

	// SourceIsContainer marks the source as a container (mkv/mp4/...) not
	// raw YUV: BuildFFmpegCommand then omits the -f rawvideo / -pix_fmt /
	// -s / -r input flags so ffmpeg auto-detects the format.
	SourceIsContainer bool

	// DurationS is the analysed-window length plumbed through from
	// Job.DurationS. When the caller did NOT opt into sample-clip mode but
	// did bind a duration, the encode is still clipped to that window
	// (ADR-0506 Bug #V6-1) — otherwise a 10-second smoke run would
	// re-encode the whole source.
	DurationS float64
}

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

// resolveCodecArgs resolves the codec-specific argv slice for req, routing
// through the adapter registry per ADR-0237 / ADR-0326 and falling back to the
// historic libx264 shape for unregistered encoders.
func resolveCodecArgs(req EncodeRequest) ([]string, error) {
	adapter, err := codecadapter.Get(req.Encoder)
	if err != nil {
		return codecadapter.LegacyCodecArgs(req.Encoder, req.Preset, req.CRF), nil
	}
	// ResolveCodecArgs is FFmpegCodecArgs followed by ExtraParams, which is
	// exactly what this call site assembled by hand before the two
	// codecadapter implementations were merged.
	return adapter.ResolveCodecArgs(req.Preset, req.CRF)
}

// inputSideClipArgs returns the -ss / -t block for the request's clip mode.
//
// Precedence mirrors build_ffmpeg_command: sample-clip mode wins (with an -ss
// start), otherwise a bound DurationS emits a plain -t, otherwise nothing.
func inputSideClipArgs(req EncodeRequest) []string {
	if req.SampleClipSeconds > 0.0 {
		return []string{
			"-ss", pyjson.FloatRepr(req.SampleClipStartS),
			"-t", pyjson.FloatRepr(req.SampleClipSeconds),
		}
	}
	if req.DurationS > 0.0 {
		return []string{"-t", pyjson.FloatRepr(req.DurationS)}
	}
	return nil
}

// BuildFFmpegCommand composes the ffmpeg argv for a single encode.
//
// Pure function — no I/O — so tests can pin the exact command line.
//
// -ss / -t are input-side options (before -i) so ffmpeg fast-seeks the raw YUV
// by skipping start*framerate frame-sized byte chunks; output-side seeking
// would still decode the whole source and defeat the speedup.
//
// When PassNumber != 0 the adapter's two-pass argv is spliced in before
// ExtraParams; pass 1 redirects the bitstream to "-f null -" while pass 2 keeps
// the requested Output destination.
func BuildFFmpegCommand(req EncodeRequest, ffmpegBin string) ([]string, error) {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	cmd := []string{ffmpegBin, "-y", "-hide_banner", "-loglevel", "info"}
	if !req.SourceIsContainer {
		// Raw YUV source: tell ffmpeg the format explicitly.
		cmd = append(cmd,
			"-f", "rawvideo",
			"-pix_fmt", req.PixFmt,
			"-s", fmt.Sprintf("%dx%d", req.Width, req.Height),
			"-r", pyjson.FloatRepr(req.Framerate),
		)
	}
	cmd = append(cmd, inputSideClipArgs(req)...)
	cmd = append(cmd, "-i", req.Source)

	codecArgs, err := resolveCodecArgs(req)
	if err != nil {
		return nil, err
	}
	cmd = append(cmd, codecArgs...)

	if req.PassNumber != 0 {
		if req.StatsPath == "" {
			return nil, fmt.Errorf("BuildFFmpegCommand: pass_number != 0 requires stats_path")
		}
		adapter, aErr := codecadapter.Get(req.Encoder)
		if aErr != nil {
			return nil, aErr
		}
		if !adapter.SupportsTwoPass {
			return nil, fmt.Errorf(
				"BuildFFmpegCommand: encoder %q does not support 2-pass encoding "+
					"(supports_two_pass = False)", req.Encoder)
		}
		twoPass, tErr := adapter.TwoPassArgs(req.PassNumber, req.StatsPath)
		if tErr != nil {
			return nil, tErr
		}
		cmd = append(cmd, twoPass...)
	}

	cmd = append(cmd, req.ExtraParams...)

	if req.PassNumber == 1 {
		// Pass 1 only writes the stats file; the bitstream is discarded
		// via the null muxer.
		cmd = append(cmd, "-f", "null", "-")
	} else {
		cmd = append(cmd, req.Output)
	}
	return cmd, nil
}

// Version-banner patterns. Kept in lockstep with encode.py's regex table.
var (
	ffmpegVersionRe = regexp.MustCompile(`ffmpeg version (\S+)`)
	// Accepts both "x264 - core 164" (the canonical libx264 banner) and the
	// "x264-core 164" variant some downstream builds emit in their
	// configure summary (ADR-0498 follow-up #7).
	x264VersionRe      = regexp.MustCompile(`x264\s*-?\s*core\s+(\d+)`)
	x265VersionRe      = regexp.MustCompile(`x265 \[info\]: HEVC encoder version (\S+)`)
	libvpxVP9VersionRe = regexp.MustCompile(`\[libvpx-vp9 @ [^\]]+\]\s+v(\S+)`)
	svtAV1VersionRe    = regexp.MustCompile(`(?i)SVT-AV1 Encoder(?:\s+Lib)?\s+v(\S+)`)
	libaomVersionRe    = regexp.MustCompile(
		`(?i)\[libaom(?:-av1)?\s*@\s*[^\]]+\]\s+(?:libaom-av1\s+v|AOM version:\s*)(\S+)`)
	libvvencVersionRe = regexp.MustCompile(
		`(?i)\[libvvenc\s*@\s*[^\]]+\]\s+(?:Fraunhofer\s+VVC/H\.266\s+Encoder\s+)?VVenC\s+v(\S+)`)
)

// hwEncoderTokens identify encoders that advertise no version in stderr; the
// encoder token itself is returned as the stable identifier.
var hwEncoderTokens = []string{"_nvenc", "_amf", "_qsv", "_videotoolbox"}

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
// encoder selects the per-codec version regex. Hardware encoders do not
// advertise a version, so the encoder token is returned verbatim. Missing
// matches yield "unknown" rather than an error — corpus rows record what can be
// detected and move on.
func ParseVersions(stderr, encoder string) (string, string) {
	ffm := firstSubmatch(ffmpegVersionRe, stderr)
	if ffm == "" {
		ffm = "unknown"
	}

	const defaultEncoder = "libx264"
	var enc string
	switch {
	case encoder == defaultEncoder || encoder == "":
		// Auto-detect: the x264 banner takes priority (it appears first
		// in multi-codec logs), then x265, then SVT-AV1.
		if v := firstSubmatch(x264VersionRe, stderr); v != "" {
			enc = "libx264-" + v
		} else if v := firstSubmatch(x265VersionRe, stderr); v != "" {
			enc = "libx265-" + v
		} else if v := firstSubmatch(svtAV1VersionRe, stderr); v != "" {
			enc = "libsvtav1-" + v
		} else {
			enc = "unknown"
		}
	case encoder == "libx265":
		enc = suffixOrUnknown("libx265", firstSubmatch(x265VersionRe, stderr))
	case encoder == "libsvtav1" || encoder == "libsvtav1-vbr":
		enc = suffixOrUnknown("libsvtav1", firstSubmatch(svtAV1VersionRe, stderr))
	case encoder == "libvpx-vp9":
		enc = suffixOrUnknown("libvpx-vp9", firstSubmatch(libvpxVP9VersionRe, stderr))
	case encoder == "libaom-av1":
		enc = suffixOrFallback("libaom-av1", firstSubmatch(libaomVersionRe, stderr))
	case encoder == "libvvenc":
		enc = suffixOrFallback("libvvenc", firstSubmatch(libvvencVersionRe, stderr))
	default:
		enc = "unknown"
		for _, tok := range hwEncoderTokens {
			if strings.Contains(encoder, tok) {
				enc = encoder
				break
			}
		}
	}
	return ffm, enc
}

// suffixOrUnknown joins name and version, or returns "unknown".
func suffixOrUnknown(name, version string) string {
	if version == "" {
		return "unknown"
	}
	return name + "-" + version
}

// suffixOrFallback joins name and version, or returns the stable adapter name
// when the banner is absent (quiet builds).
func suffixOrFallback(name, version string) string {
	if version == "" {
		return name
	}
	return name + "-" + version
}

// versionProbePatterns maps encoders to the configure-line marker that proves
// the codec was compiled into the ffmpeg binary (ADR-0498 follow-up #7).
var versionProbePatterns = map[string]*regexp.Regexp{
	"libx264":    regexp.MustCompile(`--enable-libx264`),
	"libsvtav1":  regexp.MustCompile(`--enable-libsvtav1`),
	"libx265":    regexp.MustCompile(`--enable-libx265`),
	"libvpx-vp9": regexp.MustCompile(`--enable-libvpx`),
	"libaom-av1": regexp.MustCompile(`--enable-libaom`),
	"libvvenc":   regexp.MustCompile(`--enable-libvvenc`),
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
	pattern, ok := versionProbePatterns[encoder]
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
	if pattern.MatchString(res.Stdout + res.Stderr) {
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
	if !req.SourceIsContainer {
		cmd = append(cmd,
			"-f", "rawvideo",
			"-pix_fmt", req.PixFmt,
			"-s", fmt.Sprintf("%dx%d", req.Width, req.Height),
			"-r", pyjson.FloatRepr(req.Framerate),
		)
	}
	cmd = append(cmd, inputSideClipArgs(req)...)
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
