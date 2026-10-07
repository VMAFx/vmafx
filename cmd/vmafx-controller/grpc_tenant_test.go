// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/grpc_tenant_test.go — every job read and every node
// session is scoped to the caller's tenant (ADR-1522).
//
// The handler tests call the controllerServer methods with an injected tenant
// context, as the auth interceptor leaves it. TestCrossTenantReadRefusedOverTheWire
// repeats the read checks over gRPC against the production graph with real
// tokens of two tenants.

//go:build cgo

package main

import (
	"context"
	"errors"
	"io"
	"log/slog"
	"os"
	"strings"
	"testing"
	"time"

	googlegrpc "google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/auth/authtest"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/backend"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/scheduler"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

const (
	tenantA = "tenant-a"
	tenantB = "tenant-b"
)

// tenantCtx returns a handler context for tenant with the admin role, so the
// tests below exercise tenancy alone (roles are grpc_roles_test.go's).
func tenantCtx(tenant string) context.Context {
	return auth.ContextWithClaims(context.Background(), auth.Claims{
		Subject: "user-" + tenant, TenantID: tenant, Roles: []string{auth.RoleAdmin},
	})
}

// submitAs submits one job as the tenant of ctx and returns its ID.
func submitAs(t *testing.T, f *grpcFixture, ctx context.Context, ref string) string {
	t.Helper()
	resp, err := f.srv.SubmitJob(ctx, &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: ref, Distorted: "/d.yuv"},
	})
	if err != nil {
		t.Fatalf("SubmitJob(%s): %v", ref, err)
	}
	return resp.GetJobId()
}

var cpuCap = &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 1}

// registerAs registers a node as the tenant of ctx.
func registerAs(t *testing.T, f *grpcFixture, ctx context.Context, name string) (string, string) {
	t.Helper()
	reg, err := f.srv.RegisterNode(ctx, &controllerv1.RegisterNodeRequest{Name: name, Capability: cpuCap})
	if err != nil {
		t.Fatalf("RegisterNode(%s): %v", name, err)
	}
	return reg.GetNodeId(), reg.GetSessionToken()
}

// pullOneJob submits a job as submitCtx's tenant, registers a node as
// nodeCtx's tenant and has it pull; it returns the node's session and the job.
func pullOneJob(t *testing.T, f *grpcFixture, submitCtx, nodeCtx context.Context) (string, string, string) {
	t.Helper()
	jobID := submitAs(t, f, submitCtx, "/r.yuv")
	nodeID, token := registerAs(t, f, nodeCtx, "puller")
	resp, err := f.srv.PullWork(nodeCtx, &controllerv1.PullWorkRequest{
		NodeId: nodeID, SessionToken: token, Capability: cpuCap,
	})
	if err != nil || resp.GetJob().GetId() != jobID {
		t.Fatalf("PullWork: job %q err %v, want job %q", resp.GetJob().GetId(), err, jobID)
	}
	return nodeID, token, jobID
}

// streamedIDs runs StreamJobs in ctx and returns the IDs it sent.
func streamedIDs(t *testing.T, f *grpcFixture, ctx context.Context, filter ...controllerv1.JobStatus) []string {
	t.Helper()
	stream := newTestStream(ctx)
	if err := f.srv.StreamJobs(&controllerv1.StreamJobsRequest{StatusFilter: filter}, stream); err != nil {
		t.Fatalf("StreamJobs: %v", err)
	}
	ids := make([]string, 0, len(stream.sent))
	for _, j := range stream.sent {
		ids = append(ids, j.GetId())
	}
	return ids
}

// sameIDs reports whether got and want hold the same IDs in the same order.
func sameIDs(got, want []string) bool {
	return strings.Join(got, ",") == strings.Join(want, ",")
}

func TestStreamJobsSendsOnlyTheCallersTenant(t *testing.T) {
	f := newGRPCFixture(t)
	a1 := submitAs(t, f, tenantCtx(tenantA), "/a1.yuv")
	b1 := submitAs(t, f, tenantCtx(tenantB), "/b1.yuv")
	a2 := submitAs(t, f, tenantCtx(tenantA), "/a2.yuv")

	if got := streamedIDs(t, f, tenantCtx(tenantA)); !sameIDs(got, []string{a1, a2}) {
		t.Errorf("tenant A streamed %v, want [%s %s]", got, a1, a2)
	}
	if got := streamedIDs(t, f, tenantCtx(tenantB)); !sameIDs(got, []string{b1}) {
		t.Errorf("tenant B streamed %v, want [%s]", got, b1)
	}
	pending := controllerv1.JobStatus_PENDING
	if got := streamedIDs(t, f, tenantCtx(tenantB), pending); !sameIDs(got, []string{b1}) {
		t.Errorf("tenant B with a status filter streamed %v, want [%s]", got, b1)
	}
	if got := streamedIDs(t, f, tenantCtx("tenant-c")); len(got) != 0 {
		t.Errorf("a tenant without jobs streamed %v, want none", got)
	}
	err := f.srv.StreamJobs(&controllerv1.StreamJobsRequest{}, newTestStream(context.Background()))
	if codeOf(err) != codes.Unauthenticated {
		t.Errorf("StreamJobs without a tenant: %v, want Unauthenticated", err)
	}
}

func TestCrossTenantGetAndCancelRefused(t *testing.T) {
	f := newGRPCFixture(t)
	jobID := submitAs(t, f, tenantCtx(tenantA), "/a.yuv")

	_, err := f.srv.GetJob(tenantCtx(tenantB), &controllerv1.GetJobRequest{JobId: jobID})
	if codeOf(err) != codes.PermissionDenied {
		t.Fatalf("GetJob of tenant A's job as tenant B: %v, want PermissionDenied", err)
	}
	if strings.Contains(err.Error(), tenantA) {
		t.Errorf("refusal names the owning tenant: %v", err)
	}
	_, err = f.srv.CancelJob(tenantCtx(tenantB), &controllerv1.CancelJobRequest{JobId: jobID})
	if codeOf(err) != codes.PermissionDenied {
		t.Fatalf("CancelJob of tenant A's job as tenant B: %v, want PermissionDenied", err)
	}
	job, err := f.srv.GetJob(tenantCtx(tenantA), &controllerv1.GetJobRequest{JobId: jobID})
	if err != nil || job.GetStatus() != controllerv1.JobStatus_PENDING {
		t.Errorf("tenant A's job after B's cancel: %v (err %v), want PENDING", job.GetStatus(), err)
	}
}

func TestNodeIsOnlyGivenItsTenantsJobs(t *testing.T) {
	f := newGRPCFixture(t)
	jobID := submitAs(t, f, tenantCtx(tenantA), "/a.yuv")
	nodeB, tokB := registerAs(t, f, tenantCtx(tenantB), "node-b")
	resp, err := f.srv.PullWork(tenantCtx(tenantB), &controllerv1.PullWorkRequest{
		NodeId: nodeB, SessionToken: tokB, Capability: cpuCap,
	})
	if err != nil || resp.GetJob() != nil {
		t.Fatalf("tenant B's node pulled %v (err %v), want no job", resp.GetJob(), err)
	}
	nodeA, tokA := registerAs(t, f, tenantCtx(tenantA), "node-a")
	resp, err = f.srv.PullWork(tenantCtx(tenantA), &controllerv1.PullWorkRequest{
		NodeId: nodeA, SessionToken: tokA, Capability: cpuCap,
	})
	if err != nil || resp.GetJob().GetId() != jobID {
		t.Errorf("tenant A's node pulled %v (err %v), want %s", resp.GetJob(), err, jobID)
	}
}

func TestNodeSessionRefusedForAnotherTenant(t *testing.T) {
	f := newGRPCFixture(t)
	nodeID, token, jobID := pullOneJob(t, f, tenantCtx(tenantA), tenantCtx(tenantA))
	asB := tenantCtx(tenantB)

	hb, err := f.srv.Heartbeat(asB, &controllerv1.HeartbeatRequest{NodeId: nodeID, SessionToken: token})
	if err != nil || hb.GetOk() {
		t.Errorf("Heartbeat with tenant A's session as tenant B: ok=%v err=%v, want ok=false", hb.GetOk(), err)
	}
	_, err = f.srv.PullWork(asB, &controllerv1.PullWorkRequest{NodeId: nodeID, SessionToken: token, Capability: cpuCap})
	if codeOf(err) != codes.PermissionDenied {
		t.Errorf("PullWork with tenant A's session as tenant B: %v, want PermissionDenied", err)
	}
	_, err = f.srv.ReportResult(asB, &controllerv1.ReportResultRequest{
		NodeId: nodeID, SessionToken: token, JobId: jobID, Final: true, Score: 1,
	})
	if codeOf(err) != codes.PermissionDenied {
		t.Errorf("ReportResult with tenant A's session as tenant B: %v, want PermissionDenied", err)
	}
	job, _ := f.srv.GetJob(tenantCtx(tenantA), &controllerv1.GetJobRequest{JobId: jobID})
	if job.GetStatus() != controllerv1.JobStatus_RUNNING {
		t.Errorf("job after tenant B's report: %v, want RUNNING", job.GetStatus())
	}
}

// reportAs sends a final result for jobID as nodeID.
func reportAs(f *grpcFixture, ctx context.Context, nodeID, token, jobID string) error {
	_, err := f.srv.ReportResult(ctx, &controllerv1.ReportResultRequest{
		NodeId: nodeID, SessionToken: token, JobId: jobID, Final: true, Score: 99,
	})
	return err
}

func TestReportResultRefusedForJobNotAssignedToTheNode(t *testing.T) {
	f := newGRPCFixture(t)
	ctx := tenantCtx(tenantA)
	_, _, running := pullOneJob(t, f, ctx, ctx)
	pending := submitAs(t, f, ctx, "/pending.yuv")
	other, otherTok := registerAs(t, f, ctx, "other")

	for name, jobID := range map[string]string{"running on another node": running, "never pulled": pending, "unknown": "no-such-job"} {
		if err := reportAs(f, ctx, other, otherTok, jobID); codeOf(err) != codes.PermissionDenied {
			t.Errorf("final report for a job %s: %v, want PermissionDenied", name, err)
		}
		_, err := f.srv.ReportResult(ctx, &controllerv1.ReportResultRequest{NodeId: other, SessionToken: otherTok, JobId: jobID})
		if codeOf(err) != codes.PermissionDenied {
			t.Errorf("partial report for a job %s: %v, want PermissionDenied", name, err)
		}
	}
	for jobID, want := range map[string]controllerv1.JobStatus{running: controllerv1.JobStatus_RUNNING, pending: controllerv1.JobStatus_PENDING} {
		if job, _ := f.srv.GetJob(ctx, &controllerv1.GetJobRequest{JobId: jobID}); job.GetStatus() != want || job.GetFinalScore() != 0 {
			t.Errorf("job %s after refused reports: %v score %v, want %v score 0", jobID, job.GetStatus(), job.GetFinalScore(), want)
		}
	}
}

func TestReportResultRetryAfterCompletionIsIdempotent(t *testing.T) {
	f := newGRPCFixture(t)
	ctx := tenantCtx(tenantA)
	nodeID, token, jobID := pullOneJob(t, f, ctx, ctx)
	for attempt := range 2 {
		if err := reportAs(f, ctx, nodeID, token, jobID); err != nil {
			t.Fatalf("report attempt %d by the assigned node: %v", attempt, err)
		}
	}
	if job, _ := f.srv.GetJob(ctx, &controllerv1.GetJobRequest{JobId: jobID}); job.GetStatus() != controllerv1.JobStatus_COMPLETED {
		t.Errorf("status: %v, want COMPLETED", job.GetStatus())
	}
}

// wireStreamIDs runs StreamJobs over cc with token and returns the job IDs.
func wireStreamIDs(t *testing.T, cc *googlegrpc.ClientConn, token string) []string {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	ctx = metadata.AppendToOutgoingContext(ctx, "authorization", "Bearer "+token)
	s, err := controllerClient(cc).StreamJobs(ctx, &controllerv1.StreamJobsRequest{})
	if err != nil {
		t.Fatalf("StreamJobs: %v", err)
	}
	var ids []string
	for range 16 {
		j, rerr := s.Recv()
		if errors.Is(rerr, io.EOF) {
			return ids
		}
		if rerr != nil {
			t.Fatalf("StreamJobs Recv: %v", rerr)
		}
		ids = append(ids, j.GetId())
	}
	t.Fatalf("StreamJobs sent more than 16 jobs: %v", ids)
	return nil
}

// tokenCtx returns a 5-second RPC context carrying token.
func tokenCtx(t *testing.T, token string) context.Context {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	t.Cleanup(cancel)
	return metadata.AppendToOutgoingContext(ctx, "authorization", "Bearer "+token)
}

func TestCrossTenantReadRefusedOverTheWire(t *testing.T) {
	iss := authtest.NewIssuer(t)
	t.Setenv("VMAFX_SCORING_ROOTS", "/") // every local path (ADR-1577); tenancy is the subject here
	cc := startAuthEnabledController(t, iss)
	writerA := iss.Token(t, map[string]any{"tid": tenantA, "vmafx_roles": []string{auth.RoleWriter}})
	readerB := iss.Token(t, map[string]any{"tid": tenantB, "vmafx_roles": []string{auth.RoleReader}})

	sub, err := controllerClient(cc).SubmitJob(tokenCtx(t, writerA), &controllerv1.SubmitJobRequest{
		Scoring: &controllerv1.ScoringParams{Reference: "/a.yuv", Distorted: "/d.yuv"},
	})
	if err != nil {
		t.Fatalf("SubmitJob as tenant A: %v", err)
	}
	if got := wireStreamIDs(t, cc, readerB); len(got) != 0 {
		t.Errorf("tenant B streamed %v, want none of tenant A's jobs", got)
	}
	if got := wireStreamIDs(t, cc, writerA); !sameIDs(got, []string{sub.GetJobId()}) {
		t.Errorf("tenant A streamed %v, want [%s]", got, sub.GetJobId())
	}
	_, err = controllerClient(cc).GetJob(tokenCtx(t, readerB), &controllerv1.GetJobRequest{JobId: sub.GetJobId()})
	if codeOf(err) != codes.PermissionDenied {
		t.Errorf("GetJob of tenant A's job as tenant B: %v, want PermissionDenied", err)
	}
}

// restartedController returns a controller sharing f's job queue with a fresh,
// empty node registry: the controller after a restart (sessions are not
// persisted, jobs are).
func restartedController(t *testing.T, f *grpcFixture) *grpcFixture {
	t.Helper()
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	reg := nodes.NewRegistry(log)
	t.Cleanup(reg.Close)
	sch := scheduler.New(f.queue, reg, log)
	return &grpcFixture{
		srv:   newControllerServer(backend.NewLegacy(f.queue, reg, sch), allowAllScopes(), f.metrics, log),
		queue: f.queue, registry: reg, sched: sch, metrics: f.metrics,
	}
}

// TestNodeReportsAcrossAControllerRestart: a node that registers again after a
// controller restart reports the job it finished under its old session
// (ADR-1524); the controller accepts it from the job's tenant only, and still
// refuses a job whose node is live.
func TestNodeReportsAcrossAControllerRestart(t *testing.T) {
	before := newGRPCFixture(t)
	asA := tenantCtx(tenantA)
	_, _, jobID := pullOneJob(t, before, asA, asA)

	after := restartedController(t, before)
	nodeB, tokB := registerAs(t, after, tenantCtx(tenantB), "rival")
	if err := reportAs(after, tenantCtx(tenantB), nodeB, tokB, jobID); codeOf(err) != codes.PermissionDenied {
		t.Errorf("tenant B's node reporting tenant A's orphaned job: %v, want PermissionDenied", err)
	}
	nodeA, tokA := registerAs(t, after, asA, "puller-again")
	if err := reportAs(after, asA, nodeA, tokA, jobID); err != nil {
		t.Fatalf("tenant A's re-registered node reporting its orphaned job: %v", err)
	}
	if job, _ := after.srv.GetJob(asA, &controllerv1.GetJobRequest{JobId: jobID}); job.GetStatus() != controllerv1.JobStatus_COMPLETED {
		t.Errorf("orphaned job after the report: %v, want COMPLETED", job.GetStatus())
	}

	// A job whose node is live on the restarted controller stays its node's.
	_, _, liveJob := pullOneJob(t, after, asA, asA)
	if err := reportAs(after, asA, nodeA, tokA, liveJob); codeOf(err) != codes.PermissionDenied {
		t.Errorf("reporting a live node's job: %v, want PermissionDenied", err)
	}
}
