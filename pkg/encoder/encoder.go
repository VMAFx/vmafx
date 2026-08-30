// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package encoder defines the Encoder interface and software encoder
// implementations for vmafx-tune. Hardware encoders (NVENC, QSV, AMF) are
// Stage-2 scope; this package ships libx264 and libx265 only.
//
// Each encoder shells out to ffmpeg for the actual encode so there is no
// dependency on libavcodec at the Go layer.
package encoder

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// defaultEncodeTimeout is the upper bound on a single ffmpeg encode
// subprocess. A hung ffmpeg (deadlocked encoder, blocked I/O, GPU driver
// freeze) would otherwise pin the bisect loop forever. Override via
// VMAFX_TUNE_ENCODE_TIMEOUT (Go duration). 0 or negative disables the
// timeout.
const defaultEncodeTimeout = 60 * time.Minute

// defaultProbeTimeout bounds ffprobe invocations. Probing a finite-size
// container file should take well under a second; a 30-second cap is
// generous enough to absorb cold-cache stalls without indefinitely
// stalling a sweep. Override via VMAFX_TUNE_PROBE_TIMEOUT.
const defaultProbeTimeout = 30 * time.Second

// encodeTimeout returns the per-call ffmpeg-encode timeout, honouring
// VMAFX_TUNE_ENCODE_TIMEOUT for operator overrides.
func encodeTimeout() time.Duration {
	if raw := os.Getenv("VMAFX_TUNE_ENCODE_TIMEOUT"); raw != "" {
		if d, err := time.ParseDuration(raw); err == nil {
			return d
		}
	}
	return defaultEncodeTimeout
}

// probeTimeout returns the per-call ffprobe timeout, honouring
// VMAFX_TUNE_PROBE_TIMEOUT for operator overrides.
func probeTimeout() time.Duration {
	if raw := os.Getenv("VMAFX_TUNE_PROBE_TIMEOUT"); raw != "" {
		if d, err := time.ParseDuration(raw); err == nil {
			return d
		}
	}
	return defaultProbeTimeout
}

// EncodeParams holds the per-encode configuration passed to Encoder.Encode.
type EncodeParams struct {
	// CRF is the quality parameter for the encoder. Higher = smaller file,
	// lower quality. Range is codec-specific (x264/x265: 0-51).
	CRF int

	// FFmpegBin is the path to the ffmpeg binary. Defaults to "ffmpeg" (PATH
	// lookup) when empty.
	FFmpegBin string

	// OutputDir is the directory where the encoded file is written.
	// Defaults to os.TempDir() when empty.
	OutputDir string

	// ExtraArgs are passed verbatim to ffmpeg after the encoder -c:v flag.
	// Use sparingly; Stage-1 does not expose these to the CLI.
	ExtraArgs []string

	// InputArgs are passed verbatim to ffmpeg *before* the "-i <src>" pair.
	// They carry demuxer / geometry options that ffmpeg only accepts as input
	// options, e.g. the raw-video quartet
	//   -f rawvideo -pix_fmt yuv420p -s 1920x1080 -r 24
	// required when src is a headerless planar YUV file. ExtraArgs cannot be
	// used for this: they land after "-c:v", where ffmpeg treats them as
	// output options and a raw-YUV input would be mis-probed.
	//
	// Added for the per-shot tuner (pkg/pershot), which extracts each detected
	// shot to a raw YUV file before bisecting it. Empty for container inputs.
	InputArgs []string
}

// EncodeResult holds the outcome of a single encode.
type EncodeResult struct {
	// OutputPath is the path to the encoded file.
	OutputPath string

	// BitratekBps is the bitrate of the encoded file in kilobits per second.
	BitratekBps float64

	// EncoderVersion is the version string reported by the encoder (e.g.
	// "core 164 r3094M 0bfe3e0" for x264). Empty when not parseable.
	EncoderVersion string

	// EncodeTimeMS is the wall time of the encode in milliseconds.
	EncodeTimeMS float64
}

// Encoder abstracts a single software video encoder.
type Encoder interface {
	// Name returns the ffmpeg codec name (e.g. "libx264", "libx265").
	Name() string

	// Encode encodes src using params and returns the result. The caller is
	// responsible for removing the output file when it is no longer needed.
	Encode(src string, params EncodeParams) (EncodeResult, error)

	// CRFRange returns the (min, max) CRF values supported by this encoder.
	// min = lowest CRF (best quality / largest file);
	// max = highest CRF (worst quality / smallest file).
	CRFRange() (int, int)
}

// ffmpegBin returns the ffmpeg binary path, defaulting to "ffmpeg".
func ffmpegBin(params EncodeParams) string {
	if params.FFmpegBin != "" {
		return params.FFmpegBin
	}
	return "ffmpeg"
}

// outputDir returns the output directory, defaulting to os.TempDir().
func outputDir(params EncodeParams) string {
	if params.OutputDir != "" {
		return params.OutputDir
	}
	return os.TempDir()
}

// probeBitrateKbps reads the bitrate of path by invoking ffprobe.
// Returns 0.0 when ffprobe is unavailable or the file cannot be probed.
func probeBitrateKbps(path, ffmpegBin string) float64 {
	// Derive ffprobe path from ffmpeg path.
	dir := filepath.Dir(ffmpegBin)
	var probeBin string
	if dir == "." {
		probeBin = "ffprobe"
	} else {
		probeBin = filepath.Join(dir, "ffprobe")
	}
	ctx := context.Background()
	cancel := func() {}
	if to := probeTimeout(); to > 0 {
		ctx, cancel = context.WithTimeout(ctx, to)
	}
	defer cancel()
	// #nosec G204 -- probeBin is derived from ffmpegBin (operator-configured
	// EncodeParams) via filepath.Join; `path` is the temp file produced by
	// runEncode (os.CreateTemp output). ctx enforces probeTimeout().
	out, err := exec.CommandContext(
		ctx, probeBin,
		"-v", "error",
		"-select_streams", "v:0",
		"-show_entries", "stream=bit_rate",
		"-of", "default=noprint_wrappers=1:nokey=1",
		path,
	).Output()
	if err != nil {
		return 0.0
	}
	s := strings.TrimSpace(string(out))
	bps, err := strconv.ParseFloat(s, 64)
	if err != nil || bps <= 0 {
		return 0.0
	}
	return bps / 1000.0
}

// extractEncoderVersion extracts the encoder version string from ffmpeg
// stderr. Returns empty string when not found.
func extractEncoderVersion(stderr string, codec string) string {
	// ffmpeg emits lines like:
	//   libx264 - core 164 r3094M 0bfe3e0 H.264/MPEG-4 AVC codec
	// We look for a line containing the codec name.
	for _, line := range strings.Split(stderr, "\n") {
		if strings.Contains(line, codec) && strings.Contains(line, "core") {
			trimmed := strings.TrimSpace(line)
			// Drop leading whitespace / dash separators
			if idx := strings.Index(trimmed, "-"); idx >= 0 {
				return strings.TrimSpace(trimmed[idx+1:])
			}
			return trimmed
		}
	}
	return ""
}

// runEncode invokes ffmpeg to encode src to a temporary MKV file and returns
// the result. codec is the ffmpeg encoder name (e.g. "libx264").
//
// It is the fixed-shape wrapper around runEncodeArgv used by every built-in
// Encoder implementation: the codec argv is always the "-c:v <codec>
// <crfFlag> <CRF>" triple. Callers that need a codec-specific argv shape
// (preset tokens, "-rc cqp -qp_i N -qp_p N", "-cpu-used N", ...) go through
// AdapterEncoder, which calls runEncodeArgv directly.
func runEncode(src string, params EncodeParams, codec string, crfFlag string) (EncodeResult, error) {
	codecArgs := []string{"-c:v", codec, crfFlag, strconv.Itoa(params.CRF)}
	return runEncodeArgv(src, params, codec, codecArgs)
}

// runEncodeArgv invokes ffmpeg to encode src to a temporary MKV file using the
// caller-supplied codec argv slice (everything from "-c:v" onwards, excluding
// the output path). codecName is used only for the temp-file name and for
// encoder-version extraction from ffmpeg stderr.
func runEncodeArgv(
	src string,
	params EncodeParams,
	codecName string,
	codecArgs []string,
) (EncodeResult, error) {
	codec := codecName
	bin := ffmpegBin(params)
	dir := outputDir(params)

	// Create a temp file with the right extension.
	tmpFile, err := os.CreateTemp(dir, fmt.Sprintf("vmafx-tune-%s-crf%d-*.mkv", codec, params.CRF))
	if err != nil {
		return EncodeResult{}, fmt.Errorf("create temp output: %w", err)
	}
	outPath := tmpFile.Name()
	// Close immediately; ffmpeg will write to it.
	if closeErr := tmpFile.Close(); closeErr != nil {
		return EncodeResult{}, fmt.Errorf("close temp file: %w", closeErr)
	}
	// Remove the empty placeholder so ffmpeg can create the real file.
	if removeErr := os.Remove(outPath); removeErr != nil {
		return EncodeResult{}, fmt.Errorf("remove temp placeholder: %w", removeErr)
	}

	argv := []string{
		bin,
		"-hide_banner",
		"-loglevel", "warning",
		"-y",
	}
	// Input options must precede "-i"; see EncodeParams.InputArgs.
	argv = append(argv, params.InputArgs...)
	argv = append(argv, "-i", src, "-an") // -an drops audio
	argv = append(argv, codecArgs...)
	argv = append(argv, params.ExtraArgs...)
	argv = append(argv, outPath)

	ctx := context.Background()
	cancel := func() {}
	if to := encodeTimeout(); to > 0 {
		ctx, cancel = context.WithTimeout(ctx, to)
	}
	defer cancel()

	t0 := time.Now()
	// #nosec G204 -- argv[0] is ffmpegBin (operator-configured via
	// EncodeParams); argv[1:] mixes fixed flags with `src` (validated by the
	// caller / `pkg/storage` mount path), integer CRF, and the os.CreateTemp
	// output path. ctx enforces encodeTimeout().
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	// Capture stderr for version extraction; stdout is discarded.
	stderrBytes, runErr := cmd.CombinedOutput()
	elapsed := time.Since(t0)

	if runErr != nil {
		// Clean up temp file if encode failed. Surface any cleanup failure
		// via errors.Join so a disk-leak does not get silently swallowed by
		// the primary encode error.
		var ffErr error
		if ctx.Err() == context.DeadlineExceeded {
			ffErr = fmt.Errorf("ffmpeg encode timed out after %s (crf=%d, codec=%s): %w\n%s",
				encodeTimeout(), params.CRF, codec, runErr, string(stderrBytes))
		} else {
			ffErr = fmt.Errorf("ffmpeg encode failed (crf=%d, codec=%s): %w\n%s",
				params.CRF, codec, runErr, string(stderrBytes))
		}
		if rmErr := os.Remove(outPath); rmErr != nil && !errors.Is(rmErr, os.ErrNotExist) {
			return EncodeResult{}, errors.Join(
				ffErr,
				fmt.Errorf("remove failed-encode temp %q: %w", outPath, rmErr),
			)
		}
		return EncodeResult{}, ffErr
	}

	bitrateKbps := probeBitrateKbps(outPath, bin)
	version := extractEncoderVersion(string(stderrBytes), codec)

	return EncodeResult{
		OutputPath:     outPath,
		BitratekBps:    bitrateKbps,
		EncoderVersion: version,
		EncodeTimeMS:   float64(elapsed.Milliseconds()),
	}, nil
}

// LibX264Encoder implements Encoder for libx264.
type LibX264Encoder struct{}

// Name returns "libx264".
func (e LibX264Encoder) Name() string { return "libx264" }

// CRFRange returns [0, 51].
func (e LibX264Encoder) CRFRange() (int, int) { return 0, 51 }

// Encode encodes src at the given CRF using libx264.
func (e LibX264Encoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	return runEncode(src, params, "libx264", "-crf")
}

// LibX265Encoder implements Encoder for libx265.
type LibX265Encoder struct{}

// Name returns "libx265".
func (e LibX265Encoder) Name() string { return "libx265" }

// CRFRange returns [0, 51].
func (e LibX265Encoder) CRFRange() (int, int) { return 0, 51 }

// Encode encodes src at the given CRF using libx265.
func (e LibX265Encoder) Encode(src string, params EncodeParams) (EncodeResult, error) {
	return runEncode(src, params, "libx265", "-crf")
}

// KnownEncoders returns the set of software encoder names supported in Stage 1.
func KnownEncoders() []string {
	return []string{"libx264", "libx265"}
}

// New returns an Encoder for the given codec name or an error when the codec
// is not known. This covers only Stage-1 software encoders.
func New(codec string) (Encoder, error) {
	switch codec {
	case "libx264":
		return LibX264Encoder{}, nil
	case "libx265":
		return LibX265Encoder{}, nil
	default:
		return nil, fmt.Errorf("unknown encoder %q (Stage-1 supports: %s); "+
			"hardware encoders (NVENC, QSV, AMF) are Stage-2 scope",
			codec, strings.Join(KnownEncoders(), ", "))
	}
}
