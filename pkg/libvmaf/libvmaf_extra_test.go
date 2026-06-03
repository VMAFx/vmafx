// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/libvmaf/libvmaf_extra_test.go — additional unit tests for the libvmaf
// Go wrapper covering paths not reached by libvmaf_test.go:
//
//   - New() PATH fallback (no explicit binary path).
//   - New() PATH fallback failure (vmaf not on PATH).
//   - resolveModel: absolute path that exists.
//   - resolveModel: modelDir lookup with .json suffix already present.
//   - resolveModel: empty modelDir returns error.
//   - parseOutput: corrupt file returns error.
//   - Score: nil context treated as context.Background().
//   - Close() on a nil-binary Scorer does not panic.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.

//go:build cgo

package libvmaf

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// ---------------------------------------------------------------------------
// New() PATH fallback
// ---------------------------------------------------------------------------

// TestNew_PathFallback_Found verifies that New("", dir) succeeds when "vmaf"
// is resolvable on PATH by temporarily prepending a dir containing our stub.
func TestNew_PathFallback_Found(t *testing.T) {
	// Not parallel: modifies PATH env var.
	scriptPath := writeVmafScript(t, goldenJSON)
	dir := filepath.Dir(scriptPath)

	// Prepend stub dir to PATH so LookPath finds our stub.
	orig := os.Getenv("PATH")
	t.Setenv("PATH", dir+":"+orig)

	s, err := New("", "")
	if err != nil {
		t.Fatalf("New with PATH fallback: %v", err)
	}
	if s == nil {
		t.Fatal("expected non-nil Scorer")
	}
}

// TestNew_PathFallback_NotFound verifies New("", "") fails gracefully when
// "vmaf" is absent from PATH.
func TestNew_PathFallback_NotFound(t *testing.T) {
	// Not parallel: modifies PATH env var.
	t.Setenv("PATH", t.TempDir()) // directory with no vmaf binary

	_, err := New("", "")
	if err == nil {
		t.Fatal("expected error when vmaf not on PATH, got nil")
	}
	if !strings.Contains(err.Error(), "vmaf") {
		t.Errorf("expected error to mention 'vmaf', got: %v", err)
	}
}

// ---------------------------------------------------------------------------
// resolveModel edge cases
// ---------------------------------------------------------------------------

// TestResolveModel_AbsolutePath verifies that an absolute path that exists is
// returned unchanged.
func TestResolveModel_AbsolutePath(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	modelPath := filepath.Join(dir, "custom_model.json")
	if err := os.WriteFile(modelPath, []byte(`{}`), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	s := &Scorer{modelDir: ""}
	got, err := s.resolveModel(modelPath)
	if err != nil {
		t.Fatalf("resolveModel with absolute path: %v", err)
	}
	if got != modelPath {
		t.Errorf("resolveModel: got %q, want %q", got, modelPath)
	}
}

// TestResolveModel_WithJsonSuffix verifies that modelDir lookup accepts a name
// that already includes the .json suffix.
func TestResolveModel_WithJsonSuffix(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	// Write the file with the .json suffix in the name.
	modelFile := filepath.Join(dir, "vmaf_v0.6.1.json")
	if err := os.WriteFile(modelFile, []byte(`{}`), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	s := &Scorer{modelDir: dir}
	// Pass the name WITH the .json suffix — resolveModel should find the second
	// candidate (filepath.Join(modelDir, name)) directly.
	got, err := s.resolveModel("vmaf_v0.6.1.json")
	if err != nil {
		t.Fatalf("resolveModel with .json suffix: %v", err)
	}
	if got != modelFile {
		t.Errorf("resolveModel: got %q, want %q", got, modelFile)
	}
}

// TestResolveModel_EmptyModelDir verifies that an empty modelDir and a
// relative model name returns an error.
func TestResolveModel_EmptyModelDir(t *testing.T) {
	t.Parallel()
	s := &Scorer{modelDir: ""}
	_, err := s.resolveModel("missing_model")
	if err == nil {
		t.Fatal("expected error for missing model with empty modelDir, got nil")
	}
}

// ---------------------------------------------------------------------------
// parseOutput edge cases
// ---------------------------------------------------------------------------

// TestParseOutput_CorruptFile verifies parseOutput returns an error when the
// file contains non-JSON content.
func TestParseOutput_CorruptFile(t *testing.T) {
	t.Parallel()
	tmp, err := os.CreateTemp(t.TempDir(), "vmafx-test-corrupt-*.json")
	if err != nil {
		t.Fatalf("CreateTemp: %v", err)
	}
	defer tmp.Close()

	if _, err := tmp.WriteString("not json at all $$$$"); err != nil {
		t.Fatalf("Write: %v", err)
	}
	tmp.Close()

	_, _, err = parseOutput(tmp.Name())
	if err == nil {
		t.Fatal("expected error for corrupt JSON, got nil")
	}
}

// ---------------------------------------------------------------------------
// Score() nil-context fast path
// ---------------------------------------------------------------------------

// TestScore_NilContextTreatedAsBackground verifies that passing a nil ctx
// does not panic — it is treated as context.Background().  The call will
// fail at the model-resolution step (no modelDir), but must not panic.
func TestScore_NilContextTreatedAsBackground(t *testing.T) {
	t.Parallel()
	scriptPath := writeVmafScript(t, goldenJSON)
	s, err := New(scriptPath, "") // no modelDir → resolveModel will fail
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	// nil ctx must not panic; an error is expected because modelDir is empty.
	//nolint:staticcheck // intentional nil-ctx test for defensive code path
	_, _, scoreErr := s.Score(nil, "ref.yuv", "dis.yuv", "vmaf_v0.6.1")
	if scoreErr == nil {
		t.Fatal("expected an error (model not found), got nil")
	}
}

// ---------------------------------------------------------------------------
// Close() smoke
// ---------------------------------------------------------------------------

// TestClose_CalledTwice verifies Close is safe to call multiple times.
func TestClose_CalledTwice(t *testing.T) {
	t.Parallel()
	scriptPath := writeVmafScript(t, goldenJSON)
	s, err := New(scriptPath, "")
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	// Must not panic on repeated calls.
	s.Close()
	s.Close()
}

// ---------------------------------------------------------------------------
// Score() with an already-cancelled context (pre-flight check)
// ---------------------------------------------------------------------------

// TestScore_AlreadyCancelled_ModelResolutionFastPath verifies the pre-flight
// ctx.Err() check fires before we even try to resolve the model when the
// context is already cancelled.
func TestScore_AlreadyCancelled_ModelResolutionFastPath(t *testing.T) {
	t.Parallel()
	scriptPath := writeVmafScript(t, goldenJSON)
	modelDir := t.TempDir()
	writeModel(t, modelDir, "vmaf_v0.6.1")

	s, err := New(scriptPath, modelDir)
	if err != nil {
		t.Fatalf("New: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	_, _, scoreErr := s.Score(ctx, "ref.yuv", "dis.yuv", "vmaf_v0.6.1")
	if scoreErr == nil {
		t.Fatal("expected error for pre-cancelled context")
	}
	if !strings.Contains(scoreErr.Error(), "context") {
		t.Errorf("expected error to mention context, got: %v", scoreErr)
	}
}
