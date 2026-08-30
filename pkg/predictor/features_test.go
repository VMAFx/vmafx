// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package predictor_test

import (
	"context"
	"errors"
	"math"
	"os"
	"slices"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/pershot"
	"github.com/VMAFx/vmafx/pkg/predictor"
)

// writeStub materialises a placeholder file at path so the extractor's
// os.Stat check on the decoded raw YUV succeeds without a real decode.
func writeStub(t *testing.T, path string) {
	t.Helper()

	if err := os.WriteFile(path, []byte("raw"), 0o600); err != nil {
		t.Fatalf("write stub %q: %v", path, err)
	}
}

// TestParseFPS covers ffprobe's rational frame-rate syntax and its failure
// modes.
func TestParseFPS(t *testing.T) {
	t.Parallel()

	tests := []struct {
		input string
		want  float64
	}{
		{"24/1", 24.0},
		{"30000/1001", 30000.0 / 1001.0},
		{"25", 25.0},
		{"0/0", 0.0},
		{"24/0", 0.0},
		{"", 0.0},
		{"garbage", 0.0},
		{"a/b", 0.0},
	}
	for _, tc := range tests {
		t.Run(tc.input, func(t *testing.T) {
			t.Parallel()

			if got := predictor.ParseFPS(tc.input); math.Abs(got-tc.want) > 1e-12 {
				t.Errorf("ParseFPS(%q) = %v, want %v", tc.input, got, tc.want)
			}
		})
	}
}

// TestGeometryCommand pins the ffprobe argv.
func TestGeometryCommand(t *testing.T) {
	t.Parallel()

	want := []string{
		"ffprobe", "-v", "error",
		"-select_streams", "v:0",
		"-show_entries", "stream=width,height,r_frame_rate",
		"-of", "json",
		"/src/a.mkv",
	}
	if got := predictor.GeometryCommand("/src/a.mkv", ""); !slices.Equal(got, want) {
		t.Errorf("GeometryCommand = %v, want %v", got, want)
	}
}

// TestProbeGeometry drives the ffprobe seam, including the degradation paths
// that must return zero geometry rather than erroring.
func TestProbeGeometry(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name       string
		stdout     string
		exitStatus int
		runErr     error
		want       predictor.Geometry
	}{
		{
			name:   "well-formed probe",
			stdout: `{"streams":[{"width":1920,"height":1080,"r_frame_rate":"24/1"}]}`,
			want:   predictor.Geometry{Width: 1920, Height: 1080, FPS: 24},
		},
		{
			name:       "ffprobe failed",
			stdout:     "",
			exitStatus: 1,
		},
		{
			name:   "ffprobe missing",
			runErr: errors.New("executable file not found"),
		},
		{
			name:   "no video streams",
			stdout: `{"streams":[]}`,
		},
		{
			name:   "malformed JSON",
			stdout: `{"streams":`,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			run := func(context.Context, []string) (string, string, int, error) {
				return tc.stdout, "", tc.exitStatus, tc.runErr
			}
			got := predictor.ProbeGeometry(
				context.Background(), "/src/a.mkv",
				predictor.DefaultExtractorConfig(), run)
			if got != tc.want {
				t.Errorf("ProbeGeometry = %+v, want %+v", got, tc.want)
			}
		})
	}
}

// TestProbeCommand pins the probe-encode argv, including the codec adapter's
// probe args landing between the input flags and the null muxer.
func TestProbeCommand(t *testing.T) {
	t.Parallel()

	shot := pershot.Shot{StartFrame: 480, EndFrame: 720}
	cfg := predictor.DefaultExtractorConfig()

	got, err := predictor.ProbeCommand(shot, "/src/a.mkv", "libx264", cfg, "/tmp/vstats.txt", 240)
	if err != nil {
		t.Fatalf("ProbeCommand: %v", err)
	}
	want := []string{
		"ffmpeg", "-hide_banner", "-y",
		"-vstats_file", "/tmp/vstats.txt",
		"-ss", "480",
		"-i", "/src/a.mkv",
		"-frames:v", "240",
		"-c:v", "libx264", "-preset", "ultrafast", "-crf", "28",
		"-an", "-f", "null", "/dev/null",
	}
	if !slices.Equal(got, want) {
		t.Errorf("ProbeCommand mismatch\n got: %v\nwant: %v", got, want)
	}

	if _, unknownErr := predictor.ProbeCommand(
		shot, "/src/a.mkv", "libtheora", cfg, "/tmp/v.txt", 10); unknownErr == nil {
		t.Error("expected an error for an unregistered codec")
	}
}

// TestParseBitrate asserts the LAST bitrate line wins — the earlier ones are
// progress reports, only the final line is the summary.
func TestParseBitrate(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		stderr string
		want   float64
	}{
		{
			name: "final summary wins",
			stderr: "frame=  100 bitrate=1234.5kbits/s\n" +
				"frame=  200 bitrate= 987.6kbits/s\n",
			want: 987.6,
		},
		{
			name:   "single line",
			stderr: "bitrate=4200.0kbits/s",
			want:   4200.0,
		},
		{
			name:   "case insensitive",
			stderr: "BITRATE=100.0KBITS/S",
			want:   100.0,
		},
		{
			name:   "no bitrate reported",
			stderr: "Conversion failed!",
			want:   0.0,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if got := predictor.ParseBitrate(tc.stderr); math.Abs(got-tc.want) > 1e-9 {
				t.Errorf("ParseBitrate = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestParseVStats averages per-frame-type sizes and reports 0 for types the
// encode never produced.
func TestParseVStats(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name                string
		content             string
		wantI, wantP, wantB float64
	}{
		{
			// Real FFmpeg -vstats_file lines. Note that "f_size=" is what
			// carries the frame size; the size regex matches its suffix,
			// which is the intended behaviour and matches the Python parser
			// (verified against _parse_frame_sizes on this exact input).
			name: "real vstats lines",
			content: "frame=    1 q= 0.0 PSNR= 0.00 f_size=  1000 s_size=  1kB " +
				"time= 0.00 br=   0.0kbits/s avg_br=   0.0kbits/s type= I\n" +
				"frame=    2 q= 0.0 f_size=   200 type= P\n" +
				"frame=    3 q= 0.0 f_size=   400 type= P\n" +
				"frame=    4 q= 0.0 f_size=    50 type= B\n",
			wantI: 1000, wantP: 300, wantB: 50,
		},
		{
			name:    "codec with no B frames",
			content: "frame= 1 f_size= 800 type= I\nframe= 2 f_size= 100 type= P\n",
			wantI:   800, wantP: 100, wantB: 0,
		},
		{
			name:    "unknown frame types are ignored",
			content: "frame= 1 f_size= 900 type= X\nframe= 2 f_size= 500 type= I\n",
			wantI:   500,
		},
		{
			name:    "empty vstats",
			content: "",
		},
		{
			name:    "lines without a size are skipped",
			content: "frame= 1 type= I\n",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			gotI, gotP, gotB := predictor.ParseVStats(tc.content)
			if math.Abs(gotI-tc.wantI) > 1e-9 ||
				math.Abs(gotP-tc.wantP) > 1e-9 ||
				math.Abs(gotB-tc.wantB) > 1e-9 {
				t.Errorf("ParseVStats = (%v, %v, %v), want (%v, %v, %v)",
					gotI, gotP, gotB, tc.wantI, tc.wantP, tc.wantB)
			}
		})
	}
}

// TestParseSignalstats covers the YAVG / YDIF averaging and the YHIGH+YLOW
// spread proxy.
func TestParseSignalstats(t *testing.T) {
	t.Parallel()

	metadata := strings.Join([]string{
		"lavfi.signalstats.YAVG=100.0",
		"lavfi.signalstats.YDIF=5.0",
		"lavfi.signalstats.YLOW=16.0",
		"lavfi.signalstats.YHIGH=235.0",
		"lavfi.signalstats.YAVG=110.0",
		"lavfi.signalstats.YDIF=7.0",
		"lavfi.signalstats.UAVG=128.0",
		"not a metadata line",
	}, "\n")

	got := predictor.ParseSignalstats(metadata)
	if math.Abs(got.YAvg-105.0) > 1e-9 {
		t.Errorf("YAvg = %v, want 105", got.YAvg)
	}
	if math.Abs(got.FrameDiffMean-6.0) > 1e-9 {
		t.Errorf("FrameDiffMean = %v, want 6", got.FrameDiffMean)
	}
	// (16 + 235) / 2 = 125.5
	if math.Abs(got.YVar-125.5) > 1e-9 {
		t.Errorf("YVar = %v, want 125.5", got.YVar)
	}

	empty := predictor.ParseSignalstats("")
	if empty != (predictor.SignalStats{}) {
		t.Errorf("empty metadata = %+v, want the zero value", empty)
	}
}

// TestShotStartArg covers the seconds / frame-index switch.
func TestShotStartArg(t *testing.T) {
	t.Parallel()

	shot := pershot.Shot{StartFrame: 48, EndFrame: 96}
	if got := predictor.ShotStartArg(shot, 24.0); got != "2.000000" {
		t.Errorf("ShotStartArg with fps = %q, want 2.000000", got)
	}
	if got := predictor.ShotStartArg(shot, 0.0); got != "48" {
		t.Errorf("ShotStartArg without fps = %q, want the raw frame index 48", got)
	}
}

// TestSignalstatsCommand and TestRawDecodeCommand pin the two optional-pass
// argv shapes.
func TestSignalstatsCommand(t *testing.T) {
	t.Parallel()

	want := []string{
		"ffmpeg", "-hide_banner",
		"-ss", "480",
		"-i", "/src/a.mkv",
		"-frames:v", "240",
		"-vf", "signalstats,metadata=mode=print:file=-",
		"-f", "null", "/dev/null",
	}
	got := predictor.SignalstatsCommand(
		pershot.Shot{StartFrame: 480, EndFrame: 720},
		"/src/a.mkv", predictor.DefaultExtractorConfig(), 240)
	if !slices.Equal(got, want) {
		t.Errorf("SignalstatsCommand mismatch\n got: %v\nwant: %v", got, want)
	}
}

func TestRawDecodeCommand(t *testing.T) {
	t.Parallel()

	want := []string{
		"ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
		"-ss", "20.000000",
		"-i", "/src/a.mkv",
		"-frames:v", "240",
		"-pix_fmt", "yuv420p",
		"-f", "rawvideo",
		"/tmp/shot.yuv",
	}
	got := predictor.RawDecodeCommand(
		pershot.Shot{StartFrame: 480, EndFrame: 720},
		"/src/a.mkv", "/tmp/shot.yuv",
		predictor.DefaultExtractorConfig(), 240, 24.0)
	if !slices.Equal(got, want) {
		t.Errorf("RawDecodeCommand mismatch\n got: %v\nwant: %v", got, want)
	}
}

// TestExtractFeatures drives the whole extractor through the injected
// subprocess seam.
func TestExtractFeatures(t *testing.T) {
	t.Parallel()

	shot := pershot.Shot{StartFrame: 0, EndFrame: 100}
	geometry := predictor.Geometry{Width: 1920, Height: 1080, FPS: 24}

	tests := []struct {
		name        string
		cfg         predictor.ExtractorConfig
		saliency    predictor.SaliencyFunc
		wantBitrate float64
		wantYAvg    float64
		wantSalMean float64
		wantSalVar  float64
	}{
		{
			name: "probe only",
			cfg: predictor.ExtractorConfig{
				FFmpegBin: "ffmpeg", FFprobeBin: "ffprobe", ProbeMaxFrames: 240,
			},
			wantBitrate: 4200.0,
		},
		{
			name: "probe plus signalstats",
			cfg: predictor.ExtractorConfig{
				FFmpegBin: "ffmpeg", FFprobeBin: "ffprobe",
				UseSignalstats: true, ProbeMaxFrames: 240,
			},
			wantBitrate: 4200.0, wantYAvg: 100.0,
		},
		{
			name: "probe plus saliency",
			cfg: predictor.ExtractorConfig{
				FFmpegBin: "ffmpeg", FFprobeBin: "ffprobe",
				UseSaliency: true, SaliencyFrameSamples: 8, ProbeMaxFrames: 240,
			},
			saliency: func(string, int, int, int, string) (float64, float64, error) {
				return 0.42, 0.05, nil
			},
			wantBitrate: 4200.0, wantSalMean: 0.42, wantSalVar: 0.05,
		},
		{
			name: "saliency failure degrades to zero",
			cfg: predictor.ExtractorConfig{
				FFmpegBin: "ffmpeg", FFprobeBin: "ffprobe",
				UseSaliency: true, SaliencyFrameSamples: 8, ProbeMaxFrames: 240,
			},
			saliency: func(string, int, int, int, string) (float64, float64, error) {
				return 0, 0, errors.New("onnxruntime unavailable")
			},
			wantBitrate: 4200.0,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			run := func(_ context.Context, argv []string) (string, string, int, error) {
				joined := strings.Join(argv, " ")
				switch {
				case strings.Contains(joined, "signalstats"):
					return "lavfi.signalstats.YAVG=100.0\n" +
						"lavfi.signalstats.YDIF=5.0\n", "", 0, nil
				case strings.Contains(joined, "rawvideo"):
					// The saliency decode: materialise the file the extractor
					// stats before calling the saliency func.
					for i := range argv {
						if argv[i] == "rawvideo" && i+1 < len(argv) {
							writeStub(t, argv[len(argv)-1])
						}
					}
					return "", "", 0, nil
				default:
					// The probe encode: report a bitrate on stderr.
					return "", "frame= 100 bitrate=4200.0kbits/s\n", 0, nil
				}
			}

			got, err := predictor.ExtractFeatures(
				context.Background(), shot, "/src/a.mkv", "libx264",
				geometry, tc.cfg, run, tc.saliency)
			if err != nil {
				t.Fatalf("ExtractFeatures: %v", err)
			}
			if math.Abs(got.ProbeBitrateKbps-tc.wantBitrate) > 1e-9 {
				t.Errorf("ProbeBitrateKbps = %v, want %v",
					got.ProbeBitrateKbps, tc.wantBitrate)
			}
			if math.Abs(got.YAvg-tc.wantYAvg) > 1e-9 {
				t.Errorf("YAvg = %v, want %v", got.YAvg, tc.wantYAvg)
			}
			if math.Abs(got.SaliencyMean-tc.wantSalMean) > 1e-9 ||
				math.Abs(got.SaliencyVar-tc.wantSalVar) > 1e-9 {
				t.Errorf("saliency = (%v, %v), want (%v, %v)",
					got.SaliencyMean, got.SaliencyVar, tc.wantSalMean, tc.wantSalVar)
			}
			// Structural metadata always comes straight through.
			if got.ShotLengthFrames != 100 || got.Width != 1920 ||
				got.Height != 1080 || got.FPS != 24 {
				t.Errorf("structural metadata = %+v, want the shot / geometry values", got)
			}
		})
	}
}

// TestExtractFeatures_probeFailureIsNotFatal asserts a failed probe zeroes
// the complexity signals instead of aborting: the analytical predictor still
// works from the structural metadata alone.
func TestExtractFeatures_probeFailureIsNotFatal(t *testing.T) {
	t.Parallel()

	run := func(context.Context, []string) (string, string, int, error) {
		return "", "Conversion failed!", 1, nil
	}
	got, err := predictor.ExtractFeatures(
		context.Background(),
		pershot.Shot{StartFrame: 0, EndFrame: 100},
		"/src/a.mkv", "libx264",
		predictor.Geometry{Width: 640, Height: 480, FPS: 30},
		predictor.DefaultExtractorConfig(), run, nil)
	if err != nil {
		t.Fatalf("ExtractFeatures: %v", err)
	}
	if got.ProbeBitrateKbps != 0 {
		t.Errorf("ProbeBitrateKbps = %v, want 0 after a failed probe", got.ProbeBitrateKbps)
	}
	if got.Width != 640 || got.ShotLengthFrames != 100 {
		t.Errorf("structural metadata should survive a failed probe; got %+v", got)
	}
}

// TestExtractFeatures_unknownCodecIsAnError asserts the codec typo surfaces
// rather than silently producing a zeroed feature vector.
func TestExtractFeatures_unknownCodecIsAnError(t *testing.T) {
	t.Parallel()

	run := func(context.Context, []string) (string, string, int, error) {
		return "", "", 0, nil
	}
	_, err := predictor.ExtractFeatures(
		context.Background(),
		pershot.Shot{StartFrame: 0, EndFrame: 100},
		"/src/a.mkv", "libtheora",
		predictor.Geometry{Width: 640, Height: 480, FPS: 30},
		predictor.DefaultExtractorConfig(), run, nil)
	if err == nil {
		t.Fatal("expected an error for an unregistered codec")
	}
}
