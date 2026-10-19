// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/queue/requeue_test.go — RequeueNode returns an evicted
// node's RUNNING jobs to PENDING and leaves everything else alone.

package queue_test

import (
	"context"
	"testing"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
)

func submitJobs(t *testing.T, q *queue.SQLiteQueue, n int) []string {
	t.Helper()
	ids := make([]string, 0, n)
	for range n {
		id, err := q.Submit(context.Background(), &queue.Job{TenantID: "t", Scoring: queue.ScoringParams{Reference: "/r", Distorted: "/d"}})
		if err != nil {
			t.Fatalf("Submit: %v", err)
		}
		ids = append(ids, id)
	}
	return ids
}

func pullJobFor(t *testing.T, q *queue.SQLiteQueue, node string) *queue.Job {
	t.Helper()
	j, err := q.PullWork(context.Background(), node, "t", queue.NodeCapacity{Slots: 1})
	if err != nil {
		t.Fatalf("PullWork(%s): %v", node, err)
	}
	return j
}

// TestRequeueNode: the evicted node's running jobs return to PENDING without
// an assigned node, ahead of newer pending work; another node's job and a
// finished job are untouched (positive and boundary).
func TestRequeueNode(t *testing.T) {
	q := newTestQueue(t)
	ctx := context.Background()
	ids := submitJobs(t, q, 4)
	a1, b, a2 := pullJobFor(t, q, "node-a"), pullJobFor(t, q, "node-b"), pullJobFor(t, q, "node-a")
	if _, err := q.ReportResult(ctx, queue.Report{NodeID: "node-a", JobID: a1.ID, Result: &queue.JobResult{Score: 80}}); err != nil {
		t.Fatalf("ReportResult: %v", err)
	}

	n, err := q.RequeueNode(ctx, "node-a")
	if err != nil || n != 1 {
		t.Fatalf("RequeueNode = %d, %v; want 1 (one running job, one already completed)", n, err)
	}
	got, err := q.Get(ctx, a2.ID)
	if err != nil || got.Status != queue.StatusPending || got.AssignedNode != "" {
		t.Fatalf("requeued job = %+v, %v; want PENDING with no node", got, err)
	}
	if done, _ := q.Get(ctx, a1.ID); done.Status != queue.StatusCompleted {
		t.Fatalf("completed job became %s", done.Status)
	}
	if other, _ := q.Get(ctx, b.ID); other.Status != queue.StatusRunning || other.AssignedNode != "node-b" {
		t.Fatalf("node-b's job changed: %+v", other)
	}
	if next := pullJobFor(t, q, "node-c"); next == nil || next.ID != a2.ID {
		t.Fatalf("next pull = %+v, want the requeued job %s ahead of %s", next, a2.ID, ids[3])
	}
}

// TestRequeueNode_UnknownNode: a node without jobs moves nothing (negative).
func TestRequeueNode_UnknownNode(t *testing.T) {
	q := newTestQueue(t)
	submitJobs(t, q, 1)
	pullJobFor(t, q, "node-a")
	if n, err := q.RequeueNode(context.Background(), "node-z"); err != nil || n != 0 {
		t.Fatalf("RequeueNode(node-z) = %d, %v; want 0, nil", n, err)
	}
	if q.RunningCount() != 1 {
		t.Fatalf("running count = %d, want 1", q.RunningCount())
	}
}
