// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/grpc_server.go — gRPC server implementation for vmafx-controller.
//
// Implements two gRPC services on a single port:
//   - VmafxScoring  (gen/go/vmafxv1) — direct scoring retained from Phase 4a
//   - VmafxController (gen/go/controller/controllerv1) — job queue + node API (Phase 4b.1)
//
// The scoring service delegates to pkg/libvmaf.
// The controller service delegates to cmd/vmafx-controller/{queue,nodes,scheduler}.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service (origin).
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-0782: OpenTelemetry tracing.

//go:build cgo

package main

import (
	"context"
	"fmt"
	"log/slog"
	"time"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/scheduler"
	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// ---------------------------------------------------------------------------
// VmafxScoring service (Phase 4a, retained)
// ---------------------------------------------------------------------------

// scoringServer implements vmafxv1.VmafxScoringServer.
type scoringServer struct {
	vmafxv1.UnimplementedVmafxScoringServer
	scorer  *libvmaf.Scorer
	metrics *observability.Metrics
	log     *slog.Logger
}

func newScoringServer(scorer *libvmaf.Scorer, metrics *observability.Metrics, log *slog.Logger) *scoringServer {
	return &scoringServer{scorer: scorer, metrics: metrics, log: log}
}

// Score implements VmafxScoring.Score.
func (s *scoringServer) Score(ctx context.Context, req *vmafxv1.ScoreRequest) (*vmafxv1.ScoreResponse, error) {
	s.metrics.ScoreRequests.Inc()
	start := time.Now()

	s.log.Info("grpc Score request",
		"reference", req.GetReference(),
		"distorted", req.GetDistorted(),
		"model", req.GetModel(),
	)

	if req.GetReference() == "" || req.GetDistorted() == "" {
		s.metrics.ScoreErrors.Inc()
		return nil, status.Errorf(codes.InvalidArgument, "reference and distorted paths are required")
	}

	// Pass the gRPC handler context so a client disconnect or RPC
	// deadline tears down the vmaf subprocess via exec.CommandContext.
	// Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
	score, features, err := s.scorer.Score(ctx, req.GetReference(), req.GetDistorted(), req.GetModel())
	elapsed := time.Since(start).Seconds()
	s.metrics.ScoreDuration.Observe(elapsed)

	if err != nil {
		s.metrics.ScoreErrors.Inc()
		s.log.Error("grpc Score failed", "error", err, "duration_s", elapsed)
		return nil, status.Errorf(codes.Internal, "scoring failed: %v", err)
	}

	protoFeatures := make(map[string]float64, len(features))
	for k, v := range features {
		protoFeatures[k] = v
	}

	s.log.Info("grpc Score completed", "score", fmt.Sprintf("%.4f", score), "duration_s", elapsed)
	return &vmafxv1.ScoreResponse{
		Score:    score,
		Features: protoFeatures,
	}, nil
}

// Health implements VmafxScoring.Health.
func (s *scoringServer) Health(_ context.Context, _ *vmafxv1.HealthRequest) (*vmafxv1.HealthResponse, error) {
	s.metrics.HealthRequests.Inc()
	return &vmafxv1.HealthResponse{Ok: true, Message: "ok"}, nil
}

// ---------------------------------------------------------------------------
// VmafxController service (Phase 4b.1)
// ---------------------------------------------------------------------------

// controllerServer implements controllerv1.VmafxControllerServer.
type controllerServer struct {
	controllerv1.UnimplementedVmafxControllerServer
	queue    queue.Queue
	registry *nodes.Registry
	sched    *scheduler.Scheduler
	metrics  *observability.Metrics
	log      *slog.Logger
}

func newControllerServer(
	q queue.Queue,
	r *nodes.Registry,
	s *scheduler.Scheduler,
	metrics *observability.Metrics,
	log *slog.Logger,
) *controllerServer {
	return &controllerServer{queue: q, registry: r, sched: s, metrics: metrics, log: log}
}

// SubmitJob enqueues a new scoring job.
// ADR-0782: vmafx.job.submit span traces the full enqueue path.
func (c *controllerServer) SubmitJob(ctx context.Context, req *controllerv1.SubmitJobRequest) (*controllerv1.SubmitJobResponse, error) {
	if req.GetScoring() == nil {
		return nil, status.Errorf(codes.InvalidArgument, "scoring params are required")
	}
	sp := req.GetScoring()
	if sp.GetReference() == "" || sp.GetDistorted() == "" {
		return nil, status.Errorf(codes.InvalidArgument, "reference and distorted paths are required")
	}

	// Extract tenant_id from the auth context (ADR-0794).
	tenantID := auth.TenantIDFromCtx(ctx)
	if tenantID == "" {
		return nil, status.Errorf(codes.Unauthenticated, "tenant_id not found in token")
	}

	spanCtx, span := observability.StartSpan(ctx, observability.SpanJobSubmit,
		observability.AttrModel.String(sp.GetModel()),
		observability.AttrBackend.String(sp.GetBackend()),
	)
	var spanErr error
	defer observability.EndSpan(span, &spanErr)

	j := &queue.Job{
		TenantID: tenantID,
		Scoring: queue.ScoringParams{
			Reference: sp.GetReference(),
			Distorted: sp.GetDistorted(),
			Model:     sp.GetModel(),
			Backend:   sp.GetBackend(),
		},
	}

	id, err := c.queue.Submit(spanCtx, j)
	if err != nil {
		spanErr = err
		c.log.Error("SubmitJob failed", "error", err)
		return nil, status.Errorf(codes.Internal, "submit job: %v", err)
	}

	span.SetAttributes(observability.AttrJobID.String(id))
	c.metrics.JobsSubmitted.Inc()
	c.log.Info("job submitted via gRPC", "job_id", id, "tenant_id", tenantID, "reference", sp.GetReference(), "backend", sp.GetBackend())
	return &controllerv1.SubmitJobResponse{JobId: id}, nil
}

// GetJob retrieves the current state of a job, scoped to the caller's tenant.
func (c *controllerServer) GetJob(ctx context.Context, req *controllerv1.GetJobRequest) (*controllerv1.Job, error) {
	if req.GetJobId() == "" {
		return nil, status.Errorf(codes.InvalidArgument, "job_id is required")
	}
	j, err := c.queue.Get(ctx, req.GetJobId())
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "job %q not found: %v", req.GetJobId(), err)
	}
	// Enforce tenant isolation: callers may not read jobs from other tenants (ADR-0794).
	if err := auth.AssertTenantOwns(ctx, j.TenantID); err != nil {
		return nil, err
	}
	return queueJobToProto(j), nil
}

// CancelJob requests cancellation of a pending or running job, scoped to caller's tenant.
func (c *controllerServer) CancelJob(ctx context.Context, req *controllerv1.CancelJobRequest) (*controllerv1.CancelJobResponse, error) {
	if req.GetJobId() == "" {
		return nil, status.Errorf(codes.InvalidArgument, "job_id is required")
	}
	// Fetch first so we can enforce tenant ownership before mutation (ADR-0794).
	j, err := c.queue.Get(ctx, req.GetJobId())
	if err != nil {
		return nil, status.Errorf(codes.NotFound, "job %q not found: %v", req.GetJobId(), err)
	}
	if err := auth.AssertTenantOwns(ctx, j.TenantID); err != nil {
		return nil, err
	}
	if err := c.queue.Cancel(ctx, req.GetJobId()); err != nil {
		return nil, status.Errorf(codes.Internal, "cancel job %q: %v", req.GetJobId(), err)
	}
	return &controllerv1.CancelJobResponse{Ok: true, Message: "cancellation requested"}, nil
}

// StreamJobs is a server-streaming RPC that pushes job updates.
// Phase 4b.1 implementation: streams the current snapshot of all jobs matching
// the optional status filter once, then closes.  A persistent push model is a
// Phase 4b.2 enhancement.
//
// ADR-0962: previously this was a no-op (silent return nil after a log line),
// causing callers to see an empty stream and incorrectly infer "no jobs queued".
// Now it performs a real SQLite snapshot via queue.ListAll and streams each job.
func (c *controllerServer) StreamJobs(req *controllerv1.StreamJobsRequest, stream controllerv1.VmafxController_StreamJobsServer) error {
	// Convert proto status filter values to queue status strings.
	protoFilter := req.GetStatusFilter()
	statusFilter := make([]string, 0, len(protoFilter))
	for _, ps := range protoFilter {
		statusFilter = append(statusFilter, protoStatusToQueue(ps))
	}

	c.log.Info("StreamJobs snapshot requested",
		"filter", statusFilter,
		"filter_len", len(statusFilter),
	)

	jobs, err := c.queue.ListAll(stream.Context(), statusFilter)
	if err != nil {
		c.log.Error("StreamJobs: ListAll failed", "error", err)
		return status.Errorf(codes.Internal, "StreamJobs: list jobs: %v", err)
	}

	for _, j := range jobs {
		if sendErr := stream.Send(queueJobToProto(j)); sendErr != nil {
			// Client disconnected mid-stream — treat as a normal close.
			c.log.Debug("StreamJobs: Send interrupted", "error", sendErr)
			return sendErr
		}
	}

	c.log.Info("StreamJobs snapshot complete", "sent", len(jobs))
	return nil
}

// RegisterNode handles vmafx-node registration.
func (c *controllerServer) RegisterNode(_ context.Context, req *controllerv1.RegisterNodeRequest) (*controllerv1.RegisterNodeResponse, error) {
	if req.GetName() == "" {
		return nil, status.Errorf(codes.InvalidArgument, "node name is required")
	}

	cap := protoCapToNodes(req.GetCapability())
	nodeID, token, err := c.registry.Register(req.GetName(), cap)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "register node: %v", err)
	}

	c.log.Info("node registered via gRPC", "node_id", nodeID, "name", req.GetName())
	return &controllerv1.RegisterNodeResponse{NodeId: nodeID, SessionToken: token}, nil
}

// Heartbeat processes a node keepalive ping.
func (c *controllerServer) Heartbeat(_ context.Context, req *controllerv1.HeartbeatRequest) (*controllerv1.HeartbeatResponse, error) {
	ok := c.registry.Heartbeat(req.GetNodeId(), req.GetSessionToken(), int(req.GetJobsRunning()))
	return &controllerv1.HeartbeatResponse{Ok: ok}, nil
}

// PullWork assigns the next matching job to the requesting node.
func (c *controllerServer) PullWork(ctx context.Context, req *controllerv1.PullWorkRequest) (*controllerv1.PullWorkResponse, error) {
	cap := protoCapToNodes(req.GetCapability())
	job, err := c.sched.Assign(ctx, req.GetNodeId(), req.GetSessionToken(), cap)
	if err != nil {
		return nil, status.Errorf(codes.PermissionDenied, "pull work: %v", err)
	}
	if job == nil {
		return &controllerv1.PullWorkResponse{}, nil
	}
	return &controllerv1.PullWorkResponse{Job: queueJobToProto(job)}, nil
}

// ReportResult records the terminal (or partial) outcome of a job.
func (c *controllerServer) ReportResult(ctx context.Context, req *controllerv1.ReportResultRequest) (*controllerv1.ReportResultResponse, error) {
	if !c.registry.ValidateSession(req.GetNodeId(), req.GetSessionToken()) {
		return nil, status.Errorf(codes.PermissionDenied, "invalid session for node %q", req.GetNodeId())
	}

	if !req.GetFinal() {
		// Partial result: no terminal state update needed for Phase 4b.1.
		c.log.Debug("partial result received", "job_id", req.GetJobId(), "node_id", req.GetNodeId())
		return &controllerv1.ReportResultResponse{Ok: true}, nil
	}

	result := &queue.JobResult{
		Score:    req.GetScore(),
		Features: req.GetFeatures(),
		Err:      req.GetError(),
	}
	if err := c.queue.ReportResult(ctx, req.GetJobId(), result); err != nil {
		return nil, status.Errorf(codes.Internal, "report result: %v", err)
	}

	if req.GetError() != "" {
		c.metrics.JobsFailed.Inc()
	} else {
		c.metrics.JobsCompleted.Inc()
	}
	return &controllerv1.ReportResultResponse{Ok: true}, nil
}

// ---------------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------------

// queueJobToProto converts a queue.Job to its proto representation.
func queueJobToProto(j *queue.Job) *controllerv1.Job {
	return &controllerv1.Job{
		Id:     j.ID,
		Status: queueStatusToProto(j.Status),
		Scoring: &controllerv1.ScoringParams{
			Reference: j.Scoring.Reference,
			Distorted: j.Scoring.Distorted,
			Model:     j.Scoring.Model,
			Backend:   j.Scoring.Backend,
		},
		AssignedNode: j.AssignedNode,
		Error:        j.Error,
		CreatedAt:    j.CreatedAt.Unix(),
		UpdatedAt:    j.UpdatedAt.Unix(),
	}
}

// queueStatusToProto converts a queue status string to its proto enum value.
func queueStatusToProto(s string) controllerv1.JobStatus {
	switch s {
	case queue.StatusRunning:
		return controllerv1.JobStatus_RUNNING
	case queue.StatusCompleted:
		return controllerv1.JobStatus_COMPLETED
	case queue.StatusFailed:
		return controllerv1.JobStatus_FAILED
	case queue.StatusCancelled:
		return controllerv1.JobStatus_CANCELLED
	default:
		return controllerv1.JobStatus_PENDING
	}
}

// protoStatusToQueue converts a proto JobStatus enum to its queue status string.
// Used by StreamJobs to translate the caller's status filter.
func protoStatusToQueue(s controllerv1.JobStatus) string {
	switch s {
	case controllerv1.JobStatus_RUNNING:
		return queue.StatusRunning
	case controllerv1.JobStatus_COMPLETED:
		return queue.StatusCompleted
	case controllerv1.JobStatus_FAILED:
		return queue.StatusFailed
	case controllerv1.JobStatus_CANCELLED:
		return queue.StatusCancelled
	default:
		return queue.StatusPending
	}
}

// protoCapToNodes converts a proto NodeCapability to a nodes.Capability.
func protoCapToNodes(cap *controllerv1.NodeCapability) nodes.Capability {
	if cap == nil {
		return nodes.Capability{}
	}
	return nodes.Capability{
		GPUVendor:   cap.GetGpuVendor(),
		Backends:    cap.GetBackends(),
		Concurrency: int(cap.GetConcurrency()),
	}
}

// ---------------------------------------------------------------------------
// Server construction note (ADR-1119)
// ---------------------------------------------------------------------------
//
// The controller no longer hand-rolls its gRPC server. golusoris grpc.Module
// provides the *grpc.Server (with OTel + logging + panic-recovery interceptors
// baked in) and owns the listener lifecycle (OnStart bind, OnStop GracefulStop).
// The JWT auth interceptors are injected via grpc.ProvideServerOption in main.go
// (golusoris#225). The scoringServer / controllerServer impls above are
// registered onto that server by an fx.Invoke. See main.go productionOptions.
