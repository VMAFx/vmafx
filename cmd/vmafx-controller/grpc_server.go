// SPDX-License-Identifier: EUPL-1.2
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
// ADR-1522: every job read is scoped to the caller's tenant, and a node session
// to the tenant that registered it.

//go:build cgo

package main

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"maps"
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
	scopes  *scoringScopes
	metrics *observability.Metrics
	log     *slog.Logger
}

func newScoringServer(scorer *libvmaf.Scorer, scopes *scoringScopes, metrics *observability.Metrics, log *slog.Logger) *scoringServer {
	return &scoringServer{scorer: scorer, scopes: scopes, metrics: metrics, log: log}
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
	ref, dis, err := s.scopedInputs(ctx, req.GetReference(), req.GetDistorted())
	if err != nil {
		s.metrics.ScoreErrors.Inc()
		return nil, err
	}

	// Pass the gRPC handler context so a client disconnect or RPC
	// deadline tears down the vmaf subprocess via exec.CommandContext.
	// Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
	score, features, err := s.scorer.Score(ctx, ref, dis, req.GetModel())
	elapsed := time.Since(start).Seconds()
	s.metrics.ObserveScoreDuration(ctx, elapsed)

	if err != nil {
		s.metrics.ScoreErrors.Inc()
		s.log.Error("grpc Score failed", "error", err, "duration_s", elapsed)
		return nil, status.Errorf(codes.Internal, "scoring failed: %v", err)
	}

	s.metrics.ObserveScore(auth.TenantIDFromCtx(ctx), req.GetModel(), score)
	protoFeatures := make(map[string]float64, len(features))
	maps.Copy(protoFeatures, features)

	s.log.Info("grpc Score completed", "score", fmt.Sprintf("%.4f", score), "duration_s", elapsed)
	return &vmafxv1.ScoreResponse{
		Score:    score,
		Features: protoFeatures,
	}, nil
}

// scopedInputs admits both inputs only under the caller tenant's scoring
// roots and returns their real paths, the ones scored (ADR-1577).
func (s *scoringServer) scopedInputs(ctx context.Context, ref, dis string) (string, string, error) {
	tenantID, err := callerTenant(ctx)
	if err != nil {
		return "", "", err
	}
	return s.scopes.resolveInputs(tenantID, ref, dis)
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
	scopes   *scoringScopes
	metrics  *controllerMetrics
	log      *slog.Logger
}

func newControllerServer(
	q queue.Queue,
	r *nodes.Registry,
	s *scheduler.Scheduler,
	scopes *scoringScopes,
	metrics *controllerMetrics,
	log *slog.Logger,
) *controllerServer {
	return &controllerServer{queue: q, registry: r, sched: s, scopes: scopes, metrics: metrics, log: log}
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
	tenantID, err := callerTenant(ctx)
	if err != nil {
		return nil, err
	}
	// A node reads the inputs; the controller admits them under the tenant's
	// scoring roots here and the node resolves them again (ADR-1577).
	if err := c.scopes.checkInputs(tenantID, sp.GetReference(), sp.GetDistorted()); err != nil {
		return nil, err
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
	c.metrics.jobSubmitted(tenantID)
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

// CancelJob cancels a pending or running job, scoped to the caller's tenant.
// A running job's node is told on its next Heartbeat and stops it (ADR-1567).
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
	cancelled, err := c.queue.Cancel(ctx, req.GetJobId())
	if err != nil {
		return nil, status.Errorf(codes.Internal, "cancel job %q: %v", req.GetJobId(), err)
	}
	if cancelled {
		c.metrics.jobFinished(j, queue.StatusCancelled)
	}
	return &controllerv1.CancelJobResponse{Ok: true, Message: "cancellation requested"}, nil
}

// StreamJobs is a server-streaming RPC that pushes job updates.
// Phase 4b.1 implementation: streams the current snapshot of the caller's
// tenant's jobs matching the optional status filter once, then closes.  A
// persistent push model is a Phase 4b.2 enhancement and must keep the filter.
//
// ADR-0962: previously this was a no-op (silent return nil after a log line),
// causing callers to see an empty stream and incorrectly infer "no jobs queued".
// Now it performs a real SQLite snapshot via queue.ListByTenant and streams each
// job.
//
// ADR-1522: the tenant is read once from the authenticated context and passed
// into the SQL WHERE clause; no other tenant's job is read, let alone sent.
func (c *controllerServer) StreamJobs(req *controllerv1.StreamJobsRequest, stream controllerv1.VmafxController_StreamJobsServer) error {
	tenantID, err := callerTenant(stream.Context())
	if err != nil {
		return err
	}

	// Convert proto status filter values to queue status strings.
	protoFilter := req.GetStatusFilter()
	statusFilter := make([]string, 0, len(protoFilter))
	for _, ps := range protoFilter {
		statusFilter = append(statusFilter, protoStatusToQueue(ps))
	}

	c.log.Info("StreamJobs snapshot requested",
		"tenant_id", tenantID,
		"filter", statusFilter,
		"filter_len", len(statusFilter),
	)

	jobs, err := c.queue.ListByTenant(stream.Context(), tenantID, statusFilter)
	if err != nil {
		c.log.Error("StreamJobs: ListByTenant failed", "error", err)
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

// callerTenant returns the tenant of the authenticated caller, or an
// Unauthenticated status when the context carries none.
func callerTenant(ctx context.Context) (string, error) {
	tenantID := auth.TenantIDFromCtx(ctx)
	if tenantID == "" {
		return "", status.Errorf(codes.Unauthenticated, "tenant_id not found in token")
	}
	return tenantID, nil
}

// RegisterNode handles vmafx-node registration. The session belongs to the
// caller's tenant: only calls with a token of that tenant may use it, and the
// node is only given that tenant's jobs (ADR-1522).
func (c *controllerServer) RegisterNode(ctx context.Context, req *controllerv1.RegisterNodeRequest) (*controllerv1.RegisterNodeResponse, error) {
	if req.GetName() == "" {
		return nil, status.Errorf(codes.InvalidArgument, "node name is required")
	}
	tenantID, err := callerTenant(ctx)
	if err != nil {
		return nil, err
	}

	cap := protoCapToNodes(req.GetCapability())
	nodeID, token, err := c.registry.Register(req.GetName(), tenantID, cap)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "register node: %v", err)
	}

	c.log.Info("node registered via gRPC", "node_id", nodeID, "name", req.GetName(), "tenant_id", tenantID)
	return &controllerv1.RegisterNodeResponse{NodeId: nodeID, SessionToken: token}, nil
}

// maxHeartbeatJobs bounds running_job_ids: a node runs at most 64 jobs at
// once (cmd/vmafx-node maxNodeSlots), and the list sizes one SQL IN clause.
const maxHeartbeatJobs = 64

// Heartbeat processes a node keepalive ping. A session registered by another
// tenant answers ok=false, as an unknown one does. An accepted heartbeat names
// the node's running jobs that were cancelled, so the node stops them
// (ADR-1567).
func (c *controllerServer) Heartbeat(ctx context.Context, req *controllerv1.HeartbeatRequest) (*controllerv1.HeartbeatResponse, error) {
	tenantID, err := callerTenant(ctx)
	if err != nil {
		return nil, err
	}
	running := req.GetRunningJobIds()
	if len(running) > maxHeartbeatJobs {
		return nil, status.Errorf(codes.InvalidArgument,
			"running_job_ids has %d entries, at most %d are accepted", len(running), maxHeartbeatJobs)
	}
	if !c.registry.Heartbeat(req.GetNodeId(), req.GetSessionToken(), tenantID, int(req.GetJobsRunning())) {
		return &controllerv1.HeartbeatResponse{Ok: false}, nil
	}
	cancelled, err := c.queue.CancelledAmong(ctx, tenantID, running)
	if err != nil {
		c.log.Error("Heartbeat: look up cancelled jobs", "node_id", req.GetNodeId(), "error", err)
		return nil, status.Errorf(codes.Internal, "heartbeat: look up cancelled jobs: %v", err)
	}
	if len(cancelled) > 0 {
		c.log.Info("telling node to stop cancelled jobs", "node_id", req.GetNodeId(), "jobs", cancelled)
	}
	return &controllerv1.HeartbeatResponse{Ok: true, CancelJobIds: cancelled}, nil
}

// PullWork assigns the next matching job of the caller's tenant to the
// requesting node.
func (c *controllerServer) PullWork(ctx context.Context, req *controllerv1.PullWorkRequest) (*controllerv1.PullWorkResponse, error) {
	tenantID, err := callerTenant(ctx)
	if err != nil {
		return nil, err
	}
	cap := protoCapToNodes(req.GetCapability())
	job, err := c.sched.Assign(ctx, req.GetNodeId(), req.GetSessionToken(), tenantID, cap)
	if err != nil {
		return nil, status.Errorf(codes.PermissionDenied, "pull work: %v", err)
	}
	if job == nil {
		return &controllerv1.PullWorkResponse{}, nil
	}
	c.metrics.jobAssigned(job)
	pj := queueJobToProto(job)
	// The node checks the inputs again where it reads them (ADR-1577).
	if pj.ScoringRoots, err = c.scopes.rootsFor(tenantID); err != nil {
		c.log.Error("PullWork: scoring roots", "tenant_id", tenantID, "error", err)
	}
	return &controllerv1.PullWorkResponse{Job: pj}, nil
}

// ReportResult records the terminal (or partial) outcome of a job. The node's
// session must belong to the caller's tenant, and the job must be assigned to
// the node, or be a running job of the same tenant whose node has no live
// session (a node that registered again reports what it finished under its
// old session, ADR-1524). A report for any other job is refused and changes
// nothing (ADR-1522).
func (c *controllerServer) ReportResult(ctx context.Context, req *controllerv1.ReportResultRequest) (*controllerv1.ReportResultResponse, error) {
	tenantID, err := callerTenant(ctx)
	if err != nil {
		return nil, err
	}
	if !c.registry.ValidateSession(req.GetNodeId(), req.GetSessionToken(), tenantID) {
		return nil, status.Errorf(codes.PermissionDenied, "invalid session for node %q", req.GetNodeId())
	}

	report := queue.Report{
		NodeID:   req.GetNodeId(),
		TenantID: tenantID,
		JobID:    req.GetJobId(),
		Result:   &queue.JobResult{Score: req.GetScore(), Features: req.GetFeatures(), Err: req.GetError()},
		Orphaned: c.nodeOrphaned,
	}
	if !req.GetFinal() {
		return c.acceptPartialResult(ctx, report)
	}
	finished, err := c.queue.ReportResult(ctx, report)
	if err != nil {
		if errors.Is(err, queue.ErrNotAssigned) {
			return nil, status.Errorf(codes.PermissionDenied,
				"job %q is not assigned to node %q", req.GetJobId(), req.GetNodeId())
		}
		return nil, status.Errorf(codes.Internal, "report result: %v", err)
	}
	if finished {
		c.recordFinished(ctx, req.GetJobId(), tenantID, req.GetError() != "")
	}
	return &controllerv1.ReportResultResponse{Ok: true}, nil
}

// recordFinished feeds the job metrics with a job this report moved to its
// terminal state. A repeated report of a finished job never reaches it, so a
// job is counted once.
func (c *controllerServer) recordFinished(ctx context.Context, jobID, tenantID string, failed bool) {
	finishedStatus := queue.StatusCompleted
	if failed {
		finishedStatus = queue.StatusFailed
	}
	job, err := c.queue.Get(ctx, jobID)
	if err != nil {
		// Counted without its duration or score: the job could not be read back.
		c.log.Warn("finished job not readable for its metrics", "job_id", jobID, "error", err)
		job = &queue.Job{ID: jobID, TenantID: tenantID}
	}
	c.metrics.jobFinished(job, finishedStatus)
}

// acceptPartialResult acknowledges a partial result of a job the reporting
// node may report (queue.MayReport). Phase 4b.1 stores no partial state; the
// check keeps the call from confirming anything about another node's job.
func (c *controllerServer) acceptPartialResult(ctx context.Context, r queue.Report) (*controllerv1.ReportResultResponse, error) {
	if !c.queue.MayReport(ctx, r) {
		return nil, status.Errorf(codes.PermissionDenied,
			"job %q is not assigned to node %q", r.JobID, r.NodeID)
	}
	c.log.Debug("partial result received", "job_id", r.JobID, "node_id", r.NodeID)
	return &controllerv1.ReportResultResponse{Ok: true}, nil
}

// nodeOrphaned reports whether a node ID has no live session: the controller
// restarted or evicted it. Node IDs are never reissued, so a node that is
// orphaned stays orphaned.
func (c *controllerServer) nodeOrphaned(nodeID string) bool {
	_, live := c.registry.Get(nodeID)
	return !live
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
		// FinalScore mirrors the aggregate VMAF score recorded by ReportResult.
		// The vmafx-operator copies it into VmafxJob.Status.Score; omitting it
		// here made every Succeeded job report a score of 0 (round-3 R3-1).
		FinalScore: j.Score,
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
