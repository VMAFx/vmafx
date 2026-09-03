// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/fast_test.go — in-package tests for the `fast`
// subcommand: flag surface parity with the Python `vmaf-tune fast`, the JSON
// payload schema, and the 0 / 2 / 3 exit-code contract.

package cmd

import (
	"encoding/json"
	"errors"
	"github.com/VMAFx/vmafx/pkg/model"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/fast"
)

// TestFastCmdFlagSurface asserts every flag the Python `vmaf-tune fast`
// parser wires (cli._add_fast_args) exists on the Go command with the same
// name and the same default, so operators can swap binaries without
// re-learning the surface.
func TestFastCmdFlagSurface(t *testing.T) {
	t.Parallel()

	cmd := newFastCmd()

	tests := []struct {
		flag        string
		wantDefault string
	}{
		{flag: "src", wantDefault: ""},
		{flag: "width", wantDefault: "0"},
		{flag: "height", wantDefault: "0"},
		{flag: "pix-fmt", wantDefault: "yuv420p"},
		{flag: "framerate", wantDefault: "24"},
		{flag: "target-vmaf", wantDefault: "0"},
		{flag: "encoder", wantDefault: "libx264"},
		{flag: "preset", wantDefault: "medium"},
		{flag: "crf-min", wantDefault: "10"},
		{flag: "crf-max", wantDefault: "51"},
		{flag: "n-trials", wantDefault: "0"},
		{flag: "time-budget-s", wantDefault: "300"},
		{flag: "proxy-tolerance", wantDefault: "1.5"},
		{flag: "sample-chunk-seconds", wantDefault: "5"},
		{flag: "smoke", wantDefault: "false"},
		{flag: "score-backend", wantDefault: "auto"},
		{flag: "ffmpeg-bin", wantDefault: "ffmpeg"},
		{flag: "vmaf-bin", wantDefault: "vmaf"},
		// Derived, not literal: the Go and Python defaults must match each
		// other, and ADR-1169 moved both to the v1.0.16 generation.
		{flag: "vmaf-model", wantDefault: model.DefaultVersion},
		{flag: "encode-dir", wantDefault: ".workingdir2/fast"},
		{flag: "output", wantDefault: ""},
	}

	for _, tc := range tests {
		t.Run(tc.flag, func(t *testing.T) {
			t.Parallel()
			f := cmd.Flags().Lookup(tc.flag)
			if f == nil {
				t.Fatalf("--%s is missing from the Go fast subcommand", tc.flag)
			}
			if f.DefValue != tc.wantDefault {
				t.Errorf("--%s default = %q, want %q (Python parity)",
					tc.flag, f.DefValue, tc.wantDefault)
			}
			if f.Usage == "" {
				t.Errorf("--%s has no help text", tc.flag)
			}
		})
	}
}

// TestFastCmdDefaultsTrackPackageConstants guards against the CLI defaults
// drifting away from the constants the Python module exports.
func TestFastCmdDefaultsTrackPackageConstants(t *testing.T) {
	t.Parallel()

	if fast.DefaultCRFLo != 10 || fast.DefaultCRFHi != 51 {
		t.Errorf("CRF range constants drifted: [%d, %d], want [10, 51]",
			fast.DefaultCRFLo, fast.DefaultCRFHi)
	}
	if fast.ProdNTrials != 30 {
		t.Errorf("ProdNTrials = %d, want 30", fast.ProdNTrials)
	}
	if fast.SmokeNTrials != 50 {
		t.Errorf("SmokeNTrials = %d, want 50", fast.SmokeNTrials)
	}
	if fast.DefaultProxyTolerance != 1.5 {
		t.Errorf("DefaultProxyTolerance = %v, want 1.5", fast.DefaultProxyTolerance)
	}
	if fast.SampleChunkSeconds != 5.0 {
		t.Errorf("SampleChunkSeconds = %v, want 5.0", fast.SampleChunkSeconds)
	}
	if fast.DefaultTimeBudgetSeconds != 300 {
		t.Errorf("DefaultTimeBudgetSeconds = %d, want 300", fast.DefaultTimeBudgetSeconds)
	}
}

// TestFastCmdRequiresTargetVMAF verifies --target-vmaf is a required flag,
// matching the Python parser's required=True.
func TestFastCmdRequiresTargetVMAF(t *testing.T) {
	t.Parallel()

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{"fast", "--smoke"})
	root.Cobra().SetOut(&strings.Builder{})
	root.Cobra().SetErr(&strings.Builder{})
	err := root.Execute()
	if err == nil {
		t.Fatal("want an error when --target-vmaf is omitted, got nil")
	}
	if !strings.Contains(err.Error(), "target-vmaf") {
		t.Errorf("error should name the missing flag; got %v", err)
	}
}

// TestFastSmokeEndToEnd drives the clikit root with `fast --smoke` and checks
// the emitted document against the Python payload schema.
func TestFastSmokeEndToEnd(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	out := filepath.Join(dir, "fast.json")

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"fast", "--smoke", "--target-vmaf", "90", "--output", out,
	})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute fast --smoke: %v", err)
	}

	raw, err := os.ReadFile(out) //nolint:gosec // test-local temp path
	if err != nil {
		t.Fatalf("read payload: %v", err)
	}

	// The document must carry exactly the Python key set for a smoke run
	// (score_backend is production-only), in sorted order.
	var generic map[string]any
	if err := json.Unmarshal(raw, &generic); err != nil {
		t.Fatalf("payload is not valid JSON: %v\n%s", err, raw)
	}
	gotKeys := make([]string, 0, len(generic))
	for k := range generic {
		gotKeys = append(gotKeys, k)
	}
	sort.Strings(gotKeys)
	wantKeys := []string{
		"encoder", "n_trials", "notes", "predicted_kbps", "predicted_vmaf",
		"proxy_verify_gap", "recommended_crf", "smoke", "target_vmaf", "verify_vmaf",
	}
	if strings.Join(gotKeys, ",") != strings.Join(wantKeys, ",") {
		t.Errorf("payload keys = %v, want %v", gotKeys, wantKeys)
	}

	// Keys must appear in sorted order in the bytes, matching Python's
	// json.dumps(..., sort_keys=True).
	lastIdx := -1
	for _, k := range wantKeys {
		idx := strings.Index(string(raw), `"`+k+`"`)
		if idx < 0 {
			t.Fatalf("key %q missing from the rendered bytes", k)
		}
		if idx < lastIdx {
			t.Errorf("key %q is out of sorted order in the rendered document", k)
		}
		lastIdx = idx
	}

	var result fast.RecommendResult
	if err := json.Unmarshal(raw, &result); err != nil {
		t.Fatalf("payload does not parse into RecommendResult: %v", err)
	}
	if !result.Smoke {
		t.Error("smoke = false in a --smoke run")
	}
	if result.NTrials != fast.SmokeNTrials {
		t.Errorf("n_trials = %d, want %d", result.NTrials, fast.SmokeNTrials)
	}
	if result.Encoder != "libx264" {
		t.Errorf("encoder = %q, want libx264", result.Encoder)
	}
	if result.TargetVMAF != 90 {
		t.Errorf("target_vmaf = %v, want 90", result.TargetVMAF)
	}
	if result.VerifyVMAF != nil || result.ProxyVerifyGap != nil {
		t.Error("smoke mode must leave verify_vmaf / proxy_verify_gap null")
	}
	if result.RecommendedCRF < fast.DefaultCRFLo || result.RecommendedCRF > fast.DefaultCRFHi {
		t.Errorf("recommended_crf = %d, outside [%d, %d]",
			result.RecommendedCRF, fast.DefaultCRFLo, fast.DefaultCRFHi)
	}

	// Integral floats keep their ".0" so the bytes match Python's repr.
	if !strings.Contains(string(raw), `"target_vmaf": 90.0`) {
		t.Errorf("target_vmaf must render as 90.0 (Python repr parity); got:\n%s", raw)
	}
	// The document ends with a newline, like the Python writer.
	if n := len(raw); n == 0 || raw[n-1] != '\n' {
		t.Error("payload must end with a newline")
	}
}

// TestFastSmokeHonoursSearchFlags checks the CRF window and trial budget reach
// the search.
func TestFastSmokeHonoursSearchFlags(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	out := filepath.Join(dir, "fast.json")

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"fast", "--smoke", "--target-vmaf", "80",
		"--crf-min", "25", "--crf-max", "30",
		"--n-trials", "9", "--encoder", "libx265",
		"--output", out,
	})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute: %v", err)
	}

	raw, err := os.ReadFile(out) //nolint:gosec // test-local temp path
	if err != nil {
		t.Fatalf("read payload: %v", err)
	}
	var result fast.RecommendResult
	if err := json.Unmarshal(raw, &result); err != nil {
		t.Fatalf("parse payload: %v", err)
	}
	if result.NTrials != 9 {
		t.Errorf("n_trials = %d, want 9", result.NTrials)
	}
	if result.RecommendedCRF < 25 || result.RecommendedCRF > 30 {
		t.Errorf("recommended_crf = %d, outside the requested [25, 30]", result.RecommendedCRF)
	}
	if result.Encoder != "libx265" {
		t.Errorf("encoder = %q, want libx265", result.Encoder)
	}
}

// TestFastUsageErrors covers the exit-2 validation paths.
func TestFastUsageErrors(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		args    []string
		wantErr string
	}{
		{
			name:    "inverted CRF range",
			args:    []string{"fast", "--smoke", "--target-vmaf", "90", "--crf-min", "40", "--crf-max", "20"},
			wantErr: "invalid CRF range",
		},
		{
			name:    "negative crf-min",
			args:    []string{"fast", "--smoke", "--target-vmaf", "90", "--crf-min", "-1"},
			wantErr: "invalid CRF range",
		},
		{
			name:    "target above 100",
			args:    []string{"fast", "--smoke", "--target-vmaf", "101"},
			wantErr: "out of range",
		},
		{
			name:    "target of zero",
			args:    []string{"fast", "--smoke", "--target-vmaf", "0"},
			wantErr: "out of range",
		},
		{
			name:    "non-positive time budget",
			args:    []string{"fast", "--smoke", "--target-vmaf", "90", "--time-budget-s", "0"},
			wantErr: "--time-budget-s must be > 0",
		},
		{
			name:    "production without --src",
			args:    []string{"fast", "--target-vmaf", "90"},
			wantErr: "--src is required",
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
				t.Fatalf("want an error containing %q, got nil", tc.wantErr)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("error %q does not contain %q", err, tc.wantErr)
			}
			code, ok := fastExitCode(err)
			if !ok {
				t.Fatalf("error carries no fast exit code: %v", err)
			}
			if code != exitUsage {
				t.Errorf("exit code = %d, want %d (usage)", code, exitUsage)
			}
		})
	}
}

// TestFastProductionMissingSourceFile covers the stat check on --src.
func TestFastProductionMissingSourceFile(t *testing.T) {
	t.Parallel()

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"fast", "--target-vmaf", "90",
		"--src", filepath.Join(t.TempDir(), "absent.yuv"),
		"--width", "1920", "--height", "1080",
	})
	root.Cobra().SetOut(&strings.Builder{})
	root.Cobra().SetErr(&strings.Builder{})
	err := root.Execute()
	if err == nil {
		t.Fatal("want an error for a missing --src, got nil")
	}
	if code, ok := fastExitCode(err); !ok || code != exitUsage {
		t.Errorf("exit code = %d (ok=%v), want %d", code, ok, exitUsage)
	}
}

// TestFastProductionUnknownEncoder covers the encoder validation, which must
// fire before any subprocess work.
func TestFastProductionUnknownEncoder(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	src := filepath.Join(dir, "ref.yuv")
	if err := os.WriteFile(src, make([]byte, 64), 0o600); err != nil {
		t.Fatalf("write fixture: %v", err)
	}

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"fast", "--target-vmaf", "90", "--src", src,
		"--width", "16", "--height", "16", "--encoder", "libnope",
	})
	root.Cobra().SetOut(&strings.Builder{})
	root.Cobra().SetErr(&strings.Builder{})
	err := root.Execute()
	if err == nil {
		t.Fatal("want an error for an unknown encoder, got nil")
	}
	if !strings.Contains(err.Error(), "unknown encoder") {
		t.Errorf("error %q should name the unknown encoder", err)
	}
	if code, ok := fastExitCode(err); !ok || code != exitUsage {
		t.Errorf("exit code = %d (ok=%v), want %d", code, ok, exitUsage)
	}
}

// TestFastExitCode covers the exit-status carrier.
func TestFastExitCode(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name     string
		err      error
		wantCode int
		wantOK   bool
	}{
		{name: "nil error carries nothing", err: nil, wantOK: false},
		{name: "plain error carries nothing", err: errors.New("boom"), wantOK: false},
		{
			name:     "usage error carries 2",
			err:      usageError(errors.New("bad flag")),
			wantCode: exitUsage, wantOK: true,
		},
		{
			name:     "out-of-distribution carries 3",
			err:      &fastExitError{code: exitOOD, err: errors.New("gap")},
			wantCode: exitOOD, wantOK: true,
		},
		{
			name: "a wrapped exit error is still found",
			err: errors.Join(
				errors.New("shutdown"),
				&fastExitError{code: exitOOD, err: errors.New("gap")},
			),
			wantCode: exitOOD, wantOK: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			code, ok := fastExitCode(tc.err)
			if ok != tc.wantOK {
				t.Fatalf("ok = %v, want %v", ok, tc.wantOK)
			}
			if ok && code != tc.wantCode {
				t.Errorf("code = %d, want %d", code, tc.wantCode)
			}
		})
	}
}

// TestFastExitErrorUnwrap verifies the wrapper keeps the domain error
// inspectable so errors.Is / errors.As still work through it.
func TestFastExitErrorUnwrap(t *testing.T) {
	t.Parallel()

	sentinel := errors.New("root cause")
	wrapped := usageError(sentinel)
	if !errors.Is(wrapped, sentinel) {
		t.Error("usageError must keep the domain error inspectable")
	}
	if wrapped.Error() != sentinel.Error() {
		t.Errorf("Error() = %q, want %q", wrapped.Error(), sentinel.Error())
	}
}

// TestFastCmdRegisteredAsPorted verifies `fast` is a real subcommand and not
// the loud-fail stub any more.
func TestFastCmdRegisteredAsPorted(t *testing.T) {
	t.Parallel()

	root := newRoot("dev")
	var fastCmd bool
	for _, c := range root.Cobra().Commands() {
		if c.Name() == "fast" {
			fastCmd = true
			if strings.Contains(c.Short, "not yet ported") {
				t.Error("fast is still registered as a not-yet-ported stub")
			}
			if c.Flags().Lookup("target-vmaf") == nil {
				t.Error("the registered fast command carries no --target-vmaf flag")
			}
		}
	}
	if !fastCmd {
		t.Fatal("root is missing the fast subcommand")
	}
}
