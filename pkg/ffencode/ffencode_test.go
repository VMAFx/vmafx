// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package ffencode_test

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/ffencode"
)

// TestBuildFFmpegCommand pins the argv against a dump of the Python
// vmaftune.encode.build_ffmpeg_command for the same requests. Every "want"
// slice below is copy-pasted from that dump, so a divergence in the Go
// driver's flag ordering, float formatting, or codec-arg layering fails here.
func TestBuildFFmpegCommand(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		req  ffencode.Request
		want []string
	}{
		{
			name: "raw yuv libx264",
			req: ffencode.Request{
				Source: "/src/ref.yuv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p", Framerate: 24.0,
				Encoder: "libx264", Preset: "medium", CRF: 23,
				Output: "/out/enc.mp4",
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/src/ref.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "23",
				"/out/enc.mp4",
			},
		},
		{
			name: "container source skips raw geometry",
			req: ffencode.Request{
				Source: "/src/ref.mkv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p", Framerate: 24.0,
				Encoder: "libx265", Preset: "slow", CRF: 28,
				Output: "/out/enc.mp4", SourceIsContainer: true,
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-i", "/src/ref.mkv",
				"-c:v", "libx265", "-preset", "slow", "-crf", "28",
				"/out/enc.mp4",
			},
		},
		{
			name: "sample clip window seeks input-side",
			req: ffencode.Request{
				Source: "/src/ref.yuv", Width: 1280, Height: 720,
				PixFmt: "yuv420p", Framerate: 23.976,
				Encoder: "libx264", Preset: "fast", CRF: 27,
				Output:            "/out/enc.mp4",
				SampleClipSeconds: 10.0, SampleClipStartS: 2.5,
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1280x720", "-r", "23.976",
				"-ss", "2.5", "-t", "10.0",
				"-i", "/src/ref.yuv",
				"-c:v", "libx264", "-preset", "fast", "-crf", "27",
				"/out/enc.mp4",
			},
		},
		{
			name: "duration fallback bounds the encode",
			req: ffencode.Request{
				Source: "/src/ref.yuv", Width: 640, Height: 480,
				PixFmt: "yuv420p", Framerate: 30.0,
				Encoder: "libx264", Preset: "medium", CRF: 23,
				Output: "/out/enc.mp4", DurationS: 5.0,
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "640x480", "-r", "30.0",
				"-t", "5.0",
				"-i", "/src/ref.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "23",
				"/out/enc.mp4",
			},
		},
		{
			name: "extra params land after the codec args",
			req: ffencode.Request{
				Source: "/src/ref.yuv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p", Framerate: 24.0,
				Encoder: "libx264", Preset: "medium", CRF: 30,
				Output: "/out/probe.mp4",
				ExtraParams: []string{
					"-vf", "pelorus_deband_vulkan=range=15:thry=0.012",
				},
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/src/ref.yuv",
				"-c:v", "libx264", "-preset", "medium", "-crf", "30",
				"-vf", "pelorus_deband_vulkan=range=15:thry=0.012",
				"/out/probe.mp4",
			},
		},
		{
			name: "pass 1 discards the bitstream",
			req: ffencode.Request{
				Source: "/src/ref.yuv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p", Framerate: 24.0,
				Encoder: "libx264", Preset: "medium", CRF: 23,
				Output: "/out/enc.mp4", PassNumber: 1, StatsPath: "/tmp/stats",
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/src/ref.yuv",
				"-c:v", "libx264", "-preset", "medium",
				"-pass", "1", "-passlogfile", "/tmp/stats",
				"-f", "null", "-",
			},
		},
		{
			name: "pass 2 keeps the output",
			req: ffencode.Request{
				Source: "/src/ref.yuv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p", Framerate: 24.0,
				Encoder: "libx264", Preset: "medium", CRF: 23,
				Output: "/out/enc.mp4", PassNumber: 2, StatsPath: "/tmp/stats",
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/src/ref.yuv",
				"-c:v", "libx264", "-preset", "medium",
				"-pass", "2", "-passlogfile", "/tmp/stats",
				"/out/enc.mp4",
			},
		},
		{
			name: "pass 2 with bitrate in ExtraParams retains bitrate flag without crf conflict",
			req: ffencode.Request{
				Source: "/src/ref.yuv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p", Framerate: 24.0,
				Encoder: "libx264", Preset: "medium", CRF: 23,
				Output: "/out/enc.mp4", PassNumber: 2, StatsPath: "/tmp/stats",
				ExtraParams: []string{"-b:v", "2000k"},
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/src/ref.yuv",
				"-c:v", "libx264", "-preset", "medium",
				"-pass", "2", "-passlogfile", "/tmp/stats",
				"-b:v", "2000k",
				"/out/enc.mp4",
			},
		},
		{
			name: "svtav1 maps medium onto -preset 7",
			req: ffencode.Request{
				Source: "/src/ref.yuv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p", Framerate: 24.0,
				Encoder: "libsvtav1", Preset: "medium", CRF: 35,
				Output: "/out/enc.mp4",
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/src/ref.yuv",
				"-c:v", "libsvtav1", "-preset", "7", "-crf", "35",
				"/out/enc.mp4",
			},
		},
		{
			name: "libvpx appends -b:v 0 then -row-mt 1",
			req: ffencode.Request{
				Source: "/src/ref.yuv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p", Framerate: 24.0,
				Encoder: "libvpx-vp9", Preset: "medium", CRF: 32,
				Output: "/out/enc.webm",
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/src/ref.yuv",
				"-c:v", "libvpx-vp9", "-deadline", "good", "-cpu-used", "3",
				"-crf", "32", "-b:v", "0", "-row-mt", "1",
				"/out/enc.webm",
			},
		},
		{
			name: "unregistered encoder falls back to the legacy shape",
			req: ffencode.Request{
				Source: "/src/ref.yuv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p", Framerate: 24.0,
				Encoder: "libtheora", Preset: "medium", CRF: 23,
				Output: "/out/enc.ogv",
			},
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "info",
				"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24.0",
				"-i", "/src/ref.yuv",
				"-c:v", "libtheora", "-preset", "medium", "-crf", "23",
				"/out/enc.ogv",
			},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := ffencode.BuildFFmpegCommand(tc.req, "ffmpeg")
			if err != nil {
				t.Fatalf("BuildFFmpegCommand: %v", err)
			}
			if !slices.Equal(got, tc.want) {
				t.Errorf("argv mismatch\n got: %v\nwant: %v", got, tc.want)
			}
		})
	}
}

// TestBuildFFmpegCommand_errors covers the two rejection paths.
func TestBuildFFmpegCommand_errors(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		req  ffencode.Request
	}{
		{
			name: "two-pass without a stats path",
			req: ffencode.Request{
				Source: "/src/ref.yuv", PixFmt: "yuv420p", Framerate: 24,
				Encoder: "libx264", Preset: "medium", CRF: 23,
				Output: "/out/enc.mp4", PassNumber: 1,
			},
		},
		{
			name: "two-pass on an encoder without stats support",
			req: ffencode.Request{
				Source: "/src/ref.yuv", PixFmt: "yuv420p", Framerate: 24,
				Encoder: "h264_nvenc", Preset: "medium", CRF: 23,
				Output: "/out/enc.mp4", PassNumber: 1, StatsPath: "/tmp/s",
			},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if _, err := ffencode.BuildFFmpegCommand(tc.req, "ffmpeg"); err == nil {
				t.Error("expected an error")
			}
		})
	}
}

// TestParseVersions mirrors vmaftune.encode.parse_versions, including the
// auto-detect cascade and the per-codec fallbacks.
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
			name:       "x264 banner",
			stderr:     "ffmpeg version 8.1\nx264 - core 164 r3094M 0bfe3e0\n",
			encoder:    "libx264",
			wantFFmpeg: "8.1", wantEncoder: "libx264-164",
		},
		{
			name:       "x264 alternate banner spelling",
			stderr:     "ffmpeg version n8.1\nx264-core 163\n",
			encoder:    "libx264",
			wantFFmpeg: "n8.1", wantEncoder: "libx264-163",
		},
		{
			name:       "auto-detect falls through to x265",
			stderr:     "ffmpeg version 8.1\nx265 [info]: HEVC encoder version 3.5\n",
			encoder:    "",
			wantFFmpeg: "8.1", wantEncoder: "libx265-3.5",
		},
		{
			name:       "auto-detect falls through to svt-av1",
			stderr:     "ffmpeg version 8.1\nSvt[info]:SVT-AV1 Encoder Lib v2.1.0\n",
			encoder:    "libx264",
			wantFFmpeg: "8.1", wantEncoder: "libsvtav1-2.1.0",
		},
		{
			name:       "explicit x265",
			stderr:     "x265 [info]: HEVC encoder version 3.5\n",
			encoder:    "libx265",
			wantFFmpeg: "unknown", wantEncoder: "libx265-3.5",
		},
		{
			name:       "libvpx-vp9",
			stderr:     "[libvpx-vp9 @ 0x55] v1.13.1\n",
			encoder:    "libvpx-vp9",
			wantFFmpeg: "unknown", wantEncoder: "libvpx-vp9-1.13.1",
		},
		{
			name:       "libaom falls back to the adapter name",
			stderr:     "ffmpeg version 8.1\n",
			encoder:    "libaom-av1",
			wantFFmpeg: "8.1", wantEncoder: "libaom-av1",
		},
		{
			name:       "libaom banner when present",
			stderr:     "[libaom-av1 @ 0x55] libaom-av1 v3.9.0\n",
			encoder:    "libaom-av1",
			wantFFmpeg: "unknown", wantEncoder: "libaom-av1-3.9.0",
		},
		{
			name:       "libvvenc falls back to the adapter name",
			stderr:     "",
			encoder:    "libvvenc",
			wantFFmpeg: "unknown", wantEncoder: "libvvenc",
		},
		{
			name:       "hardware encoders return their token",
			stderr:     "ffmpeg version 8.1\n",
			encoder:    "hevc_nvenc",
			wantFFmpeg: "8.1", wantEncoder: "hevc_nvenc",
		},
		{
			name:       "wholly unknown encoder",
			stderr:     "ffmpeg version 8.1\n",
			encoder:    "libtheora",
			wantFFmpeg: "8.1", wantEncoder: "unknown",
		},
		{
			name:       "no banners at all",
			stderr:     "",
			encoder:    "libx264",
			wantFFmpeg: "unknown", wantEncoder: "unknown",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			gotFF, gotEnc := ffencode.ParseVersions(tc.stderr, tc.encoder)
			if gotFF != tc.wantFFmpeg || gotEnc != tc.wantEncoder {
				t.Errorf("ParseVersions = (%q, %q), want (%q, %q)",
					gotFF, gotEnc, tc.wantFFmpeg, tc.wantEncoder)
			}
		})
	}
}

// TestBitrateKbps pins the size->kbps conversion including the
// unknown-duration guard.
func TestBitrateKbps(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		sizeBytes int64
		durationS float64
		want      float64
	}{
		{"one megabyte over ten seconds", 1_000_000, 10.0, 800.0},
		{"zero duration is undefined", 1_000_000, 0.0, 0.0},
		{"negative duration is undefined", 1_000_000, -1.0, 0.0},
		{"empty file", 0, 10.0, 0.0},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			if got := ffencode.BitrateKbps(tc.sizeBytes, tc.durationS); got != tc.want {
				t.Errorf("BitrateKbps(%d, %g) = %g, want %g",
					tc.sizeBytes, tc.durationS, got, tc.want)
			}
		})
	}
}

// TestProbeEncoderLabel covers the availability fallback that fills in the
// version field when -hide_banner suppressed the per-encoder banner.
func TestProbeEncoderLabel(t *testing.T) {
	t.Parallel()

	const configLine = "ffmpeg version 8.1\nconfiguration: --prefix=/usr " +
		"--enable-libx264 --enable-libx265 --enable-gpl\n"

	tests := []struct {
		name    string
		encoder string
		output  string
		want    string
	}{
		{"compiled in", "libx264", configLine, "libx264-enabled"},
		{"also compiled in", "libx265", configLine, "libx265-enabled"},
		{"not compiled in", "libsvtav1", configLine, ""},
		{"encoder with no probe pattern", "h264_nvenc", configLine, ""},
		{"empty ffmpeg output", "libx264", "", ""},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			// A unique binary path per subtest keeps the memo cache from
			// leaking answers between cases.
			bin := "/fake/ffmpeg-" + tc.name
			runner := func(context.Context, []string) (string, int, error) {
				return tc.output, 0, nil
			}
			got := ffencode.ProbeEncoderLabel(
				context.Background(), bin, tc.encoder, runner)
			if got != tc.want {
				t.Errorf("ProbeEncoderLabel = %q, want %q", got, tc.want)
			}
		})
	}
}

// TestProbeEncoderLabel_memoises asserts the probe forks ffmpeg at most once
// per (binary, encoder). A sweep would otherwise pay it per cell.
func TestProbeEncoderLabel_memoises(t *testing.T) {
	t.Parallel()

	calls := 0
	runner := func(context.Context, []string) (string, int, error) {
		calls++
		return "configuration: --enable-libx264\n", 0, nil
	}
	const bin = "/fake/ffmpeg-memo"
	for range 5 {
		if got := ffencode.ProbeEncoderLabel(
			context.Background(), bin, "libx264", runner); got != "libx264-enabled" {
			t.Fatalf("ProbeEncoderLabel = %q", got)
		}
	}
	if calls != 1 {
		t.Errorf("probe forked ffmpeg %d times, want 1", calls)
	}
}

// TestRun_injectedRunner drives Run through the subprocess seam so the test
// needs no ffmpeg. A non-zero ffmpeg exit must surface through
// Result.ExitStatus, not as a Go error — the search loops depend on that
// contract to score a failed probe as 0 VMAF instead of aborting the sweep.
func TestRun_injectedRunner(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name           string
		exitStatus     int
		runnerErr      error
		writeOutput    bool
		wantErr        bool
		wantExitStatus int
		wantSize       int64
	}{
		{
			name: "successful encode reports size", exitStatus: 0,
			writeOutput: true, wantExitStatus: 0, wantSize: 5,
		},
		{
			name: "failed encode surfaces exit status", exitStatus: 1,
			writeOutput: false, wantExitStatus: 1, wantSize: 0,
		},
		{
			name: "spawn failure is a Go error", runnerErr: errors.New("no ffmpeg"),
			wantErr: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			dir := t.TempDir()
			out := filepath.Join(dir, "enc.mp4")
			if tc.writeOutput {
				if err := os.WriteFile(out, []byte("bytes"), 0o600); err != nil {
					t.Fatalf("seed output: %v", err)
				}
			}
			req := ffencode.Request{
				Source: "/src/ref.yuv", Width: 320, Height: 240,
				PixFmt: "yuv420p", Framerate: 24.0,
				Encoder: "libx264", Preset: "medium", CRF: 23, Output: out,
			}
			var sawArgv []string
			runner := func(_ context.Context, argv []string) (string, int, error) {
				sawArgv = argv
				return "ffmpeg version 8.1\nx264 - core 164\n", tc.exitStatus, tc.runnerErr
			}

			res, err := ffencode.Run(context.Background(), req, "ffmpeg", runner)
			if (err != nil) != tc.wantErr {
				t.Fatalf("Run error = %v, wantErr %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if res.ExitStatus != tc.wantExitStatus {
				t.Errorf("ExitStatus = %d, want %d", res.ExitStatus, tc.wantExitStatus)
			}
			if res.EncodeSizeBytes != tc.wantSize {
				t.Errorf("EncodeSizeBytes = %d, want %d", res.EncodeSizeBytes, tc.wantSize)
			}
			if res.EncoderVersion != "libx264-164" {
				t.Errorf("EncoderVersion = %q, want libx264-164", res.EncoderVersion)
			}
			if len(sawArgv) == 0 || sawArgv[0] != "ffmpeg" {
				t.Errorf("runner saw argv %v", sawArgv)
			}
		})
	}
}

// TestBuildFFmpegCommandQSVChain: a QSV encode carries the device chain before
// -i and the upload at the end of the caller's -vf chain, as the Python
// build_ffmpeg_command does (ADR-0601); the encode-profile argv used to have
// neither.
func TestBuildFFmpegCommandQSVChain(t *testing.T) {
	t.Parallel()
	req := ffencode.Request{
		Source: "ref.yuv", Width: 1920, Height: 1080, PixFmt: "yuv420p", Framerate: 24,
		Encoder: "hevc_qsv", Preset: "medium", CRF: 23, Output: "out.mp4",
		ExtraParams: []string{"-vf", "scale=640:360"}, VAAPIDevice: "/dev/dri/renderD131",
	}
	argv, err := ffencode.BuildFFmpegCommand(req, "ffmpeg")
	if err != nil {
		t.Fatal(err)
	}
	joined := strings.Join(argv, " ")
	chain := "-init_hw_device vaapi=va:/dev/dri/renderD131 -init_hw_device qsv=qsv_dev@va " +
		"-filter_hw_device qsv_dev"
	if !strings.Contains(joined, chain) || strings.Index(joined, chain) > strings.Index(joined, " -i ") {
		t.Errorf("device chain missing or after -i: %v", argv)
	}
	if strings.Count(joined, "-vf") != 1 ||
		!strings.Contains(joined, "-vf scale=640:360,format=nv12,hwupload=extra_hw_frames=64") {
		t.Errorf("upload must join the caller's -vf chain: %v", argv)
	}
	req.Encoder = "libx264"
	argv, err = ffencode.BuildFFmpegCommand(req, "ffmpeg")
	if err != nil {
		t.Fatal(err)
	}
	if j := strings.Join(argv, " "); strings.Contains(j, "init_hw_device") || strings.Contains(j, "hwupload") {
		t.Errorf("non-QSV encode got the QSV chain: %v", argv)
	}
}

// TestBuildFFmpegCommandABRSwapsCRFForBitrate is ADR-1565's pass 2. Python:
// tests/test_x265_two_pass_abr.py::test_abr_request_swaps_crf_for_bitrate.
func TestBuildFFmpegCommandABRSwapsCRFForBitrate(t *testing.T) {
	t.Parallel()

	req := ffencode.Request{
		Source: "/ref.yuv", Width: 64, Height: 64, PixFmt: "yuv420p", Framerate: 24,
		Encoder: "libx265", Preset: "medium", CRF: 28, Output: "/out.mp4",
		PassNumber: 2, StatsPath: "/s.stats", ABRBitrateKbps: 1234.4,
	}
	cmd, err := ffencode.BuildFFmpegCommand(req, "ffmpeg")
	if err != nil {
		t.Fatalf("BuildFFmpegCommand: %v", err)
	}
	joined := " " + strings.Join(cmd, " ") + " "
	if strings.Contains(joined, " -crf ") {
		t.Errorf("ABR argv still carries -crf: %v", cmd)
	}
	if !strings.Contains(joined, " -b:v 1234k ") || strings.Count(joined, " -b:v ") != 1 {
		t.Errorf("ABR argv wants exactly one -b:v 1234k: %v", cmd)
	}

	req.ABRBitrateKbps = 0
	cmd, _ = ffencode.BuildFFmpegCommand(req, "ffmpeg")
	if joined = " " + strings.Join(cmd, " ") + " "; !strings.Contains(joined, " -crf 28 ") ||
		strings.Contains(joined, " -b:v ") {
		t.Errorf("without ABR the argv must keep -crf and add no -b:v: %v", cmd)
	}
}

func TestBuildFFmpegCommandPass1OutputReplacesTheNullMuxer(t *testing.T) {
	t.Parallel()

	req := ffencode.Request{
		Source: "/ref.yuv", Width: 64, Height: 64, PixFmt: "yuv420p", Framerate: 24,
		Encoder: "libx265", Preset: "medium", CRF: 28, Output: "/out.mp4",
		PassNumber: 1, StatsPath: "/s.stats", Pass1Output: "/p1.mp4",
	}
	cmd, err := ffencode.BuildFFmpegCommand(req, "ffmpeg")
	if err != nil {
		t.Fatalf("BuildFFmpegCommand: %v", err)
	}
	if cmd[len(cmd)-1] != "/p1.mp4" {
		t.Errorf("pass 1 with Pass1Output must end in it: %v", cmd)
	}
	req.Pass1Output = ""
	cmd, _ = ffencode.BuildFFmpegCommand(req, "ffmpeg")
	if got := strings.Join(cmd[len(cmd)-3:], " "); got != "-f null -" {
		t.Errorf("pass 1 default sink = %q, want -f null -", got)
	}
}

// TestBuildFFmpegCommand_mergesCodecParams: a two-pass adapter, a saliency
// augment and an HDR block each add their own -x265-params, and FFmpeg keeps
// only the last occurrence of a string option; the argv carries one.
func TestBuildFFmpegCommand_mergesCodecParams(t *testing.T) {
	t.Parallel()

	req := ffencode.Request{
		Source: "/src/ref.yuv", Width: 1920, Height: 1080,
		PixFmt: "yuv420p", Framerate: 24.0,
		Encoder: "libx265", Preset: "medium", CRF: 23,
		Output: "/out/enc.mp4", PassNumber: 2, StatsPath: "/tmp/stats",
		ExtraParams: []string{
			"-x265-params", "zones=0,9,q=-4",
			"-color_primaries", "bt2020",
			"-x265-params", "master-display=G(1,2)B(3,4)R(5,6)WP(7,8)L(9,1)",
		},
	}
	argv, err := ffencode.BuildFFmpegCommand(req, "ffmpeg")
	if err != nil {
		t.Fatalf("BuildFFmpegCommand: %v", err)
	}
	var values []string
	for i, a := range argv {
		if a == "-x265-params" {
			values = append(values, argv[i+1])
		}
	}
	if len(values) != 1 {
		t.Fatalf("got %d -x265-params options, want one: %q", len(values), argv)
	}
	for _, want := range []string{"pass=2:stats=/tmp/stats", "zones=0,9,q=-4", "master-display=G(1,2)"} {
		if !strings.Contains(values[0], want) {
			t.Errorf("merged -x265-params %q lacks %q", values[0], want)
		}
	}
	if !slices.Contains(argv, "-color_primaries") {
		t.Errorf("an unrelated option between the two was lost: %q", argv)
	}
}

func TestMergeCodecParams(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		in   []string
		want []string
	}{
		{"none", []string{"-c:v", "libx264", "out.mp4"}, []string{"-c:v", "libx264", "out.mp4"}},
		{"one", []string{"-x264-params", "a=1", "o"}, []string{"-x264-params", "a=1", "o"}},
		{
			"two of one flag at the first position",
			[]string{"-x265-params", "a=1", "-b:v", "2k", "-x265-params", "b=2", "o"},
			[]string{"-x265-params", "a=1:b=2", "-b:v", "2k", "o"},
		},
		{
			"flags stay separate",
			[]string{"-x265-params", "a=1", "-svtav1-params", "b=2", "-x265-params", "c=3"},
			[]string{"-x265-params", "a=1:c=3", "-svtav1-params", "b=2"},
		},
		{"empty value adds nothing", []string{"-x265-params", "a=1", "-x265-params", ""}, []string{"-x265-params", "a=1"}},
		{"trailing flag without a value is kept", []string{"-x265-params"}, []string{"-x265-params"}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			if got := ffencode.MergeCodecParams(tt.in); !slices.Equal(got, tt.want) {
				t.Errorf("MergeCodecParams(%q) = %q, want %q", tt.in, got, tt.want)
			}
		})
	}
}
