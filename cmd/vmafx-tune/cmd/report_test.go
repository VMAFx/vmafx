// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/report_test.go — unit tests for the "report" subcommand.
//
// Tests cover:
//   - Happy path: Markdown rendering from a compare JSON file.
//   - Happy path: HTML rendering from a ladder JSON file.
//   - Sad path: unknown --format returns an error.
//   - Sad path: missing input returns an error from cobra.
//   - loadReportFile: valid compare payload.
//   - loadReportFile: valid ladder payload.
//   - loadReportFile: bad JSON returns error.
//   - loadReportFile: non-existent path returns error.
//   - writeOutput: empty output path writes to stdout (smoke test).
//   - writeOutput: non-empty output path writes to file.
//
// ADR-0770: vmafx-tune Stage 4 — report subcommand.

package cmd

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// comparePayload builds a minimal compare JSON payload as raw bytes.
func comparePayloadJSON(src string, targetVMAF float64) []byte {
	bitrate := 1234.5
	vmaf := 76.6683
	encTime := 123.4
	payload := map[string]any{
		"src":          src,
		"target_vmaf":  targetVMAF,
		"tool_version": "dev",
		"wall_time_ms": 500.0,
		"rows": []map[string]any{
			{
				"codec":           "libx264",
				"adapter":         "cpu",
				"runtime_variant": "",
				"ffmpeg_bin":      "ffmpeg",
				"encoder_version": "x264 0.164",
				"best_crf":        23,
				"bitrate_kbps":    bitrate,
				"encode_time_ms":  encTime,
				"vmaf_score":      vmaf,
				"target_vmaf":     targetVMAF,
				"ok":              true,
				"error":           "",
			},
		},
	}
	b, _ := json.Marshal(payload)
	return b
}

// ladderPayloadJSON builds a minimal ladder JSON payload as raw bytes.
func ladderPayloadJSON() []byte {
	payload := map[string]any{
		"schema_version": 1,
		"src":            "clip.yuv",
		"encoder":        "libx264",
		"target_vmafs":   []float64{90.0, 95.0},
		"resolutions":    []string{"1920x1080", "1280x720"},
		"tool_version":   "dev",
		"wall_time_ms":   1000.0,
		"renditions": []map[string]any{
			{"width": 1920, "height": 1080, "bitrate_kbps": 4000.0, "vmaf": 93.2, "crf": 22},
			{"width": 1280, "height": 720, "bitrate_kbps": 2000.0, "vmaf": 90.1, "crf": 24},
		},
	}
	b, _ := json.Marshal(payload)
	return b
}

// writeTempJSON writes payload to a temp file and returns its path.
func writeTempJSON(t *testing.T, payload []byte) string {
	t.Helper()
	f, err := os.CreateTemp(t.TempDir(), "report-*.json")
	if err != nil {
		t.Fatalf("CreateTemp: %v", err)
	}
	if _, err := f.Write(payload); err != nil {
		t.Fatalf("Write: %v", err)
	}
	f.Close()
	return f.Name()
}

// ---------------------------------------------------------------------------
// runReport tests
// ---------------------------------------------------------------------------

func TestRunReport_MarkdownFromCompare(t *testing.T) {
	t.Parallel()
	path := writeTempJSON(t, comparePayloadJSON("clip.yuv", 90.0))

	flags := &reportFlags{
		inputs: []string{path},
		output: "",
		format: "markdown",
	}
	if err := runReport(context.Background(), testDeps(), flags); err != nil {
		t.Fatalf("runReport markdown: %v", err)
	}
}

func TestRunReport_HTMLFromLadder(t *testing.T) {
	t.Parallel()
	path := writeTempJSON(t, ladderPayloadJSON())

	outFile := filepath.Join(t.TempDir(), "out.html")
	flags := &reportFlags{
		inputs: []string{path},
		output: outFile,
		format: "html",
	}
	if err := runReport(context.Background(), testDeps(), flags); err != nil {
		t.Fatalf("runReport html: %v", err)
	}
	data, err := os.ReadFile(outFile)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if !strings.Contains(string(data), "<html") {
		t.Error("expected HTML output to contain '<html'")
	}
}

func TestRunReport_UnknownFormat(t *testing.T) {
	t.Parallel()
	path := writeTempJSON(t, comparePayloadJSON("clip.yuv", 90.0))

	flags := &reportFlags{
		inputs: []string{path},
		format: "pdf",
	}
	err := runReport(context.Background(), testDeps(), flags)
	if err == nil {
		t.Fatal("expected error for unknown format, got nil")
	}
	if !strings.Contains(err.Error(), "pdf") {
		t.Errorf("expected error to mention 'pdf', got: %v", err)
	}
}

func TestRunReport_MultipleInputs(t *testing.T) {
	t.Parallel()
	p1 := writeTempJSON(t, comparePayloadJSON("clip1.yuv", 85.0))
	p2 := writeTempJSON(t, comparePayloadJSON("clip2.yuv", 92.0))

	flags := &reportFlags{
		inputs: []string{p1, p2},
		format: "markdown",
	}
	if err := runReport(context.Background(), testDeps(), flags); err != nil {
		t.Fatalf("runReport multiple inputs: %v", err)
	}
}

// ---------------------------------------------------------------------------
// loadReportFile tests
// ---------------------------------------------------------------------------

func TestLoadReportFile_CompareSchema(t *testing.T) {
	t.Parallel()
	path := writeTempJSON(t, comparePayloadJSON("clip.yuv", 90.0))
	mr, err := loadReportFile(path)
	if err != nil {
		t.Fatalf("loadReportFile compare: %v", err)
	}
	if mr.Src != "clip.yuv" {
		t.Errorf("Src: got %q, want %q", mr.Src, "clip.yuv")
	}
}

func TestLoadReportFile_LadderSchema(t *testing.T) {
	t.Parallel()
	path := writeTempJSON(t, ladderPayloadJSON())
	mr, err := loadReportFile(path)
	if err != nil {
		t.Fatalf("loadReportFile ladder: %v", err)
	}
	if mr.Src != "clip.yuv" {
		t.Errorf("Src: got %q, want %q", mr.Src, "clip.yuv")
	}
}

func TestLoadReportFile_BadJSON(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	path := filepath.Join(dir, "bad.json")
	if err := os.WriteFile(path, []byte(`{not valid json`), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	_, err := loadReportFile(path)
	if err == nil {
		t.Fatal("expected error for bad JSON, got nil")
	}
}

func TestLoadReportFile_NonExistentPath(t *testing.T) {
	t.Parallel()
	_, err := loadReportFile("/nonexistent/path/report.json")
	if err == nil {
		t.Fatal("expected error for non-existent file, got nil")
	}
}

// ---------------------------------------------------------------------------
// newReportCmd smoke test
// ---------------------------------------------------------------------------

func TestNewReportCmd_IsNotNil(t *testing.T) {
	t.Parallel()
	cmd := newReportCmd()
	if cmd == nil {
		t.Fatal("newReportCmd returned nil")
	}
	if cmd.Use == "" {
		t.Error("Use field is empty")
	}
	if cmd.RunE == nil {
		t.Error("RunE is nil")
	}
}

func TestNewReportCmd_RequiresArgs(t *testing.T) {
	t.Parallel()
	cmd := newReportCmd()
	// Validate args with zero arguments — should return an error.
	err := cmd.Args(cmd, []string{})
	if err == nil {
		t.Fatal("expected error for zero args, got nil")
	}
	if !strings.Contains(err.Error(), "at least one") {
		t.Errorf("unexpected error message: %v", err)
	}
}

func TestNewReportCmd_AcceptsArgs(t *testing.T) {
	t.Parallel()
	cmd := newReportCmd()
	// One argument is sufficient.
	err := cmd.Args(cmd, []string{"some.json"})
	if err != nil {
		t.Errorf("unexpected error for one arg: %v", err)
	}
}
