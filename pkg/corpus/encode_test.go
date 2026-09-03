// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/encode_test.go — ffmpeg argv and version-parsing tests.
//
// BuildFFmpegCommand is the pure function that decides what the corpus sweep
// actually encodes, so the expected argv slices here were read off
// vmaftune.encode.build_ffmpeg_command. A drift means the Go and Python sweeps
// produce different bitstreams for the same cell.

package corpus

import (
	"context"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

// baseEncodeRequest is a raw-YUV single-pass request.
func baseEncodeRequest() EncodeRequest {
	return EncodeRequest{
		Source:    "/refs/clip.yuv",
		Width:     1920,
		Height:    1080,
		PixFmt:    "yuv420p",
		Framerate: 24.0,
		Encoder:   "libx264",
		Preset:    "medium",
		CRF:       26,
		Output:    "/enc/clip__libx264__medium__crf26.mp4",
	}
}

func TestBuildFFmpegCommand(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		mutate  func(*EncodeRequest)
		want    []string
		wantErr bool
	}{
		{
			name: "raw YUV single pass",
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/refs/clip.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "26",
				"/enc/clip__libx264__medium__crf26.mp4",
			},
		},
		{
			name: "container source drops the rawvideo demuxer block",
			mutate: func(r *EncodeRequest) {
				r.Source = "/refs/clip.mkv"
				r.SourceIsContainer = true
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-i", "/refs/clip.mkv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "26",
				"/enc/clip__libx264__medium__crf26.mp4",
			},
		},
		{
			name: "sample-clip mode fast-seeks on the input side",
			mutate: func(r *EncodeRequest) {
				r.SampleClipSeconds = 10
				r.SampleClipStartS = 25.5
				// duration_s is ignored while sample-clip mode is on.
				r.DurationS = 60
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-ss", "25.5", "-t", "10.0",
				"-i", "/refs/clip.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "26",
				"/enc/clip__libx264__medium__crf26.mp4",
			},
		},
		{
			name: "a bound duration clips the encode without sample-clip mode",
			mutate: func(r *EncodeRequest) {
				r.DurationS = 10
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-t", "10.0",
				"-i", "/refs/clip.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "26",
				"/enc/clip__libx264__medium__crf26.mp4",
			},
		},
		{
			name: "extra params land after the codec slice",
			mutate: func(r *EncodeRequest) {
				r.ExtraParams = []string{"-vf", "scale=1280:720"}
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/refs/clip.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "26",
				"-vf", "scale=1280:720",
				"/enc/clip__libx264__medium__crf26.mp4",
			},
		},
		{
			name: "pass 1 discards the bitstream through the null muxer",
			mutate: func(r *EncodeRequest) {
				r.PassNumber = 1
				r.StatsPath = "/tmp/stats"
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/refs/clip.yuv",
				"-c:v", "libx264", "-preset", "medium",
				"-pass", "1", "-passlogfile", "/tmp/stats",
				"-f", "null", "-",
			},
		},
		{
			name: "pass 2 writes the requested output",
			mutate: func(r *EncodeRequest) {
				r.PassNumber = 2
				r.StatsPath = "/tmp/stats"
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/refs/clip.yuv",
				"-c:v", "libx264", "-preset", "medium",
				"-pass", "2", "-passlogfile", "/tmp/stats",
				"/enc/clip__libx264__medium__crf26.mp4",
			},
		},
		{
			name: "a fractional framerate renders as CPython repr",
			mutate: func(r *EncodeRequest) {
				r.Framerate = 23.976
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "23.976",
				"-i", "/refs/clip.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "26",
				"/enc/clip__libx264__medium__crf26.mp4",
			},
		},
		{
			name: "an unregistered encoder falls back to the legacy shape",
			mutate: func(r *EncodeRequest) {
				r.Encoder = "libwhatever"
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/refs/clip.yuv",
				"-c:v", "libwhatever", "-preset", "medium", "-crf", "26",
				"/enc/clip__libx264__medium__crf26.mp4",
			},
		},
		{
			name: "a non-zero pass without a stats path is rejected",
			mutate: func(r *EncodeRequest) {
				r.PassNumber = 1
			},
			wantErr: true,
		},
		{
			name: "two-pass against a single-pass-only encoder is rejected",
			mutate: func(r *EncodeRequest) {
				r.Encoder = "h264_nvenc"
				r.Preset = "medium"
				r.PassNumber = 1
				r.StatsPath = "/tmp/stats"
			},
			wantErr: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			req := baseEncodeRequest()
			if tc.mutate != nil {
				tc.mutate(&req)
			}
			got, err := BuildFFmpegCommand(req, "ffmpeg")
			if (err != nil) != tc.wantErr {
				t.Fatalf("BuildFFmpegCommand error = %v, wantErr = %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("BuildFFmpegCommand() =\n  %v\nwant\n  %v", got, tc.want)
			}
		})
	}
}

func TestBuildPass1StatsCommand(t *testing.T) {
	t.Parallel()

	req := baseEncodeRequest()
	req.DurationS = 5
	got := BuildPass1StatsCommand(req, "/tmp/prefix", "ffmpeg")
	want := []string{
		"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
		"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
		"-t", "5.0",
		"-i", "/refs/clip.yuv",
		"-c:v", "libx264", "-preset", "medium", "-crf", "26",
		"-pass", "1", "-passlogfile", "/tmp/prefix",
		"-f", "null", os.DevNull,
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("BuildPass1StatsCommand() =\n  %v\nwant\n  %v", got, want)
	}
}

func TestParseVersions(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name        string
		stderr      string
		encoder     string
		wantFFmpeg  string
		wantEncoder string
	}{
		{
			name:        "x264 banner with the ffmpeg version",
			stderr:      "ffmpeg version 7.1 Copyright\nx264 - core 164 r3095 baee400\n",
			encoder:     "libx264",
			wantFFmpeg:  "7.1",
			wantEncoder: "libx264-164",
		},
		{
			name:        "the x264-core variant is accepted too",
			stderr:      "x264-core 163\n",
			encoder:     "libx264",
			wantFFmpeg:  "unknown",
			wantEncoder: "libx264-163",
		},
		{
			name:        "an explicit x265 encoder reads the x265 banner",
			stderr:      "x265 [info]: HEVC encoder version 3.5+1-f0c1022b6\n",
			encoder:     "libx265",
			wantFFmpeg:  "unknown",
			wantEncoder: "libx265-3.5+1-f0c1022b6",
		},
		{
			name:        "the default encoder auto-detects x265 when x264 is absent",
			stderr:      "x265 [info]: HEVC encoder version 3.5\n",
			encoder:     "libx264",
			wantFFmpeg:  "unknown",
			wantEncoder: "libx265-3.5",
		},
		{
			name:        "svt-av1 banner",
			stderr:      "Svt[info]:SVT-AV1 Encoder Lib v2.1.0\n",
			encoder:     "libsvtav1",
			wantFFmpeg:  "unknown",
			wantEncoder: "libsvtav1-2.1.0",
		},
		{
			name:        "libaom falls back to the stable adapter name",
			stderr:      "",
			encoder:     "libaom-av1",
			wantFFmpeg:  "unknown",
			wantEncoder: "libaom-av1",
		},
		{
			name:        "libvvenc falls back to the stable adapter name",
			stderr:      "",
			encoder:     "libvvenc",
			wantFFmpeg:  "unknown",
			wantEncoder: "libvvenc",
		},
		{
			name:        "hardware encoders report their own token",
			stderr:      "ffmpeg version n7.1\n",
			encoder:     "hevc_nvenc",
			wantFFmpeg:  "n7.1",
			wantEncoder: "hevc_nvenc",
		},
		{
			name:        "an unknown encoder with no banner is unknown",
			stderr:      "",
			encoder:     "libmystery",
			wantFFmpeg:  "unknown",
			wantEncoder: "unknown",
		},
		{
			name:        "no banner at all under the default encoder",
			stderr:      "ffmpeg version 6.0\n",
			encoder:     "libx264",
			wantFFmpeg:  "6.0",
			wantEncoder: "unknown",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			gotFFmpeg, gotEncoder := ParseVersions(tc.stderr, tc.encoder)
			if gotFFmpeg != tc.wantFFmpeg {
				t.Errorf("ffmpeg version = %q, want %q", gotFFmpeg, tc.wantFFmpeg)
			}
			if gotEncoder != tc.wantEncoder {
				t.Errorf("encoder version = %q, want %q", gotEncoder, tc.wantEncoder)
			}
		})
	}
}

func TestBitrateKbps(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		size      int64
		durationS float64
		want      float64
	}{
		{name: "1 MB over 8 seconds", size: 1_000_000, durationS: 8, want: 1000},
		{name: "zero duration is zero", size: 1_000_000, durationS: 0, want: 0},
		{name: "negative duration is zero", size: 1_000_000, durationS: -1, want: 0},
		{name: "zero size", size: 0, durationS: 10, want: 0},
	}
	for _, tc := range tests {
		if got := BitrateKbps(tc.size, tc.durationS); got != tc.want {
			t.Errorf("%s: BitrateKbps(%d, %v) = %v, want %v",
				tc.name, tc.size, tc.durationS, got, tc.want)
		}
	}
}

func TestIterGrid(t *testing.T) {
	t.Parallel()

	got := IterGrid([]string{"fast", "medium"}, []int{20, 26})
	want := []Cell{
		{Preset: "fast", CRF: 20},
		{Preset: "fast", CRF: 26},
		{Preset: "medium", CRF: 20},
		{Preset: "medium", CRF: 26},
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("IterGrid() = %v, want %v", got, want)
	}
	if got := IterGrid(nil, []int{20}); len(got) != 0 {
		t.Errorf("IterGrid with no presets = %v, want empty", got)
	}
}

func TestRunEncodeRecordsFailureRatherThanAborting(t *testing.T) {
	t.Parallel()

	ResetEncoderVersionProbeCache()
	req := baseEncodeRequest()
	stub := func(_ context.Context, argv []string) RunResult {
		if len(argv) > 1 && argv[1] == "-version" {
			return RunResult{}
		}
		return RunResult{Stderr: "boom\n", ReturnCode: 187}
	}
	got := RunEncode(context.Background(), req, "ffmpeg", stub)
	if got.ExitStatus != 187 {
		t.Errorf("ExitStatus = %d, want 187", got.ExitStatus)
	}
	if got.EncodeSizeBytes != 0 {
		t.Errorf("EncodeSizeBytes = %d, want 0 on a failed encode", got.EncodeSizeBytes)
	}
	if !strings.Contains(got.StderrTail, "boom") {
		t.Errorf("StderrTail = %q, want it to carry the ffmpeg diagnostic", got.StderrTail)
	}
}

func TestRunEncodeMeasuresOutputSize(t *testing.T) {
	t.Parallel()

	ResetEncoderVersionProbeCache()
	dir := t.TempDir()
	out := filepath.Join(dir, "out.mp4")
	payload := []byte("0123456789")

	req := baseEncodeRequest()
	req.Output = out
	stub := func(_ context.Context, argv []string) RunResult {
		if len(argv) > 1 && argv[1] == "-version" {
			return RunResult{Stdout: "configuration: --enable-libx264\n"}
		}
		if err := os.WriteFile(out, payload, 0o600); err != nil {
			return RunResult{ReturnCode: 1, Stderr: err.Error()}
		}
		return RunResult{Stderr: "ffmpeg version 7.1\n"}
	}
	got := RunEncode(context.Background(), req, "ffmpeg", stub)
	if got.ExitStatus != 0 {
		t.Fatalf("ExitStatus = %d, want 0", got.ExitStatus)
	}
	if got.EncodeSizeBytes != int64(len(payload)) {
		t.Errorf("EncodeSizeBytes = %d, want %d", got.EncodeSizeBytes, len(payload))
	}
	// The banner is absent under -hide_banner, so the driver falls back to
	// the "ffmpeg -version" configure-line probe.
	if got.EncoderVersion != "libx264-enabled" {
		t.Errorf("EncoderVersion = %q, want the configure-line fallback", got.EncoderVersion)
	}
}

func TestRunEncodeSkipsSizeProbeOnPass1(t *testing.T) {
	t.Parallel()

	ResetEncoderVersionProbeCache()
	dir := t.TempDir()
	out := filepath.Join(dir, "out.mp4")
	// A stale artefact from an earlier cell must not be reported as this
	// pass-1 invocation's output size.
	if err := os.WriteFile(out, []byte("stale"), 0o600); err != nil {
		t.Fatalf("seed stale output: %v", err)
	}

	req := baseEncodeRequest()
	req.Output = out
	req.PassNumber = 1
	req.StatsPath = filepath.Join(dir, "stats")
	stub := func(context.Context, []string) RunResult {
		return RunResult{Stderr: "x264 - core 164\n"}
	}
	got := RunEncode(context.Background(), req, "ffmpeg", stub)
	if got.EncodeSizeBytes != 0 {
		t.Errorf("pass-1 EncodeSizeBytes = %d, want 0", got.EncodeSizeBytes)
	}
}

func TestRunTwoPassEncodeFallsBackForUnsupportedCodecs(t *testing.T) {
	t.Parallel()

	ResetEncoderVersionProbeCache()
	req := baseEncodeRequest()
	req.Encoder = "h264_nvenc"

	var seen [][]string
	stub := func(_ context.Context, argv []string) RunResult {
		seen = append(seen, argv)
		return RunResult{}
	}
	got := RunTwoPassEncode(context.Background(), req, "ffmpeg", stub, t.TempDir())
	if got.ExitStatus != 0 {
		t.Fatalf("ExitStatus = %d, want 0", got.ExitStatus)
	}
	if len(seen) != 1 {
		t.Fatalf("ran %d ffmpeg invocations, want exactly 1 (single-pass fallback)", len(seen))
	}
	for _, arg := range seen[0] {
		if arg == "-pass" {
			t.Error("the fallback single-pass encode carried a -pass flag")
		}
	}
}

func TestRunTwoPassEncodeSkipsPass2OnPass1Failure(t *testing.T) {
	t.Parallel()

	ResetEncoderVersionProbeCache()
	req := baseEncodeRequest()

	calls := 0
	stub := func(_ context.Context, argv []string) RunResult {
		if len(argv) > 1 && argv[1] == "-version" {
			return RunResult{}
		}
		calls++
		return RunResult{Stderr: "pass 1 exploded\n", ReturnCode: 3}
	}
	got := RunTwoPassEncode(context.Background(), req, "ffmpeg", stub, t.TempDir())
	if calls != 1 {
		t.Errorf("ran %d encode invocations, want 1 — pass 2 should be skipped", calls)
	}
	if got.ExitStatus != 3 {
		t.Errorf("ExitStatus = %d, want the pass-1 status 3", got.ExitStatus)
	}
	if !strings.HasPrefix(got.StderrTail, "[pass 1 failed]") {
		t.Errorf("StderrTail = %q, want the pass-1 marker prefix", got.StderrTail)
	}
}

func TestRunEncodeWithStatsParsesTheSidecar(t *testing.T) {
	t.Parallel()

	ResetEncoderVersionProbeCache()
	dir := t.TempDir()
	req := baseEncodeRequest()
	req.Output = filepath.Join(dir, "out.mp4")

	statsBody := "#options: whatever\n" +
		"in:0 out:0 type:I q:25.0 tex:100 mv:0 misc:10 imb:80 pmb:0 smb:0;\n" +
		"in:1 out:1 type:P q:27.0 tex:50 mv:5 misc:5 imb:0 pmb:40 smb:40;\n"

	stub := func(_ context.Context, argv []string) RunResult {
		if len(argv) > 1 && argv[1] == "-version" {
			return RunResult{}
		}
		// The pass-1 invocation writes the "<prefix>-0.log" sidecar.
		for i, a := range argv {
			if a == "-passlogfile" && i+1 < len(argv) {
				logPath := argv[i+1] + "-0.log"
				if err := os.WriteFile(logPath, []byte(statsBody), 0o600); err != nil {
					return RunResult{ReturnCode: 1, Stderr: err.Error()}
				}
				return RunResult{}
			}
		}
		if err := os.WriteFile(req.Output, []byte("bits"), 0o600); err != nil {
			return RunResult{ReturnCode: 1, Stderr: err.Error()}
		}
		return RunResult{Stderr: "x264 - core 164\n"}
	}

	got := RunEncodeWithStats(context.Background(), req, "ffmpeg", stub, dir)
	if got.ExitStatus != 0 {
		t.Fatalf("ExitStatus = %d, want 0", got.ExitStatus)
	}
	if len(got.EncoderStats) != 2 {
		t.Fatalf("captured %d stats frames, want 2", len(got.EncoderStats))
	}
	if got.EncoderStats[0].FrameType != "I" || got.EncoderStats[1].FrameType != "P" {
		t.Errorf("frame types = %q / %q, want I / P",
			got.EncoderStats[0].FrameType, got.EncoderStats[1].FrameType)
	}
}
