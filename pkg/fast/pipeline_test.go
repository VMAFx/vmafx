// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/fast/pipeline_test.go — table-driven tests for the production
// probe-encode / libvmaf-score plumbing ported from cli._build_fast_* and
// vmaftune/score.py.

package fast

import (
	"context"
	"github.com/VMAFx/vmafx/pkg/model"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestBuildVMAFCommand pins the libvmaf argv against score.build_vmaf_command.
func TestBuildVMAFCommand(t *testing.T) {
	t.Parallel()

	base := PipelineConfig{
		Src:       "/refs/clip.yuv",
		Width:     1920,
		Height:    1080,
		PixFmt:    "yuv420p",
		VMAFBin:   "vmaf",
		VMAFModel: "vmaf_v0.6.1",
	}

	tests := []struct {
		name         string
		cfg          PipelineConfig
		backend      string
		frameSkipRef int
		frameCnt     int
		want         []string
	}{
		{
			name: "8-bit 4:2:0 without a backend",
			cfg:  base,
			want: []string{
				"vmaf",
				"--reference", "/refs/clip.yuv",
				"--distorted", "/tmp/dist.mp4",
				"--width", "1920",
				"--height", "1080",
				"--pixel_format", "420",
				"--bitdepth", "8",
				"--model", "version=vmaf_v0.6.1",
				"--json",
				"--output", "/tmp/out.json",
			},
		},
		{
			name:    "explicit backend appends --backend",
			cfg:     base,
			backend: "cuda",
			want: []string{
				"vmaf",
				"--reference", "/refs/clip.yuv",
				"--distorted", "/tmp/dist.mp4",
				"--width", "1920",
				"--height", "1080",
				"--pixel_format", "420",
				"--bitdepth", "8",
				"--model", "version=vmaf_v0.6.1",
				"--json",
				"--output", "/tmp/out.json",
				"--backend", "cuda",
			},
		},
		{
			name:     "sample-clip window adds frame_skip_ref / frame_cnt",
			cfg:      base,
			frameCnt: 120,
			want: []string{
				"vmaf",
				"--reference", "/refs/clip.yuv",
				"--distorted", "/tmp/dist.mp4",
				"--width", "1920",
				"--height", "1080",
				"--pixel_format", "420",
				"--bitdepth", "8",
				"--model", "version=vmaf_v0.6.1",
				"--json",
				"--output", "/tmp/out.json",
				"--frame_cnt", "120",
			},
		},
		{
			name:         "both window flags",
			cfg:          base,
			frameSkipRef: 24,
			frameCnt:     120,
			want: []string{
				"vmaf",
				"--reference", "/refs/clip.yuv",
				"--distorted", "/tmp/dist.mp4",
				"--width", "1920",
				"--height", "1080",
				"--pixel_format", "420",
				"--bitdepth", "8",
				"--model", "version=vmaf_v0.6.1",
				"--json",
				"--output", "/tmp/out.json",
				"--frame_skip_ref", "24",
				"--frame_cnt", "120",
			},
		},
		{
			name: "10-bit 4:2:2 maps pixel_format and bitdepth",
			cfg: PipelineConfig{
				Src: "/refs/clip.yuv", Width: 3840, Height: 2160,
				PixFmt: "yuv422p10le", VMAFBin: "/opt/vmaf", VMAFModel: "vmaf_v0.6.1",
			},
			want: []string{
				"/opt/vmaf",
				"--reference", "/refs/clip.yuv",
				"--distorted", "/tmp/dist.mp4",
				"--width", "3840",
				"--height", "2160",
				"--pixel_format", "422",
				"--bitdepth", "10",
				"--model", "version=vmaf_v0.6.1",
				"--json",
				"--output", "/tmp/out.json",
			},
		},
		{
			name: "pre-formatted model string passes through",
			cfg: PipelineConfig{
				Src: "/refs/clip.yuv", Width: 640, Height: 480,
				PixFmt: "yuv444p12le", VMAFModel: "path=/models/hdr.json",
			},
			want: []string{
				"vmaf",
				"--reference", "/refs/clip.yuv",
				"--distorted", "/tmp/dist.mp4",
				"--width", "640",
				"--height", "480",
				"--pixel_format", "444",
				"--bitdepth", "12",
				"--model", "path=/models/hdr.json",
				"--json",
				"--output", "/tmp/out.json",
				"--feature", "vif",
			},
		},
		{
			name: "default model appends --feature vif",
			cfg: PipelineConfig{
				Src: "/refs/clip.yuv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p",
			},
			want: []string{
				"vmaf",
				"--reference", "/refs/clip.yuv",
				"--distorted", "/tmp/dist.mp4",
				"--width", "1920",
				"--height", "1080",
				"--pixel_format", "420",
				"--bitdepth", "8",
				"--model", "version=" + model.DefaultVersion,
				"--json",
				"--output", "/tmp/out.json",
				"--feature", "vif",
			},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := BuildVMAFCommand(tc.cfg, "/tmp/dist.mp4", "/tmp/out.json",
				tc.backend, tc.frameSkipRef, tc.frameCnt)
			if strings.Join(got, " ") != strings.Join(tc.want, " ") {
				t.Errorf("argv mismatch:\n got %v\nwant %v", got, tc.want)
			}
		})
	}
}

// TestInputArgs pins the ffmpeg input-side options against
// encode.build_ffmpeg_command.
func TestInputArgs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name  string
		cfg   PipelineConfig
		start float64
		clip  float64
		want  []string
	}{
		{
			name: "raw YUV full source",
			cfg: PipelineConfig{
				Src: "ref.yuv", Width: 1920, Height: 1080,
				PixFmt: "yuv420p", Framerate: 24,
			},
			want: []string{"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "1920x1080", "-r", "24"},
		},
		{
			name: "raw YUV sample clip fast-seeks on the input side",
			cfg: PipelineConfig{
				Src: "ref.yuv", Width: 1280, Height: 720,
				PixFmt: "yuv420p10le", Framerate: 29.97,
			},
			start: 2.5,
			clip:  5,
			want: []string{
				"-f", "rawvideo", "-pix_fmt", "yuv420p10le", "-s", "1280x720", "-r", "29.97",
				"-ss", "2.5", "-t", "5",
			},
		},
		{
			name: "container source omits rawvideo geometry",
			cfg:  PipelineConfig{Src: "ref.mp4", Width: 1920, Height: 1080, Framerate: 24},
			clip: 5,
			want: []string{"-ss", "0", "-t", "5"},
		},
		{
			name: "container full source has no input options at all",
			cfg:  PipelineConfig{Src: "ref.mkv", Framerate: 24},
			want: nil,
		},
		{
			name: "extensionless source is treated as raw",
			cfg: PipelineConfig{
				Src: "ref", Width: 176, Height: 144, PixFmt: "yuv420p", Framerate: 30,
			},
			want: []string{"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "176x144", "-r", "30"},
		},
		{
			name: "y4m is treated as raw",
			cfg: PipelineConfig{
				Src: "ref.Y4M", Width: 176, Height: 144, PixFmt: "yuv420p", Framerate: 30,
			},
			want: []string{"-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", "176x144", "-r", "30"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := tc.cfg.inputArgs(tc.start, tc.clip)
			if strings.Join(got, " ") != strings.Join(tc.want, " ") {
				t.Errorf("input args mismatch:\n got %v\nwant %v", got, tc.want)
			}
		})
	}
}

// TestParseCanonical6Means covers the integer_-prefixed pooled keys modern
// libvmaf emits, the bare-key fallback, and the per-frame fallback.
func TestParseCanonical6Means(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		payload string
		want    []float64
		wantErr bool
	}{
		{
			name: "integer_-prefixed pooled keys (real libvmaf output)",
			payload: `{"pooled_metrics": {
				"integer_adm2":       {"mean": 0.95},
				"integer_vif_scale0": {"mean": 0.51},
				"integer_vif_scale1": {"mean": 0.86},
				"integer_vif_scale2": {"mean": 0.91},
				"integer_vif_scale3": {"mean": 0.94},
				"integer_motion2":    {"mean": 8.9},
				"vmaf":               {"mean": 92.0}
			}}`,
			want: []float64{0.95, 0.51, 0.86, 0.91, 0.94, 8.9},
		},
		{
			name: "options-suffixed pooled keys resolve via prefix matching",
			payload: `{"pooled_metrics": {
				"integer_adm2_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02": {"mean": 0.961},
				"integer_vif_scale0": {"mean": 0.505},
				"integer_vif_scale1": {"mean": 0.879},
				"integer_vif_scale2": {"mean": 0.938},
				"integer_vif_scale3": {"mean": 0.964},
				"integer_motion2_mmxv_18": {"mean": 1.25},
				"vmaf": {"mean": 89.5}
			}}`,
			want: []float64{0.961, 0.505, 0.879, 0.938, 0.964, 1.25},
		},
		{
			name: "bare pooled keys still resolve",
			payload: `{"pooled_metrics": {
				"adm2":       {"mean": 0.1},
				"vif_scale0": {"mean": 0.2},
				"vif_scale1": {"mean": 0.3},
				"vif_scale2": {"mean": 0.4},
				"vif_scale3": {"mean": 0.5},
				"motion2":    {"mean": 0.6}
			}}`,
			want: []float64{0.1, 0.2, 0.3, 0.4, 0.5, 0.6},
		},
		{
			name: "per-frame fallback averages the frames",
			payload: `{"frames": [
				{"metrics": {"integer_adm2": 0.8, "integer_motion2": 4.0}},
				{"metrics": {"integer_adm2": 1.0, "integer_motion2": 6.0}}
			]}`,
			want: []float64{0.9, 0, 0, 0, 0, 5.0},
		},
		{
			name:    "missing features fill 0.0, not NaN",
			payload: `{"pooled_metrics": {"vmaf": {"mean": 92.0}}}`,
			want:    []float64{0, 0, 0, 0, 0, 0},
		},
		{
			name: "pooled wins over per-frame",
			payload: `{
				"pooled_metrics": {"integer_adm2": {"mean": 0.5}},
				"frames": [{"metrics": {"integer_adm2": 9.9}}]
			}`,
			want: []float64{0.5, 0, 0, 0, 0, 0},
		},
		{
			name:    "malformed JSON is an error",
			payload: `{`,
			wantErr: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := ParseCanonical6Means([]byte(tc.payload))
			if tc.wantErr {
				if err == nil {
					t.Fatal("want an error, got nil")
				}
				return
			}
			if err != nil {
				t.Fatalf("ParseCanonical6Means: %v", err)
			}
			if len(got) != len(tc.want) {
				t.Fatalf("length = %d, want %d", len(got), len(tc.want))
			}
			for i := range tc.want {
				if math.Abs(got[i]-tc.want[i]) > 1e-9 {
					t.Errorf("%s = %v, want %v", canonical6[i], got[i], tc.want[i])
				}
			}
		})
	}
}

// TestParseVMAFScore covers the modern and legacy pooled shapes.
func TestParseVMAFScore(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		payload string
		want    float64
		wantErr bool
	}{
		{
			name:    "modern pooled_metrics shape",
			payload: `{"pooled_metrics": {"vmaf": {"mean": 91.5}}}`,
			want:    91.5,
		},
		{
			name:    "legacy top-level score",
			payload: `{"VMAF score": 88.25}`,
			want:    88.25,
		},
		{
			name:    "neither shape is an error",
			payload: `{"pooled_metrics": {"integer_adm2": {"mean": 0.9}}}`,
			wantErr: true,
		},
		{
			name:    "malformed JSON is an error",
			payload: `not json`,
			wantErr: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := ParseVMAFScore([]byte(tc.payload))
			if tc.wantErr {
				if err == nil {
					t.Fatalf("want an error, got %v", got)
				}
				return
			}
			if err != nil {
				t.Fatalf("ParseVMAFScore: %v", err)
			}
			if got != tc.want {
				t.Errorf("score = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestBitrateKbps pins the size-to-bitrate conversion.
func TestBitrateKbps(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name     string
		bytes    int64
		duration float64
		want     float64
	}{
		{name: "1 MB over 8 s", bytes: 1_000_000, duration: 8, want: 1000},
		{name: "5-second probe", bytes: 625_000, duration: 5, want: 1000},
		{name: "zero size yields zero", bytes: 0, duration: 5, want: 0},
		{name: "negative size yields zero", bytes: -1, duration: 5, want: 0},
		{name: "zero duration yields zero", bytes: 1000, duration: 0, want: 0},
		{name: "negative duration yields zero", bytes: 1000, duration: -1, want: 0},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := BitrateKbps(tc.bytes, tc.duration); math.Abs(got-tc.want) > 1e-9 {
				t.Errorf("BitrateKbps(%d, %v) = %v, want %v", tc.bytes, tc.duration, got, tc.want)
			}
		})
	}
}

// TestRawFrameBytes pins the raw-frame geometry used to derive a raw-YUV
// source's playing time.
func TestRawFrameBytes(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		pixFmt string
		w, h   int
		want   int64
	}{
		{name: "1080p 8-bit 4:2:0", pixFmt: "yuv420p", w: 1920, h: 1080, want: 1920 * 1080 * 3 / 2},
		{name: "1080p 10-bit", pixFmt: "yuv420p10le", w: 1920, h: 1080, want: 1920 * 1080 * 3},
		{name: "1080p 12-bit", pixFmt: "yuv420p12le", w: 1920, h: 1080, want: 1920 * 1080 * 3},
		{name: "odd dimensions floor the chroma planes", pixFmt: "yuv420p", w: 3, h: 3, want: 9 + 2},
		{name: "zero width yields zero", pixFmt: "yuv420p", w: 0, h: 1080, want: 0},
		{name: "negative height yields zero", pixFmt: "yuv420p", w: 1920, h: -1, want: 0},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := RawFrameBytes(tc.pixFmt, tc.w, tc.h); got != tc.want {
				t.Errorf("RawFrameBytes(%q, %d, %d) = %d, want %d",
					tc.pixFmt, tc.w, tc.h, got, tc.want)
			}
		})
	}
}

// TestRawClipDurationSeconds checks the size-derived clip duration. It is the
// correction cli.py carries for the fast verify pass: deriving the denominator
// from wall-clock encode time makes the reported bitrate machine-dependent.
func TestRawClipDurationSeconds(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	// 10 frames of 16x16 yuv420p = 10 * 384 bytes.
	frame := RawFrameBytes("yuv420p", 16, 16)
	src := filepath.Join(dir, "ref.yuv")
	if err := os.WriteFile(src, make([]byte, frame*10), 0o600); err != nil {
		t.Fatalf("write fixture: %v", err)
	}

	tests := []struct {
		name      string
		src       string
		w, h      int
		framerate float64
		want      float64
	}{
		{name: "10 frames at 10 fps is 1 s", src: src, w: 16, h: 16, framerate: 10, want: 1.0},
		{name: "10 frames at 25 fps is 0.4 s", src: src, w: 16, h: 16, framerate: 25, want: 0.4},
		{name: "missing file yields zero", src: filepath.Join(dir, "nope.yuv"), w: 16, h: 16, framerate: 25},
		{name: "unknown geometry yields zero", src: src, w: 0, h: 0, framerate: 25},
		{name: "zero framerate yields zero", src: src, w: 16, h: 16, framerate: 0},
		{name: "frame larger than the file yields zero", src: src, w: 1920, h: 1080, framerate: 25},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := RawClipDurationSeconds(tc.src, "yuv420p", tc.w, tc.h, tc.framerate)
			if math.Abs(got-tc.want) > 1e-9 {
				t.Errorf("RawClipDurationSeconds = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestCRFNorm pins the [0, 1] CRF axis the proxy's codec block carries.
func TestCRFNorm(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		cfg  PipelineConfig
		crf  int
		want float64
	}{
		{name: "range floor", cfg: PipelineConfig{CRFLo: 10, CRFHi: 51}, crf: 10, want: 0},
		{name: "range ceiling", cfg: PipelineConfig{CRFLo: 10, CRFHi: 51}, crf: 51, want: 1},
		{name: "midpoint", cfg: PipelineConfig{CRFLo: 0, CRFHi: 50}, crf: 25, want: 0.5},
		{name: "degenerate range does not divide by zero", cfg: PipelineConfig{CRFLo: 23, CRFHi: 23}, crf: 23, want: 0},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := tc.cfg.crfNorm(tc.crf); math.Abs(got-tc.want) > 1e-12 {
				t.Errorf("crfNorm(%d) = %v, want %v", tc.crf, got, tc.want)
			}
		})
	}
}

// TestPipelineConfigDefaults covers the zero-value fallbacks.
func TestPipelineConfigDefaults(t *testing.T) {
	t.Parallel()

	var cfg PipelineConfig
	if got := cfg.pixFmtOrDefault(); got != "yuv420p" {
		t.Errorf("default pix_fmt = %q, want yuv420p", got)
	}
	if got := cfg.vmafModelOrDefault(); got != model.DefaultVersion {
		t.Errorf("default model = %q, want %q", got, model.DefaultVersion)
	}
	if got := cfg.presetArgs(); got != nil {
		t.Errorf("empty preset must emit no args, got %v", got)
	}
	withPreset := PipelineConfig{Preset: "slow"}
	if got := strings.Join(withPreset.presetArgs(), " "); got != "-preset slow" {
		t.Errorf("preset args = %q, want \"-preset slow\"", got)
	}
}

// TestFormatFFmpegFloat checks the argv float rendering matches Python's
// f-string formatting for the values that reach an ffmpeg command line.
func TestFormatFFmpegFloat(t *testing.T) {
	t.Parallel()

	tests := []struct {
		in   float64
		want string
	}{
		{in: 24, want: "24"},
		{in: 29.97, want: "29.97"},
		{in: 5, want: "5"},
		{in: 2.5, want: "2.5"},
		{in: 0, want: "0"},
		{in: 23.976023976023978, want: "23.976023976023978"},
	}
	for _, tc := range tests {
		if got := formatFFmpegFloat(tc.in); got != tc.want {
			t.Errorf("formatFFmpegFloat(%v) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

// TestNewSamplePredictorValidation covers the constructor's contract.
func TestNewSamplePredictorValidation(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		cfg     PipelineConfig
		wantErr string
	}{
		{
			name:    "missing proxy rejected",
			cfg:     PipelineConfig{Encoder: "libx264"},
			wantErr: "requires a proxy",
		},
		{
			name: "unknown encoder rejected",
			cfg: PipelineConfig{
				Encoder: "libnope",
				Proxy: ProxyFunc(func(context.Context, []float64, string, float64, float64) (float64, error) {
					return 0, nil
				}),
			},
			wantErr: "unknown encoder",
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			_, err := NewSamplePredictor(context.Background(), tc.cfg)
			if err == nil {
				t.Fatalf("want error containing %q, got nil", tc.wantErr)
			}
			if !containsStr(err.Error(), tc.wantErr) {
				t.Errorf("error %q does not contain %q", err, tc.wantErr)
			}
		})
	}
}

// TestNewVerifierValidation covers the verify-pass constructor.
func TestNewVerifierValidation(t *testing.T) {
	t.Parallel()

	if _, err := NewVerifier(PipelineConfig{Encoder: "libnope"}); err == nil {
		t.Fatal("want an error for an unknown encoder, got nil")
	}
	if _, err := NewVerifier(PipelineConfig{Encoder: "libx264"}); err != nil {
		t.Fatalf("libx264 verifier: %v", err)
	}
}
