// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/grpc_server_test.go — unit tests for the
// controllerServer + scoringServer gRPC implementations.
//
// Tests use real SQLite-backed queue instances to avoid mock drift.  The
// StreamJobs stream is exercised through a minimal in-process mock that
// satisfies the VmafxController_StreamJobsServer interface without a live
// gRPC connection.  We do not stand up a real grpc.Server; the per-RPC
// handler methods are called directly with synthetic request structs.
//
// ADR-0703 / ADR-0711 — Phase 4b.1 controller scope.
// ADR-0962: StreamJobs was a no-op; these tests pin the fixed snapshot
//           behaviour.

//go:build cgo

package main

import (
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/prometheus/client_golang/prometheus"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/scheduler"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// testTenantCtx returns a context with a "test-tenant" auth context injected.
// Used by tests that call gRPC handler methods directly (bypassing the
// interceptor) so SubmitJob / GetJob / CancelJob can find a tenant_id.
func testTenantCtx() context.Context {
	return auth.ContextWithClaims(context.Background(), auth.Claims{
		Subject:  "test-user",
		TenantID: "test-tenant",
		Roles:    []string{auth.RoleAdmin},
	})
}

// ---------------------------------------------------------------------------
// In-process mock stream — exercises StreamJobs and captures sent messages.
// ---------------------------------------------------------------------------

// mockStreamJobsServer implements controllerv1.VmafxController_StreamJobsServer
// for unit testing without a live gRPC connection.
type mockStreamJobsServer struct {
	grpc.ServerStream
	ctx  context.Context
	sent []*controllerv1.Job
}

func (m *mockStreamJobsServer) Send(j *controllerv1.Job) error {
	m.sent = append(m.sent, j)
	return nil
}

func (m *mockStreamJobsServer) Context() context.Context { return m.ctx }

// Satisfy the grpc.ServerStream methods that are not provided by the zero-value embed.
func (m *mockStreamJobsServer) SetHeader(metadata.MD) error  { return nil }
func (m *mockStreamJobsServer) SendHeader(metadata.MD) error { return nil }
func (m *mockStreamJobsServer) SetTrailer(metadata.MD)       {}
func (m *mockStreamJobsServer) SendMsg(any) error            { return nil }
func (m *mockStreamJobsServer) RecvMsg(any) error            { return nil }

// dummyStreamServer is the minimal stub required by StreamJobs's signature for
// tests that do not introspect the sent messages.  Send swallows the payload;
// Context returns a background context so the handler's cancellation check is
// safe to call.
type dummyStreamServer struct {
	controllerv1.VmafxController_StreamJobsServer
}

func (d *dummyStreamServer) Send(*controllerv1.Job) error { return nil }
func (d *dummyStreamServer) Context() context.Context     { return context.Background() }
func (d *dummyStreamServer) SetHeader(metadata.MD) error  { return nil }
func (d *dummyStreamServer) SendHeader(metadata.MD) error { return nil }
func (d *dummyStreamServer) SetTrailer(metadata.MD)       {}
func (d *dummyStreamServer) SendMsg(any) error            { return nil }
func (d *dummyStreamServer) RecvMsg(any) error            { return nil }

// ---------------------------------------------------------------------------
// Test fixtures
// ---------------------------------------------------------------------------

func newTestControllerServer(t *testing.T) *controllerServer {
	t.Helper()
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "test.db")
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))

	q, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("queue.New: %v", err)
	}
	t.Cleanup(func() { _ = q.Close() })

	r := nodes.NewRegistry(log)
	t.Cleanup(func() { r.Close() })

	s := scheduler.New(q, r, log)
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)

	return newControllerServer(q, r, s, metrics, log)
}

func newTestStream(ctx context.Context) *mockStreamJobsServer {
	return &mockStreamJobsServer{ctx: ctx}
}

// submitTestJob submits a job directly to the queue inside a controllerServer.
func submitTestJob(t *testing.T, cs *controllerServer, ref, dis string) string {
	t.Helper()
	id, err := cs.queue.Submit(context.Background(), &queue.Job{
		Scoring: queue.ScoringParams{Reference: ref, Distorted: dis},
	})
	if err != nil {
		t.Fatalf("Submit: %v", err)
	}
	return id
}

// grpcFixture holds the wired controllerServer + its backing deps for tests
// that need direct access to the registry / queue / scheduler / metrics.
type grpcFixture struct {
	srv      *controllerServer
	queue    *queue.SQLiteQueue
	registry *nodes.Registry
	sched    *scheduler.Scheduler
	metrics  *observability.Metrics
}

func newGRPCFixture(t *testing.T) *grpcFixture {
	t.Helper()
	dir := t.TempDir()
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	q, err := queue.New(filepath.Join(dir, "grpc.db"), log)
	if err != nil {
		t.Fatalf("queue.New: %v", err)
	}
	t.Cleanup(func() { _ = q.Close() })

	reg := nodes.NewRegistry(log)
	t.Cleanup(func() { reg.Close() })
	sch := scheduler.New(q, reg, log)
	reg2 := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg2)

	return &grpcFixture{
		srv:      newControllerServer(q, reg, sch, metrics, log),
		queue:    q,
		registry: reg,
		sched:    sch,
		metrics:  metrics,
	}
}

// codeOf extracts the grpc status code from an error, defaulting to OK.
func codeOf(err error) codes.Code {
	st, _ := status.FromError(err)
	return st.Code()
}

// ---------------------------------------------------------------------------
// Tests — Bug B.3: StreamJobs must not be a no-op
// ---------------------------------------------------------------------------

// TestStreamJobs_EmptyQueue_ReturnsOK verifies that StreamJobs on an empty
// queue completes with no error and sends zero messages.
func TestStreamJobs_EmptyQueue_ReturnsOK(t *testing.T) {
	cs := newTestControllerServer(t)
	stream := newTestStream(context.Background())

	err := cs.StreamJobs(&controllerv1.StreamJobsRequest{}, stream)
	if err != nil {
		t.Fatalf("StreamJobs on empty queue: unexpected error: %v", err)
	}
	if len(stream.sent) != 0 {
		t.Errorf("sent: got %d messages, want 0", len(stream.sent))
	}
}

// TestStreamJobs_WithJobs_StreamsSnapshot verifies that 3 submitted jobs are
// all streamed by StreamJobs (no filter) before it closes.
func TestStreamJobs_WithJobs_StreamsSnapshot(t *testing.T) {
	cs := newTestControllerServer(t)
	stream := newTestStream(context.Background())

	// Submit 3 jobs.
	ids := make(map[string]bool)
	for i := range 3 {
		id := submitTestJob(t, cs, "/ref.yuv", "/dis.yuv")
		ids[id] = true
		_ = i
	}

	err := cs.StreamJobs(&controllerv1.StreamJobsRequest{}, stream)
	if err != nil {
		t.Fatalf("StreamJobs: unexpected error: %v", err)
	}
	if len(stream.sent) != 3 {
		t.Fatalf("sent: got %d messages, want 3", len(stream.sent))
	}

	// Verify the IDs match what we submitted.
	for _, j := range stream.sent {
		if !ids[j.GetId()] {
			t.Errorf("received unexpected job ID %q", j.GetId())
		}
	}
}

// ---------------------------------------------------------------------------
// SubmitJob
// ---------------------------------------------------------------------------

func TestSubmitJob_HappyPath(t *testing.T) {
	f := newGRPCFixture(t)
	req := &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{
			Reference: "/r.yuv",
			Distorted: "/d.yuv",
			Model:     "vmaf_v0.6.1",
			Backend:   "cpu",
		},
	}
	resp, err := f.srv.SubmitJob(testTenantCtx(), req)
	if err != nil {
		t.Fatalf("SubmitJob: %v", err)
	}
	if resp.GetJobId() == "" {
		t.Error("empty job_id in response")
	}
	if f.queue.PendingCount() != 1 {
		t.Errorf("PendingCount: got %d, want 1", f.queue.PendingCount())
	}
}

func TestSubmitJob_NilScoringRejected(t *testing.T) {
	f := newGRPCFixture(t)
	_, err := f.srv.SubmitJob(context.Background(), &controllerv1.SubmitJobRequest{})
	if err == nil {
		t.Fatal("expected error for nil scoring")
	}
	if got := codeOf(err); got != codes.InvalidArgument {
		t.Errorf("code: got %v, want InvalidArgument", got)
	}
}

func TestSubmitJob_MissingPathsRejected(t *testing.T) {
	f := newGRPCFixture(t)
	req := &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: "", Distorted: ""},
	}
	_, err := f.srv.SubmitJob(context.Background(), req)
	if err == nil {
		t.Fatal("expected error for empty paths")
	}
	if got := codeOf(err); got != codes.InvalidArgument {
		t.Errorf("code: got %v, want InvalidArgument", got)
	}
}

// ---------------------------------------------------------------------------
// GetJob
// ---------------------------------------------------------------------------

func TestGetJob_HappyPath(t *testing.T) {
	f := newGRPCFixture(t)
	ctx := testTenantCtx()
	subResp, err := f.srv.SubmitJob(ctx, &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv", Backend: "cuda"},
	})
	if err != nil {
		t.Fatalf("Submit: %v", err)
	}
	got, err := f.srv.GetJob(ctx, &controllerv1.GetJobRequest{JobId: subResp.GetJobId()})
	if err != nil {
		t.Fatalf("GetJob: %v", err)
	}
	if got.GetId() != subResp.GetJobId() {
		t.Errorf("id: got %q, want %q", got.GetId(), subResp.GetJobId())
	}
	if got.GetStatus() != controllerv1.JobStatus_PENDING {
		t.Errorf("status: got %v, want PENDING", got.GetStatus())
	}
	if got.GetScoring().GetBackend() != "cuda" {
		t.Errorf("backend round-trip: got %q, want cuda", got.GetScoring().GetBackend())
	}
}

func TestGetJob_EmptyIDRejected(t *testing.T) {
	f := newGRPCFixture(t)
	_, err := f.srv.GetJob(context.Background(), &controllerv1.GetJobRequest{JobId: ""})
	if codeOf(err) != codes.InvalidArgument {
		t.Errorf("expected InvalidArgument, got %v", codeOf(err))
	}
}

func TestGetJob_MissingReturnsNotFound(t *testing.T) {
	f := newGRPCFixture(t)
	_, err := f.srv.GetJob(context.Background(), &controllerv1.GetJobRequest{JobId: "ghost-id"})
	if codeOf(err) != codes.NotFound {
		t.Errorf("expected NotFound, got %v", codeOf(err))
	}
}

// ---------------------------------------------------------------------------
// CancelJob
// ---------------------------------------------------------------------------

func TestCancelJob_HappyPath(t *testing.T) {
	f := newGRPCFixture(t)
	ctx := testTenantCtx()
	sub, err := f.srv.SubmitJob(ctx, &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv"},
	})
	if err != nil {
		t.Fatalf("SubmitJob: %v", err)
	}
	resp, err := f.srv.CancelJob(ctx, &controllerv1.CancelJobRequest{JobId: sub.GetJobId()})
	if err != nil {
		t.Fatalf("CancelJob: %v", err)
	}
	if !resp.GetOk() {
		t.Error("ok: got false, want true")
	}
	if resp.GetMessage() == "" {
		t.Error("expected non-empty message")
	}
}

func TestCancelJob_EmptyIDRejected(t *testing.T) {
	f := newGRPCFixture(t)
	_, err := f.srv.CancelJob(context.Background(), &controllerv1.CancelJobRequest{JobId: ""})
	if codeOf(err) != codes.InvalidArgument {
		t.Errorf("expected InvalidArgument, got %v", codeOf(err))
	}
}

// ---------------------------------------------------------------------------
// StreamJobs filter coverage (ADR-0962 + Phase 4b.1 snapshot)
// ---------------------------------------------------------------------------

func TestStreamJobs_DummyStream_ReturnsNil(t *testing.T) {
	f := newGRPCFixture(t)
	err := f.srv.StreamJobs(&controllerv1.StreamJobsRequest{StatusFilter: []controllerv1.JobStatus{controllerv1.JobStatus_PENDING}}, &dummyStreamServer{})
	if err != nil {
		t.Errorf("StreamJobs snapshot with dummy stream should return nil, got %v", err)
	}
}

// ---------------------------------------------------------------------------
// RegisterNode / Heartbeat
// ---------------------------------------------------------------------------

func TestRegisterNode_HappyPath(t *testing.T) {
	f := newGRPCFixture(t)
	req := &controllerv1.RegisterNodeRequest{
		Name: "test-node-1",
		Capability: &controllerv1.NodeCapability{
			GpuVendor:   "nvidia",
			Backends:    []string{"cuda", "cpu"},
			Concurrency: 4,
		},
	}
	resp, err := f.srv.RegisterNode(context.Background(), req)
	if err != nil {
		t.Fatalf("RegisterNode: %v", err)
	}
	if resp.GetNodeId() == "" {
		t.Error("empty node_id")
	}
	if resp.GetSessionToken() == "" {
		t.Error("empty session_token")
	}
	if f.registry.Count() != 1 {
		t.Errorf("Registry.Count: got %d, want 1", f.registry.Count())
	}
}

func TestRegisterNode_EmptyNameRejected(t *testing.T) {
	f := newGRPCFixture(t)
	_, err := f.srv.RegisterNode(context.Background(), &controllerv1.RegisterNodeRequest{Name: ""})
	if codeOf(err) != codes.InvalidArgument {
		t.Errorf("expected InvalidArgument, got %v", codeOf(err))
	}
}

func TestRegisterNode_NilCapHandled(t *testing.T) {
	f := newGRPCFixture(t)
	_, err := f.srv.RegisterNode(context.Background(), &controllerv1.RegisterNodeRequest{Name: "n", Capability: nil})
	if err != nil {
		t.Fatalf("RegisterNode with nil cap: %v", err)
	}
}

func TestHeartbeat_HappyPath(t *testing.T) {
	f := newGRPCFixture(t)
	reg, _ := f.srv.RegisterNode(context.Background(), &controllerv1.RegisterNodeRequest{
		Name:       "hb-node",
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})
	hb, err := f.srv.Heartbeat(context.Background(), &controllerv1.HeartbeatRequest{
		NodeId:       reg.GetNodeId(),
		SessionToken: reg.GetSessionToken(),
		JobsRunning:  2,
	})
	if err != nil {
		t.Fatalf("Heartbeat: %v", err)
	}
	if !hb.GetOk() {
		t.Error("Heartbeat ok: got false, want true")
	}
}

func TestHeartbeat_BadSessionRejected(t *testing.T) {
	f := newGRPCFixture(t)
	reg, _ := f.srv.RegisterNode(context.Background(), &controllerv1.RegisterNodeRequest{
		Name:       "hb-node",
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})
	hb, _ := f.srv.Heartbeat(context.Background(), &controllerv1.HeartbeatRequest{
		NodeId:       reg.GetNodeId(),
		SessionToken: "wrong-token",
	})
	if hb.GetOk() {
		t.Error("Heartbeat with wrong token should return ok=false")
	}
}

// ---------------------------------------------------------------------------
// PullWork
// ---------------------------------------------------------------------------

func TestPullWork_NoJobReturnsEmpty(t *testing.T) {
	f := newGRPCFixture(t)
	reg, _ := f.srv.RegisterNode(context.Background(), &controllerv1.RegisterNodeRequest{
		Name:       "pw-node",
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})
	resp, err := f.srv.PullWork(context.Background(), &controllerv1.PullWorkRequest{
		NodeId:       reg.GetNodeId(),
		SessionToken: reg.GetSessionToken(),
		Capability:   &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})
	if err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if resp.GetJob() != nil {
		t.Errorf("expected nil job on empty queue, got %v", resp.GetJob())
	}
}

func TestPullWork_HappyPath(t *testing.T) {
	f := newGRPCFixture(t)
	_, err := f.srv.SubmitJob(testTenantCtx(), &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv"},
	})
	if err != nil {
		t.Fatalf("SubmitJob: %v", err)
	}
	reg, _ := f.srv.RegisterNode(context.Background(), &controllerv1.RegisterNodeRequest{
		Name:       "pw-node",
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})
	resp, err := f.srv.PullWork(context.Background(), &controllerv1.PullWorkRequest{
		NodeId:       reg.GetNodeId(),
		SessionToken: reg.GetSessionToken(),
		Capability:   &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})
	if err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if resp.GetJob() == nil {
		t.Fatal("PullWork returned no job")
	}
	if resp.GetJob().GetStatus() != controllerv1.JobStatus_RUNNING {
		t.Errorf("status: got %v, want RUNNING", resp.GetJob().GetStatus())
	}
}

func TestPullWork_InvalidSessionRejected(t *testing.T) {
	f := newGRPCFixture(t)
	_, err := f.srv.PullWork(context.Background(), &controllerv1.PullWorkRequest{
		NodeId:       "no-such-node",
		SessionToken: "bogus",
		Capability:   &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})
	if codeOf(err) != codes.PermissionDenied {
		t.Errorf("expected PermissionDenied, got %v (%v)", codeOf(err), err)
	}
}

// ---------------------------------------------------------------------------
// ReportResult
// ---------------------------------------------------------------------------

func TestReportResult_FinalSuccess(t *testing.T) {
	f := newGRPCFixture(t)
	ctx := testTenantCtx()
	sub, err := f.srv.SubmitJob(ctx, &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv"},
	})
	if err != nil {
		t.Fatalf("SubmitJob: %v", err)
	}
	reg, _ := f.srv.RegisterNode(context.Background(), &controllerv1.RegisterNodeRequest{
		Name:       "rr-node",
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})
	// Pull the work so the job moves to RUNNING.
	_, _ = f.srv.PullWork(context.Background(), &controllerv1.PullWorkRequest{
		NodeId:       reg.GetNodeId(),
		SessionToken: reg.GetSessionToken(),
		Capability:   &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})

	resp, err := f.srv.ReportResult(context.Background(), &controllerv1.ReportResultRequest{
		NodeId:       reg.GetNodeId(),
		SessionToken: reg.GetSessionToken(),
		JobId:        sub.GetJobId(),
		Final:        true,
		Score:        76.6683,
		Features:     map[string]float64{"adm2": 0.99},
	})
	if err != nil {
		t.Fatalf("ReportResult: %v", err)
	}
	if !resp.GetOk() {
		t.Error("ok: got false, want true")
	}
	job, _ := f.srv.GetJob(ctx, &controllerv1.GetJobRequest{JobId: sub.GetJobId()})
	if job.GetStatus() != controllerv1.JobStatus_COMPLETED {
		t.Errorf("status: got %v, want COMPLETED", job.GetStatus())
	}
}

func TestReportResult_FinalWithError(t *testing.T) {
	f := newGRPCFixture(t)
	ctx := testTenantCtx()
	sub, err := f.srv.SubmitJob(ctx, &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv"},
	})
	if err != nil {
		t.Fatalf("SubmitJob: %v", err)
	}
	reg, _ := f.srv.RegisterNode(context.Background(), &controllerv1.RegisterNodeRequest{
		Name:       "rr-node",
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})
	_, _ = f.srv.PullWork(context.Background(), &controllerv1.PullWorkRequest{
		NodeId:       reg.GetNodeId(),
		SessionToken: reg.GetSessionToken(),
		Capability:   &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})

	if _, err := f.srv.ReportResult(context.Background(), &controllerv1.ReportResultRequest{
		NodeId:       reg.GetNodeId(),
		SessionToken: reg.GetSessionToken(),
		JobId:        sub.GetJobId(),
		Final:        true,
		Error:        "libvmaf exit 1",
	}); err != nil {
		t.Fatalf("ReportResult: %v", err)
	}
	job, _ := f.srv.GetJob(ctx, &controllerv1.GetJobRequest{JobId: sub.GetJobId()})
	if job.GetStatus() != controllerv1.JobStatus_FAILED {
		t.Errorf("status: got %v, want FAILED", job.GetStatus())
	}
	if !strings.Contains(job.GetError(), "libvmaf") {
		t.Errorf("error: got %q", job.GetError())
	}
}

func TestReportResult_PartialIgnored(t *testing.T) {
	f := newGRPCFixture(t)
	reg, _ := f.srv.RegisterNode(context.Background(), &controllerv1.RegisterNodeRequest{
		Name:       "rr-node",
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1},
	})
	resp, err := f.srv.ReportResult(context.Background(), &controllerv1.ReportResultRequest{
		NodeId:       reg.GetNodeId(),
		SessionToken: reg.GetSessionToken(),
		JobId:        "any-id",
		Final:        false,
	})
	if err != nil {
		t.Fatalf("partial ReportResult: %v", err)
	}
	if !resp.GetOk() {
		t.Error("partial ok: got false, want true")
	}
}

func TestReportResult_InvalidSessionRejected(t *testing.T) {
	f := newGRPCFixture(t)
	_, err := f.srv.ReportResult(context.Background(), &controllerv1.ReportResultRequest{
		NodeId:       "no-such-node",
		SessionToken: "bogus",
		Final:        true,
	})
	if codeOf(err) != codes.PermissionDenied {
		t.Errorf("expected PermissionDenied, got %v", codeOf(err))
	}
}

// ---------------------------------------------------------------------------
// Conversion helpers
// ---------------------------------------------------------------------------

func TestQueueStatusToProto(t *testing.T) {
	t.Parallel()
	cases := []struct {
		in   string
		want controllerv1.JobStatus
	}{
		{queue.StatusPending, controllerv1.JobStatus_PENDING},
		{queue.StatusRunning, controllerv1.JobStatus_RUNNING},
		{queue.StatusCompleted, controllerv1.JobStatus_COMPLETED},
		{queue.StatusFailed, controllerv1.JobStatus_FAILED},
		{queue.StatusCancelled, controllerv1.JobStatus_CANCELLED},
		{"unknown-status", controllerv1.JobStatus_PENDING},
		{"", controllerv1.JobStatus_PENDING},
	}
	for _, tc := range cases {
		if got := queueStatusToProto(tc.in); got != tc.want {
			t.Errorf("queueStatusToProto(%q) = %v, want %v", tc.in, got, tc.want)
		}
	}
}

// TestStreamJobs_WithStatusFilter_PendingOnly verifies that a PENDING-only
// filter returns only the pending jobs (not completed ones).
func TestStreamJobs_WithStatusFilter_PendingOnly(t *testing.T) {
	cs := newTestControllerServer(t)
	stream := newTestStream(context.Background())

	// Submit 2 jobs.
	submitTestJob(t, cs, "/ref1.yuv", "/dis1.yuv")
	submitTestJob(t, cs, "/ref2.yuv", "/dis2.yuv")

	req := &controllerv1.StreamJobsRequest{
		StatusFilter: []controllerv1.JobStatus{controllerv1.JobStatus_PENDING},
	}
	if err := cs.StreamJobs(req, stream); err != nil {
		t.Fatalf("StreamJobs with filter: unexpected error: %v", err)
	}
	if len(stream.sent) != 2 {
		t.Errorf("sent with PENDING filter: got %d, want 2", len(stream.sent))
	}
	for _, j := range stream.sent {
		if j.GetStatus() != controllerv1.JobStatus_PENDING {
			t.Errorf("job %q has status %v, want PENDING", j.GetId(), j.GetStatus())
		}
	}
}

func TestProtoCapToNodes_Nil(t *testing.T) {
	t.Parallel()
	got := protoCapToNodes(nil)
	if got.GPUVendor != "" || got.Concurrency != 0 || len(got.Backends) != 0 {
		t.Errorf("nil cap should map to zero-value, got %+v", got)
	}
}

func TestProtoCapToNodes_NonNil(t *testing.T) {
	t.Parallel()
	in := &controllerv1.NodeCapability{
		GpuVendor:   "amd",
		Backends:    []string{"hip", "cpu"},
		Concurrency: 8,
	}
	got := protoCapToNodes(in)
	if got.GPUVendor != "amd" {
		t.Errorf("GPUVendor: got %q", got.GPUVendor)
	}
	if got.Concurrency != 8 {
		t.Errorf("Concurrency: got %d", got.Concurrency)
	}
	if len(got.Backends) != 2 || got.Backends[0] != "hip" {
		t.Errorf("Backends: got %v", got.Backends)
	}
}

func TestQueueJobToProto_FieldsCopiedCorrectly(t *testing.T) {
	t.Parallel()
	j := &queue.Job{
		ID:           "abc",
		Status:       queue.StatusRunning,
		AssignedNode: "node-9",
		Error:        "",
		Scoring: queue.ScoringParams{
			Reference: "/r.yuv",
			Distorted: "/d.yuv",
			Model:     "vmaf_v0.6.1",
			Backend:   "cuda",
		},
	}
	got := queueJobToProto(j)
	if got.GetId() != "abc" {
		t.Errorf("Id: got %q", got.GetId())
	}
	if got.GetStatus() != controllerv1.JobStatus_RUNNING {
		t.Errorf("Status: got %v", got.GetStatus())
	}
	if got.GetAssignedNode() != "node-9" {
		t.Errorf("AssignedNode: got %q", got.GetAssignedNode())
	}
	if got.GetScoring().GetReference() != "/r.yuv" {
		t.Errorf("Scoring.Reference: got %q", got.GetScoring().GetReference())
	}
}

// ---------------------------------------------------------------------------
// scoringServer (Health endpoint only — Score requires a real libvmaf scorer)
// ---------------------------------------------------------------------------

func TestScoringServer_HealthReportsOK(t *testing.T) {
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	s := newScoringServer(nil, metrics, log)
	// Health does not touch the scorer.
	resp, err := s.Health(context.Background(), nil)
	if err != nil {
		t.Fatalf("Health: %v", err)
	}
	if !resp.GetOk() {
		t.Error("Health ok: got false, want true")
	}
}
