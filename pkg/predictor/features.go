// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Feature extraction — the Go port of tools/vmaf-tune/src/vmaftune/
// predictor_features.py.
//
// Three passes per shot, in decreasing order of necessity:
//
//  1. One probe encode via the codec adapter's ProbeArgs (fast preset, fixed
//     quality) with output to the null muxer. Its bitrate and per-frame-type
//     sizes are the complexity barometer the predictor learns from. This is
//     the only mandatory pass.
//  2. An optional FFmpeg signalstats pass for the luma / frame-difference
//     signals.
//  3. Optional saliency moments over a raw decode of the shot.
//
// When 2 or 3 are unavailable the corresponding fields stay at zero and the
// analytical predictor — which reads only the probe bitrate and the
// structural metadata — degrades gracefully.
//
// Every subprocess goes through an injectable runner so unit tests never
// spawn a real process, matching the Python module's own policy.

package predictor

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/pershot"
)

// CommandRunner runs argv and returns its stdout, stderr and exit status.
type CommandRunner func(ctx context.Context, argv []string) (stdout, stderr string, exitStatus int, err error)

// defaultFeatureTimeout bounds one probe / signalstats / decode subprocess.
const defaultFeatureTimeout = 30 * time.Minute

// DefaultCommandRunner executes argv with a timeout, capturing both streams.
func DefaultCommandRunner(ctx context.Context, argv []string) (string, string, int, error) {
	cancel := func() {}
	if _, hasDeadline := ctx.Deadline(); !hasDeadline {
		ctx, cancel = context.WithTimeout(ctx, defaultFeatureTimeout)
	}
	defer cancel()

	// #nosec G204 -- argv[0] is an operator-configured ffmpeg / ffprobe path
	// and argv[1:] is assembled from validated CLI flags.
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	cmd.WaitDelay = 2 * time.Second
	var stdout, stderr strings.Builder
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	err := cmd.Run()
	if err != nil {
		var exitErr *exec.ExitError
		if errors.As(err, &exitErr) {
			return stdout.String(), stderr.String(), exitErr.ExitCode(), nil
		}
		return stdout.String(), stderr.String(), 1, err
	}
	return stdout.String(), stderr.String(), 0, nil
}

// ExtractorConfig configures ExtractFeatures.
type ExtractorConfig struct {
	FFmpegBin  string
	FFprobeBin string
	// UseSignalstats runs the optional luma / frame-difference pass.
	UseSignalstats bool
	// UseSaliency runs the optional saliency-moment pass. SaliencyFunc must
	// be set for it to do anything.
	UseSaliency          bool
	SaliencyModel        string
	SaliencyFrameSamples int
	// ProbeMaxFrames caps the probe encode: for very long shots the
	// complexity signal saturates after a few seconds, so encoding the whole
	// shot buys nothing.
	ProbeMaxFrames int
}

// DefaultExtractorConfig mirrors the Python FeatureExtractorConfig defaults.
func DefaultExtractorConfig() ExtractorConfig {
	return ExtractorConfig{
		FFmpegBin:            "ffmpeg",
		FFprobeBin:           "ffprobe",
		UseSignalstats:       true,
		SaliencyFrameSamples: 8,
		ProbeMaxFrames:       240,
	}
}

// SaliencyFunc computes (mean, variance) of the saliency map over a raw
// yuv420p decode of one shot. Supplied by the caller so this package does not
// depend on pkg/saliency (which would make the dependency cyclic once the
// saliency path grows its own predictor use).
type SaliencyFunc func(rawYUVPath string, width, height, frameSamples int, modelPath string) (mean, variance float64, err error)

// Geometry is the (width, height, fps) triple ffprobe reports.
type Geometry struct {
	Width  int
	Height int
	FPS    float64
}

// ffprobeStreams is the subset of ffprobe's JSON this driver reads.
type ffprobeStreams struct {
	Streams []struct {
		Width      int    `json:"width"`
		Height     int    `json:"height"`
		RFrameRate string `json:"r_frame_rate"`
	} `json:"streams"`
}

// GeometryCommand composes the ffprobe argv. Pure function for test pinning.
func GeometryCommand(source, ffprobeBin string) []string {
	if ffprobeBin == "" {
		ffprobeBin = "ffprobe"
	}
	return []string{
		ffprobeBin,
		"-v", "error",
		"-select_streams", "v:0",
		"-show_entries", "stream=width,height,r_frame_rate",
		"-of", "json",
		source,
	}
}

// ProbeGeometry reads width / height / fps from ffprobe.
//
// A missing or failing ffprobe returns the zero Geometry rather than an
// error: the analytical predictor tolerates missing structural metadata, and
// the caller decides whether zero geometry is fatal for its own purposes.
func ProbeGeometry(ctx context.Context, source string, cfg ExtractorConfig, run CommandRunner) Geometry {
	if run == nil {
		run = DefaultCommandRunner
	}
	stdout, _, exitStatus, err := run(ctx, GeometryCommand(source, cfg.FFprobeBin))
	if err != nil || exitStatus != 0 {
		return Geometry{}
	}
	var doc ffprobeStreams
	if unmarshalErr := json.Unmarshal([]byte(stdout), &doc); unmarshalErr != nil {
		return Geometry{}
	}
	if len(doc.Streams) == 0 {
		return Geometry{}
	}
	s := doc.Streams[0]
	return Geometry{Width: s.Width, Height: s.Height, FPS: ParseFPS(s.RFrameRate)}
}

// ParseFPS parses ffprobe's "num/den" rational frame rate. A zero or
// unparseable denominator yields 0.
func ParseFPS(rational string) float64 {
	num, den, found := strings.Cut(rational, "/")
	if !found {
		v, err := strconv.ParseFloat(rational, 64)
		if err != nil {
			return 0.0
		}
		return v
	}
	n, numErr := strconv.ParseFloat(num, 64)
	d, denErr := strconv.ParseFloat(den, 64)
	if numErr != nil || denErr != nil || d <= 0.0 {
		return 0.0
	}
	return n / d
}

// ProbeStats are the probe encode's complexity signals.
type ProbeStats struct {
	BitrateKbps    float64
	IFrameAvgBytes float64
	PFrameAvgBytes float64
	BFrameAvgBytes float64
}

// SignalStats are the optional luma / frame-difference signals.
type SignalStats struct {
	FrameDiffMean float64
	YAvg          float64
	YVar          float64
}

// ProbeCommand composes the probe-encode argv. vstatsPath receives FFmpeg's
// per-frame size/type table; the bitstream goes to the null muxer.
func ProbeCommand(shot pershot.Shot, source, codec string, cfg ExtractorConfig, vstatsPath string, frames int) ([]string, error) {
	adapter, err := codecadapter.Get(codec)
	if err != nil {
		return nil, err
	}
	probeArgs, argErr := adapter.ProbeArgs()
	if argErr != nil {
		return nil, argErr
	}
	ffmpegBin := cfg.FFmpegBin
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	cmd := []string{
		ffmpegBin,
		"-hide_banner",
		"-y",
		"-vstats_file", vstatsPath,
		"-ss", strconv.Itoa(shot.StartFrame),
		"-i", source,
		"-frames:v", strconv.Itoa(frames),
	}
	cmd = append(cmd, probeArgs...)
	return append(cmd, "-an", "-f", "null", "/dev/null"), nil
}

var bitrateRE = regexp.MustCompile(`(?i)bitrate=\s*([\d.]+)kbits/s`)

// ParseBitrate pulls the bitrate from FFmpeg's stderr. The LAST match wins:
// earlier ones are progress reports, the final one is the summary.
func ParseBitrate(stderr string) float64 {
	matches := bitrateRE.FindAllStringSubmatch(stderr, -1)
	if len(matches) == 0 {
		return 0.0
	}
	v, err := strconv.ParseFloat(matches[len(matches)-1][1], 64)
	if err != nil {
		return 0.0
	}
	return v
}

var (
	vstatsTypeRE = regexp.MustCompile(`type=\s*(\w)`)
	vstatsSizeRE = regexp.MustCompile(`size=\s*(\d+)`)
)

// ParseVStats averages the I / P / B frame sizes from FFmpeg's -vstats_file
// table. Frame types the encode never produced report 0.
func ParseVStats(content string) (iAvg, pAvg, bAvg float64) {
	sums := map[string]float64{"I": 0, "P": 0, "B": 0}
	counts := map[string]int{"I": 0, "P": 0, "B": 0}

	for _, line := range strings.Split(content, "\n") {
		typeMatch := vstatsTypeRE.FindStringSubmatch(line)
		sizeMatch := vstatsSizeRE.FindStringSubmatch(line)
		if typeMatch == nil || sizeMatch == nil {
			continue
		}
		frameType := typeMatch[1]
		if _, known := sums[frameType]; !known {
			continue
		}
		size, err := strconv.ParseFloat(sizeMatch[1], 64)
		if err != nil {
			continue
		}
		sums[frameType] += size
		counts[frameType]++
	}
	avg := func(k string) float64 {
		if counts[k] == 0 {
			return 0.0
		}
		return sums[k] / float64(counts[k])
	}
	return avg("I"), avg("P"), avg("B")
}

// SignalstatsCommand composes the signalstats argv.
func SignalstatsCommand(shot pershot.Shot, source string, cfg ExtractorConfig, frames int) []string {
	ffmpegBin := cfg.FFmpegBin
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	return []string{
		ffmpegBin,
		"-hide_banner",
		"-ss", strconv.Itoa(shot.StartFrame),
		"-i", source,
		"-frames:v", strconv.Itoa(frames),
		"-vf", "signalstats,metadata=mode=print:file=-",
		"-f", "null",
		"/dev/null",
	}
}

// ParseSignalstats averages YAVG (luma mean) and YDIF (frame difference) from
// FFmpeg's metadata output.
//
// YVar uses the mean of the YHIGH / YLOW readings as a spread proxy rather
// than a true second moment — signalstats does not emit one, and asking for a
// full variance pass would cost another decode. The Python original documents
// the same approximation.
func ParseSignalstats(metadata string) SignalStats {
	var yavg, ydif, yvar []float64
	for _, line := range strings.Split(metadata, "\n") {
		switch {
		case strings.Contains(line, "lavfi.signalstats.YAVG="):
			yavg = append(yavg, parseMetadataFloat(line))
		case strings.Contains(line, "lavfi.signalstats.YDIF="):
			ydif = append(ydif, parseMetadataFloat(line))
		case strings.Contains(line, "lavfi.signalstats.YHIGH="),
			strings.Contains(line, "lavfi.signalstats.YLOW="):
			yvar = append(yvar, parseMetadataFloat(line))
		}
	}
	return SignalStats{
		YAvg:          mean(yavg),
		FrameDiffMean: mean(ydif),
		YVar:          mean(yvar),
	}
}

// parseMetadataFloat reads the value after the last "=" on a metadata line.
func parseMetadataFloat(line string) float64 {
	idx := strings.LastIndex(line, "=")
	if idx < 0 {
		return 0.0
	}
	v, err := strconv.ParseFloat(strings.TrimSpace(line[idx+1:]), 64)
	if err != nil {
		return 0.0
	}
	return v
}

// mean returns the arithmetic mean, 0 for an empty slice.
func mean(values []float64) float64 {
	if len(values) == 0 {
		return 0.0
	}
	sum := 0.0
	for _, v := range values {
		sum += v
	}
	return sum / float64(len(values))
}

// ShotStartArg renders the -ss value for a shot: seconds when fps is known,
// otherwise the raw frame index.
func ShotStartArg(shot pershot.Shot, fps float64) string {
	if fps > 0.0 {
		return strconv.FormatFloat(float64(shot.StartFrame)/fps, 'f', 6, 64)
	}
	return strconv.Itoa(shot.StartFrame)
}

// RawDecodeCommand composes the argv that decodes one shot to raw yuv420p,
// which the saliency pass consumes.
func RawDecodeCommand(shot pershot.Shot, source, rawPath string, cfg ExtractorConfig, frames int, fps float64) []string {
	ffmpegBin := cfg.FFmpegBin
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}
	return []string{
		ffmpegBin,
		"-hide_banner",
		"-loglevel", "error",
		"-y",
		"-ss", ShotStartArg(shot, fps),
		"-i", source,
		"-frames:v", strconv.Itoa(frames),
		"-pix_fmt", "yuv420p",
		"-f", "rawvideo",
		rawPath,
	}
}

// ExtractFeatures builds the ShotFeatures vector for one shot.
//
// geometry is passed in rather than probed per shot because the caller
// already needs it for shot detection; probing once per source instead of
// once per shot saves an ffprobe per shot on a 1800-shot movie.
func ExtractFeatures(
	ctx context.Context,
	shot pershot.Shot,
	source, codec string,
	geometry Geometry,
	cfg ExtractorConfig,
	run CommandRunner,
	saliency SaliencyFunc,
) (ShotFeatures, error) {
	if run == nil {
		run = DefaultCommandRunner
	}
	frames := shot.Length()
	if cfg.ProbeMaxFrames > 0 && frames > cfg.ProbeMaxFrames {
		frames = cfg.ProbeMaxFrames
	}

	probe := ProbeStats{}
	if frames > 0 {
		var probeErr error
		probe, probeErr = runProbeEncode(ctx, shot, source, codec, cfg, run, frames)
		if probeErr != nil {
			return ShotFeatures{}, probeErr
		}
	}

	sig := SignalStats{}
	if cfg.UseSignalstats && frames > 0 {
		sig = runSignalstats(ctx, shot, source, cfg, run, frames)
	}

	salMean, salVar := 0.0, 0.0
	if cfg.UseSaliency && saliency != nil && frames > 0 &&
		geometry.Width > 0 && geometry.Height > 0 {
		salMean, salVar = runSaliency(ctx, shot, source, cfg, run, saliency, geometry, frames)
	}

	return ShotFeatures{
		ProbeBitrateKbps:    probe.BitrateKbps,
		ProbeIFrameAvgBytes: probe.IFrameAvgBytes,
		ProbePFrameAvgBytes: probe.PFrameAvgBytes,
		ProbeBFrameAvgBytes: probe.BFrameAvgBytes,
		SaliencyMean:        salMean,
		SaliencyVar:         salVar,
		FrameDiffMean:       sig.FrameDiffMean,
		YAvg:                sig.YAvg,
		YVar:                sig.YVar,
		ShotLengthFrames:    shot.Length(),
		FPS:                 geometry.FPS,
		Width:               geometry.Width,
		Height:              geometry.Height,
	}, nil
}

// runProbeEncode runs the probe encode and parses its stderr + vstats file.
// A non-zero ffmpeg exit yields zeroed stats, not an error: a shot the probe
// could not measure still gets a prediction from the structural metadata.
func runProbeEncode(
	ctx context.Context,
	shot pershot.Shot,
	source, codec string,
	cfg ExtractorConfig,
	run CommandRunner,
	frames int,
) (ProbeStats, error) {
	tmpDir, err := os.MkdirTemp("", "vmafx-tune-probe-")
	if err != nil {
		return ProbeStats{}, fmt.Errorf("create probe workdir: %w", err)
	}
	defer func() {
		if rmErr := os.RemoveAll(tmpDir); rmErr != nil {
			_ = rmErr // best-effort cleanup
		}
	}()

	vstatsPath := filepath.Join(tmpDir, "vstats.txt")
	argv, cmdErr := ProbeCommand(shot, source, codec, cfg, vstatsPath, frames)
	if cmdErr != nil {
		return ProbeStats{}, cmdErr
	}
	_, stderr, exitStatus, runErr := run(ctx, argv)
	if runErr != nil || exitStatus != 0 {
		return ProbeStats{}, nil
	}
	// #nosec G304 -- vstatsPath is this function's own MkdirTemp output.
	vstats, _ := os.ReadFile(vstatsPath)
	iAvg, pAvg, bAvg := ParseVStats(string(vstats))
	return ProbeStats{
		BitrateKbps:    ParseBitrate(stderr),
		IFrameAvgBytes: iAvg,
		PFrameAvgBytes: pAvg,
		BFrameAvgBytes: bAvg,
	}, nil
}

// runSignalstats runs the optional luma pass, returning zeros on failure.
func runSignalstats(
	ctx context.Context,
	shot pershot.Shot,
	source string,
	cfg ExtractorConfig,
	run CommandRunner,
	frames int,
) SignalStats {
	stdout, _, exitStatus, err := run(ctx, SignalstatsCommand(shot, source, cfg, frames))
	if err != nil || exitStatus != 0 {
		return SignalStats{}
	}
	return ParseSignalstats(stdout)
}

// runSaliency decodes the shot to raw YUV and computes the saliency moments,
// returning zeros on any failure (saliency is strictly best-effort).
func runSaliency(
	ctx context.Context,
	shot pershot.Shot,
	source string,
	cfg ExtractorConfig,
	run CommandRunner,
	saliency SaliencyFunc,
	geometry Geometry,
	frames int,
) (float64, float64) {
	tmpDir, err := os.MkdirTemp("", "vmafx-tune-saliency-")
	if err != nil {
		return 0.0, 0.0
	}
	defer func() {
		if rmErr := os.RemoveAll(tmpDir); rmErr != nil {
			_ = rmErr // best-effort cleanup
		}
	}()

	rawPath := filepath.Join(tmpDir, "shot.yuv")
	argv := RawDecodeCommand(shot, source, rawPath, cfg, frames, geometry.FPS)
	_, _, exitStatus, runErr := run(ctx, argv)
	if runErr != nil || exitStatus != 0 {
		return 0.0, 0.0
	}
	if _, statErr := os.Stat(rawPath); statErr != nil {
		return 0.0, 0.0
	}
	samples := cfg.SaliencyFrameSamples
	if samples < 1 {
		samples = 1
	}
	if samples > frames {
		samples = frames
	}
	mean, variance, salErr := saliency(
		rawPath, geometry.Width, geometry.Height, samples, cfg.SaliencyModel)
	if salErr != nil {
		return 0.0, 0.0
	}
	return mean, variance
}
