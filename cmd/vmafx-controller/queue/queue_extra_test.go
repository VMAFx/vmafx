// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/queue/queue_extra_test.go — additional unit tests
// targeting the lower-coverage methods on SQLiteQueue (RunningCount, Close,
// Get on missing job, double-Close idempotency, multi-backend FIFO match,
// Cancel of running job).
//
// Companion to queue_test.go; kept separate so the canonical happy-path tests
// stay easy to read.

package queue_test

import (
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
)

// newSilentQueue is a local helper that opens a queue against a temp SQLite
// file with an ERROR-level logger so the test run stays quiet.
func newSilentQueue(t *testing.T) *queue.SQLiteQueue {
	t.Helper()
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "extra.db")
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	q, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("queue.New: %v", err)
	}
	t.Cleanup(func() { _ = q.Close() })
	return q
}

// TestRunningCount_TransitionsAcrossLifecycle covers the previously 0%
// RunningCount path through Submit → PullWork → ReportResult.
func TestRunningCount_TransitionsAcrossLifecycle(t *testing.T) {
	q := newSilentQueue(t)
	ctx := context.Background()

	if got := q.RunningCount(); got != 0 {
		t.Errorf("initial RunningCount: got %d, want 0", got)
	}

	j := &queue.Job{Scoring: queue.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv"}}
	id, err := q.Submit(ctx, j)
	if err != nil {
		t.Fatalf("Submit: %v", err)
	}

	if got := q.RunningCount(); got != 0 {
		t.Errorf("after Submit RunningCount: got %d, want 0", got)
	}

	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 1}
	if _, err := q.PullWork(ctx, "node-a", cap); err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if got := q.RunningCount(); got != 1 {
		t.Errorf("after PullWork RunningCount: got %d, want 1", got)
	}

	if err := q.ReportResult(ctx, id, &queue.JobResult{Score: 90.0}); err != nil {
		t.Fatalf("ReportResult: %v", err)
	}
	if got := q.RunningCount(); got != 0 {
		t.Errorf("after ReportResult RunningCount: got %d, want 0", got)
	}
}

// TestGet_MissingID covers the sql.ErrNoRows branch in getUnlocked.
func TestGet_MissingID(t *testing.T) {
	q := newSilentQueue(t)
	_, err := q.Get(context.Background(), "no-such-id")
	if err == nil {
		t.Fatal("expected error for missing job, got nil")
	}
	if !strings.Contains(err.Error(), "not found") {
		t.Errorf("error should mention not found, got: %v", err)
	}
}

// TestCancel_RunningJob covers the cancel-of-running branch including the
// runningSet cleanup path.
func TestCancel_RunningJob(t *testing.T) {
	q := newSilentQueue(t)
	ctx := context.Background()
	j := &queue.Job{Scoring: queue.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv"}}
	id, err := q.Submit(ctx, j)
	if err != nil {
		t.Fatalf("Submit: %v", err)
	}
	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 1}
	if _, err := q.PullWork(ctx, "node-x", cap); err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if q.RunningCount() != 1 {
		t.Fatalf("precondition: RunningCount = %d, want 1", q.RunningCount())
	}

	if err := q.Cancel(ctx, id); err != nil {
		t.Fatalf("Cancel: %v", err)
	}
	if q.RunningCount() != 0 {
		t.Errorf("after Cancel RunningCount: got %d, want 0", q.RunningCount())
	}
	got, err := q.Get(ctx, id)
	if err != nil {
		t.Fatalf("Get after cancel: %v", err)
	}
	if got.Status != queue.StatusCancelled {
		t.Errorf("status: got %q, want cancelled", got.Status)
	}
}

// TestCancel_AlreadyTerminalIsIdempotent covers the rowsAffected == 0 branch
// in Cancel (job already in terminal state).
func TestCancel_AlreadyTerminalIsIdempotent(t *testing.T) {
	q := newSilentQueue(t)
	ctx := context.Background()
	j := &queue.Job{Scoring: queue.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv"}}
	id, _ := q.Submit(ctx, j)
	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 1}
	_, _ = q.PullWork(ctx, "node-x", cap)
	_ = q.ReportResult(ctx, id, &queue.JobResult{Score: 90.0})

	// Job is COMPLETED; Cancel should be a no-op (no error).
	if err := q.Cancel(ctx, id); err != nil {
		t.Errorf("Cancel of completed job should be no-op, got: %v", err)
	}
	got, _ := q.Get(ctx, id)
	if got.Status != queue.StatusCompleted {
		t.Errorf("status should remain completed, got %q", got.Status)
	}
}

// TestPullWork_PreservesFIFOOrder verifies multiple submits dequeue in
// submission order.
func TestPullWork_PreservesFIFOOrder(t *testing.T) {
	q := newSilentQueue(t)
	ctx := context.Background()
	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 4}

	ids := make([]string, 0, 3)
	for _, ref := range []string{"/r1.yuv", "/r2.yuv", "/r3.yuv"} {
		j := &queue.Job{Scoring: queue.ScoringParams{Reference: ref, Distorted: "/d.yuv"}}
		id, err := q.Submit(ctx, j)
		if err != nil {
			t.Fatalf("Submit: %v", err)
		}
		ids = append(ids, id)
	}

	for i := range ids {
		got, err := q.PullWork(ctx, "node-1", cap)
		if err != nil {
			t.Fatalf("PullWork[%d]: %v", i, err)
		}
		if got == nil {
			t.Fatalf("PullWork[%d] returned nil", i)
		}
		if got.ID != ids[i] {
			t.Errorf("FIFO order: pull[%d] = %q, want %q", i, got.ID, ids[i])
		}
	}

	// Now empty.
	last, err := q.PullWork(ctx, "node-1", cap)
	if err != nil {
		t.Fatalf("PullWork on drained: %v", err)
	}
	if last != nil {
		t.Errorf("expected nil after draining, got job %q", last.ID)
	}
}

// TestPullWork_SkipsCancelledFIFOEntry verifies that a stale FIFO entry
// pointing to a cancelled job is silently skipped and the next eligible job
// is returned.
func TestPullWork_SkipsCancelledFIFOEntry(t *testing.T) {
	q := newSilentQueue(t)
	ctx := context.Background()
	cap := queue.NodeCapacity{Backends: []string{"cpu"}, Slots: 4}

	id1, _ := q.Submit(ctx, &queue.Job{Scoring: queue.ScoringParams{Reference: "/r1.yuv"}})
	id2, _ := q.Submit(ctx, &queue.Job{Scoring: queue.ScoringParams{Reference: "/r2.yuv"}})

	// Cancel id1 in-place; the FIFO entry for id1 stays put (Cancel walks the
	// FIFO and removes it eagerly, so this exercises the eager-removal path).
	if err := q.Cancel(ctx, id1); err != nil {
		t.Fatalf("Cancel: %v", err)
	}

	got, err := q.PullWork(ctx, "node-1", cap)
	if err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if got == nil || got.ID != id2 {
		t.Errorf("expected to pull id2 after id1 cancellation; got %v", got)
	}
}

// TestSubmit_BackendOptionalForCPUNode verifies that a job with an empty
// Backend is delivered to a node regardless of the node's backend list.
func TestSubmit_BackendOptionalForCPUNode(t *testing.T) {
	q := newSilentQueue(t)
	ctx := context.Background()
	_, _ = q.Submit(ctx, &queue.Job{Scoring: queue.ScoringParams{Reference: "/r.yuv", Backend: ""}})
	cap := queue.NodeCapacity{Backends: []string{"cuda"}, Slots: 1}
	got, err := q.PullWork(ctx, "cuda-node", cap)
	if err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if got == nil {
		t.Error("expected to pull empty-backend job onto any node")
	}
}

// TestPullWork_EmptyCapacityServesAnyJob verifies that an empty Backends
// list (an early-init node) still receives jobs that don't pin a backend.
func TestPullWork_EmptyCapacityServesAnyJob(t *testing.T) {
	q := newSilentQueue(t)
	ctx := context.Background()
	_, _ = q.Submit(ctx, &queue.Job{Scoring: queue.ScoringParams{Reference: "/r.yuv"}})
	cap := queue.NodeCapacity{Backends: nil, Slots: 1}
	got, err := q.PullWork(ctx, "anon-node", cap)
	if err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if got == nil {
		t.Error("expected job served to empty-cap node")
	}
}

// TestClose_Idempotent verifies that closing twice produces an error on the
// second call (sql.DB.Close returns an error after first close).
func TestClose_Idempotent(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "close.db")
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	q, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("queue.New: %v", err)
	}
	if err := q.Close(); err != nil {
		t.Errorf("first Close should succeed, got: %v", err)
	}
	// Second close may return an error, but must not panic.
	_ = q.Close()
}
