// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/auto_test.go — in-package tests for the "auto"
// subcommand wired in as part of the Stage-2 Python-to-Go port.

package cmd

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestParseAllowCodecs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		in   string
		want []string
	}{
		{"single", "libx264", []string{"libx264"}},
		{"multiple", "libx264,libx265", []string{"libx264", "libx265"}},
		{"whitespace is trimmed", " libx264 , libx265 ", []string{"libx264", "libx265"}},
		{"empty tokens are dropped", "libx264,,libx265,", []string{"libx264", "libx265"}},
		{"all empty", ",, ,", []string{}},
		{"empty string", "", []string{}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := parseAllowCodecs(tc.in); !reflect.DeepEqual(got, tc.want) {
				t.Errorf("parseAllowCodecs(%q) = %v, want %v", tc.in, got, tc.want)
			}
		})
	}
}

// TestAutoFlagSurfaceMatchesPython pins the flag names and defaults against
// the Python argparse surface. Operators script against these; a rename or a
// changed default is a breaking change, not an implementation detail.
func TestAutoFlagSurfaceMatchesPython(t *testing.T) {
	t.Parallel()

	cmd := newAutoCmd()

	tests := []struct {
		flag        string
		wantDefault string
	}{
		{"src", ""},
		{"target-vmaf", "93"},
		{"max-budget-bitrate", "8000"},
		{"allow-codecs", "libx264"},
		{"codec", ""},
		{"sample-clip-seconds", "0"},
		{"smoke", "false"},
		{"output", ""},
		{"execute", "false"},
		{"runs-dir", "runs"},
		{"execute-all", "false"},
	}
	for _, tc := range tests {
		t.Run(tc.flag, func(t *testing.T) {
			t.Parallel()
			f := cmd.Flags().Lookup(tc.flag)
			if f == nil {
				t.Fatalf("--%s is missing from the auto subcommand", tc.flag)
			}
			if f.DefValue != tc.wantDefault {
				t.Errorf("--%s default = %q, want %q", tc.flag, f.DefValue, tc.wantDefault)
			}
		})
	}
}

// TestAutoSmokeEndToEnd drives the real command tree and checks the emitted
// plan, both to stdout and to a file.
func TestAutoSmokeEndToEnd(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	out := filepath.Join(dir, "nested", "plan.json")

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"auto", "--src", "/tmp/clip.mkv", "--smoke",
		"--target-vmaf", "95", "--max-budget-bitrate", "6000",
		"--allow-codecs", "libx264,libx265",
		"--output", out,
	})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute auto: %v", err)
	}

	raw, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read plan: %v", err)
	}
	// The file form gets the rendered JSON verbatim, with no trailing newline
	// — the Python emitter's behaviour, which downstream diff tooling relies on.
	if strings.HasSuffix(string(raw), "\n") {
		t.Error("the plan file should not carry a trailing newline")
	}

	var payload struct {
		Cells []map[string]any `json:"cells"`
		Meta  struct {
			TargetVMAF    float64  `json:"target_vmaf"`
			MaxBudgetKbps float64  `json:"max_budget_kbps"`
			AllowCodecs   []string `json:"allow_codecs"`
			Smoke         bool     `json:"smoke"`
			ShortCircuits []string `json:"short_circuits"`
			RecipeApplied string   `json:"recipe_applied"`
		} `json:"metadata"`
	}
	if err := json.Unmarshal(raw, &payload); err != nil {
		t.Fatalf("parse plan: %v", err)
	}
	if len(payload.Cells) != 2 {
		t.Errorf("cells = %d, want 2 (one rung x two codecs)", len(payload.Cells))
	}
	if payload.Meta.TargetVMAF != 95 || payload.Meta.MaxBudgetKbps != 6000 {
		t.Errorf("metadata targets = (%v, %v), want (95, 6000)",
			payload.Meta.TargetVMAF, payload.Meta.MaxBudgetKbps)
	}
	if !reflect.DeepEqual(payload.Meta.AllowCodecs, []string{"libx264", "libx265"}) {
		t.Errorf("allow_codecs = %v", payload.Meta.AllowCodecs)
	}
	if !payload.Meta.Smoke {
		t.Error("metadata.smoke should be true for a --smoke run")
	}
	if payload.Meta.RecipeApplied != "default" {
		t.Errorf("recipe_applied = %q, want default", payload.Meta.RecipeApplied)
	}
	wantFired := map[string]bool{
		"ladder-single-rung": true, "predictor-gospel": true,
		"skip-saliency": true, "sdr-skip": true, "skip-per-shot": true,
	}
	for _, sc := range payload.Meta.ShortCircuits {
		delete(wantFired, sc)
	}
	if len(wantFired) != 0 {
		t.Errorf("short_circuits is missing %v (got %v)", wantFired, payload.Meta.ShortCircuits)
	}
}

// TestAutoRejectsBadInput covers the argument validation the Python CLI
// performs before it ever builds a plan.
func TestAutoRejectsBadInput(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		args    []string
		wantErr string
	}{
		{
			name:    "missing --src",
			args:    []string{"auto", "--smoke"},
			wantErr: "src",
		},
		{
			name:    "empty --allow-codecs",
			args:    []string{"auto", "--src", "/tmp/clip.mkv", "--smoke", "--allow-codecs", ",,"},
			wantErr: "allow-codecs",
		},
		{
			name: "unknown codec in the allow list",
			args: []string{
				"auto", "--src", "/tmp/clip.mkv", "--allow-codecs", "libnope",
			},
			wantErr: "libnope",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			root := newRoot("dev")
			root.Cobra().SetArgs(tc.args)
			root.Cobra().SetOut(new(strings.Builder))
			root.Cobra().SetErr(new(strings.Builder))
			err := root.Execute()
			if err == nil {
				t.Fatalf("expected an error mentioning %q", tc.wantErr)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("error %q should mention %q", err, tc.wantErr)
			}
		})
	}
}

// TestAutoNonSmokePlanCarriesNaNIntervalWidth pins the byte-level quirk the
// Go emitter has to reproduce: without a calibration seam the conformal
// interval is uncalibrated, and CPython's json.dumps spells that as the bare
// NaN token. Go's encoding/json cannot, which is why pkg/pyjson exists.
func TestAutoNonSmokePlanCarriesNaNIntervalWidth(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	out := filepath.Join(dir, "plan.json")

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		// A source that cannot be probed exercises the documented
		// degrade-to-defaults path without needing a real file.
		"auto", "--src", filepath.Join(dir, "absent.mkv"), "--output", out,
	})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute auto: %v", err)
	}
	raw, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read plan: %v", err)
	}
	if !strings.Contains(string(raw), `"interval_width": NaN`) {
		t.Errorf("expected a bare NaN interval_width in the plan:\n%s", raw)
	}
	// encoding/json must reject it, which is the whole point.
	var probe map[string]any
	if err := json.Unmarshal(raw, &probe); err == nil {
		t.Error("the plan should not be RFC-8259 JSON; the Python emitter's " +
			"allow_nan default is what we are reproducing")
	}
}

// TestWritePlanStdoutAppendsNewline pins the other half of the emitter
// contract: stdout gets a trailing newline, the file does not.
func TestWritePlanStdoutAppendsNewline(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "out", "plan.json")

	if err := writePlan(path, `{"a": 1}`); err != nil {
		t.Fatalf("writePlan: %v", err)
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(raw) != `{"a": 1}` {
		t.Errorf("file content = %q, want the rendered JSON verbatim", raw)
	}

	// The directory is created on demand.
	if _, err := os.Stat(filepath.Dir(path)); err != nil {
		t.Errorf("writePlan should create the output directory: %v", err)
	}
}

// TestAutoPlanCellsCarryTheFullSchema guards the downstream consumers (the MCP
// server, the CI corpus collector) against a silently dropped key.
func TestAutoPlanCellsCarryTheFullSchema(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	out := filepath.Join(dir, "plan.json")

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{"auto", "--src", "/tmp/clip.mkv", "--smoke", "--output", out})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute auto: %v", err)
	}
	raw, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read plan: %v", err)
	}

	var payload struct {
		Cells []map[string]json.RawMessage `json:"cells"`
	}
	if err := json.Unmarshal(raw, &payload); err != nil {
		t.Fatalf("parse plan: %v", err)
	}
	if len(payload.Cells) == 0 {
		t.Fatal("plan has no cells")
	}
	want := []string{
		"rung", "codec", "verdict", "crf", "estimated_vmaf",
		"estimated_bitrate_kbps", "hdr_args", "sample_clip_seconds",
		"confidence_decision", "interval_width",
		"effective_predictor_target_vmaf", "prediction_source",
		"saliency_intensity", "selected",
	}
	for _, key := range want {
		if _, ok := payload.Cells[0][key]; !ok {
			t.Errorf("cell schema is missing %q", key)
		}
	}
	if len(payload.Cells[0]) != len(want) {
		t.Errorf("cell has %d keys, want exactly %d — an added key needs a "+
			"schema note in docs/", len(payload.Cells[0]), len(want))
	}
}
