// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/executor.go — job dispatch for vmafx-node.
//
// The Executor receives a Job from the controller and dispatches to the
// correct pipeline:
//
//   SCORING  → encode (if needed) → libvmaf.Score → return result
//   AI       → ai.Infer → return result
//   COMPARE  → multiple encode+score combinations (Stage 2)
//
// Stage 1 implements SCORING only.  AI and COMPARE return "unsupported"
// so the controller can re-queue on a capable node.
//
// ADR-0713: vmafx-node Go worker binary.

package main

import (
	"context"
	"fmt"
	"log/slog"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/ai"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// jobType distinguishes job categories based on the scoring params.
// Stage 1 treats every job as SCORING; future job types extend this.
type jobType string

const (
	jobTypeScoring jobType = "SCORING"
	jobTypeAI      jobType = "AI"
	jobTypeCompare jobType = "COMPARE"
	jobTypeUnknown jobType = "UNKNOWN"
)

// ExecuteResult holds the outcome of a single job execution.
type ExecuteResult struct {
	// Score is the aggregate VMAF score (SCORING jobs).
	Score float64
	// Features is the per-feature score map (SCORING jobs).
	Features map[string]float64
	// AIOutputs is the inference output vector (AI jobs).
	AIOutputs []float64
	// Error is non-nil when the job failed.
	Error error
}

// Executor dispatches jobs to the correct pipeline.
type Executor struct {
	scorer  *libvmaf.Scorer
	aiReg   *ai.Registry
	backend string
	log     *slog.Logger
}

// NewExecutor creates an Executor.
// scorer and aiReg may be nil when the corresponding pipeline is not required.
func NewExecutor(scorer *libvmaf.Scorer, aiReg *ai.Registry, backend string, log *slog.Logger) *Executor {
	return &Executor{
		scorer:  scorer,
		aiReg:   aiReg,
		backend: backend,
		log:     log,
	}
}

// Execute runs a Job and returns its result.
func (e *Executor) Execute(ctx context.Context, job *controllerv1.Job) ExecuteResult {
	jt := classifyJob(job)
	e.log.InfoContext(ctx, "execute job",
		slog.String("job_id", job.GetId()),
		slog.String("type", string(jt)),
	)
	switch jt {
	case jobTypeScoring:
		return e.executeScoring(ctx, job)
	case jobTypeAI:
		return e.executeAI(ctx, job)
	case jobTypeCompare:
		return ExecuteResult{
			Error: fmt.Errorf("executor: COMPARE job type not supported in Stage 1 (job %s)", job.GetId()),
		}
	default:
		return ExecuteResult{
			Error: fmt.Errorf("executor: unknown job type for job %s", job.GetId()),
		}
	}
}

// classifyJob determines the job type from its scoring params.
// Stage 1 rule: any job with both Reference and Distorted set is SCORING.
func classifyJob(job *controllerv1.Job) jobType {
	if job.GetScoring() == nil {
		return jobTypeUnknown
	}
	sp := job.GetScoring()
	if sp.GetReference() != "" && sp.GetDistorted() != "" {
		return jobTypeScoring
	}
	// Heuristic: if only one path is set and no model is set, treat as AI.
	if sp.GetReference() != "" && sp.GetDistorted() == "" && sp.GetModel() != "" {
		return jobTypeAI
	}
	return jobTypeUnknown
}

// executeScoring runs the VMAF scoring pipeline for the given job.
func (e *Executor) executeScoring(ctx context.Context, job *controllerv1.Job) ExecuteResult {
	if e.scorer == nil {
		return ExecuteResult{Error: fmt.Errorf("executor: scorer not initialised")}
	}
	sp := job.GetScoring()
	if sp == nil {
		return ExecuteResult{Error: fmt.Errorf("executor: job %s has no ScoringParams", job.GetId())}
	}

	e.log.InfoContext(ctx, "scoring job",
		slog.String("job_id", job.GetId()),
		slog.String("ref", sp.GetReference()),
		slog.String("dis", sp.GetDistorted()),
		slog.String("model", sp.GetModel()),
		slog.String("backend", e.backend),
	)

	score, features, err := e.scorer.Score(sp.GetReference(), sp.GetDistorted(), sp.GetModel())
	if err != nil {
		return ExecuteResult{Error: fmt.Errorf("executor: score job %s: %w", job.GetId(), err)}
	}

	e.log.InfoContext(ctx, "scoring complete",
		slog.String("job_id", job.GetId()),
		slog.Float64("score", score),
	)
	return ExecuteResult{Score: score, Features: features}
}

// executeAI runs the AI inference pipeline for the given job.
// The model name is carried in scoring.model; the reference path points to
// an input feature JSON file.
func (e *Executor) executeAI(ctx context.Context, job *controllerv1.Job) ExecuteResult {
	if e.aiReg == nil {
		return ExecuteResult{Error: fmt.Errorf("executor: AI registry not initialised")}
	}
	sp := job.GetScoring()
	if sp == nil {
		return ExecuteResult{Error: fmt.Errorf("executor: job %s has no ScoringParams", job.GetId())}
	}
	modelName := sp.GetModel()
	if modelName == "" {
		return ExecuteResult{Error: fmt.Errorf("executor: AI job %s has no model name", job.GetId())}
	}

	// Stage 1: inputs are not carried in the proto yet; log and return unsupported.
	e.log.WarnContext(ctx, "AI job received but Stage 1 has no input transport",
		slog.String("job_id", job.GetId()),
		slog.String("model", modelName),
	)
	return ExecuteResult{
		Error: fmt.Errorf("executor: AI job %s requires Stage 2 input transport (model=%s)",
			job.GetId(), modelName),
	}
}
