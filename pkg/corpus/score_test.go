// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/score_test.go — libvmaf CLI argv and JSON-parsing tests.
//
// The expected argv slices were produced by calling
// vmaftune.score.build_vmaf_command on the same requests; the parsed values by
// calling parse_vmaf_json / parse_feature_aggregates on the same payloads.
//
// Cases whose request uses Model1080P derive the expected --model argument from
// that same constant rather than spelling the version literally. These tests
// exist to pin the argv PLUMBING, not the identity of the default model; the
// default is asserted once, deliberately, in python/test/default_model_test.py
// (ADR-1169). Hardcoding it here just made three tests fail when the default
// moved, without telling anyone anything they did not already know.

package corpus

import (
	"context"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

func TestBuildVMAFCommand(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		req     ScoreRequest
		vmafBin string
		backend string
		want    []string
	}{
		{
			name: "8-bit 4:2:0 with the default model",
			req: ScoreRequest{
				Reference: "/refs/clip.yuv", Distorted: "/enc/out.yuv",
				Width: 1920, Height: 1080, PixFmt: "yuv420p", Model: Model1080P,
			},
			want: []string{
				"vmaf", "--reference", "/refs/clip.yuv", "--distorted", "/enc/out.yuv",
				"--width", "1920", "--height", "1080",
				"--pixel_format", "420", "--bitdepth", "8",
				"--model", "version=" + Model1080P,
				"--json", "--output", "/tmp/vmaf.json",
				"--feature", "vif",
			},
		},
		{
			name: "an explicit backend appends the selector",
			req: ScoreRequest{
				Reference: "/refs/clip.yuv", Distorted: "/enc/out.yuv",
				Width: 1920, Height: 1080, PixFmt: "yuv420p", Model: Model1080P,
			},
			backend: "cuda",
			want: []string{
				"vmaf", "--reference", "/refs/clip.yuv", "--distorted", "/enc/out.yuv",
				"--width", "1920", "--height", "1080",
				"--pixel_format", "420", "--bitdepth", "8",
				"--model", "version=" + Model1080P,
				"--json", "--output", "/tmp/vmaf.json",
				"--feature", "vif",
				"--backend", "cuda",
			},
		},
		{
			name: "10-bit, a path= model override and a sample-clip window",
			req: ScoreRequest{
				Reference: "/refs/clip.yuv", Distorted: "/enc/out.yuv",
				Width: 1920, Height: 1080, PixFmt: "yuv420p10le",
				Model: "path=/m/hdr.json", FrameSkipRef: 12, FrameCnt: 240,
			},
			vmafBin: "/opt/vmaf",
			want: []string{
				"/opt/vmaf", "--reference", "/refs/clip.yuv", "--distorted", "/enc/out.yuv",
				"--width", "1920", "--height", "1080",
				"--pixel_format", "420", "--bitdepth", "10",
				"--model", "path=/m/hdr.json",
				"--json", "--output", "/tmp/vmaf.json",
				"--feature", "vif",
				"--frame_skip_ref", "12", "--frame_cnt", "240",
			},
		},
		{
			name: "4:4:4 12-bit resolves both the pixel format and the bit depth",
			req: ScoreRequest{
				Reference: "a", Distorted: "b",
				Width: 8, Height: 8, PixFmt: "yuv444p12le", Model: Model1080P,
			},
			want: []string{
				"vmaf", "--reference", "a", "--distorted", "b",
				"--width", "8", "--height", "8",
				"--pixel_format", "444", "--bitdepth", "12",
				"--model", "version=" + Model1080P,
				"--json", "--output", "/tmp/vmaf.json",
				"--feature", "vif",
			},
		},
		{
			name: "v0.6.1 model already has vif so --feature vif is omitted",
			req: ScoreRequest{
				Reference: "/refs/clip.yuv", Distorted: "/enc/out.yuv",
				Width: 1920, Height: 1080, PixFmt: "yuv420p", Model: "vmaf_v0.6.1",
			},
			want: []string{
				"vmaf", "--reference", "/refs/clip.yuv", "--distorted", "/enc/out.yuv",
				"--width", "1920", "--height", "1080",
				"--pixel_format", "420", "--bitdepth", "8",
				"--model", "version=vmaf_v0.6.1",
				"--json", "--output", "/tmp/vmaf.json",
			},
		},
		{
			name: "pre-formatted version=vmaf_v0.6.1 omits --feature vif",
			req: ScoreRequest{
				Reference: "/refs/clip.yuv", Distorted: "/enc/out.yuv",
				Width: 1920, Height: 1080, PixFmt: "yuv420p", Model: "version=vmaf_v0.6.1",
			},
			want: []string{
				"vmaf", "--reference", "/refs/clip.yuv", "--distorted", "/enc/out.yuv",
				"--width", "1920", "--height", "1080",
				"--pixel_format", "420", "--bitdepth", "8",
				"--model", "version=vmaf_v0.6.1",
				"--json", "--output", "/tmp/vmaf.json",
			},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := BuildVMAFCommand(tc.req, "/tmp/vmaf.json", tc.vmafBin, tc.backend)
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("BuildVMAFCommand() =\n  %v\nwant\n  %v", got, tc.want)
			}
		})
	}
}

func TestPixFmtMapping(t *testing.T) {
	t.Parallel()

	tests := []struct {
		pixFmt       string
		wantFormat   string
		wantBitdepth int
	}{
		{pixFmt: "yuv420p", wantFormat: "420", wantBitdepth: 8},
		{pixFmt: "yuv422p", wantFormat: "422", wantBitdepth: 8},
		{pixFmt: "yuv444p", wantFormat: "444", wantBitdepth: 8},
		{pixFmt: "yuv420p10le", wantFormat: "420", wantBitdepth: 10},
		{pixFmt: "yuv422p10le", wantFormat: "422", wantBitdepth: 10},
		{pixFmt: "yuv444p12le", wantFormat: "444", wantBitdepth: 12},
		{pixFmt: "p010le", wantFormat: "420", wantBitdepth: 10},
		{pixFmt: "gbrp", wantFormat: "420", wantBitdepth: 8},
	}
	for _, tc := range tests {
		if got := pixFmtToVMAF(tc.pixFmt); got != tc.wantFormat {
			t.Errorf("pixFmtToVMAF(%q) = %q, want %q", tc.pixFmt, got, tc.wantFormat)
		}
		if got := bitdepthFor(tc.pixFmt); got != tc.wantBitdepth {
			t.Errorf("bitdepthFor(%q) = %d, want %d", tc.pixFmt, got, tc.wantBitdepth)
		}
	}
}

func TestModelArg(t *testing.T) {
	t.Parallel()

	tests := []struct{ in, want string }{
		{in: "vmaf_v0.6.1", want: "version=vmaf_v0.6.1"},
		{in: "vmaf_4k_v0.6.1neg", want: "version=vmaf_4k_v0.6.1neg"},
		{in: "path=/m/hdr.json", want: "path=/m/hdr.json"},
		{in: "version=vmaf_v0.6.1", want: "version=vmaf_v0.6.1"},
	}
	for _, tc := range tests {
		if got := modelArg(tc.in); got != tc.want {
			t.Errorf("modelArg(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

func TestParseVMAFJSON(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		payload string
		want    float64
		wantOK  bool
	}{
		{
			name:    "modern pooled_metrics shape",
			payload: `{"pooled_metrics": {"vmaf": {"mean": 93.5}}}`,
			want:    93.5, wantOK: true,
		},
		{
			name:    "legacy top-level score",
			payload: `{"VMAF score": 88.25}`,
			want:    88.25, wantOK: true,
		},
		{
			name:    "pooled_metrics without a mean falls through to the legacy key",
			payload: `{"pooled_metrics": {"vmaf": {"min": 1}}, "VMAF score": 70.0}`,
			want:    70.0, wantOK: true,
		},
		{name: "neither shape present", payload: `{"frames": []}`, wantOK: false},
		{name: "empty object", payload: `{}`, wantOK: false},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			var payload map[string]any
			if err := json.Unmarshal([]byte(tc.payload), &payload); err != nil {
				t.Fatalf("fixture is not valid JSON: %v", err)
			}
			got, ok := ParseVMAFJSON(payload)
			if ok != tc.wantOK {
				t.Fatalf("ParseVMAFJSON ok = %v, want %v", ok, tc.wantOK)
			}
			if !ok {
				if !math.IsNaN(got) {
					t.Errorf("ParseVMAFJSON returned %v on failure, want NaN", got)
				}
				return
			}
			if got != tc.want {
				t.Errorf("ParseVMAFJSON = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestParseFeatureAggregates(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		payload   string
		wantMeans map[string]float64
		wantStds  map[string]float64
	}{
		{
			name: "integer_-prefixed keys resolve to the canonical bare names",
			payload: `{"pooled_metrics": {
				"vmaf": {"mean": 93.5},
				"integer_adm2": {"mean": 0.98, "stddev": 0.01},
				"cambi": {"mean": 0.5}
			}}`,
			wantMeans: map[string]float64{"adm2": 0.98},
			wantStds:  map[string]float64{"adm2": 0.01},
		},
		{
			name: "bare keys still resolve for synthetic fixtures",
			payload: `{"pooled_metrics": {
				"motion2": {"mean": 3.25},
				"vif_scale0": {"mean": 0.5, "stddev": 0.05}
			}}`,
			wantMeans: map[string]float64{"motion2": 3.25, "vif_scale0": 0.5},
			wantStds:  map[string]float64{"vif_scale0": 0.05},
		},
		{
			name: "a real integer block carries harmonic_mean, not stddev",
			payload: `{"pooled_metrics": {
				"integer_motion2": {"min": 1, "max": 5, "mean": 3.3, "harmonic_mean": 3.1}
			}}`,
			wantMeans: map[string]float64{"motion2": 3.3},
			wantStds:  map[string]float64{},
		},
		{
			name:      "no pooled_metrics at all",
			payload:   `{"VMAF score": 88.0}`,
			wantMeans: map[string]float64{},
			wantStds:  map[string]float64{},
		},
		{
			name: "options-suffixed keys resolve via prefix matching",
			payload: `{"pooled_metrics": {
				"integer_adm2_csf_2_dlmw_0.7_egl_1_min_0.5_nw_0.02": {"mean": 0.961},
				"integer_vif_scale0": {"mean": 0.505},
				"integer_vif_scale1": {"mean": 0.879},
				"integer_vif_scale2": {"mean": 0.938},
				"integer_vif_scale3": {"mean": 0.964},
				"integer_motion2_mmxv_18": {"mean": 1.25}
			}}`,
			wantMeans: map[string]float64{
				"adm2":       0.961,
				"vif_scale0": 0.505,
				"vif_scale1": 0.879,
				"vif_scale2": 0.938,
				"vif_scale3": 0.964,
				"motion2":    1.25,
			},
			wantStds: map[string]float64{},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			var payload map[string]any
			if err := json.Unmarshal([]byte(tc.payload), &payload); err != nil {
				t.Fatalf("fixture is not valid JSON: %v", err)
			}
			means, stds := ParseFeatureAggregates(payload, Canonical6Features)
			if !reflect.DeepEqual(means, tc.wantMeans) {
				t.Errorf("means = %v, want %v", means, tc.wantMeans)
			}
			if !reflect.DeepEqual(stds, tc.wantStds) {
				t.Errorf("stds = %v, want %v", stds, tc.wantStds)
			}
		})
	}
}

func TestIsRawYUVPath(t *testing.T) {
	t.Parallel()

	tests := []struct {
		path string
		want bool
	}{
		{path: "/refs/clip.yuv", want: true},
		{path: "/refs/clip.YUV", want: true},
		{path: "/refs/clip", want: true},
		// ADR-0499: the vmaf CLI cannot read Y4M once --width / --height
		// pin the geometry, so it must be decoded like any container.
		{path: "/refs/clip.y4m", want: false},
		{path: "/enc/out.mp4", want: false},
		{path: "/enc/out.mkv", want: false},
	}
	for _, tc := range tests {
		if got := IsRawYUVPath(tc.path); got != tc.want {
			t.Errorf("IsRawYUVPath(%q) = %v, want %v", tc.path, got, tc.want)
		}
	}
}

func TestMaybeDecodeDistorted(t *testing.T) {
	t.Parallel()

	t.Run("raw input is a no-op", func(t *testing.T) {
		t.Parallel()
		req := ScoreRequest{Distorted: "/enc/out.yuv", PixFmt: "yuv420p"}
		called := false
		stub := func(context.Context, []string) RunResult {
			called = true
			return RunResult{}
		}
		got, rc := MaybeDecodeDistorted(context.Background(), req, t.TempDir(), "ffmpeg", stub)
		if rc != 0 || got.Distorted != req.Distorted {
			t.Errorf("MaybeDecodeDistorted(raw) = (%q, %d), want the input unchanged",
				got.Distorted, rc)
		}
		if called {
			t.Error("MaybeDecodeDistorted spawned ffmpeg for a raw input")
		}
	})

	t.Run("a container is decoded to a sidecar", func(t *testing.T) {
		t.Parallel()
		dir := t.TempDir()
		req := ScoreRequest{Distorted: "/enc/out.mp4", PixFmt: "yuv420p", DurationS: 5}
		var argv []string
		stub := func(_ context.Context, cmd []string) RunResult {
			argv = cmd
			// The driver checks the file exists before accepting the decode.
			if err := os.WriteFile(cmd[len(cmd)-1], []byte("yuv"), 0o600); err != nil {
				return RunResult{ReturnCode: 1}
			}
			return RunResult{}
		}
		got, rc := MaybeDecodeDistorted(context.Background(), req, dir, "ffmpeg", stub)
		if rc != 0 {
			t.Fatalf("rc = %d, want 0", rc)
		}
		want := filepath.Join(dir, "out.decoded.yuv")
		if got.Distorted != want {
			t.Errorf("decoded path = %q, want %q", got.Distorted, want)
		}
		wantArgv := []string{
			"ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
			"-i", "/enc/out.mp4",
			"-f", "rawvideo", "-pix_fmt", "yuv420p",
			"-t", "5.0",
			want,
		}
		if !reflect.DeepEqual(argv, wantArgv) {
			t.Errorf("decode argv =\n  %v\nwant\n  %v", argv, wantArgv)
		}
	})

	t.Run("a failed decode returns the original request", func(t *testing.T) {
		t.Parallel()
		req := ScoreRequest{Distorted: "/enc/out.mp4", PixFmt: "yuv420p"}
		stub := func(context.Context, []string) RunResult {
			return RunResult{ReturnCode: 1, Stderr: "decode failed"}
		}
		got, rc := MaybeDecodeDistorted(context.Background(), req, t.TempDir(), "ffmpeg", stub)
		if rc == 0 {
			t.Error("a failed decode reported rc = 0")
		}
		if got.Distorted != req.Distorted {
			t.Errorf("a failed decode rewrote the request to %q", got.Distorted)
		}
	})
}

func TestRunScore(t *testing.T) {
	t.Parallel()

	const payload = `{"pooled_metrics": {
		"vmaf": {"mean": 93.5},
		"integer_adm2": {"mean": 0.98},
		"integer_motion2": {"mean": 3.25}
	}}`

	t.Run("a successful run parses the score and the aggregates", func(t *testing.T) {
		t.Parallel()
		req := ScoreRequest{
			Reference: "/refs/clip.yuv", Distorted: "/enc/out.yuv",
			Width: 320, Height: 240, PixFmt: "yuv420p", Model: Model1080P,
		}
		stub := func(_ context.Context, argv []string) RunResult {
			// The last "--output" value is the JSON sidecar the driver reads.
			for i, a := range argv {
				if a == "--output" && i+1 < len(argv) {
					if err := os.WriteFile(argv[i+1], []byte(payload), 0o600); err != nil {
						return RunResult{ReturnCode: 1}
					}
				}
			}
			return RunResult{Stderr: "VMAF version 3.0.0\n"}
		}
		got := RunScore(context.Background(), req, "vmaf", stub, t.TempDir(), "")
		if got.ExitStatus != 0 {
			t.Fatalf("ExitStatus = %d, want 0", got.ExitStatus)
		}
		if got.VMAFScore != 93.5 {
			t.Errorf("VMAFScore = %v, want 93.5", got.VMAFScore)
		}
		if got.VMAFBinaryVersion != "3.0.0" {
			t.Errorf("VMAFBinaryVersion = %q, want 3.0.0", got.VMAFBinaryVersion)
		}
		if got.FeatureMeans["adm2"] != 0.98 || got.FeatureMeans["motion2"] != 3.25 {
			t.Errorf("FeatureMeans = %v, want adm2=0.98 motion2=3.25", got.FeatureMeans)
		}
	})

	t.Run("corrupt JSON on a zero exit is reported as a scoring error", func(t *testing.T) {
		t.Parallel()
		req := ScoreRequest{Reference: "a", Distorted: "b", Width: 8, Height: 8,
			PixFmt: "yuv420p", Model: Model1080P}
		stub := func(_ context.Context, argv []string) RunResult {
			for i, a := range argv {
				if a == "--output" && i+1 < len(argv) {
					if err := os.WriteFile(argv[i+1], []byte("{trunc"), 0o600); err != nil {
						return RunResult{ReturnCode: 1}
					}
				}
			}
			return RunResult{}
		}
		got := RunScore(context.Background(), req, "vmaf", stub, t.TempDir(), "")
		if got.ExitStatus != 65 {
			t.Errorf("ExitStatus = %d, want 65 for corrupt JSON", got.ExitStatus)
		}
		if !math.IsNaN(got.VMAFScore) {
			t.Errorf("VMAFScore = %v, want NaN", got.VMAFScore)
		}
	})

	t.Run("a non-zero vmaf exit is recorded verbatim", func(t *testing.T) {
		t.Parallel()
		req := ScoreRequest{Reference: "a", Distorted: "b", Width: 8, Height: 8,
			PixFmt: "yuv420p", Model: Model1080P}
		stub := func(context.Context, []string) RunResult {
			return RunResult{ReturnCode: 2, Stderr: "no such file\n"}
		}
		got := RunScore(context.Background(), req, "vmaf", stub, t.TempDir(), "")
		if got.ExitStatus != 2 {
			t.Errorf("ExitStatus = %d, want 2", got.ExitStatus)
		}
		if !math.IsNaN(got.VMAFScore) {
			t.Errorf("VMAFScore = %v, want NaN", got.VMAFScore)
		}
		if got.VMAFBinaryVersion != "unknown" {
			t.Errorf("VMAFBinaryVersion = %q, want unknown", got.VMAFBinaryVersion)
		}
	})
}
