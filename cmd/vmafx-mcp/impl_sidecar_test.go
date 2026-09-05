// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

package main

import (
	"context"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// fixturePath returns a real, allowlisted file usable as a path argument.
func fixturePath(t *testing.T) string {
	t.Helper()
	return filepath.Join(findRepoRoot(t), "model", "vmaf_v0.6.1.json")
}

// TestSidecarBuildersRejectOutOfRangeArgs pins every bound the C parsers in
// core/tools/ enforce. A caller that passes a bad value must get a clean MCP
// error, not a usage() dump from the sidecar.
func TestSidecarBuildersRejectOutOfRangeArgs(t *testing.T) {
	ref := fixturePath(t)

	perShot := []struct {
		name string
		args map[string]any
		want string
	}{
		{"width_too_small", map[string]any{"reference": ref, "width": 8, "height": 324}, "between 16 and 65535"},
		{"height_too_large", map[string]any{"reference": ref, "width": 576, "height": 70000}, "between 16 and 65535"},
		{"bad_pixfmt", map[string]any{"reference": ref, "width": 576, "height": 324, "pixel_format": "410"}, "invalid pixel_format"},
		{"bad_bitdepth", map[string]any{"reference": ref, "width": 576, "height": 324, "bitdepth": 14}, "invalid bitdepth"},
		{"target_out_of_range", map[string]any{"reference": ref, "width": 576, "height": 324, "target_vmaf": 101}, "between 0 and 100"},
		{"crf_out_of_range", map[string]any{"reference": ref, "width": 576, "height": 324, "crf_max": 64}, "between 0 and 63"},
		{"crf_inverted", map[string]any{"reference": ref, "width": 576, "height": 324, "crf_min": 40, "crf_max": 20}, "must not exceed"},
		{"diff_threshold_out_of_range", map[string]any{"reference": ref, "width": 576, "height": 324, "diff_threshold": 300}, "between 0 and 255"},
		{"bad_format", map[string]any{"reference": ref, "width": 576, "height": 324, "format": "yaml"}, "must be json or csv"},
		{"unallowlisted_reference", map[string]any{"reference": "/etc/passwd", "width": 576, "height": 324}, "reference:"},
	}
	for _, tc := range perShot {
		t.Run("per_shot/"+tc.name, func(t *testing.T) {
			_, _, err := buildPerShotArgv("BIN", tc.args)
			assertErrContains(t, err, tc.want)
		})
	}

	roi := []struct {
		name string
		args map[string]any
		want string
	}{
		{"width_zero", map[string]any{"reference": ref, "width": 0, "height": 1080, "frame": 0}, "between 1 and 16384"},
		{"frame_missing", map[string]any{"reference": ref, "width": 1920, "height": 1080}, "between 0 and 1000000"},
		{"frame_too_large", map[string]any{"reference": ref, "width": 1920, "height": 1080, "frame": 1000001}, "between 0 and 1000000"},
		{"ctu_too_small", map[string]any{"reference": ref, "width": 1920, "height": 1080, "frame": 0, "ctu_size": 4}, "between 8 and 128"},
		{"bad_encoder", map[string]any{"reference": ref, "width": 1920, "height": 1080, "frame": 0, "encoder": "vp9"}, "must be x265 or svt-av1"},
		{"strength_out_of_range", map[string]any{"reference": ref, "width": 1920, "height": 1080, "frame": 0, "strength": 65}, "between 0 and 64"},
		{"unallowlisted_saliency", map[string]any{"reference": ref, "width": 1920, "height": 1080, "frame": 0, "saliency_model": "/etc/passwd"}, "saliency_model:"},
	}
	for _, tc := range roi {
		t.Run("roi/"+tc.name, func(t *testing.T) {
			_, _, err := buildRoiArgv("BIN", "/tmp/out.bin", tc.args)
			assertErrContains(t, err, tc.want)
		})
	}

	bench := []struct {
		name string
		args map[string]any
		want string
	}{
		{"frames_too_few", map[string]any{"frames": 1}, "between 2 and 48"},
		{"frames_too_many", map[string]any{"frames": 49}, "between 2 and 48"},
		{"bad_resolution", map[string]any{"resolution": "800x600"}, "invalid resolution"},
		{"bad_bpc", map[string]any{"bpc": 9}, "invalid bpc"},
		{"unallowlisted_data_dir", map[string]any{"data_dir": "/etc"}, "data_dir:"},
	}
	for _, tc := range bench {
		t.Run("bench/"+tc.name, func(t *testing.T) {
			_, _, err := buildBenchArgv("BIN", tc.args)
			assertErrContains(t, err, tc.want)
		})
	}

	vpl := []struct {
		name string
		args map[string]any
		want string
	}{
		{"model_is_a_path", map[string]any{"ref": ref, "dis": ref, "model": "/models/x.json"}, "not a path"},
		{"negative_frames", map[string]any{"ref": ref, "dis": ref, "frames": -1}, "frames must be >= 0"},
		{"negative_device", map[string]any{"ref": ref, "dis": ref, "device": -1}, "device must be >= 0"},
		{"render_node_escape", map[string]any{"ref": ref, "dis": ref, "render_node": "/etc/passwd"}, "invalid render_node"},
		{"render_node_traversal", map[string]any{"ref": ref, "dis": ref, "render_node": "/dev/dri/../../etc/passwd"}, "invalid render_node"},
		{"unallowlisted_dis", map[string]any{"ref": ref, "dis": "/etc/passwd"}, "dis:"},
	}
	for _, tc := range vpl {
		t.Run("vpl/"+tc.name, func(t *testing.T) {
			_, _, err := buildVplArgv("BIN", tc.args)
			assertErrContains(t, err, tc.want)
		})
	}
}

func assertErrContains(t *testing.T, err error, want string) {
	t.Helper()
	if err == nil {
		t.Fatalf("expected an error containing %q, got nil", want)
	}
	if !strings.Contains(err.Error(), want) {
		t.Fatalf("error %q does not contain %q", err.Error(), want)
	}
}

// TestSidecarHandlersFailFastWithoutBinary covers the "binary not found" branch
// of every sidecar handler (cmd/vmafx-mcp/AGENTS.md invariant #8).
func TestSidecarHandlersFailFastWithoutBinary(t *testing.T) {
	ref := fixturePath(t)
	for name, env := range libvmaf.SidecarBinaryEnv {
		t.Setenv(env, "/nonexistent/"+name)
	}
	ctx := context.Background()

	cases := []struct {
		name    string
		handler func(context.Context, map[string]any) (any, error)
		args    map[string]any
	}{
		{"vmaf_per_shot", handleVmafPerShot, map[string]any{"reference": ref, "width": 576, "height": 324}},
		{"vmaf_roi", handleVmafRoi, map[string]any{"reference": ref, "width": 576, "height": 324, "frame": 0}},
		{"vmaf_bench", handleVmafBench, map[string]any{}},
		{"vmaf_vpl", handleVmafVpl, map[string]any{"ref": ref, "dis": ref}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := tc.handler(ctx, tc.args)
			if err == nil {
				t.Fatalf("%s: expected a not-found error, got nil", tc.name)
			}
			if !strings.Contains(err.Error(), "binary not found") {
				t.Fatalf("%s: unexpected error %q", tc.name, err.Error())
			}
		})
	}
}

// TestFindSidecarBinaryEnvOverride pins the resolution order's first step and
// the unknown-name contract.
func TestFindSidecarBinaryEnvOverride(t *testing.T) {
	t.Setenv("VMAF_ROI_BIN", "/opt/custom/vmaf_roi")
	if got := libvmaf.FindSidecarBinary("vmaf_roi"); got != "/opt/custom/vmaf_roi" {
		t.Fatalf("env override ignored: got %q", got)
	}
	if got := libvmaf.FindSidecarBinary("not_a_sidecar"); got != "" {
		t.Fatalf("unknown sidecar name should return \"\", got %q", got)
	}
}

// TestSidecarFailureMessage pins the uniform non-zero-exit error text.
func TestSidecarFailureMessage(t *testing.T) {
	err := sidecarFailure("vmaf_roi", 2, "some stdout", "")
	if !strings.Contains(err.Error(), "vmaf_roi exited 2: some stdout") {
		t.Fatalf("unexpected message %q", err.Error())
	}
	err = sidecarFailure("vmaf_roi", 3, "", "")
	if !strings.Contains(err.Error(), "no output") {
		t.Fatalf("unexpected message %q", err.Error())
	}
}
