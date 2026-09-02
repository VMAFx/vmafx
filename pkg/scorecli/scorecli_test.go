// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package scorecli_test

import (
	"context"
	"math"
	"os"
	"slices"
	"testing"

	"github.com/VMAFx/vmafx/pkg/scorecli"
)

// TestBuildCommand pins the libvmaf CLI argv against a dump of the Python
// vmaftune.score.build_vmaf_command for the same requests.
func TestBuildCommand(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		req     scorecli.Request
		backend string
		want    []string
	}{
		{
			name: "8-bit 420 with an explicit backend",
			req: scorecli.Request{
				Reference: "/r.yuv", Distorted: "/d.yuv",
				Width: 1920, Height: 1080, PixFmt: "yuv420p",
			},
			backend: "cuda",
			want: []string{
				"vmaf", "--reference", "/r.yuv", "--distorted", "/d.yuv",
				"--width", "1920", "--height", "1080",
				"--pixel_format", "420", "--bitdepth", "8",
				"--model", "version=vmaf_v0.6.1",
				"--json", "--output", "/tmp/o.json",
				"--backend", "cuda",
			},
		},
		{
			name: "10-bit 444 with a pre-formatted model path",
			req: scorecli.Request{
				Reference: "/r.yuv", Distorted: "/d.yuv",
				Width: 3840, Height: 2160, PixFmt: "yuv444p10le",
				Model: "path=/m.json",
			},
			want: []string{
				"vmaf", "--reference", "/r.yuv", "--distorted", "/d.yuv",
				"--width", "3840", "--height", "2160",
				"--pixel_format", "444", "--bitdepth", "10",
				"--model", "path=/m.json",
				"--json", "--output", "/tmp/o.json",
			},
		},
		{
			name: "sample-clip window aligns the reference",
			req: scorecli.Request{
				Reference: "/r.yuv", Distorted: "/d.yuv",
				Width: 1280, Height: 720, PixFmt: "yuv422p",
				FrameSkipRef: 48, FrameCnt: 240,
			},
			want: []string{
				"vmaf", "--reference", "/r.yuv", "--distorted", "/d.yuv",
				"--width", "1280", "--height", "720",
				"--pixel_format", "422", "--bitdepth", "8",
				"--model", "version=vmaf_v0.6.1",
				"--json", "--output", "/tmp/o.json",
				"--frame_skip_ref", "48", "--frame_cnt", "240",
			},
		},
		{
			name: "12-bit source",
			req: scorecli.Request{
				Reference: "/r.yuv", Distorted: "/d.yuv",
				Width: 640, Height: 480, PixFmt: "yuv420p12le",
			},
			want: []string{
				"vmaf", "--reference", "/r.yuv", "--distorted", "/d.yuv",
				"--width", "640", "--height", "480",
				"--pixel_format", "420", "--bitdepth", "12",
				"--model", "version=vmaf_v0.6.1",
				"--json", "--output", "/tmp/o.json",
			},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := scorecli.BuildCommand(tc.req, "/tmp/o.json", "vmaf", tc.backend)
			if !slices.Equal(got, tc.want) {
				t.Errorf("argv mismatch\n got: %v\nwant: %v", got, tc.want)
			}
		})
	}
}

// TestParseJSON covers both payload shapes and the missing-score error.
func TestParseJSON(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		payload string
		want    float64
		wantErr bool
	}{
		{
			name:    "modern pooled_metrics shape",
			payload: `{"pooled_metrics":{"vmaf":{"min":80.0,"mean":93.25,"max":99.0}}}`,
			want:    93.25,
		},
		{
			name:    "legacy top-level score",
			payload: `{"VMAF score": 88.5}`,
			want:    88.5,
		},
		{
			name:    "pooled_metrics without a vmaf block",
			payload: `{"pooled_metrics":{"integer_adm2":{"mean":0.97}}}`,
			wantErr: true,
		},
		{
			name:    "not JSON at all",
			payload: `{truncated`,
			wantErr: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := scorecli.ParseJSON([]byte(tc.payload))
			if (err != nil) != tc.wantErr {
				t.Fatalf("ParseJSON error = %v, wantErr %v", err, tc.wantErr)
			}
			if !tc.wantErr && got != tc.want {
				t.Errorf("ParseJSON = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestParseFeatureAggregates covers the integer_ prefix resolution, the bare
// fallback, and the absent-feature contract (missing, not zero).
func TestParseFeatureAggregates(t *testing.T) {
	t.Parallel()

	payload := []byte(`{"pooled_metrics":{
		"integer_adm2":{"mean":0.97,"stddev":0.01},
		"vif_scale0":{"mean":0.42},
		"integer_motion2":{"mean":3.5,"harmonic_mean":3.4}
	}}`)

	means, stds := scorecli.ParseFeatureAggregates(payload, scorecli.Canonical6Features)

	if got, ok := means["adm2"]; !ok || got != 0.97 {
		t.Errorf("adm2 mean = %v (present %v), want 0.97", got, ok)
	}
	if got, ok := stds["adm2"]; !ok || got != 0.01 {
		t.Errorf("adm2 stddev = %v (present %v), want 0.01", got, ok)
	}
	// Bare-name fallback for a payload that skips the integer_ prefix.
	if got, ok := means["vif_scale0"]; !ok || got != 0.42 {
		t.Errorf("vif_scale0 mean = %v (present %v), want 0.42", got, ok)
	}
	// stddev is absent from real integer-pipeline blocks — must not appear.
	if _, ok := stds["motion2"]; ok {
		t.Error("motion2 stddev should be absent, not zero")
	}
	// A feature the run never emitted must be absent, not NaN or zero.
	if _, ok := means["vif_scale3"]; ok {
		t.Error("vif_scale3 should be absent from the means map")
	}
}

// TestNeedsDecode pins the ADR-0499 raw-suffix contract, including the
// deliberate exclusion of .y4m.
func TestNeedsDecode(t *testing.T) {
	t.Parallel()

	tests := []struct {
		path string
		want bool
	}{
		{"/x/ref.yuv", false},
		{"/x/ref.YUV", false},
		{"/x/ref", false},
		{"/x/enc.mp4", true},
		{"/x/enc.mkv", true},
		// .y4m must be decoded: vmaf-tune always pins geometry, which routes
		// the input through raw_input_open and trips its size guard.
		{"/x/ref.y4m", true},
	}
	for _, tc := range tests {
		t.Run(tc.path, func(t *testing.T) {
			t.Parallel()

			if got := scorecli.NeedsDecode(tc.path); got != tc.want {
				t.Errorf("NeedsDecode(%q) = %v, want %v", tc.path, got, tc.want)
			}
		})
	}
}

// TestDecodeCommand covers the duration clamp that keeps a short probe from
// materialising the whole source as raw YUV.
func TestDecodeCommand(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		durationS float64
		want      []string
	}{
		{
			name: "unbounded decode", durationS: 0,
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
				"-i", "/e.mp4", "-f", "rawvideo", "-pix_fmt", "yuv420p", "/d.yuv",
			},
		},
		{
			name: "clamped decode", durationS: 10,
			want: []string{
				"ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
				"-i", "/e.mp4", "-f", "rawvideo", "-pix_fmt", "yuv420p",
				"-t", "10", "/d.yuv",
			},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := scorecli.DecodeCommand("/e.mp4", "/d.yuv", "yuv420p", "ffmpeg", tc.durationS)
			if !slices.Equal(got, tc.want) {
				t.Errorf("argv mismatch\n got: %v\nwant: %v", got, tc.want)
			}
		})
	}
}

// TestRun_injectedRunner exercises the three outcomes the search loops branch
// on: a clean score, a vmaf failure, and vmaf exiting 0 with unreadable JSON
// (which must be reported as exit 65 with a NaN score, never a crash).
func TestRun_injectedRunner(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name           string
		exitStatus     int
		jsonPayload    string
		writeJSON      bool
		wantExitStatus int
		wantScore      float64
		wantNaN        bool
	}{
		{
			name: "clean score", exitStatus: 0, writeJSON: true,
			jsonPayload:    `{"pooled_metrics":{"vmaf":{"mean":93.25}}}`,
			wantExitStatus: 0, wantScore: 93.25,
		},
		{
			name: "vmaf failed", exitStatus: 1, writeJSON: false,
			wantExitStatus: 1, wantNaN: true,
		},
		{
			name: "exit 0 but no JSON written", exitStatus: 0, writeJSON: false,
			wantExitStatus: 65, wantNaN: true,
		},
		{
			name: "exit 0 but truncated JSON", exitStatus: 0, writeJSON: true,
			jsonPayload:    `{"pooled_met`,
			wantExitStatus: 65, wantNaN: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			req := scorecli.Request{
				Reference: "/r.yuv", Distorted: "/d.yuv",
				Width: 320, Height: 240, PixFmt: "yuv420p",
			}
			runner := func(_ context.Context, argv []string) (string, int, error) {
				if tc.writeJSON {
					// argv ends with the --output path (plus optional flags);
					// find it so the stub writes where Run will read.
					for i := range argv {
						if argv[i] == "--output" && i+1 < len(argv) {
							if err := os.WriteFile(
								argv[i+1], []byte(tc.jsonPayload), 0o600); err != nil {
								t.Errorf("stub write JSON: %v", err)
							}
						}
					}
				}
				return "VMAF version 3.0.0\n", tc.exitStatus, nil
			}

			res, err := scorecli.Run(context.Background(), req, "vmaf", "", runner)
			if err != nil {
				t.Fatalf("Run: %v", err)
			}
			if res.ExitStatus != tc.wantExitStatus {
				t.Errorf("ExitStatus = %d, want %d", res.ExitStatus, tc.wantExitStatus)
			}
			if tc.wantNaN {
				if !math.IsNaN(res.VMAFScore) {
					t.Errorf("VMAFScore = %v, want NaN", res.VMAFScore)
				}
			} else if res.VMAFScore != tc.wantScore {
				t.Errorf("VMAFScore = %v, want %v", res.VMAFScore, tc.wantScore)
			}
			if res.VMAFBinaryVersion != "3.0.0" {
				t.Errorf("VMAFBinaryVersion = %q, want 3.0.0", res.VMAFBinaryVersion)
			}
		})
	}
}
