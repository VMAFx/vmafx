// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/libvmaf/libvmaf_test.go — unit tests for the libvmaf Go wrapper.
//
// These tests exercise the JSON-parsing and model-resolution logic without
// requiring a live vmaf binary (mocked via a small shell script written to
// a temp dir).
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.

//go:build cgo

package libvmaf

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// writeVmafScript writes a minimal vmaf stub that emits a canned JSON
// response and returns its path.
func writeVmafScript(t *testing.T, jsonPayload string) string {
	t.Helper()
	dir := t.TempDir()

	// Write the canned JSON output file when the stub receives -o <path>.
	script := `#!/bin/sh
# Minimal vmaf stub: parse -o flag and write canned JSON.
outfile=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) outfile="$2"; shift 2 ;;
    *)  shift ;;
  esac
done
if [ -n "$outfile" ]; then
  cat > "$outfile" <<'EOF'
` + jsonPayload + `
EOF
fi
exit 0
`
	scriptPath := filepath.Join(dir, "vmaf")
	if err := os.WriteFile(scriptPath, []byte(script), 0o700); err != nil {
		t.Fatalf("writeVmafScript: %v", err)
	}
	return scriptPath
}

// writeModel writes a placeholder .json model file to dir and returns its path.
func writeModel(t *testing.T, dir, name string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatalf("writeModel MkdirAll: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, name+".json"), []byte(`{}`), 0o600); err != nil {
		t.Fatalf("writeModel WriteFile: %v", err)
	}
}

// goldenJSON is a representative vmaf CLI JSON output payload.
const goldenJSON = `{
  "pooled_metrics": {
    "vmaf":          {"mean": 76.6683, "harmonic_mean": 76.1234},
    "vif_scale0":    {"mean": 0.8912},
    "vif_scale1":    {"mean": 0.9301},
    "motion2":       {"mean": 2.3456},
    "adm2":          {"mean": 0.9876}
  }
}`

func TestNew_BinaryNotFound(t *testing.T) {
	_, err := New("/nonexistent/vmaf", "")
	if err == nil {
		t.Fatal("expected error for nonexistent binary, got nil")
	}
}

func TestNew_UsesPath(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	s, err := New(scriptPath, "")
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	if s == nil {
		t.Fatal("expected non-nil Scorer")
	}
}

func TestScore_ParsesGoldenJSON(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	modelDir := t.TempDir()
	writeModel(t, modelDir, "vmaf_v0.6.1")

	s, err := New(scriptPath, modelDir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	score, features, err := s.Score("ref.yuv", "dis.yuv", "vmaf_v0.6.1")
	if err != nil {
		t.Fatalf("Score: %v", err)
	}

	const wantScore = 76.6683
	if score != wantScore {
		t.Errorf("score: got %v, want %v", score, wantScore)
	}
	if len(features) == 0 {
		t.Error("expected non-empty features map")
	}
	if _, ok := features["vmaf"]; !ok {
		t.Error("features map missing 'vmaf' key")
	}
}

func TestScore_DefaultModel(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	modelDir := t.TempDir()
	writeModel(t, modelDir, "vmaf_v0.6.1")

	s, err := New(scriptPath, modelDir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	// Empty model name → falls back to vmaf_v0.6.1.
	score, _, err := s.Score("ref.yuv", "dis.yuv", "")
	if err != nil {
		t.Fatalf("Score with default model: %v", err)
	}
	if score != 76.6683 {
		t.Errorf("expected 76.6683, got %v", score)
	}
}

func TestScore_ModelNotFound(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	s, err := New(scriptPath, t.TempDir())
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	_, _, err = s.Score("ref.yuv", "dis.yuv", "nonexistent_model")
	if err == nil {
		t.Fatal("expected error for missing model, got nil")
	}
}

func TestParseOutput_MissingVmafKey(t *testing.T) {
	tmp, err := os.CreateTemp("", "vmafx-test-*.json")
	if err != nil {
		t.Fatalf("CreateTemp: %v", err)
	}
	defer os.Remove(tmp.Name())

	// JSON without a "vmaf" key in pooled_metrics.
	data := map[string]any{
		"pooled_metrics": map[string]any{
			"vif_scale0": map[string]any{"mean": 0.89},
		},
	}
	b, _ := json.Marshal(data)
	if err := os.WriteFile(tmp.Name(), b, 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	tmp.Close()

	_, _, err = parseOutput(tmp.Name())
	if err == nil {
		t.Fatal("expected error for missing vmaf key, got nil")
	}
}

func TestClose_IsNoOp(t *testing.T) {
	scriptPath := writeVmafScript(t, goldenJSON)
	s, err := New(scriptPath, "")
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	// Must not panic.
	s.Close()
}
