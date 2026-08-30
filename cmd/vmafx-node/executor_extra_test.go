// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/executor_extra_test.go — coverage-extending tests for
// executeScoring (non-nil scorer → error path) and executeAI (non-nil
// registry → Stage 1 unsupported body).
//
// All paths that require an external binary or kernel privilege are covered
// by supplying a minimal shell-script stub (for the vmaf binary) or a
// temporary model directory (for the AI registry). No real vmaf binary or
// ONNX Runtime is needed — the goal is line coverage on the error-handling
// and observability paths, not end-to-end scoring correctness.
//
// ADR-0713: vmafx-node Go worker binary.
// ADR-0782: OpenTelemetry tracing (SpanScoring, SpanFrameExtraction,
//           SpanONNXInference).

package main

import (
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"testing"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/ai"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// writeFakeVmafBin writes a minimal shell script that exits non-zero so that
// Scorer.Score returns an error without requiring a real vmaf installation.
func writeFakeVmafBin(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	script := "#!/bin/sh\nexit 1\n"
	binPath := filepath.Join(dir, "vmaf")
	if err := os.WriteFile(binPath, []byte(script), 0o700); err != nil {
		t.Fatalf("writeFakeVmafBin: %v", err)
	}
	return binPath
}

// newScorerWithFakeBin creates a *libvmaf.Scorer backed by a failing stub
// binary so tests can exercise the executeScoring error-handling body
// without a real vmaf installation.
func newScorerWithFakeBin(t *testing.T) *libvmaf.Scorer {
	t.Helper()
	binPath := writeFakeVmafBin(t)
	s, err := libvmaf.New(binPath, t.TempDir())
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	return s
}

// TestExecuteScoring_ScorerFailsReturnsError verifies that executeScoring
// surfaces a wrapped error (rather than panic or nil result) when the
// underlying scorer subprocess fails.
//
// Coverage target: executor.go executeScoring lines after nil-scorer guard,
// including the OTel span open/close, the Score call, error wrapping, and
// the log.InfoContext("scoring job") path.
func TestExecuteScoring_ScorerFailsReturnsError(t *testing.T) {
	t.Parallel()

	scorer := newScorerWithFakeBin(t)
	exec := NewExecutor(scorer, nil, "cpu", slog.Default())

	// A valid-looking scoring job whose binary invocation will fail because
	// the stub binary exits 1.
	job := &controllerv1.Job{
		Id: "score-fail-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/nonexistent/ref.yuv",
			Distorted: "/nonexistent/dis.yuv",
			Model:     "vmaf_v0.6.1",
		},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error from failing vmaf stub binary, got nil")
	}
	t.Logf("expected error: %v", result.Error)
}

// TestExecuteScoring_CancelledContextReturnsError verifies that a
// pre-cancelled context causes executeScoring to return an error
// (context-cancellation propagation via exec.CommandContext).
func TestExecuteScoring_CancelledContextReturnsError(t *testing.T) {
	t.Parallel()

	scorer := newScorerWithFakeBin(t)
	exec := NewExecutor(scorer, nil, "cpu", slog.Default())

	ctx, cancel := context.WithCancel(context.Background())
	cancel() // cancel before Execute

	job := &controllerv1.Job{
		Id: "score-cancel-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/nonexistent/ref.yuv",
			Distorted: "/nonexistent/dis.yuv",
			Model:     "vmaf_v0.6.1",
		},
	}
	result := exec.Execute(ctx, job)
	if result.Error == nil {
		t.Fatal("expected error on pre-cancelled context, got nil")
	}
	t.Logf("expected cancellation error: %v", result.Error)
}

// TestExecuteScoring_NilScoringParams verifies that executeScoring returns
// a clear error when ScoringParams is nil (defensive guard after the nil-
// scorer check).
func TestExecuteScoring_NilScoringParams(t *testing.T) {
	t.Parallel()

	scorer := newScorerWithFakeBin(t)
	exec := NewExecutor(scorer, nil, "cpu", slog.Default())

	// Force a job with a non-nil scorer but nil ScoringParams by calling
	// executeScoring directly (it is an unexported method on *Executor).
	// We reach it by building a job whose classifyJob returns SCORING but
	// whose Scoring field we nil out after constructing the executor.
	//
	// classifyJob uses job.GetScoring() which returns the proto field. The
	// sentinel check inside executeScoring (sp == nil) is a defensive
	// guard for direct calls; replicate that path via the internal method.
	result := exec.executeScoring(context.Background(), &controllerv1.Job{
		Id:      "score-nil-sp-1",
		Scoring: nil, // would normally be caught by classifyJob, not reaching SCORING
	})
	if result.Error == nil {
		t.Fatal("expected error for nil ScoringParams in executeScoring, got nil")
	}
	t.Logf("expected nil-params error: %v", result.Error)
}

// TestExecuteAI_WithNonNilRegistry verifies that executeAI with a properly
// constructed (non-nil) ai.Registry reaches the Stage 1 "no input transport"
// body and returns the expected sentinel error.
//
// Coverage target: executor.go executeAI lines 195–209 (OTel span, log.Warn,
// error return) — previously only the nil-registry guard on line 183 was hit.
func TestExecuteAI_WithNonNilRegistry(t *testing.T) {
	t.Parallel()

	// Use a temporary directory as the model dir; no .onnx files present.
	aiReg := ai.NewRegistry(t.TempDir())
	exec := NewExecutor(nil, aiReg, "cpu", slog.Default())

	job := &controllerv1.Job{
		Id: "ai-stage1-body-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/features.json",
			Model:     "nr_metric_v1",
		},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected Stage 1 error for AI job with non-nil registry, got nil")
	}
	t.Logf("expected Stage 1 error: %v", result.Error)
}

// TestExecuteAI_EmptyModelName verifies that executeAI returns a clear error
// when the model name is empty (guard before the OTel span is opened).
func TestExecuteAI_EmptyModelName(t *testing.T) {
	t.Parallel()

	aiReg := ai.NewRegistry(t.TempDir())
	exec := NewExecutor(nil, aiReg, "cpu", slog.Default())

	// Build a job that classifyJob routes to AI but has empty model name.
	// classifyJob requires Model != "" for AI classification, so this job
	// will actually land in UNKNOWN. Call executeAI directly instead.
	result := exec.executeAI(context.Background(), &controllerv1.Job{
		Id: "ai-no-model-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/features.json",
			Model:     "", // deliberately empty
		},
	})
	if result.Error == nil {
		t.Fatal("expected error for empty model name in executeAI, got nil")
	}
	t.Logf("expected empty-model error: %v", result.Error)
}

// TestExecuteAI_NilScoringParams verifies that executeAI returns a clear error
// when ScoringParams is nil (defensive guard).
func TestExecuteAI_NilScoringParams(t *testing.T) {
	t.Parallel()

	aiReg := ai.NewRegistry(t.TempDir())
	exec := NewExecutor(nil, aiReg, "cpu", slog.Default())

	result := exec.executeAI(context.Background(), &controllerv1.Job{
		Id:      "ai-nil-sp-1",
		Scoring: nil,
	})
	if result.Error == nil {
		t.Fatal("expected error for nil ScoringParams in executeAI, got nil")
	}
	t.Logf("expected nil-params error: %v", result.Error)
}
