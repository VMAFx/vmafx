// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/encodeprofile/encode.go — the single-pass slice of vmaftune.encode:
// argv composition, the ffmpeg/encoder version parsers, and the subprocess
// driver.

package encodeprofile

import (
	"context"
	"os"
	"os/exec"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/VMAFx/vmafx/internal/pyjson"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
)

// BuildFFmpegCommand composes the ffmpeg argv for a single encode.
//
// Pure function — no I/O — so tests can pin the exact command line.
//
// When SampleClipSeconds > 0, `-ss <start> -t <N>` are inserted as INPUT-side
// options (before -i) so FFmpeg fast-seeks the source by skipping whole
// frame-sized byte chunks. Output-side seeking would still decode (and the
// rawvideo demuxer would still read) the entire source, defeating the speedup.
//
// When the caller did not opt into sample-clip mode but the profile bound a
// duration, that duration becomes an input-side `-t` so the encode is bounded
// to the analysed window (ADR-0506) — otherwise a 9-minute source bound to a
// 10-second analysis window would encode all 9 minutes.
//
// Hardware-encoder caveat, inherited verbatim from Python: this argv carries
// NO `-init_hw_device` chain. FFmpeg's QSV bridge on Linux needs
// `-init_hw_device vaapi=va:<node> -init_hw_device qsv=qsv_dev@va
// -filter_hw_device va` before the first -i, plus a `format=nv12,hwupload`
// filter, or the encode fails with -22 (ADR-0601). vmaftune.compare injects
// that chain itself via its own pre-input argv; vmaftune.encode.
// build_ffmpeg_command — the function this ports, and the one encode-profile
// calls — never has. Emitting the chain here would make the Go --dry-run argv
// differ from Python's for every QSV row, so the gap is preserved and
// documented rather than silently fixed. See the package docs and the
// subcommand's --help.
func BuildFFmpegCommand(req EncodeRequest, ffmpegBin string) ([]string, error) {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	cmd := []string{ffmpegBin, "-y", "-hide_banner", "-loglevel", "info"}

	fallbackDuration := 0.0
	if req.SampleClipSeconds <= 0 && req.DurationS > 0 {
		fallbackDuration = req.DurationS
	}

	if !req.SourceIsContainer {
		// Raw source: FFmpeg must be told the format explicitly.
		cmd = append(cmd,
			"-f", "rawvideo",
			"-pix_fmt", req.PixFmt,
			"-s", strconv.Itoa(req.Width)+"x"+strconv.Itoa(req.Height),
			"-r", pyjson.Repr(req.Framerate),
		)
	}
	switch {
	case req.SampleClipSeconds > 0:
		cmd = append(cmd,
			"-ss", pyjson.Repr(req.SampleClipStartS),
			"-t", pyjson.Repr(req.SampleClipSeconds),
		)
	case fallbackDuration > 0:
		cmd = append(cmd, "-t", pyjson.Repr(fallbackDuration))
	}
	cmd = append(cmd, "-i", req.Source)

	codecArgs, err := codecadapter.ResolveCodecArgs(req.Encoder, req.Preset, req.CRF)
	if err != nil {
		return nil, err
	}
	cmd = append(cmd, codecArgs...)
	cmd = append(cmd, req.ExtraParams...)
	cmd = append(cmd, req.Output)
	return cmd, nil
}

// ---------------------------------------------------------------------------
// Version parsing
// ---------------------------------------------------------------------------

var (
	ffmpegVersionRe = regexp.MustCompile(`ffmpeg version (\S+)`)

	// x264 banner formats: the canonical "x264 - core 164 r3094 bfc87b7"
	// plus the defensive "x264-core 164" some downstream builds emit in their
	// configure summary (ADR-0498 follow-up #7).
	x264VersionRe = regexp.MustCompile(`x264\s*-?\s*core\s+(\d+)`)

	x265VersionRe    = regexp.MustCompile(`x265 \[info\]: HEVC encoder version (\S+)`)
	libvpxVP9Version = regexp.MustCompile(`\[libvpx-vp9 @ [^\]]+\]\s+v(\S+)`)

	// SVT-AV1 banner formats across versions: "SVT-AV1 ENCODER v1.7.0",
	// "Svt[info]:SVT-AV1 Encoder Lib v2.1.0", "SVT [info]: ... Lib v1.7.0".
	svtAV1VersionRe = regexp.MustCompile(`(?i)SVT-AV1 Encoder(?:\s+Lib)?\s+v(\S+)`)

	// libaom: FFmpeg emits "[libaom-av1 @ 0x...] libaom-av1 v3.x.y" or
	// "[libaom @ 0x...] AOM version: 3.x.y" depending on FFmpeg vintage.
	libaomVersionRe = regexp.MustCompile(
		`(?i)\[libaom(?:-av1)?\s*@\s*[^\]]+\]\s+(?:libaom-av1\s+v|AOM version:\s*)(\S+)`)

	// VVenC: "[libvvenc @ 0x...] VVenC v1.14.0", optionally prefixed with
	// "Fraunhofer VVC/H.266 Encoder".
	libvvencVersionRe = regexp.MustCompile(
		`(?i)\[libvvenc\s*@\s*[^\]]+\]\s+(?:Fraunhofer\s+VVC/H\.266\s+Encoder\s+)?VVenC\s+v(\S+)`)
)

// hwEncoderTokens identify hardware encoders, which advertise no version in
// stderr; the encoder token itself becomes the stable identifier.
var hwEncoderTokens = []string{"_nvenc", "_amf", "_qsv", "_videotoolbox"}

// ParseVersions returns (ffmpegVersion, encoderVersion) extracted from stderr.
//
// encoder selects the per-codec regex. Missing matches yield "unknown" rather
// than an error — corpus rows record what can be detected and move on.
func ParseVersions(stderr, encoder string) (string, string) {
	ffmpegVersion := "unknown"
	if m := ffmpegVersionRe.FindStringSubmatch(stderr); m != nil {
		ffmpegVersion = m[1]
	}

	var encoderVersion string
	switch {
	case encoder == "libx264" || encoder == "":
		// The caller did not pass an explicit encoder override (still at the
		// "libx264" default), so auto-detect: x264's banner appears first in
		// multi-codec logs, then x265, then SVT-AV1.
		switch {
		case x264VersionRe.MatchString(stderr):
			encoderVersion = "libx264-" + x264VersionRe.FindStringSubmatch(stderr)[1]
		case x265VersionRe.MatchString(stderr):
			encoderVersion = "libx265-" + x265VersionRe.FindStringSubmatch(stderr)[1]
		case svtAV1VersionRe.MatchString(stderr):
			encoderVersion = "libsvtav1-" + svtAV1VersionRe.FindStringSubmatch(stderr)[1]
		default:
			encoderVersion = "unknown"
		}
	case encoder == "libx265":
		encoderVersion = matchOr(x265VersionRe, stderr, "libx265-", "unknown")
	case encoder == "libsvtav1" || encoder == "libsvtav1-vbr":
		encoderVersion = matchOr(svtAV1VersionRe, stderr, "libsvtav1-", "unknown")
	case encoder == "libvpx-vp9":
		encoderVersion = matchOr(libvpxVP9Version, stderr, "libvpx-vp9-", "unknown")
	case encoder == "libaom-av1":
		// libaom emits a banner only on verbose builds; fall back to the
		// stable adapter name rather than "unknown".
		encoderVersion = matchOr(libaomVersionRe, stderr, "libaom-av1-", "libaom-av1")
	case encoder == "libvvenc":
		encoderVersion = matchOr(libvvencVersionRe, stderr, "libvvenc-", "libvvenc")
	default:
		// Hardware encoders advertise no version in stderr, so the encoder
		// token itself becomes the stable identifier.
		encoderVersion = "unknown"
		if slices.ContainsFunc(hwEncoderTokens, func(tok string) bool {
			return strings.Contains(encoder, tok)
		}) {
			encoderVersion = encoder
		}
	}

	return ffmpegVersion, encoderVersion
}

// matchOr returns prefix+capture on a match, or fallback.
func matchOr(re *regexp.Regexp, s, prefix, fallback string) string {
	if m := re.FindStringSubmatch(s); m != nil {
		return prefix + m[1]
	}
	return fallback
}

// versionProbePatterns maps an encoder onto the `ffmpeg -version` configure
// flag that proves it was compiled in. The configure summary carries no
// per-encoder version, so the probe settles for an "<encoder>-enabled" marker.
var versionProbePatterns = map[string]*regexp.Regexp{
	"libx264":    regexp.MustCompile(`--enable-libx264`),
	"libsvtav1":  regexp.MustCompile(`--enable-libsvtav1`),
	"libx265":    regexp.MustCompile(`--enable-libx265`),
	"libvpx-vp9": regexp.MustCompile(`--enable-libvpx`),
	"libaom-av1": regexp.MustCompile(`--enable-libaom`),
	"libvvenc":   regexp.MustCompile(`--enable-libvvenc`),
}

// probeCache memoises the `ffmpeg -version` fallback, keyed by
// (ffmpegBin, encoder) so a caller that swaps the binary path re-probes.
var probeCache sync.Map

// probeEncoderVersion returns a best-effort version label, or "" when nothing
// is parseable (ADR-0498 follow-up #7). Modern FFmpeg builds suppress the
// per-encoder banner under -hide_banner, which left corpus rows recording
// "unknown" even with the encoder plainly running.
func probeEncoderVersion(ffmpegBin, encoder string, run Runner) string {
	pattern, ok := versionProbePatterns[encoder]
	if !ok {
		return ""
	}
	key := ffmpegBin + "\x00" + encoder
	if cached, hit := probeCache.Load(key); hit {
		return cached.(string)
	}

	label := ""
	res, err := run([]string{ffmpegBin, "-version"})
	if err == nil && pattern.MatchString(res.Stdout+res.Stderr) {
		label = encoder + "-enabled"
	}
	probeCache.Store(key, label)
	return label
}

// ---------------------------------------------------------------------------
// Encode driver
// ---------------------------------------------------------------------------

// RunResult is one subprocess outcome. It mirrors the attributes the Python
// driver reads off subprocess.CompletedProcess.
type RunResult struct {
	Stdout     string
	Stderr     string
	ReturnCode int
}

// Runner executes an argv and returns its captured output. It is the
// integration seam: tests substitute a stub instead of running ffmpeg, exactly
// as the Python tests mock subprocess.run.
type Runner func(argv []string) (RunResult, error)

// EncodeResult is the outcome of one encode call.
type EncodeResult struct {
	Request         EncodeRequest
	EncodeSizeBytes int64
	EncodeTimeMS    float64
	EncoderVersion  string
	FFmpegVersion   string
	ExitStatus      int
	StderrTail      string
}

// stderrTailBytes is the stderr window the result carries, matching Python's
// _tail(stderr, n=2048).
const stderrTailBytes = 2048

// defaultEncodeTimeout bounds a single ffmpeg encode. A hung ffmpeg
// (deadlocked encoder, blocked I/O, GPU driver freeze) would otherwise pin the
// command forever. It mirrors pkg/encoder's own bound, and its env override,
// so both encode paths behave the same. Python has no timeout here — that is a
// deliberate improvement, not a parity gap, and it only changes behaviour in
// the case where Python would hang indefinitely.
const defaultEncodeTimeout = 60 * time.Minute

// encodeTimeout returns the per-call ffmpeg timeout, honouring
// VMAFX_TUNE_ENCODE_TIMEOUT. A non-positive value disables the bound.
func encodeTimeout() time.Duration {
	if raw := os.Getenv("VMAFX_TUNE_ENCODE_TIMEOUT"); raw != "" {
		if d, err := time.ParseDuration(raw); err == nil {
			return d
		}
	}
	return defaultEncodeTimeout
}

// ExecRunner is the production Runner: it shells out to the given argv and
// captures stdout and stderr separately.
func ExecRunner(argv []string) (RunResult, error) {
	if len(argv) == 0 {
		return RunResult{}, os.ErrInvalid
	}
	ctx := context.Background()
	cancel := func() {}
	if to := encodeTimeout(); to > 0 {
		ctx, cancel = context.WithTimeout(ctx, to)
	}
	defer cancel()

	// #nosec G204 -- argv[0] is the operator-supplied ffmpeg binary and the
	// tail is composed by BuildFFmpegCommand from profile values and CLI
	// flags. No shell is involved; ctx enforces encodeTimeout().
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	var stdout, stderr strings.Builder
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr

	err := cmd.Run()
	res := RunResult{Stdout: stdout.String(), Stderr: stderr.String()}
	if err != nil {
		var exitErr *exec.ExitError
		if ok := errorsAs(err, &exitErr); ok {
			// A non-zero exit is a normal outcome here — Python passes
			// check=False and reads returncode — so it is not an error.
			res.ReturnCode = exitErr.ExitCode()
			return res, nil
		}
		// The binary could not be started at all (missing, not executable,
		// timed out). Python's subprocess.run raises here too.
		return res, err
	}
	return res, nil
}

// RunEncode drives ffmpeg to produce req.Output.
func RunEncode(req EncodeRequest, ffmpegBin string, run Runner) (EncodeResult, error) {
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	if run == nil {
		run = ExecRunner
	}
	argv, err := BuildFFmpegCommand(req, ffmpegBin)
	if err != nil {
		return EncodeResult{}, err
	}

	started := time.Now()
	res, runErr := run(argv)
	elapsedMS := float64(time.Since(started).Nanoseconds()) / 1e6

	rc := res.ReturnCode
	if runErr != nil {
		// Python's subprocess.run would raise; the CLI surfaces that as a
		// failure rather than a zero-exit encode.
		return EncodeResult{}, runErr
	}

	var size int64
	if rc == 0 {
		if st, statErr := os.Stat(req.Output); statErr == nil {
			size = st.Size()
		}
	}

	ffmpegVersion, encoderVersion := ParseVersions(res.Stderr, req.Encoder)
	if rc == 0 && encoderVersion == "unknown" {
		if probed := probeEncoderVersion(ffmpegBin, req.Encoder, run); probed != "" {
			encoderVersion = probed
		}
	}

	return EncodeResult{
		Request:         req,
		EncodeSizeBytes: size,
		EncodeTimeMS:    elapsedMS,
		EncoderVersion:  encoderVersion,
		FFmpegVersion:   ffmpegVersion,
		ExitStatus:      rc,
		StderrTail:      tail(res.Stderr, stderrTailBytes),
	}, nil
}

// tail returns the last n characters of s.
func tail(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[len(s)-n:]
}
