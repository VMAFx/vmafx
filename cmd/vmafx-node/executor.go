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
// ADR-0782: OpenTelemetry tracing (SpanScoring, SpanFrameExtraction, SpanONNXInference).

package main

import (
	"context"
	"fmt"
	"log/slog"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/ai"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
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
// A nil log is replaced with slog.Default() so the executor never panics on
// its own logging calls — important because executor_test.go and the gRPC
// server both construct executors in code paths where a logger may not be
// readily available.
func NewExecutor(scorer *libvmaf.Scorer, aiReg *ai.Registry, backend string, log *slog.Logger) *Executor {
	if log == nil {
		log = slog.Default()
	}
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
	// Heuristic: if only the reference path is set AND a model is named,
	// treat as AI inference (the "reference" path doubles as the input
	// feature/tensor JSON for the AI pipeline — see executeAI).
	if sp.GetReference() != "" && sp.GetDistorted() == "" && sp.GetModel() != "" {
		return jobTypeAI
	}
	return jobTypeUnknown
}

// executeScoring runs the VMAF scoring pipeline for the given job.
// Emits two OTel spans: SpanScoring (outer) and SpanFrameExtraction (inner,
// wrapping the libvmaf.Score call that performs per-frame feature extraction).
// ADR-0782.
func (e *Executor) executeScoring(ctx context.Context, job *controllerv1.Job) ExecuteResult {
	if e.scorer == nil {
		return ExecuteResult{Error: fmt.Errorf("executor: scorer not initialised")}
	}
	sp := job.GetScoring()
	if sp == nil {
		return ExecuteResult{Error: fmt.Errorf("executor: job %s has no ScoringParams", job.GetId())}
	}

	// Outer span: full scoring pipeline.
	spanCtx, outerSpan := observability.StartSpan(ctx, observability.SpanScoring,
		observability.AttrJobID.String(job.GetId()),
		observability.AttrModel.String(sp.GetModel()),
		observability.AttrBackend.String(e.backend),
	)
	var outerErr error
	defer observability.EndSpan(outerSpan, &outerErr)

	e.log.InfoContext(spanCtx, "scoring job",
		slog.String("job_id", job.GetId()),
		slog.String("ref", sp.GetReference()),
		slog.String("dis", sp.GetDistorted()),
		slog.String("model", sp.GetModel()),
		slog.String("backend", e.backend),
	)

	// Inner span: per-frame feature extraction inside libvmaf.
	extractCtx, extractSpan := observability.StartSpan(spanCtx, observability.SpanFrameExtraction,
		observability.AttrJobID.String(job.GetId()),
	)
	var extractErr error
	defer observability.EndSpan(extractSpan, &extractErr)

	// Pass the executor context so a controller-driven job cancellation
	// (or worker shutdown) tears down the vmaf subprocess via
	// exec.CommandContext.  Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
	score, features, err := e.scorer.Score(extractCtx, sp.GetReference(), sp.GetDistorted(), sp.GetModel())
	if err != nil {
		outerErr = err
		extractErr = err
		return ExecuteResult{Error: fmt.Errorf("executor: score job %s: %w", job.GetId(), err)}
	}

	e.log.InfoContext(spanCtx, "scoring complete",
		slog.String("job_id", job.GetId()),
		slog.Float64("score", score),
	)
	return ExecuteResult{Score: score, Features: features}
}

// executeAI runs the AI inference pipeline for the given job.
// The model name is carried in scoring.model; the reference path points to
// an input feature JSON file.
// ADR-0782: SpanONNXInference traces the ONNX Runtime invocation path.
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

	// OTel span for ONNX inference path — ADR-0782.
	spanCtx, span := observability.StartSpan(ctx, observability.SpanONNXInference,
		observability.AttrJobID.String(job.GetId()),
		observability.AttrModel.String(modelName),
	)
	var spanErr error
	defer observability.EndSpan(span, &spanErr)

	// Stage 1: inputs are not carried in the proto yet; log and return unsupported.
	e.log.WarnContext(spanCtx, "AI job received but Stage 1 has no input transport",
		slog.String("job_id", job.GetId()),
		slog.String("model", modelName),
	)
	spanErr = fmt.Errorf("executor: AI job %s requires Stage 2 input transport (model=%s)",
		job.GetId(), modelName)
	return ExecuteResult{Error: spanErr}
}
