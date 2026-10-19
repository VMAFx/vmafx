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
// The controller service delegates to cmd/vmafx-controller/backend (ADR-2350).
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
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/backend"
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
	s.metrics.ScoreDuration.Observe(elapsed)

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

// controllerServer implements controllerv1.VmafxControllerServer on a
// backend.Backend: the embedded SQLite queue (one replica) or the PostgreSQL
// store (any number of replicas, ADR-2350). Every call reads the tenant of its
// token once and hands it to the backend (ADR-1522).
type controllerServer struct {
	controllerv1.UnimplementedVmafxControllerServer
	backend backend.Backend
	scopes  *scoringScopes
	metrics *controllerMetrics
	log     *slog.Logger
}

func newControllerServer(
	b backend.Backend,
	scopes *scoringScopes,
	metrics *controllerMetrics,
	log *slog.Logger,
) *controllerServer {
	return &controllerServer{backend: b, scopes: scopes, metrics: metrics, log: log}
}

// SubmitJob enqueues a new scoring job.
// ADR-0782: vmafx.job.submit span traces the full enqueue path.
func (c *controllerServer) SubmitJob(ctx context.Context, req *controllerv1.SubmitJobRequest) (*controllerv1.SubmitJobResponse, error) {
	sp := req.GetScoring()
	if sp == nil {
		return nil, status.Errorf(codes.InvalidArgument, "scoring params are required")
	}
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

	id, err := c.backend.Submit(spanCtx, tenantID, backend.Scoring{
		Reference: sp.GetReference(), Distorted: sp.GetDistorted(), Model: sp.GetModel(), Backend: sp.GetBackend(),
	})
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
	j, err := c.ownJob(ctx, req.GetJobId())
	if err != nil {
		return nil, err
	}
	return jobToProto(j), nil
}

// ownJob reads a job of the caller's tenant. Another tenant's job is
// PermissionDenied and the refusal names no tenant (ADR-0794, ADR-1522).
func (c *controllerServer) ownJob(ctx context.Context, jobID string) (*backend.Job, error) {
	if jobID == "" {
		return nil, status.Errorf(codes.InvalidArgument, "job_id is required")
	}
	tenantID, err := callerTenant(ctx)
	if err != nil {
		return nil, err
	}
	j, err := c.backend.Get(ctx, tenantID, jobID)
	switch {
	case errors.Is(err, backend.ErrForbidden):
		return nil, status.Error(codes.PermissionDenied, "resource belongs to another tenant")
	case errors.Is(err, backend.ErrNotFound):
		return nil, status.Errorf(codes.NotFound, "job %q not found", jobID)
	case err != nil:
		return nil, status.Errorf(codes.Internal, "read job %q: %v", jobID, err)
	}
	return j, nil
}

// CancelJob cancels a pending or running job, scoped to the caller's tenant.
// A running job's node is told on its next Heartbeat and stops it (ADR-1567).
func (c *controllerServer) CancelJob(ctx context.Context, req *controllerv1.CancelJobRequest) (*controllerv1.CancelJobResponse, error) {
	j, err := c.ownJob(ctx, req.GetJobId())
	if err != nil {
		return nil, err
	}
	cancelled, err := c.backend.Cancel(ctx, j.TenantID, j.ID)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "cancel job %q: %v", j.ID, err)
	}
	if cancelled {
		c.metrics.jobFinished(j, backend.StatusCancelled)
	}
	return &controllerv1.CancelJobResponse{Ok: true, Message: "cancellation requested"}, nil
}

// StreamJobs is a server-streaming RPC that sends the current snapshot of the
// caller's tenant's jobs matching the optional status filter once, then
// closes (ADR-0962). The tenant is read once from the authenticated context
// and handed to the backend; no other tenant's job is read (ADR-1522).
func (c *controllerServer) StreamJobs(req *controllerv1.StreamJobsRequest, stream controllerv1.VmafxController_StreamJobsServer) error {
	tenantID, err := callerTenant(stream.Context())
	if err != nil {
		return err
	}
	statusFilter := make([]string, 0, len(req.GetStatusFilter()))
	for _, ps := range req.GetStatusFilter() {
		statusFilter = append(statusFilter, protoStatusToBackend(ps))
	}
	c.log.Info("StreamJobs snapshot requested", "tenant_id", tenantID, "filter", statusFilter)

	jobs, err := c.backend.List(stream.Context(), tenantID, statusFilter)
	if err != nil {
		c.log.Error("StreamJobs: list jobs failed", "error", err)
		return status.Errorf(codes.Internal, "StreamJobs: list jobs: %v", err)
	}
	for _, j := range jobs {
		if sendErr := stream.Send(jobToProto(j)); sendErr != nil {
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
	sess, err := c.backend.Register(ctx, tenantID, req.GetName(), protoCapToBackend(req.GetCapability()))
	if err != nil {
		return nil, status.Errorf(codes.Internal, "register node: %v", err)
	}
	c.log.Info("node registered via gRPC", "node_id", sess.NodeID, "name", req.GetName(), "tenant_id", tenantID)
	return &controllerv1.RegisterNodeResponse{NodeId: sess.NodeID, SessionToken: sess.Token}, nil
}

// maxHeartbeatJobs bounds running_job_ids: a node runs at most 64 jobs at
// once (cmd/vmafx-node maxNodeSlots), and the list sizes one SQL IN clause.
const maxHeartbeatJobs = 64

// Heartbeat processes a node keepalive ping. A session registered by another
// tenant answers ok=false, as an unknown one does. An accepted heartbeat names
// the node's running jobs that were cancelled, so the node stops them
// (ADR-1567); with the PostgreSQL backend it also renews their leases.
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
	sess := backend.Session{NodeID: req.GetNodeId(), Token: req.GetSessionToken()}
	cancelled, err := c.backend.Heartbeat(ctx, tenantID, sess, running)
	if errors.Is(err, backend.ErrInvalidSession) {
		return &controllerv1.HeartbeatResponse{Ok: false}, nil
	}
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
	sess := backend.Session{NodeID: req.GetNodeId(), Token: req.GetSessionToken()}
	job, err := c.backend.Pull(ctx, tenantID, sess, protoCapToBackend(req.GetCapability()))
	if errors.Is(err, backend.ErrInvalidSession) {
		return nil, status.Errorf(codes.PermissionDenied, "pull work: %v", err)
	}
	if err != nil {
		return nil, status.Errorf(codes.Internal, "pull work: %v", err)
	}
	if job == nil {
		return &controllerv1.PullWorkResponse{}, nil
	}
	c.metrics.jobAssigned(job)
	pj := jobToProto(job)
	// The node checks the inputs again where it reads them (ADR-1577).
	if pj.ScoringRoots, err = c.scopes.rootsFor(tenantID); err != nil {
		c.log.Error("PullWork: scoring roots", "tenant_id", tenantID, "error", err)
	}
	return &controllerv1.PullWorkResponse{Job: pj}, nil
}

// ReportResult records the final (or acknowledges a partial) outcome of a
// job. The node's session must belong to the caller's tenant and run the
// job; with the SQLite backend a new session of the tenant may also report a
// running job whose node has no live session (ADR-1524). Any other report is
// refused and changes nothing (ADR-1522).
func (c *controllerServer) ReportResult(ctx context.Context, req *controllerv1.ReportResultRequest) (*controllerv1.ReportResultResponse, error) {
	tenantID, err := callerTenant(ctx)
	if err != nil {
		return nil, err
	}
	sess := backend.Session{NodeID: req.GetNodeId(), Token: req.GetSessionToken()}
	if !req.GetFinal() {
		return c.acceptPartialResult(ctx, tenantID, sess, req.GetJobId())
	}
	finished, err := c.backend.Report(ctx, tenantID, sess, req.GetJobId(),
		backend.Result{Score: req.GetScore(), Features: req.GetFeatures(), Err: req.GetError()})
	if err := reportStatus(err, req); err != nil {
		return nil, err
	}
	if finished {
		c.recordFinished(ctx, req.GetJobId(), tenantID, req.GetError() != "")
	}
	return &controllerv1.ReportResultResponse{Ok: true}, nil
}

// reportStatus maps a backend refusal of a report onto its gRPC status.
func reportStatus(err error, req *controllerv1.ReportResultRequest) error {
	switch {
	case err == nil:
		return nil
	case errors.Is(err, backend.ErrInvalidSession):
		return status.Errorf(codes.PermissionDenied, "invalid session for node %q", req.GetNodeId())
	case errors.Is(err, backend.ErrNotAssigned), errors.Is(err, backend.ErrNotFound):
		return status.Errorf(codes.PermissionDenied, "job %q is not assigned to node %q", req.GetJobId(), req.GetNodeId())
	default:
		return status.Errorf(codes.Internal, "report result: %v", err)
	}
}

// recordFinished feeds the job metrics with a job this report moved to its
// terminal state. A repeated report of a finished job never reaches it, so a
// job is counted once.
func (c *controllerServer) recordFinished(ctx context.Context, jobID, tenantID string, failed bool) {
	finishedStatus := backend.StatusCompleted
	if failed {
		finishedStatus = backend.StatusFailed
	}
	job, err := c.backend.Get(ctx, tenantID, jobID)
	if err != nil {
		// Counted without its duration or score: the job could not be read back.
		c.log.Warn("finished job not readable for its metrics", "job_id", jobID, "error", err)
		job = &backend.Job{ID: jobID, TenantID: tenantID}
	}
	c.metrics.jobFinished(job, finishedStatus)
}

// acceptPartialResult acknowledges a partial result of a job the reporting
// session may report (backend.MayReport). No partial state is stored; the
// check keeps the call from confirming anything about another node's job.
func (c *controllerServer) acceptPartialResult(ctx context.Context, tenantID string, sess backend.Session, jobID string) (*controllerv1.ReportResultResponse, error) {
	ok, err := c.backend.MayReport(ctx, tenantID, sess, jobID)
	if errors.Is(err, backend.ErrInvalidSession) {
		return nil, status.Errorf(codes.PermissionDenied, "invalid session for node %q", sess.NodeID)
	}
	if err != nil {
		return nil, status.Errorf(codes.Internal, "report result: %v", err)
	}
	if !ok {
		return nil, status.Errorf(codes.PermissionDenied, "job %q is not assigned to node %q", jobID, sess.NodeID)
	}
	c.log.Debug("partial result received", "job_id", jobID, "node_id", sess.NodeID)
	return &controllerv1.ReportResultResponse{Ok: true}, nil
}

// ---------------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------------

// jobToProto converts a backend job to its proto representation.
func jobToProto(j *backend.Job) *controllerv1.Job {
	return &controllerv1.Job{
		Id:     j.ID,
		Status: backendStatusToProto(j.Status),
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

// backendStatusToProto converts a job status string to its proto enum value.
func backendStatusToProto(s string) controllerv1.JobStatus {
	switch s {
	case backend.StatusRunning:
		return controllerv1.JobStatus_RUNNING
	case backend.StatusCompleted:
		return controllerv1.JobStatus_COMPLETED
	case backend.StatusFailed:
		return controllerv1.JobStatus_FAILED
	case backend.StatusCancelled:
		return controllerv1.JobStatus_CANCELLED
	default:
		return controllerv1.JobStatus_PENDING
	}
}

// protoStatusToBackend converts a proto JobStatus enum to its status string.
// Used by StreamJobs to translate the caller's status filter.
func protoStatusToBackend(s controllerv1.JobStatus) string {
	switch s {
	case controllerv1.JobStatus_RUNNING:
		return backend.StatusRunning
	case controllerv1.JobStatus_COMPLETED:
		return backend.StatusCompleted
	case controllerv1.JobStatus_FAILED:
		return backend.StatusFailed
	case controllerv1.JobStatus_CANCELLED:
		return backend.StatusCancelled
	default:
		return backend.StatusPending
	}
}

// protoCapToBackend converts a proto NodeCapability.
func protoCapToBackend(c *controllerv1.NodeCapability) backend.Capability {
	if c == nil {
		return backend.Capability{}
	}
	return backend.Capability{
		GPUVendor:   c.GetGpuVendor(),
		Backends:    c.GetBackends(),
		Concurrency: int(c.GetConcurrency()),
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
