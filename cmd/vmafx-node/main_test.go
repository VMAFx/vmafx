// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/main_test.go — lifecycle tests for vmafx-node.
//
// Tests exercise the executor dispatch path via the mock controller server.
// Config-loading and node-registration tests were removed in the go-workspace
// audit (2026-05-29): the controller-registration path was replaced by the
// ffmpeg-probe / gRPC server model in the current main.go; those symbols
// (loadConfig, node, register, executeAndReport) no longer exist.
//
// ADR-0713: vmafx-node Go worker binary.

package main

import (
	"context"
	"log/slog"
	"testing"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/ai"
)

// ---------------------------------------------------------------------------
// Executor tests
// ---------------------------------------------------------------------------
//
// The previous mockController scaffolding (RegisterNode / Heartbeat /
// PullWork / ReportResult) was removed in the go-nilness audit (2026-05-30,
// staticcheck U1000) — it was orphaned when the controller-registration path
// was replaced by the ffmpeg-probe / gRPC server model documented in the
// header above. The remaining tests exercise the Executor directly.

func TestExecutor_NilScorer(t *testing.T) {
	t.Parallel()
	exec := NewExecutor(nil, nil, "cpu", slog.Default())
	job := &controllerv1.Job{
		Id: "test-job-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/ref.yuv",
			Distorted: "/dis.yuv",
		},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error when scorer is nil")
	}
}

func TestExecutor_UnsupportedCompareJob(t *testing.T) {
	t.Parallel()
	// A job with no reference+distorted does not classify as SCORING or AI.
	exec := NewExecutor(nil, nil, "cpu", slog.Default())
	job := &controllerv1.Job{
		Id:      "compare-job-1",
		Scoring: &controllerv1.ScoringParams{},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error for unclassifiable job")
	}
}

func TestExecutor_AIJobUnsupportedStage1(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	reg := ai.NewRegistry(dir)
	exec := NewExecutor(nil, reg, "cpu", slog.Default())
	// AI job: has reference but no distorted, model set.
	job := &controllerv1.Job{
		Id: "ai-job-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/features.json",
			Model:     "nr_metric_v1",
		},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error — Stage 1 AI jobs have no input transport")
	}
}
