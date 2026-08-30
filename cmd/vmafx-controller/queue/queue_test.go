// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/queue/queue_test.go — unit tests for the job queue.
//
// Tests run against a real SQLite database in a temp directory.
// No mocks required; the in-process SQLite driver is fast enough.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-0961: TestPullWork_GetUnlockedFailure_RollsBackToPending added.

package queue_test

import (
	"context"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
)

// newTestQueue opens a queue backed by a temp SQLite file and registers
// cleanup.
func newTestQueue(t *testing.T) *queue.SQLiteQueue {
	t.Helper()
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "test.db")
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	q, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("queue.New: %v", err)
	}
	t.Cleanup(func() { _ = q.Close() })
	return q
}

// submit is a helper that creates a minimal Job and returns the assigned ID.
func submit(t *testing.T, q *queue.SQLiteQueue, ref, dis, backend string) string {
	t.Helper()
	j := &queue.Job{
		Scoring: queue.ScoringParams{
			Reference: ref,
			Distorted: dis,
			Backend:   backend,
		},
	}
	id, err := q.Submit(context.Background(), j)
	if err != nil {
		t.Fatalf("Submit: %v", err)
	}
	return id
}

// TestSubmitAndGet verifies that a submitted job can be retrieved by ID.
func TestSubmitAndGet(t *testing.T) {
	q := newTestQueue(t)

	id := submit(t, q, "/ref.yuv", "/dis.yuv", "cpu")

	got, err := q.Get(context.Background(), id)
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	if got.ID != id {
		t.Errorf("id: got %q, want %q", got.ID, id)
	}
	if got.Status != queue.StatusPending {
		t.Errorf("status: got %q, want %q", got.Status, queue.StatusPending)
	}
	if got.Scoring.Reference != "/ref.yuv" {
		t.Errorf("reference: got %q, want /ref.yuv", got.Scoring.Reference)
	}
}

// TestPullWorkHappyPath verifies that PullWork transitions a PENDING job to
// RUNNING and assigns it to the requesting node.
func TestPullWorkHappyPath(t *testing.T) {
	q := newTestQueue(t)
	id := submit(t, q, "/ref.yuv", "/dis.yuv", "cpu")

	cap := queue.NodeCapacity{Backends: []string{"cpu", "cuda"}, Slots: 4}
	job, err := q.PullWork(context.Background(), "node-1", cap)
	if err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if job == nil {
		t.Fatal("PullWork returned nil, expected a job")
	}
	if job.ID != id {
		t.Errorf("job ID: got %q, want %q", job.ID, id)
	}
	if job.Status != queue.StatusRunning {
		t.Errorf("status: got %q, want %q", job.Status, queue.StatusRunning)
	}
	if job.AssignedNode != "node-1" {
		t.Errorf("assigned_node: got %q, want node-1", job.AssignedNode)
	}
}

// TestPullWorkEmptyQueue verifies that PullWork returns (nil, nil) when no
// jobs are available.
func TestPullWorkEmptyQueue(t *testing.T) {
	q := newTestQueue(t)

	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 2}
	job, err := q.PullWork(context.Background(), "node-1", cap)
	if err != nil {
		t.Fatalf("PullWork on empty queue: %v", err)
	}
	if job != nil {
		t.Errorf("expected nil job on empty queue, got %v", job)
	}
}

// TestPullWorkBackendFilter verifies that a job requiring "cuda" is not
// returned to a node that only offers "cpu".
func TestPullWorkBackendFilter(t *testing.T) {
	q := newTestQueue(t)
	_ = submit(t, q, "/ref.yuv", "/dis.yuv", "cuda") // requires cuda

	cpuCap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 2}
	job, err := q.PullWork(context.Background(), "cpu-node", cpuCap)
	if err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if job != nil {
		t.Errorf("cpu-only node should not receive cuda job, got job %s", job.ID)
	}

	// A cuda node should get it.
	cudaCap := queue.NodeCapacity{Backends: []string{"cuda", "cpu"}, Slots: 2}
	job, err = q.PullWork(context.Background(), "cuda-node", cudaCap)
	if err != nil {
		t.Fatalf("PullWork (cuda): %v", err)
	}
	if job == nil {
		t.Fatal("cuda node should have received the cuda job, got nil")
	}
}

// TestReportResultCompleted verifies that ReportResult marks the job COMPLETED
// and stores score + features.
func TestReportResultCompleted(t *testing.T) {
	q := newTestQueue(t)
	id := submit(t, q, "/r.yuv", "/d.yuv", "")
	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 1}
	_, _ = q.PullWork(context.Background(), "node-x", cap)

	result := &queue.JobResult{
		Score:    76.6683,
		Features: map[string]float64{"adm2": 0.98, "vif_scale0": 0.91},
	}
	if err := q.ReportResult(context.Background(), id, result); err != nil {
		t.Fatalf("ReportResult: %v", err)
	}

	got, err := q.Get(context.Background(), id)
	if err != nil {
		t.Fatalf("Get after report: %v", err)
	}
	if got.Status != queue.StatusCompleted {
		t.Errorf("status: got %q, want %q", got.Status, queue.StatusCompleted)
	}
	if got.Score != 76.6683 {
		t.Errorf("score: got %v, want 76.6683", got.Score)
	}
}

// TestReportResultFailed verifies that a failed report sets status to "failed"
// and records the error message.
func TestReportResultFailed(t *testing.T) {
	q := newTestQueue(t)
	id := submit(t, q, "/r.yuv", "/d.yuv", "")
	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 1}
	_, _ = q.PullWork(context.Background(), "node-x", cap)

	result := &queue.JobResult{Err: "libvmaf returned exit 1"}
	if err := q.ReportResult(context.Background(), id, result); err != nil {
		t.Fatalf("ReportResult: %v", err)
	}

	got, err := q.Get(context.Background(), id)
	if err != nil {
		t.Fatalf("Get after fail report: %v", err)
	}
	if got.Status != queue.StatusFailed {
		t.Errorf("status: got %q, want %q", got.Status, queue.StatusFailed)
	}
	if got.Error != "libvmaf returned exit 1" {
		t.Errorf("error: got %q, want %q", got.Error, "libvmaf returned exit 1")
	}
}

// TestCancelPending verifies that a PENDING job can be cancelled.
func TestCancelPending(t *testing.T) {
	q := newTestQueue(t)
	id := submit(t, q, "/r.yuv", "/d.yuv", "")

	if err := q.Cancel(context.Background(), id); err != nil {
		t.Fatalf("Cancel: %v", err)
	}
	got, err := q.Get(context.Background(), id)
	if err != nil {
		t.Fatalf("Get after cancel: %v", err)
	}
	if got.Status != queue.StatusCancelled {
		t.Errorf("status: got %q, want cancelled", got.Status)
	}

	// PullWork should not return the cancelled job.
	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 1}
	job, err := q.PullWork(context.Background(), "node-1", cap)
	if err != nil {
		t.Fatalf("PullWork after cancel: %v", err)
	}
	if job != nil {
		t.Errorf("expected nil after cancellation, got job %s", job.ID)
	}
}

// TestNodeDisconnectedMidJob verifies that when a controller restarts (simulated
// by reopening the queue), running jobs are reset to PENDING.
func TestNodeDisconnectedMidJob(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "test.db")
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))

	// First queue instance: submit and pull a job to put it in RUNNING state.
	q1, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("queue.New (first): %v", err)
	}
	id := submit(t, q1, "/r.yuv", "/d.yuv", "")
	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 1}
	_, err = q1.PullWork(context.Background(), "node-crash", cap)
	if err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	_ = q1.Close()

	// Simulate controller restart: open a second queue instance against the
	// same database.  The RUNNING job should be reset to PENDING.
	q2, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("queue.New (second): %v", err)
	}
	defer q2.Close()

	got, err := q2.Get(context.Background(), id)
	if err != nil {
		t.Fatalf("Get after restart: %v", err)
	}
	if got.Status != queue.StatusPending {
		t.Errorf("expected PENDING after restart, got %q", got.Status)
	}
	if got.AssignedNode != "" {
		t.Errorf("expected empty assigned_node after restart, got %q", got.AssignedNode)
	}
}

// TestPullWork_GetUnlockedFailure_RollsBackToPending verifies that when
// getUnlocked fails after the SQL UPDATE to RUNNING, PullWork rolls back:
//   - SQL status returns to "pending" with assigned_node=NULL
//   - runningSet does not contain the job
//   - FIFO has the job re-prepended (PendingCount == 1)
//
// ADR-0961: round-25 scheduler-correctness audit, bug B.1.
func TestPullWork_GetUnlockedFailure_RollsBackToPending(t *testing.T) {
	q := newTestQueue(t)
	id := submit(t, q, "/ref.yuv", "/dis.yuv", "cpu")

	// Install a hook that fails only for the target job ID.
	// The hook fires for every getUnlocked call including FIFO-scan ones; we
	// count invocations and only start failing on the second call — the first
	// is the FIFO-scan pass (which succeeds so the job is matched), the second
	// is the post-UPDATE fetch that the bug report identifies as the failure
	// point.
	var callCount int
	q.SetGetUnlockedHookForTest(func(gotID string) error {
		if gotID != id {
			return nil
		}
		callCount++
		if callCount >= 2 {
			return fmt.Errorf("injected getUnlocked failure (call %d)", callCount)
		}
		return nil
	})

	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 1}
	job, err := q.PullWork(context.Background(), "node-1", cap)

	// PullWork must return an error (not silently orphan the job).
	if err == nil {
		t.Fatal("PullWork: expected error from injected getUnlocked failure, got nil")
	}
	// PullWork must not return a job object on the error path.
	if job != nil {
		t.Errorf("PullWork: expected nil job on error path, got %+v", job)
	}

	// Remove the hook before calling Get so it reads cleanly.
	q.SetGetUnlockedHookForTest(nil)

	// SQL status must be back to "pending".
	got, getErr := q.Get(context.Background(), id)
	if getErr != nil {
		t.Fatalf("Get after failed PullWork: %v", getErr)
	}
	if got.Status != queue.StatusPending {
		t.Errorf("status after rollback: got %q, want %q", got.Status, queue.StatusPending)
	}
	if got.AssignedNode != "" {
		t.Errorf("assigned_node after rollback: got %q, want empty", got.AssignedNode)
	}

	// runningSet must not contain the job (checked via RunningCount proxy).
	if q.RunningCount() != 0 {
		t.Errorf("RunningCount after rollback: got %d, want 0", q.RunningCount())
	}

	// FIFO must have the job back (PendingCount proxy).
	if q.PendingCount() != 1 {
		t.Errorf("PendingCount after rollback: got %d, want 1", q.PendingCount())
	}

	// A subsequent PullWork (without the hook) must succeed and get the same job.
	job2, err2 := q.PullWork(context.Background(), "node-2", cap)
	if err2 != nil {
		t.Fatalf("PullWork after rollback: %v", err2)
	}
	if job2 == nil {
		t.Fatal("PullWork after rollback: expected job, got nil")
	}
	if job2.ID != id {
		t.Errorf("job ID after rollback: got %q, want %q", job2.ID, id)
	}
	if job2.Status != queue.StatusRunning {
		t.Errorf("job status after rollback: got %q, want %q", job2.Status, queue.StatusRunning)
	}
}

// TestPendingCount verifies that PendingCount decrements when work is pulled.
func TestPendingCount(t *testing.T) {
	q := newTestQueue(t)

	if q.PendingCount() != 0 {
		t.Errorf("initial pending: got %d, want 0", q.PendingCount())
	}

	_ = submit(t, q, "/r1.yuv", "/d1.yuv", "")
	_ = submit(t, q, "/r2.yuv", "/d2.yuv", "")

	if q.PendingCount() != 2 {
		t.Errorf("after 2 submits: got %d, want 2", q.PendingCount())
	}

	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 2}
	_, _ = q.PullWork(context.Background(), "node-1", cap)

	if q.PendingCount() != 1 {
		t.Errorf("after 1 pull: got %d, want 1", q.PendingCount())
	}
}
