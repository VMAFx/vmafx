// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package ffencode is the Go port of tools/vmaf-tune/src/vmaftune/encode.py —
// the FFmpeg encode driver the tuning subcommands share.
//
// It is deliberately separate from pkg/encoder: that package models the
// Stage-1 bisect abstraction (an Encoder interface keyed on CRF, container
// input only, temp-file output). The tuning subcommands need the richer
// Python EncodeRequest shape — raw-YUV geometry, named presets, an explicit
// output path, injected extra params (the deband -vf fragment, the saliency
// ROI sidecar flags) and sample-clip windowing — so this package ports that
// shape verbatim rather than bending pkg/encoder out of contract.
//
// BuildFFmpegCommand is a pure function so tests pin the exact argv, matching
// the Python module's own test posture.
package ffencode

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
)

// DefaultEncodeTimeout bounds a single ffmpeg encode subprocess. Override via
// VMAFX_TUNE_ENCODE_TIMEOUT (a Go duration); 0 or negative disables it.
// Mirrors pkg/encoder's posture so both drivers behave alike under a wedged
// encoder or a blocked GPU driver.
const DefaultEncodeTimeout = 60 * time.Minute

// encodeTimeout returns the per-call ffmpeg timeout honouring the operator
// override.
func encodeTimeout() time.Duration {
	if raw := os.Getenv("VMAFX_TUNE_ENCODE_TIMEOUT"); raw != "" {
		if d, err := time.ParseDuration(raw); err == nil {
			return d
		}
	}
	return DefaultEncodeTimeout
}

// Request mirrors vmaftune.encode.EncodeRequest.
type Request struct {
	Source    string
	Width     int
	Height    int
	PixFmt    string
	Framerate float64
	Encoder   string
	Preset    string
	CRF       int
	Output    string

	// ExtraParams are appended after the codec args and before the output
	// path — the injection point for the prefilter -vf fragment and the
	// saliency ROI sidecar flags.
	ExtraParams []string

	// SampleClipSeconds > 0 enables input-side -ss/-t fast seeking.
	SampleClipSeconds float64
	SampleClipStartS  float64

	// PassNumber selects the Nth pass of a 2-pass encode; 0 = single pass.
	PassNumber int
	StatsPath  string

	// SourceIsContainer selects container demuxing over raw-YUV geometry.
	SourceIsContainer bool

	// DurationS bounds the encode when SampleClipSeconds is unset. Also used
	// to derive achieved kbps from the output size.
	DurationS float64
}

// Result mirrors vmaftune.encode.EncodeResult.
type Result struct {
	Request         Request
	EncodeSizeBytes int64
	EncodeTimeMS    float64
	EncoderVersion  string
	FFmpegVersion   string
	ExitStatus      int
	StderrTail      string
}

// BuildFFmpegCommand composes the ffmpeg argv for a single encode. Pure
// function — no I/O — so tests pin the exact command line.
//
// When SampleClipSeconds > 0, -ss/-t are inserted as input-side options
// (before -i) so FFmpeg fast-seeks the raw YUV by skipping frame-sized byte
// chunks. Output-side seeking would still decode the whole source.
//
// When PassNumber != 0 the adapter's two-pass argv is spliced in before
// ExtraParams; pass 1 redirects the bitstream to "-f null -" (the stats file
// is the only artefact that matters) while pass 2 keeps Output.
func BuildFFmpegCommand(req Request, ffmpegBin string) ([]string, error) {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	cmd := []string{ffmpegBin, "-y", "-hide_banner", "-loglevel", "info"}

	// Honour DurationS as an input-side -t when the caller did not opt into
	// sample-clip mode, so a 10 s analysis window does not encode a 9-minute
	// source (ADR-0506 / BBB e2e v6 bug V6-1).
	fallbackDuration := 0.0
	if req.SampleClipSeconds <= 0.0 && req.DurationS > 0.0 {
		fallbackDuration = req.DurationS
	}

	if !req.SourceIsContainer {
		cmd = append(cmd,
			"-f", "rawvideo",
			"-pix_fmt", req.PixFmt,
			"-s", fmt.Sprintf("%dx%d", req.Width, req.Height),
			"-r", formatFloat(req.Framerate),
		)
	}
	switch {
	case req.SampleClipSeconds > 0.0:
		cmd = append(cmd,
			"-ss", formatFloat(req.SampleClipStartS),
			"-t", formatFloat(req.SampleClipSeconds))
	case fallbackDuration > 0.0:
		cmd = append(cmd, "-t", formatFloat(fallbackDuration))
	}
	cmd = append(cmd, "-i", req.Source)

	codecArgs, err := resolveCodecArgs(req)
	if err != nil {
		return nil, err
	}
	cmd = append(cmd, codecArgs...)

	if req.PassNumber != 0 {
		if req.StatsPath == "" {
			return nil, errors.New("BuildFFmpegCommand: PassNumber != 0 requires StatsPath")
		}
		adapter, adapterErr := codecadapter.Get(req.Encoder)
		if adapterErr != nil {
			return nil, adapterErr
		}
		passArgs, passErr := adapter.TwoPassArgs(req.PassNumber, req.StatsPath)
		if passErr != nil {
			return nil, passErr
		}
		cmd = append(cmd, passArgs...)
	}

	cmd = append(cmd, req.ExtraParams...)

	if req.PassNumber == 1 {
		cmd = append(cmd, "-f", "null", "-")
	} else {
		cmd = append(cmd, req.Output)
	}
	return cmd, nil
}

// formatFloat renders a float the way Python's str() does for the values this
// driver emits, so the argv matches the Python original byte for byte
// (24.0 -> "24.0", 0.5 -> "0.5", 10 -> "10.0").
func formatFloat(v float64) string {
	s := strconv.FormatFloat(v, 'g', -1, 64)
	if !strings.ContainsAny(s, ".eE") {
		s += ".0"
	}
	return s
}

// resolveCodecArgs routes through the codec-adapter registry, falling back to
// the historic "-c:v <enc> -preset <p> -crf <q>" shape for unregistered
// encoders (the Python _legacy_codec_args path).
func resolveCodecArgs(req Request) ([]string, error) {
	adapter, err := codecadapter.Get(req.Encoder)
	if err != nil {
		return []string{
			"-c:v", req.Encoder, "-preset", req.Preset, "-crf", strconv.Itoa(req.CRF),
		}, nil
	}
	return adapter.ResolveCodecArgs(req.Preset, req.CRF)
}

var (
	ffmpegVersionRE = regexp.MustCompile(`ffmpeg version (\S+)`)
	// x264 banners appear as "x264 - core 164" (canonical) and "x264-core
	// 164" (some downstream configure summaries) — accept both.
	x264VersionRE   = regexp.MustCompile(`x264\s*-?\s*core\s+(\d+)`)
	x265VersionRE   = regexp.MustCompile(`x265 \[info\]: HEVC encoder version (\S+)`)
	vp9VersionRE    = regexp.MustCompile(`\[libvpx-vp9 @ [^\]]+\]\s+v(\S+)`)
	svtAV1VersionRE = regexp.MustCompile(`(?i)SVT-AV1 Encoder(?:\s+Lib)?\s+v(\S+)`)
	libaomVersionRE = regexp.MustCompile(
		`(?i)\[libaom(?:-av1)?\s*@\s*[^\]]+\]\s+(?:libaom-av1\s+v|AOM version:\s*)(\S+)`)
	vvencVersionRE = regexp.MustCompile(
		`(?i)\[libvvenc\s*@\s*[^\]]+\]\s+(?:Fraunhofer\s+VVC/H\.266\s+Encoder\s+)?VVenC\s+v(\S+)`)
)

// hwTokens are the substrings that mark a hardware encoder. Those advertise
// no version in stderr, so the encoder token itself is the stable identifier.
var hwTokens = []string{"_nvenc", "_amf", "_qsv", "_videotoolbox"}

// match returns "<label>-<captured>" when re matches stderr, else "".
func match(re *regexp.Regexp, stderr, label string) string {
	if m := re.FindStringSubmatch(stderr); m != nil {
		return label + "-" + m[1]
	}
	return ""
}

// ParseVersions extracts (ffmpegVersion, encoderVersion) from ffmpeg's
// stderr, mirroring vmaftune.encode.parse_versions exactly.
//
// Missing matches return "unknown" rather than erroring — corpus rows record
// what can be detected and move on. libaom-av1 and libvvenc fall back to the
// bare adapter name (their banners are suppressed on quiet builds); hardware
// encoders return their token verbatim.
func ParseVersions(stderr, encoder string) (string, string) {
	ffmpegVersion := "unknown"
	if m := ffmpegVersionRE.FindStringSubmatch(stderr); m != nil {
		ffmpegVersion = m[1]
	}

	encoderVersion := "unknown"
	switch {
	case encoder == "" || encoder == "libx264":
		// Auto-detect: x264 banner wins (it appears first in multi-codec
		// logs), then x265, then SVT-AV1.
		for _, candidate := range []string{
			match(x264VersionRE, stderr, "libx264"),
			match(x265VersionRE, stderr, "libx265"),
			match(svtAV1VersionRE, stderr, "libsvtav1"),
		} {
			if candidate != "" {
				encoderVersion = candidate
				break
			}
		}
	case encoder == "libx265":
		if v := match(x265VersionRE, stderr, "libx265"); v != "" {
			encoderVersion = v
		}
	case encoder == "libsvtav1" || encoder == "libsvtav1-vbr":
		if v := match(svtAV1VersionRE, stderr, "libsvtav1"); v != "" {
			encoderVersion = v
		}
	case encoder == "libvpx-vp9":
		if v := match(vp9VersionRE, stderr, "libvpx-vp9"); v != "" {
			encoderVersion = v
		}
	case encoder == "libaom-av1":
		encoderVersion = "libaom-av1"
		if v := match(libaomVersionRE, stderr, "libaom-av1"); v != "" {
			encoderVersion = v
		}
	case encoder == "libvvenc":
		encoderVersion = "libvvenc"
		if v := match(vvencVersionRE, stderr, "libvvenc"); v != "" {
			encoderVersion = v
		}
	default:
		for _, tok := range hwTokens {
			if strings.Contains(encoder, tok) {
				encoderVersion = encoder
				break
			}
		}
	}
	return ffmpegVersion, encoderVersion
}

// versionProbePatterns map an encoder onto the "--enable-<codec>" token that
// confirms it is compiled into the ffmpeg binary (ADR-0498 follow-up #7,
// ADR-1077 for the libaom / VVenC entries).
var versionProbePatterns = map[string]string{
	"libx264":    "--enable-libx264",
	"libsvtav1":  "--enable-libsvtav1",
	"libx265":    "--enable-libx265",
	"libvpx-vp9": "--enable-libvpx",
	"libaom-av1": "--enable-libaom",
	"libvvenc":   "--enable-libvvenc",
}

// probeCache memoises the per-(binary, encoder) availability probe. The
// answer cannot change within one process, and a sweep would otherwise fork
// `ffmpeg -version` once per cell.
var (
	probeCacheMu sync.Mutex
	probeCache   = map[string]string{}
)

// ProbeEncoderLabel returns a stable availability label such as
// "libx264-enabled", or "" when nothing confirms the encoder.
//
// Modern ffmpeg builds suppress the per-encoder banner under -hide_banner, so
// ParseVersions often reports "unknown" even on a perfectly good encode. This
// fallback reads `ffmpeg -version`'s configuration line instead, matching
// what the Python driver does before it gives up on the version field.
func ProbeEncoderLabel(ctx context.Context, ffmpegBin, encoder string, runner Runner) string {
	pattern, ok := versionProbePatterns[encoder]
	if !ok {
		return ""
	}
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	key := ffmpegBin + "\x00" + encoder

	probeCacheMu.Lock()
	cached, hit := probeCache[key]
	probeCacheMu.Unlock()
	if hit {
		return cached
	}

	if runner == nil {
		runner = defaultRunner
	}
	out, _, err := runner(ctx, []string{ffmpegBin, "-version"})
	label := ""
	if err == nil && strings.Contains(out, pattern) {
		label = encoder + "-enabled"
	}

	probeCacheMu.Lock()
	probeCache[key] = label
	probeCacheMu.Unlock()
	return label
}

// Runner is the subprocess seam. Tests inject a stub; production passes nil
// to get the real exec.CommandContext runner.
type Runner func(ctx context.Context, argv []string) (stderr string, exitStatus int, err error)

// defaultRunner executes argv with a timeout and returns its combined output.
func defaultRunner(ctx context.Context, argv []string) (string, int, error) {
	cancel := func() {}
	if _, hasDeadline := ctx.Deadline(); !hasDeadline {
		if to := encodeTimeout(); to > 0 {
			ctx, cancel = context.WithTimeout(ctx, to)
		}
	}
	defer cancel()

	// #nosec G204 -- argv[0] is the operator-configured ffmpeg binary and
	// argv[1:] is assembled by BuildFFmpegCommand from validated CLI flags.
	// ctx enforces encodeTimeout().
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

// Run executes one encode and reports its size, wall time and versions.
//
// A non-zero ffmpeg exit is NOT an error return: it is reported through
// Result.ExitStatus with the stderr tail attached, matching the Python
// driver's contract (the search loops score a failed probe as 0 VMAF rather
// than aborting the sweep). Only a failure to spawn ffmpeg at all, or a
// malformed request, returns an error.
func Run(ctx context.Context, req Request, ffmpegBin string, runner Runner) (Result, error) {
	argv, err := BuildFFmpegCommand(req, ffmpegBin)
	if err != nil {
		return Result{}, err
	}
	if runner == nil {
		runner = defaultRunner
	}
	if req.Output != "" && req.PassNumber != 1 {
		// G301: 0o750 keeps the encode dir owner+group readable only.
		if mkErr := os.MkdirAll(filepath.Dir(req.Output), 0o750); mkErr != nil {
			return Result{}, fmt.Errorf("create encode dir: %w", mkErr)
		}
	}

	t0 := time.Now()
	stderr, exitStatus, runErr := runner(ctx, argv)
	elapsed := time.Since(t0)
	if runErr != nil {
		return Result{}, fmt.Errorf("run ffmpeg: %w", runErr)
	}

	ffmpegVersion, encoderVersion := ParseVersions(stderr, req.Encoder)
	if exitStatus == 0 && encoderVersion == "unknown" {
		if label := ProbeEncoderLabel(ctx, ffmpegBin, req.Encoder, runner); label != "" {
			encoderVersion = label
		}
	}

	var size int64
	// Pass 1 of a 2-pass encode writes only the stats file; the bitstream
	// goes to the null muxer and req.Output is never produced, so a size
	// probe there would report a spurious zero that callers read as failure.
	if exitStatus == 0 && req.PassNumber != 1 {
		if info, statErr := os.Stat(req.Output); statErr == nil {
			size = info.Size()
		}
	}

	return Result{
		Request:         req,
		EncodeSizeBytes: size,
		// Sub-millisecond resolution: Duration.Milliseconds() truncates, and
		// a short probe encode rounds to a misleading whole number.
		EncodeTimeMS:   float64(elapsed.Nanoseconds()) / 1e6,
		EncoderVersion: encoderVersion,
		FFmpegVersion:  ffmpegVersion,
		ExitStatus:     exitStatus,
		StderrTail:     tail(stderr, 4000),
	}, nil
}

// tail returns the last n bytes of text (whole string when shorter).
func tail(text string, n int) string {
	if len(text) <= n {
		return text
	}
	return text[len(text)-n:]
}

// BitrateKbps converts an encoded file size to kilobits per second over
// durationS. Returns 0 when the duration is unknown — bitrate is undefined
// without one, and inventing a number would poison the search objective.
func BitrateKbps(sizeBytes int64, durationS float64) float64 {
	if durationS <= 0.0 || sizeBytes <= 0 {
		return 0.0
	}
	return float64(sizeBytes) * 8.0 / 1000.0 / durationS
}
