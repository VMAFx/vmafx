// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/ai/infer_test.go — unit tests for the AI inference registry.
//
// Tests cover model resolution, sidecar parsing, and error paths.
// The Infer subprocess path is not exercised in unit tests (it requires
// vmafx-ort-runner in PATH); those are integration tests.
//
// ADR-0713: vmafx-node Go worker binary.

package ai

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// writeONNX writes a placeholder .onnx file to dir.
func writeONNX(t *testing.T, dir, name string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, name+".onnx"), []byte("placeholder"), 0o600); err != nil {
		t.Fatalf("writeONNX: %v", err)
	}
}

// writeSidecar writes a sidecar JSON for model name with the given inputCount.
func writeSidecar(t *testing.T, dir, name string, inputCount int) {
	t.Helper()
	data, _ := json.Marshal(map[string]int{"input_count": inputCount})
	if err := os.WriteFile(filepath.Join(dir, name+".json"), data, 0o600); err != nil {
		t.Fatalf("writeSidecar: %v", err)
	}
}

func TestNewRegistry_EnvFallback(t *testing.T) {
	// t.Setenv is incompatible with t.Parallel()
	dir := t.TempDir()
	t.Setenv("VMAFX_MODEL_DIR", dir)
	r := NewRegistry("")
	if r.modelDir != dir {
		t.Errorf("modelDir = %q, want %q", r.modelDir, dir)
	}
}

func TestNewRegistry_ExplicitDir(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	r := NewRegistry(dir)
	if r.modelDir != dir {
		t.Errorf("modelDir = %q, want %q", r.modelDir, dir)
	}
}

func TestModelPath_Found(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	writeONNX(t, dir, "nr_metric_v1")

	r := NewRegistry(dir)
	path, err := r.ModelPath("nr_metric_v1")
	if err != nil {
		t.Fatalf("ModelPath: %v", err)
	}
	if path == "" {
		t.Fatal("expected non-empty path")
	}
}

func TestModelPath_NotFound(t *testing.T) {
	t.Parallel()
	r := NewRegistry(t.TempDir())
	_, err := r.ModelPath("nonexistent")
	if err == nil {
		t.Fatal("expected error for nonexistent model")
	}
}

func TestModelPath_AbsoluteExists(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	abs := filepath.Join(dir, "my_model.onnx")
	if err := os.WriteFile(abs, []byte("placeholder"), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	r := NewRegistry(t.TempDir())
	path, err := r.ModelPath(abs)
	if err != nil {
		t.Fatalf("ModelPath with absolute path: %v", err)
	}
	if path != abs {
		t.Errorf("path = %q, want %q", path, abs)
	}
}

func TestListModels_Empty(t *testing.T) {
	t.Parallel()
	r := NewRegistry(t.TempDir())
	models := r.ListModels()
	if len(models) != 0 {
		t.Errorf("expected 0 models, got %d", len(models))
	}
}

func TestListModels_WithModels(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	writeONNX(t, dir, "nr_metric_v1")
	writeONNX(t, dir, "ss_predictor_v2")

	r := NewRegistry(dir)
	models := r.ListModels()
	if len(models) != 2 {
		t.Errorf("expected 2 models, got %d: %v", len(models), models)
	}
}

func TestListModels_MissingDir(t *testing.T) {
	t.Parallel()
	r := NewRegistry("/nonexistent/dir/that/cannot/exist/9x7k")
	models := r.ListModels()
	// len() on a nil slice is zero — no explicit nil guard needed
	// (staticcheck S1009).
	if len(models) != 0 {
		t.Errorf("expected nil/empty models for missing dir, got %v", models)
	}
}

// TestListModelsSeq_full walks every model in the registry directory and
// confirms the iter.Seq adapter visits them all.
func TestListModelsSeq_full(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	writeONNX(t, dir, "alpha")
	writeONNX(t, dir, "beta")
	writeONNX(t, dir, "gamma")

	r := NewRegistry(dir)
	seen := make(map[string]bool)
	for name := range r.ListModelsSeq() {
		seen[name] = true
	}
	for _, want := range []string{"alpha", "beta", "gamma"} {
		if !seen[want] {
			t.Errorf("ListModelsSeq did not yield %q (got %v)", want, seen)
		}
	}
}

// TestListModelsSeq_earlyBreak verifies the iter.Seq adapter honours
// caller break (load-bearing property motivating the streaming API).
func TestListModelsSeq_earlyBreak(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	for _, name := range []string{"a", "b", "c", "d", "e"} {
		writeONNX(t, dir, name)
	}

	r := NewRegistry(dir)
	var walked int
	for range r.ListModelsSeq() {
		walked++
		if walked == 2 {
			break
		}
	}
	if walked != 2 {
		t.Errorf("ListModelsSeq early-break: walked %d, want 2", walked)
	}
}

// TestListModelsSeq_missingDir mirrors TestListModels_MissingDir for the
// iterator path: it must yield nothing when the directory is absent.
func TestListModelsSeq_missingDir(t *testing.T) {
	t.Parallel()
	r := NewRegistry("/nonexistent/dir/that/cannot/exist/9x7k")
	for name := range r.ListModelsSeq() {
		t.Errorf("ListModelsSeq yielded %q on missing dir, want zero yields", name)
	}
}

// TestListModels_shimMatchesListModelsSeq guards the deprecation shim:
// the deprecated slice form continues to return what the iterator yields,
// for one release.
func TestListModels_shimMatchesListModelsSeq(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	writeONNX(t, dir, "alpha")
	writeONNX(t, dir, "beta")

	r := NewRegistry(dir)
	sliceCount := len(r.ListModels())
	var seqCount int
	for range r.ListModelsSeq() {
		seqCount++
	}
	if sliceCount != seqCount {
		t.Errorf("ListModels shim count %d != ListModelsSeq count %d", sliceCount, seqCount)
	}
}

func TestInputCount_WithSidecar(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	writeSidecar(t, dir, "nr_metric_v1", 7)

	r := NewRegistry(dir)
	n, err := r.InputCount("nr_metric_v1")
	if err != nil {
		t.Fatalf("InputCount: %v", err)
	}
	if n != 7 {
		t.Errorf("InputCount = %d, want 7", n)
	}
}

func TestInputCount_NoSidecar(t *testing.T) {
	t.Parallel()
	r := NewRegistry(t.TempDir())
	n, err := r.InputCount("no_sidecar_model")
	if err != nil {
		t.Fatalf("InputCount with no sidecar: %v", err)
	}
	if n != 0 {
		t.Errorf("expected 0 when sidecar absent, got %d", n)
	}
}

func TestInferDirect_ReturnsNotImplemented(t *testing.T) {
	t.Parallel()
	r := NewRegistry(t.TempDir())
	_, err := r.InferDirect("any_model", []float64{1.0, 2.0})
	if err == nil {
		t.Fatal("expected ErrDirectInferNotImplemented, got nil")
	}
}

func TestInfer_ORTRunnerNotFound(t *testing.T) {
	// t.Setenv is incompatible with t.Parallel()
	dir := t.TempDir()
	writeONNX(t, dir, "nr_metric_v1")

	// Make PATH empty so vmafx-ort-runner cannot be found.
	t.Setenv("PATH", "")

	r := NewRegistry(dir)
	_, err := r.Infer(context.Background(), "nr_metric_v1", []float64{0.5, 0.6})
	if err == nil {
		t.Fatal("expected error when vmafx-ort-runner not in PATH")
	}
}

func TestDiagString(t *testing.T) {
	t.Parallel()
	s := DiagString("my_model", []float64{1.5, 2.5}, []float64{0.8})
	if s == "" {
		t.Fatal("DiagString returned empty string")
	}
	for _, substr := range []string{"my_model", "1.5", "0.8"} {
		if len(s) > 0 && !containsStr(s, substr) {
			t.Errorf("DiagString %q missing %q", s, substr)
		}
	}
}

func containsStr(s, sub string) bool {
	return len(sub) > 0 && len(s) >= len(sub) && (s == sub ||
		len(s) > 0 && findSubstr(s, sub))
}

func findSubstr(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}
