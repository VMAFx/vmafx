// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/queue/queue_tenant_test.go — the queue's reads are
// scoped to one tenant and a result can only be written by the node the job
// is assigned to (ADR-1522).

package queue_test

import (
	"context"
	"errors"
	"slices"
	"testing"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
)

// submitFor submits one cpu job for tenant and returns its ID.
func submitFor(t *testing.T, q *queue.SQLiteQueue, tenant string) string {
	t.Helper()
	id, err := q.Submit(context.Background(), &queue.Job{
		TenantID: tenant,
		Scoring:  queue.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv", Backend: "cpu"},
	})
	if err != nil {
		t.Fatalf("Submit(%s): %v", tenant, err)
	}
	return id
}

var anyCPU = queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 1}

func TestListByTenantReadsOnlyThatTenant(t *testing.T) {
	q := newListTestQueue(t)
	a := submitFor(t, q, "a")
	submitFor(t, q, "b")
	legacy := submitFor(t, q, "")
	cases := map[string][]string{"a": {a}, "": {legacy}, "c": nil, "A": nil, "a ": nil}
	for tenant, want := range cases {
		for _, statuses := range [][]string{nil, {queue.StatusPending, queue.StatusRunning}} {
			jobs, err := q.ListByTenant(context.Background(), tenant, statuses)
			if err != nil {
				t.Fatalf("ListByTenant(%q, %v): %v", tenant, statuses, err)
			}
			if len(jobs) != len(want) || (len(want) == 1 && jobs[0].ID != want[0]) {
				t.Errorf("ListByTenant(%q, %v) = %d jobs, want %v", tenant, statuses, len(jobs), want)
			}
			for _, j := range jobs {
				if j.TenantID != tenant {
					t.Errorf("ListByTenant(%q) returned a job of tenant %q", tenant, j.TenantID)
				}
			}
		}
	}
}

func TestPullWorkNeverAssignsAnotherTenantsJob(t *testing.T) {
	q := newListTestQueue(t)
	ctx := context.Background()
	a := submitFor(t, q, "a")
	b := submitFor(t, q, "b")
	if job, err := q.PullWork(ctx, "node-c", "c", anyCPU); err != nil || job != nil {
		t.Fatalf("tenant c pulled %v (err %v), want nothing", job, err)
	}
	job, err := q.PullWork(ctx, "node-b", "b", anyCPU)
	if err != nil || job == nil || job.ID != b {
		t.Fatalf("tenant b pulled %v (err %v), want %s (skipping tenant a's older job)", job, err, b)
	}
	job, err = q.PullWork(ctx, "node-a", "a", anyCPU)
	if err != nil || job == nil || job.ID != a {
		t.Fatalf("tenant a pulled %v (err %v), want %s", job, err, a)
	}
	if got := q.PendingCount(); got != 0 {
		t.Errorf("PendingCount = %d, want 0", got)
	}
}

// report returns a final report of jobID by node of tenant "a" with score 42;
// orphans lists the node IDs without a live session.
func report(node, jobID string, orphans ...string) queue.Report {
	return queue.Report{
		NodeID: node, TenantID: "a", JobID: jobID, Result: &queue.JobResult{Score: 42},
		Orphaned: func(id string) bool { return slices.Contains(orphans, id) },
	}
}

// pullFor submits a job for tenant and has node pull it.
func pullFor(t *testing.T, q *queue.SQLiteQueue, tenant, node string) string {
	t.Helper()
	id := submitFor(t, q, tenant)
	if job, err := q.PullWork(context.Background(), node, tenant, anyCPU); err != nil || job == nil || job.ID != id {
		t.Fatalf("PullWork(%s): %v (err %v), want %s", node, job, err, id)
	}
	return id
}

func TestReportResultOnlyByTheAssignedNode(t *testing.T) {
	q := newListTestQueue(t)
	ctx := context.Background()
	running := pullFor(t, q, "a", "node-1")
	pending := submitFor(t, q, "a")
	for name, r := range map[string]queue.Report{
		"another live node": report("node-2", running),
		"never pulled":      report("node-1", pending, "node-1"),
		"unknown job":       report("node-1", "nope"),
	} {
		if _, err := q.ReportResult(ctx, r); !errors.Is(err, queue.ErrNotAssigned) {
			t.Errorf("%s: err = %v, want ErrNotAssigned", name, err)
		}
		if q.MayReport(ctx, r) {
			t.Errorf("%s: MayReport accepted a partial report", name)
		}
	}
	if got := q.RunningCount(); got != 1 {
		t.Errorf("RunningCount after refused reports = %d, want 1", got)
	}
	for id, want := range map[string]string{running: queue.StatusRunning, pending: queue.StatusPending} {
		if j, _ := q.Get(ctx, id); j.Status != want || j.Score != 0 {
			t.Errorf("job %s: status %s score %v, want %s score 0", id, j.Status, j.Score, want)
		}
	}
	for attempt := range 2 {
		finished, err := q.ReportResult(ctx, report("node-1", running))
		if err != nil {
			t.Fatalf("report %d by the assigned node: %v", attempt, err)
		}
		// Only the first report moves the job; the retry is idempotent.
		if finished != (attempt == 0) {
			t.Errorf("report %d finished the job = %v", attempt, finished)
		}
	}
	if j, _ := q.Get(ctx, running); j.Status != queue.StatusCompleted || j.Score != 42 {
		t.Errorf("job after the assigned node's report: %s %v, want completed 42", j.Status, j.Score)
	}
}

func TestOrphanedJobReportedByItsTenantOnly(t *testing.T) {
	q := newListTestQueue(t)
	ctx := context.Background()
	orphan := pullFor(t, q, "a", "old-node")
	other := pullFor(t, q, "b", "b-node")

	rival := report("b-node", orphan, "old-node")
	rival.TenantID = "b"
	if _, err := q.ReportResult(ctx, rival); !errors.Is(err, queue.ErrNotAssigned) {
		t.Errorf("tenant b reporting tenant a's orphaned job: %v, want ErrNotAssigned", err)
	}
	foreign := report("new-node", other, "b-node")
	if _, err := q.ReportResult(ctx, foreign); !errors.Is(err, queue.ErrNotAssigned) {
		t.Errorf("tenant a reporting tenant b's orphaned job: %v, want ErrNotAssigned", err)
	}
	if !q.MayReport(ctx, report("new-node", orphan, "old-node")) {
		t.Error("MayReport refused a partial report of the tenant's orphaned job")
	}
	if finished, err := q.ReportResult(ctx, report("new-node", orphan, "old-node")); err != nil || !finished {
		t.Fatalf("the tenant's new session reporting the orphaned job: %v, %v", finished, err)
	}
	j, _ := q.Get(ctx, orphan)
	if j.Status != queue.StatusCompleted || j.Score != 42 || j.AssignedNode != "new-node" {
		t.Errorf("adopted job: %s %v on %q, want completed 42 on new-node", j.Status, j.Score, j.AssignedNode)
	}
	if finished, err := q.ReportResult(ctx, report("newer-node", orphan, "old-node", "new-node")); err != nil || finished {
		t.Errorf("a retry after the adoption by a later session: %v, %v; want idempotent success", finished, err)
	}
	if j, _ := q.Get(ctx, other); j.Status != queue.StatusRunning {
		t.Errorf("tenant b's job after the refused reports: %s, want running", j.Status)
	}
}
