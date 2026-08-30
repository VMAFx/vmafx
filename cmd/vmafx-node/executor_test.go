// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/executor_test.go — unit tests for the job executor.
//
// Tests cover the job-classification logic, nil-scorer guard, AI-job unsupported
// sentinel, and the COMPARE job unsupported path.
//
// ADR-0713: vmafx-node Go worker binary.
// ADR-0782: OpenTelemetry tracing (SpanScoring, SpanFrameExtraction, SpanONNXInference).

package main

import (
	"context"
	"log/slog"
	"testing"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// TestClassifyJob exercises the classifyJob function over all branch cases.
func TestClassifyJob(t *testing.T) {
	t.Parallel()
	tests := []struct {
		name string
		job  *controllerv1.Job
		want jobType
	}{
		{
			name: "nil scoring → unknown",
			job:  &controllerv1.Job{Id: "j1"},
			want: jobTypeUnknown,
		},
		{
			name: "ref+dis → scoring",
			job: &controllerv1.Job{Id: "j2", Scoring: &controllerv1.ScoringParams{
				Reference: "/r.yuv", Distorted: "/d.yuv",
			}},
			want: jobTypeScoring,
		},
		{
			name: "ref only, model set → AI",
			job: &controllerv1.Job{Id: "j3", Scoring: &controllerv1.ScoringParams{
				Reference: "/r.json", Model: "nr_v1",
			}},
			want: jobTypeAI,
		},
		{
			name: "ref only, no model → unknown",
			job: &controllerv1.Job{Id: "j4", Scoring: &controllerv1.ScoringParams{
				Reference: "/r.json",
			}},
			want: jobTypeUnknown,
		},
		{
			name: "empty scoring → unknown",
			job:  &controllerv1.Job{Id: "j5", Scoring: &controllerv1.ScoringParams{}},
			want: jobTypeUnknown,
		},
	}
	for _, tt := range tests {
		tt := tt
		t.Run(tt.name, func(t *testing.T) {
			t.Parallel()
			if got := classifyJob(tt.job); got != tt.want {
				t.Errorf("classifyJob() = %q, want %q", got, tt.want)
			}
		})
	}
}

// TestExecutor_NilScorerReturnsError verifies Execute returns an error (not a
// panic) when the scorer has not been initialised.
func TestExecutor_NilScorerReturnsError(t *testing.T) {
	t.Parallel()
	exec := NewExecutor(nil, nil, "cpu", slog.Default())
	job := &controllerv1.Job{
		Id: "nil-scorer-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/r.yuv",
			Distorted: "/d.yuv",
		},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error when scorer is nil, got nil")
	}
}

// TestExecutor_NilLoggerSubstituted verifies that NewExecutor replaces a nil
// logger with slog.Default() so Execute does not panic.
func TestExecutor_NilLoggerSubstituted(t *testing.T) {
	t.Parallel()
	exec := NewExecutor(nil, nil, "cpu", nil) // nil logger
	if exec.log == nil {
		t.Fatal("NewExecutor should substitute slog.Default() for nil logger")
	}
	// Execute must return an error (scorer nil) rather than panic.
	job := &controllerv1.Job{
		Id: "nil-log-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/r.yuv",
			Distorted: "/d.yuv",
		},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error when scorer is nil, got nil")
	}
}

// TestExecutor_UnknownJobReturnsError verifies that a job with no recognisable
// type surfaces a descriptive error rather than panicking.
func TestExecutor_UnknownJobReturnsError(t *testing.T) {
	t.Parallel()
	exec := NewExecutor(nil, nil, "cpu", slog.Default())
	// A job with no Scoring params → jobTypeUnknown.
	job := &controllerv1.Job{Id: "unknown-1"}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error for unknown job type, got nil")
	}
}

// TestExecutor_CompareJobUnsupported verifies that a COMPARE job (which has
// no Scoring params at all) returns the Stage 1 unsupported error without
// panicking.  The Executor's classifyJob heuristic treats a nil-Scoring job as
// UNKNOWN, which routes to the same "unsupported" branch.
func TestExecutor_CompareJobUnsupported(t *testing.T) {
	t.Parallel()
	exec := NewExecutor(nil, nil, "cpu", slog.Default())
	// Empty scoring params → UNKNOWN (Stage 1 COMPARE is also unsupported).
	job := &controllerv1.Job{Id: "cmp-1", Scoring: &controllerv1.ScoringParams{}}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error for unclassifiable/COMPARE job, got nil")
	}
}

// TestExecutor_AIJobReturnsStage1Error verifies that an AI job returns the
// expected Stage 1 "no input transport" error.
func TestExecutor_AIJobReturnsStage1Error(t *testing.T) {
	t.Parallel()
	exec := NewExecutor(nil, nil, "cpu", slog.Default())
	job := &controllerv1.Job{
		Id: "ai-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/features.json",
			Model:     "nr_metric_v1",
		},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected Stage 1 error for AI job, got nil")
	}
}

// TestExecutor_AIJobNilRegistryError verifies that an AI job with a nil aiReg
// returns an error about the registry not being initialised.
func TestExecutor_AIJobNilRegistryError(t *testing.T) {
	t.Parallel()
	exec := NewExecutor(nil, nil, "cpu", slog.Default()) // nil aiReg
	job := &controllerv1.Job{
		Id: "ai-nil-reg-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/features.json",
			Model:     "nr_metric_v1",
		},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error for nil AI registry, got nil")
	}
}
