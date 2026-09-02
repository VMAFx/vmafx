// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// CLI-level tests for the `prefilter` subcommand. Smoke mode runs the whole
// joint search without ffmpeg, Vulkan or a GPU, so these are hermetic.

package cmd

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/prefilter"
)

// TestPrefilter_smokeEndToEnd drives the clikit root through a full smoke
// search and validates the emitted JSON payload.
func TestPrefilter_smokeEndToEnd(t *testing.T) {
	t.Parallel()

	out := filepath.Join(t.TempDir(), "rec.json")
	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"prefilter", "--smoke", "--target-vmaf", "93",
		"--n-trials", "30", "--seed", "5", "--output", out,
	})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute prefilter: %v", err)
	}

	data, readErr := os.ReadFile(out) // #nosec G304 -- test-controlled path
	if readErr != nil {
		t.Fatalf("read recommendation: %v", readErr)
	}
	var payload map[string]any
	if err := json.Unmarshal(data, &payload); err != nil {
		t.Fatalf("parse recommendation: %v\n%s", err, data)
	}

	for _, key := range []string{
		"filter_name", "encoder", "target_vmaf", "recommended_crf",
		"recommended_deband", "recommended_vf", "achieved_vmaf",
		"achieved_kbps", "n_trials", "smoke", "probes", "notes",
	} {
		if _, ok := payload[key]; !ok {
			t.Errorf("payload is missing the %q key", key)
		}
	}
	if payload["filter_name"] != prefilter.FilterName {
		t.Errorf("filter_name = %v, want %q", payload["filter_name"], prefilter.FilterName)
	}
	if smoke, ok := payload["smoke"].(bool); !ok || !smoke {
		t.Errorf("smoke = %v, want true", payload["smoke"])
	}
	if n, ok := payload["n_trials"].(float64); !ok || int(n) != 30 {
		t.Errorf("n_trials = %v, want 30", payload["n_trials"])
	}
	vf, ok := payload["recommended_vf"].(string)
	if !ok || !strings.HasPrefix(vf, prefilter.FilterName) {
		t.Errorf("recommended_vf = %v, want a %s fragment",
			payload["recommended_vf"], prefilter.FilterName)
	}
	probes, ok := payload["probes"].([]any)
	if !ok || len(probes) != 30 {
		t.Errorf("probes = %d entries, want 30", len(probes))
	}
}

// TestPrefilter_smokeSweepKnobSubset asserts --sweep-knob restricts the
// emitted fragment to the requested knobs.
func TestPrefilter_smokeSweepKnobSubset(t *testing.T) {
	t.Parallel()

	out := filepath.Join(t.TempDir(), "rec.json")
	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"prefilter", "--smoke", "--target-vmaf", "93",
		"--sweep-knob", "grainy", "--n-trials", "12", "--output", out,
	})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute prefilter: %v", err)
	}

	data, readErr := os.ReadFile(out) // #nosec G304 -- test-controlled path
	if readErr != nil {
		t.Fatalf("read recommendation: %v", readErr)
	}
	var payload struct {
		RecommendedDeband map[string]float64 `json:"recommended_deband"`
		RecommendedVF     string             `json:"recommended_vf"`
	}
	if err := json.Unmarshal(data, &payload); err != nil {
		t.Fatalf("parse recommendation: %v", err)
	}
	if len(payload.RecommendedDeband) != 1 {
		t.Errorf("recommended_deband = %v, want only the swept knob",
			payload.RecommendedDeband)
	}
	if strings.Contains(payload.RecommendedVF, "range=") {
		t.Errorf("unswept knobs must stay off the fragment; got %q",
			payload.RecommendedVF)
	}
}

// TestPrefilter_errors covers the exit-2 rejections. The Python CLI exits 2
// for a requested-but-unavailable feature, and scripts branch on that.
func TestPrefilter_errors(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name         string
		args         []string
		wantExitCode int
	}{
		{
			name: "no target",
			args: []string{"prefilter", "--smoke"},
		},
		{
			name: "inverted CRF range",
			args: []string{
				"prefilter", "--smoke", "--target-vmaf", "93",
				"--crf-min", "40", "--crf-max", "18",
			},
			wantExitCode: 2,
		},
		{
			name: "negative CRF floor",
			args: []string{
				"prefilter", "--smoke", "--target-vmaf", "93", "--crf-min", "-1",
			},
			wantExitCode: 2,
		},
		{
			name: "live loop without a source",
			args: []string{
				"prefilter", "--target-vmaf", "93",
				"--width", "1920", "--height", "1080",
			},
			wantExitCode: 2,
		},
		{
			name: "live loop without geometry",
			args: []string{
				"prefilter", "--target-vmaf", "93", "--src", "/a.yuv",
			},
			wantExitCode: 2,
		},
		{
			name: "unknown filter",
			args: []string{
				"prefilter", "--smoke", "--target-vmaf", "93",
				"--filter", "unsharp",
			},
			wantExitCode: 2,
		},
		{
			name: "unknown sweep knob",
			args: []string{
				"prefilter", "--smoke", "--target-vmaf", "93",
				"--sweep-knob", "nope",
			},
			wantExitCode: 2,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			root := newRoot("dev")
			root.Cobra().SetArgs(tc.args)
			root.Cobra().SetOut(&strings.Builder{})
			root.Cobra().SetErr(&strings.Builder{})
			err := root.Execute()
			if err == nil {
				t.Fatal("expected an error")
			}
			if tc.wantExitCode != 0 {
				var coder exitCoder
				if !errors.As(err, &coder) {
					t.Fatalf("error %v does not carry an exit code", err)
				}
				if coder.ExitCode() != tc.wantExitCode {
					t.Errorf("exit code = %d, want %d", coder.ExitCode(), tc.wantExitCode)
				}
			}
		})
	}
}

// TestPrefilter_liveLoopGatedOnTheFilter asserts the live path refuses to run
// when the Pelorus filter is absent, with an actionable message rather than
// an opaque ffmpeg failure.
func TestPrefilter_liveLoopGatedOnTheFilter(t *testing.T) {
	t.Parallel()

	src := filepath.Join(t.TempDir(), "ref.yuv")
	if err := os.WriteFile(src, make([]byte, 1024), 0o600); err != nil {
		t.Fatalf("write source: %v", err)
	}

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"prefilter", "--target-vmaf", "93", "--src", src,
		"--width", "32", "--height", "32",
		// A binary that certainly does not list the filter.
		"--ffmpeg-bin", "/nonexistent/ffmpeg",
	})
	root.Cobra().SetOut(&strings.Builder{})
	root.Cobra().SetErr(&strings.Builder{})

	err := root.Execute()
	if err == nil {
		t.Fatal("expected the live loop to be gated")
	}
	if !errors.Is(err, prefilter.ErrFilterUnavailable) {
		t.Errorf("error = %v, want it to wrap ErrFilterUnavailable", err)
	}
	if !strings.Contains(err.Error(), "--smoke") {
		t.Errorf("message should point at --smoke; got %q", err)
	}
}

// TestPrefilter_flagSurface asserts every Python flag name is present.
func TestPrefilter_flagSurface(t *testing.T) {
	t.Parallel()

	cmd := newPrefilterCmd()
	want := []string{
		"src", "width", "height", "pix-fmt", "framerate", "duration",
		"target-vmaf", "encoder", "preset", "filter", "sweep-knob",
		"crf-min", "crf-max", "n-trials", "time-budget-s", "seed", "smoke",
		"score-backend", "ffmpeg-bin", "vmaf-bin", "vmaf-model",
		"encode-dir", "output",
	}
	for _, name := range want {
		if cmd.Flags().Lookup(name) == nil {
			t.Errorf("prefilter is missing the --%s flag", name)
		}
	}
}
