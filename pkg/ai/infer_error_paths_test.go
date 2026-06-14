// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/ai/infer_error_paths_test.go — additional error-path tests for Infer,
// InputCount, and ModelPath to raise coverage from 70.9% toward 85%.

package ai

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// TestInfer_ModelNotFound verifies that Infer returns an error (not a panic)
// when the model name does not exist in the registry.
func TestInfer_ModelNotFound(t *testing.T) {
	t.Parallel()
	r := NewRegistry(t.TempDir())
	_, err := r.Infer("does_not_exist", []float64{1.0})
	if err == nil {
		t.Fatal("expected error for missing model, got nil")
	}
}

// TestInputCount_MalformedSidecar exercises the JSON-unmarshal error branch.
func TestInputCount_MalformedSidecar(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	// Write a sidecar that is syntactically invalid JSON.
	bad := filepath.Join(dir, "bad_model.json")
	if err := os.WriteFile(bad, []byte("{not valid json"), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	r := NewRegistry(dir)
	_, err := r.InputCount("bad_model")
	if err == nil {
		t.Fatal("expected error for malformed sidecar JSON, got nil")
	}
}

// TestModelPath_OnnxSuffix covers the third search candidate:
// <modelDir>/<modelName> when modelName already ends in ".onnx".
func TestModelPath_OnnxSuffix(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	// Write a file whose name already ends in ".onnx".
	name := "my_model.onnx"
	if err := os.WriteFile(filepath.Join(dir, name), []byte("placeholder"), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	r := NewRegistry(dir)
	// Passing "my_model.onnx" — the second candidate (<modelDir>/<modelName>+".onnx")
	// won't exist (double-suffix), but the third candidate (<modelDir>/<modelName>)
	// will match because the file is stored as "my_model.onnx".
	path, err := r.ModelPath(name)
	if err != nil {
		t.Fatalf("ModelPath(%q): %v", name, err)
	}
	if path == "" {
		t.Fatal("expected non-empty path")
	}
}

// TestErrORTRunnerNotFound_IsWrapped verifies that ErrORTRunnerNotFound can be
// detected via errors.Is even when it is wrapped.
func TestErrORTRunnerNotFound_IsWrapped(t *testing.T) {
	// No t.Parallel(): this test uses t.Setenv, which Go forbids combining
	// with a parallel test (panics at runtime).
	dir := t.TempDir()
	writeONNX(t, dir, "model_for_wrap_test")
	t.Setenv("PATH", "")

	r := NewRegistry(dir)
	_, err := r.Infer("model_for_wrap_test", []float64{0.1})
	if err == nil {
		t.Fatal("expected error from Infer, got nil")
	}
	if !errors.Is(err, ErrORTRunnerNotFound) {
		t.Errorf("expected errors.Is(err, ErrORTRunnerNotFound), got: %v", err)
	}
}

// TestNewRegistry_DefaultModelDir checks that NewRegistry uses DefaultModelDir
// when neither an explicit path nor the environment variable is set.
func TestNewRegistry_DefaultModelDir(t *testing.T) {
	// Unset the env var for this test so the default path is chosen.
	t.Setenv("VMAFX_MODEL_DIR", "")
	r := NewRegistry("")
	if r.modelDir != DefaultModelDir {
		t.Errorf("modelDir = %q, want DefaultModelDir = %q", r.modelDir, DefaultModelDir)
	}
}

// TestInputCount_MissingModel verifies InputCount returns 0 with no error when
// both the model file and the sidecar are absent.
func TestInputCount_MissingModel(t *testing.T) {
	t.Parallel()
	r := NewRegistry(t.TempDir())
	n, err := r.InputCount("totally_absent_model")
	if err != nil {
		t.Fatalf("InputCount for absent model: unexpected error: %v", err)
	}
	if n != 0 {
		t.Errorf("expected 0 for absent sidecar, got %d", n)
	}
}

// TestFloatsToCSV exercises the internal floatsToCSV helper via DiagString.
func TestFloatsToCSV_ViaDigString(t *testing.T) {
	t.Parallel()
	s := DiagString("mdl", []float64{}, []float64{})
	if s == "" {
		t.Fatal("DiagString with empty slices returned empty string")
	}
}

// TestListModels_DirectoryEntry verifies that non-.onnx files are skipped.
func TestListModels_NonOnnxSkipped(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	// Write a .onnx file and a .txt file; only the .onnx should be listed.
	writeONNX(t, dir, "valid_model")
	if err := os.WriteFile(filepath.Join(dir, "readme.txt"), []byte("ignored"), 0o600); err != nil {
		t.Fatalf("WriteFile txt: %v", err)
	}
	r := NewRegistry(dir)
	models := r.ListModels()
	if len(models) != 1 {
		t.Errorf("expected 1 model, got %d: %v", len(models), models)
	}
	if models[0] != "valid_model" {
		t.Errorf("expected 'valid_model', got %q", models[0])
	}
}

// TestListModels_SubdirSkipped verifies that subdirectories are not returned.
func TestListModels_SubdirSkipped(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	// Create a subdirectory named with a .onnx suffix — should NOT be listed.
	subdir := filepath.Join(dir, "subdir.onnx")
	if err := os.Mkdir(subdir, 0o700); err != nil {
		t.Fatalf("Mkdir: %v", err)
	}
	writeONNX(t, dir, "real_model")

	r := NewRegistry(dir)
	models := r.ListModels()
	if len(models) != 1 {
		t.Errorf("expected 1 model (subdir skipped), got %d: %v", len(models), models)
	}
}

// writeSidecarWithContent writes raw JSON to a sidecar file.
func writeSidecarWithContent(t *testing.T, dir, name string, content []byte) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, name+".json"), content, 0o600); err != nil {
		t.Fatalf("writeSidecarWithContent: %v", err)
	}
}

// TestInputCount_ZeroInputCount verifies InputCount correctly returns 0 from a
// valid sidecar that explicitly sets input_count to 0.
func TestInputCount_ZeroInputCount(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	data, _ := json.Marshal(map[string]int{"input_count": 0})
	writeSidecarWithContent(t, dir, "zero_model", data)

	r := NewRegistry(dir)
	n, err := r.InputCount("zero_model")
	if err != nil {
		t.Fatalf("InputCount: %v", err)
	}
	if n != 0 {
		t.Errorf("expected 0, got %d", n)
	}
}
