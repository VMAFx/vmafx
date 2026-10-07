// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/heartbeat_cancel_test.go — a CancelJob reaches the
// node running the job through the node's next Heartbeat (ADR-1567).
//
// Positive: a cancelled running job the node names comes back in
// cancel_job_ids. Negative: a job still running, an unknown ID, another
// tenant's cancelled job and a refused session name nothing. Boundary: 64
// running IDs are accepted, 65 refused.

//go:build cgo

package main

import (
	"context"
	"fmt"
	"slices"
	"testing"

	"google.golang.org/grpc/codes"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
)

// registerCPUNode registers a cpu node for the test tenant.
func registerCPUNode(t *testing.T, f *grpcFixture) *controllerv1.RegisterNodeResponse {
	t.Helper()
	reg, err := f.srv.RegisterNode(testTenantCtx(), &controllerv1.RegisterNodeRequest{
		Name:       "cancel-node",
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}, Concurrency: 2},
	})
	if err != nil {
		t.Fatalf("RegisterNode: %v", err)
	}
	return reg
}

// pullJob assigns the next pending job of the test tenant to reg's node.
func pullJob(t *testing.T, f *grpcFixture, reg *controllerv1.RegisterNodeResponse) string {
	t.Helper()
	resp, err := f.srv.PullWork(testTenantCtx(), &controllerv1.PullWorkRequest{
		NodeId: reg.GetNodeId(), SessionToken: reg.GetSessionToken(),
		Capability: &controllerv1.NodeCapability{Backends: []string{"cpu"}},
	})
	if err != nil || resp.GetJob() == nil {
		t.Fatalf("PullWork: job %v, err %v", resp.GetJob(), err)
	}
	return resp.GetJob().GetId()
}

// heartbeat sends one heartbeat for reg's node naming running.
func heartbeat(f *grpcFixture, reg *controllerv1.RegisterNodeResponse, token string, running []string) (*controllerv1.HeartbeatResponse, error) {
	return f.srv.Heartbeat(testTenantCtx(), &controllerv1.HeartbeatRequest{
		NodeId: reg.GetNodeId(), SessionToken: token,
		// #nosec G115 -- the tests pass at most 65 IDs.
		JobsRunning: int32(len(running)), RunningJobIds: running,
	})
}

func TestHeartbeatNamesCancelledRunningJobs(t *testing.T) {
	f := newGRPCFixture(t)
	reg := registerCPUNode(t, f)
	submitTestJob(t, f.srv, "/ref-a.y4m", "/dis-a.y4m")
	submitTestJob(t, f.srv, "/ref-b.y4m", "/dis-b.y4m")
	cancelled, running := pullJob(t, f, reg), pullJob(t, f, reg)

	foreign, err := f.queue.Submit(context.Background(), &queue.Job{
		TenantID: "rival", Scoring: queue.ScoringParams{Reference: "/r.y4m", Distorted: "/d.y4m"},
	})
	if err != nil {
		t.Fatalf("Submit rival job: %v", err)
	}
	if _, err := f.queue.Cancel(context.Background(), foreign); err != nil {
		t.Fatalf("Cancel rival job: %v", err)
	}
	if _, err := f.srv.CancelJob(testTenantCtx(), &controllerv1.CancelJobRequest{JobId: cancelled}); err != nil {
		t.Fatalf("CancelJob: %v", err)
	}

	hb, err := heartbeat(f, reg, reg.GetSessionToken(), []string{running, "no-such-job", foreign, cancelled})
	if err != nil || !hb.GetOk() {
		t.Fatalf("Heartbeat: ok %v, err %v", hb.GetOk(), err)
	}
	if got := hb.GetCancelJobIds(); !slices.Equal(got, []string{cancelled}) {
		t.Fatalf("cancel_job_ids = %v, want only the cancelled job %s (not the running one, an unknown ID or the rival tenant's job)", got, cancelled)
	}

	bad, err := heartbeat(f, reg, "wrong-token", []string{cancelled})
	if err != nil || bad.GetOk() || len(bad.GetCancelJobIds()) != 0 {
		t.Fatalf("refused session: ok %v, cancel_job_ids %v, err %v; want ok=false and nothing named", bad.GetOk(), bad.GetCancelJobIds(), err)
	}
}

func TestHeartbeatRunningListIsBounded(t *testing.T) {
	f := newGRPCFixture(t)
	reg := registerCPUNode(t, f)
	ids := make([]string, maxHeartbeatJobs+1)
	for i := range ids {
		ids[i] = fmt.Sprintf("job-%d", i)
	}
	if hb, err := heartbeat(f, reg, reg.GetSessionToken(), ids[:maxHeartbeatJobs]); err != nil || !hb.GetOk() {
		t.Fatalf("%d running IDs: ok %v, err %v; want accepted", maxHeartbeatJobs, hb.GetOk(), err)
	}
	if _, err := heartbeat(f, reg, reg.GetSessionToken(), ids); codeOf(err) != codes.InvalidArgument {
		t.Fatalf("%d running IDs: err %v, want InvalidArgument", len(ids), err)
	}
}

// TestHeartbeatTenantComesFromTheToken: a node session of the test tenant
// used with another tenant's token is refused (ADR-1522), and the response
// names no job.
func TestHeartbeatTenantComesFromTheToken(t *testing.T) {
	f := newGRPCFixture(t)
	reg := registerCPUNode(t, f)
	submitTestJob(t, f.srv, "/ref.y4m", "/dis.y4m")
	id := pullJob(t, f, reg)
	if _, err := f.queue.Cancel(context.Background(), id); err != nil {
		t.Fatalf("Cancel: %v", err)
	}
	hb, err := f.srv.Heartbeat(tenantCtx("rival"), &controllerv1.HeartbeatRequest{
		NodeId: reg.GetNodeId(), SessionToken: reg.GetSessionToken(), RunningJobIds: []string{id},
	})
	if err != nil || hb.GetOk() || len(hb.GetCancelJobIds()) != 0 {
		t.Fatalf("other tenant's token: ok %v, cancel_job_ids %v, err %v; want ok=false and nothing named", hb.GetOk(), hb.GetCancelJobIds(), err)
	}
}
