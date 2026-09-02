// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/encodeprofile/encode.go — the single-pass slice of vmaftune.encode:
// argv composition, the ffmpeg/encoder version parser, and the subprocess
// driver.
//
// The argv builder and the version parser are pkg/ffencode's — the one Go
// port of vmaftune.encode (ADR-1137). This file keeps the encode-profile
// contract on top of them: the strict preset / quality check on a registered
// codec, and the argv-only Runner seam the profile tests stub.

package encodeprofile

import (
	"context"
	"os"
	"os/exec"
	"strings"
	"sync"
	"time"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/ffencode"
)

// BuildFFmpegCommand composes the ffmpeg argv for a single encode.
//
// Pure function — no I/O — so tests can pin the exact command line. The argv
// itself is ffencode.BuildFFmpegCommand's: input-side `-ss <start> -t <N>`
// so FFmpeg fast-seeks the source, the bound-duration fallback of ADR-0506,
// and the codec-adapter slice. What this wrapper adds is the encode-profile
// contract that an out-of-vocabulary preset or an out-of-window quality on a
// registered codec is an error rather than something spliced into the
// command line — the values come from a profile document an operator may
// have edited by hand. An unregistered encoder still falls back to the legacy
// libx264-shaped argv, as CPython does.
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
	if adapter, err := codecadapter.Get(req.Encoder); err == nil {
		if err := adapter.Validate(req.Preset, req.CRF); err != nil {
			return nil, err
		}
	}
	return ffencode.BuildFFmpegCommand(req, ffmpegBin)
}

// ParseVersions returns (ffmpegVersion, encoderVersion) extracted from stderr.
//
// It is ffencode.ParseVersions — the one port of vmaftune.encode.parse_versions
// — under this package's name; testdata/pv_expected.json pins the full
// stderr x encoder matrix against the Python function through it. Missing
// matches yield "unknown" rather than an error — corpus rows record what can
// be detected and move on.
func ParseVersions(stderr, encoder string) (string, string) {
	return ffencode.ParseVersions(stderr, encoder)
}

// probeCache memoises the `ffmpeg -version` fallback, keyed by
// (ffmpegBin, encoder) so a caller that swaps the binary path re-probes.
var probeCache sync.Map

// probeEncoderVersion returns a best-effort version label, or "" when nothing
// is parseable (ADR-0498 follow-up #7). Modern FFmpeg builds suppress the
// per-encoder banner under -hide_banner, which left corpus rows recording
// "unknown" even with the encoder plainly running. The configure-line marker
// per encoder is ffencode.ProbePattern's table.
func probeEncoderVersion(ffmpegBin, encoder string, run Runner) string {
	pattern, ok := ffencode.ProbePattern(encoder)
	if !ok {
		return ""
	}
	key := ffmpegBin + "\x00" + encoder
	if cached, hit := probeCache.Load(key); hit {
		if label, isString := cached.(string); isString {
			return label
		}
	}

	label := ""
	res, err := run([]string{ffmpegBin, "-version"})
	if err == nil && strings.Contains(res.Stdout+res.Stderr, pattern) {
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
