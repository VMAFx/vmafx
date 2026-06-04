// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// executor_test.go — unit tests for the Executor job dispatcher.
// Tests cover classifyJob and the nil-scorer / nil-aiReg error paths
// without requiring a real vmaf binary or GPU (ADR-1050).

package main

import (
	"context"
	"log/slog"
	"testing"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// makeJob builds a minimal Job proto with the given ref/dis/model fields.
func makeJob(id, ref, dis, model string) *controllerv1.Job {
	sp := &controllerv1.ScoringParams{
		Reference: ref,
		Distorted: dis,
		Model:     model,
	}
	return &controllerv1.Job{Id: id, Scoring: sp}
}

// makeJobNoScoring builds a Job with no ScoringParams.
func makeJobNoScoring(id string) *controllerv1.Job {
	return &controllerv1.Job{Id: id}
}

// ---------------------------------------------------------------------------
// classifyJob
// ---------------------------------------------------------------------------

func TestClassifyJobScoring(t *testing.T) {
	t.Parallel()
	j := makeJob("j1", "/ref.yuv", "/dis.yuv", "vmaf_v0.6.1")
	if got := classifyJob(j); got != jobTypeScoring {
		t.Errorf("expected SCORING, got %s", got)
	}
}

func TestClassifyJobAI(t *testing.T) {
	t.Parallel()
	// AI heuristic: reference set + distorted empty + model set.
	j := makeJob("j2", "/features.json", "", "tiny_v1")
	if got := classifyJob(j); got != jobTypeAI {
		t.Errorf("expected AI, got %s", got)
	}
}

func TestClassifyJobUnknownNilScoring(t *testing.T) {
	t.Parallel()
	j := makeJobNoScoring("j3")
	if got := classifyJob(j); got != jobTypeUnknown {
		t.Errorf("expected UNKNOWN, got %s", got)
	}
}

func TestClassifyJobUnknownNoRef(t *testing.T) {
	t.Parallel()
	j := makeJob("j4", "", "", "")
	if got := classifyJob(j); got != jobTypeUnknown {
		t.Errorf("expected UNKNOWN for empty ref/dis, got %s", got)
	}
}

// ---------------------------------------------------------------------------
// Executor.Execute — error paths that do not require real scorer/aiReg
// ---------------------------------------------------------------------------

var testLog = slog.Default()

func TestExecuteNilScorerScoring(t *testing.T) {
	t.Parallel()
	ex := NewExecutor(nil, nil, "cpu", testLog)
	j := makeJob("j10", "/ref.yuv", "/dis.yuv", "vmaf_v0.6.1")
	res := ex.Execute(context.Background(), j)
	if res.Error == nil {
		t.Error("expected error when scorer is nil")
	}
}

func TestExecuteNilAIRegistryAI(t *testing.T) {
	t.Parallel()
	ex := NewExecutor(nil, nil, "cpu", testLog)
	j := makeJob("j11", "/features.json", "", "tiny_v1")
	res := ex.Execute(context.Background(), j)
	if res.Error == nil {
		t.Error("expected error when aiReg is nil")
	}
}

func TestExecuteCompareNotSupported(t *testing.T) {
	t.Parallel()
	ex := NewExecutor(nil, nil, "cpu", testLog)
	// Build a job that classifies as COMPARE — override via a direct Execute
	// call where the classification produces COMPARE. Currently classifyJob
	// returns SCORING or AI or UNKNOWN; COMPARE is reserved for Stage 2.
	// We verify the unknown path directly.
	j := makeJobNoScoring("j12")
	res := ex.Execute(context.Background(), j)
	if res.Error == nil {
		t.Error("expected error for UNKNOWN job type")
	}
}

func TestExecuteScoringNilScoringParams(t *testing.T) {
	t.Parallel()
	ex := NewExecutor(nil, nil, "cpu", testLog)
	j := makeJobNoScoring("j13")
	// Call executeScoring directly (not exported but accessible in same package).
	res := ex.executeScoring(context.Background(), j)
	if res.Error == nil {
		t.Error("expected error when scorer is nil")
	}
}
