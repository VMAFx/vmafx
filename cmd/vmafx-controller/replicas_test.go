// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/replicas_test.go — two controller replicas on one
// PostgreSQL store (ADR-2350): a node keeps working through the loss of the
// replica it talked to, and a job whose node died completes once on another.

//go:build cgo

package main

import (
	"context"
	"log/slog"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"google.golang.org/grpc/codes"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/backend"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store/storetest"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// replica is one controller process on the shared store.
func replica(t *testing.T, db storetest.DB) *controllerServer {
	t.Helper()
	b := backend.NewPostgres(db.Store, backend.PostgresOptions{})
	return newControllerServer(b, allowAllScopes(), testControllerMetrics(t, prometheus.NewRegistry()), slog.New(slog.DiscardHandler))
}

// onReplica wraps a replica as the fixture the tenant test helpers take.
func onReplica(srv *controllerServer) *grpcFixture { return &grpcFixture{srv: srv} }

func TestNodeKeepsWorkingWhenItsReplicaIsGone(t *testing.T) {
	db := storetest.New(t, storetest.Image)
	a, b := onReplica(replica(t, db)), onReplica(replica(t, db))
	ctx := tenantCtx(tenantA)
	nodeID, token, jobID := pullOneJob(t, a, ctx, ctx)

	// Replica A is gone. The node's session lives in the database, so B
	// accepts its heartbeat and its report without a new registration.
	hb, err := b.srv.Heartbeat(ctx, &controllerv1.HeartbeatRequest{
		NodeId: nodeID, SessionToken: token, RunningJobIds: []string{jobID},
	})
	if err != nil || !hb.GetOk() {
		t.Fatalf("heartbeat through B: ok=%v err=%v", hb.GetOk(), err)
	}
	if err := reportAs(b, ctx, nodeID, token, jobID); err != nil {
		t.Fatalf("report through B: %v", err)
	}
	if err := reportAs(b, ctx, nodeID, token, jobID); err != nil {
		t.Fatalf("retried report through B: %v", err)
	}
	job, err := a.srv.GetJob(ctx, &controllerv1.GetJobRequest{JobId: jobID})
	if err != nil || job.GetStatus() != controllerv1.JobStatus_COMPLETED || job.GetFinalScore() != 99 {
		t.Fatalf("job seen from A: %+v %v", job, err)
	}
}

func TestJobOfADeadNodeCompletesOnceOnAnotherNode(t *testing.T) {
	db := storetest.New(t, storetest.Image)
	a, b := onReplica(replica(t, db)), onReplica(replica(t, db))
	ctx := tenantCtx(tenantA)
	deadNode, deadTok, jobID := pullOneJob(t, a, ctx, ctx)

	// The node dies: no heartbeat renews its lease; the sweep requeues the job.
	storetest.ExpireLease(t, db.Pool, jobID)
	sweepCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if rep, err := db.Store.ExpireLeases(sweepCtx, func(int32) time.Duration { return 0 }, 10); err != nil || rep.Requeued != 1 {
		t.Fatalf("sweep: %+v %v", rep, err)
	}

	liveNode, liveTok := registerAs(t, b, ctx, "survivor")
	resp, err := b.srv.PullWork(ctx, &controllerv1.PullWorkRequest{NodeId: liveNode, SessionToken: liveTok, Capability: cpuCap})
	if err != nil || resp.GetJob().GetId() != jobID {
		t.Fatalf("survivor pull: %+v %v", resp.GetJob(), err)
	}
	if _, err := b.srv.ReportResult(ctx, &controllerv1.ReportResultRequest{
		NodeId: liveNode, SessionToken: liveTok, JobId: jobID, Final: true, Score: 42,
	}); err != nil {
		t.Fatalf("survivor report: %v", err)
	}
	// The dead node's late report is fenced and changes nothing.
	if err := reportAs(a, ctx, deadNode, deadTok, jobID); codeOf(err) != codes.PermissionDenied {
		t.Fatalf("late report of the lost attempt: %v, want PermissionDenied", err)
	}
	job, err := b.srv.GetJob(ctx, &controllerv1.GetJobRequest{JobId: jobID})
	if err != nil || job.GetStatus() != controllerv1.JobStatus_COMPLETED || job.GetFinalScore() != 42 || job.GetAssignedNode() != "survivor" {
		t.Fatalf("job after both reports: %+v %v", job, err)
	}
}

func TestPostgresReplicaKeepsTheTenantContract(t *testing.T) {
	db := storetest.New(t, storetest.Image)
	f := onReplica(replica(t, db))
	jobID := submitAs(t, f, tenantCtx(tenantA), "/a.yuv")
	if _, err := f.srv.GetJob(tenantCtx(tenantB), &controllerv1.GetJobRequest{JobId: jobID}); codeOf(err) != codes.PermissionDenied {
		t.Fatalf("GetJob of tenant A's job as tenant B: %v, want PermissionDenied", err)
	}
	if _, err := f.srv.CancelJob(tenantCtx(tenantB), &controllerv1.CancelJobRequest{JobId: jobID}); codeOf(err) != codes.PermissionDenied {
		t.Fatalf("CancelJob of tenant A's job as tenant B: %v, want PermissionDenied", err)
	}
	if _, err := f.srv.GetJob(tenantCtx(tenantB), &controllerv1.GetJobRequest{JobId: "0193f2a4-0000-7000-8000-000000000000"}); codeOf(err) != codes.NotFound {
		t.Fatalf("GetJob of an unknown job: %v, want NotFound", err)
	}
	nodeB, tokB := registerAs(t, f, tenantCtx(tenantB), "b-node")
	resp, err := f.srv.PullWork(tenantCtx(tenantB), &controllerv1.PullWorkRequest{NodeId: nodeB, SessionToken: tokB, Capability: cpuCap})
	if err != nil || resp.GetJob() != nil {
		t.Fatalf("tenant B's node pulled tenant A's job: %+v %v", resp.GetJob(), err)
	}
	hb, err := f.srv.Heartbeat(tenantCtx(tenantA), &controllerv1.HeartbeatRequest{NodeId: nodeB, SessionToken: tokB})
	if err != nil || hb.GetOk() {
		t.Fatalf("tenant B's session used as tenant A: ok=%v err=%v, want ok=false", hb.GetOk(), err)
	}
}
