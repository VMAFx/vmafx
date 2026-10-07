// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris

package queue_test

import (
	"context"
	"errors"
	"log/slog"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
)

func statsOf(t *testing.T, q *queue.SQLiteQueue) queue.Stats {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	st, err := q.Stats(ctx)
	if err != nil {
		t.Fatalf("Stats: %v", err)
	}
	return st
}

// TestStatsCountsPerTenant: pending and running counts per tenant, the oldest
// pending submission time, and nothing for finished or cancelled jobs.
func TestStatsCountsPerTenant(t *testing.T) {
	q := newTestQueue(t)
	ctx := context.Background()
	before := time.Now().Add(-time.Second)
	submitFor(t, q, "a")
	submitFor(t, q, "a")
	cancelled := submitFor(t, q, "a")
	submitFor(t, q, "b")
	if changed, err := q.Cancel(ctx, cancelled); err != nil || !changed {
		t.Fatalf("Cancel = %v, %v", changed, err)
	}
	if _, err := q.PullWork(ctx, "node-b", "b", queue.NodeCapacity{Slots: 1}); err != nil {
		t.Fatalf("PullWork: %v", err)
	}

	st := statsOf(t, q)
	if len(st.Tenants) != 2 {
		t.Fatalf("tenants = %+v, want a and b", st.Tenants)
	}
	a, b := st.Tenants[0], st.Tenants[1]
	if a.TenantID != "a" || a.Pending != 2 || a.Running != 0 || a.OldestPending.Before(before) {
		t.Errorf("tenant a = %+v, want 2 pending, 0 running, oldest after %v", a, before)
	}
	if b.TenantID != "b" || b.Pending != 0 || b.Running != 1 || !b.OldestPending.IsZero() {
		t.Errorf("tenant b = %+v, want 0 pending, 1 running, no oldest", b)
	}
}

// TestStatsEmptyQueue is the boundary: no tenant rows and no requeues.
func TestStatsEmptyQueue(t *testing.T) {
	st := statsOf(t, newTestQueue(t))
	if len(st.Tenants) != 0 || len(st.Requeued) != 0 {
		t.Errorf("empty queue stats = %+v", st)
	}
}

// TestStatsCountsRequeuesByReason covers node_lost and assign_rollback; the
// restart case is TestStatsCountsRestartRequeues.
func TestStatsCountsRequeuesByReason(t *testing.T) {
	q := newTestQueue(t)
	ctx := context.Background()
	submitFor(t, q, "t")
	submitFor(t, q, "t")
	if _, err := q.PullWork(ctx, "node-a", "t", queue.NodeCapacity{Slots: 2}); err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if n, err := q.RequeueNode(ctx, "node-a"); err != nil || n != 1 {
		t.Fatalf("RequeueNode = %d, %v", n, err)
	}
	q.SetGetUnlockedHookForTest(failSecondFetch())
	if _, err := q.PullWork(ctx, "node-b", "t", queue.NodeCapacity{Slots: 1}); err == nil {
		t.Fatal("PullWork with a failing fetch succeeded")
	}
	q.SetGetUnlockedHookForTest(nil)

	got := statsOf(t, q).Requeued
	if got[queue.RequeueNodeLost] != 1 || got[queue.RequeueRollback] != 1 || got[queue.RequeueRestart] != 0 {
		t.Errorf("requeued = %v, want node_lost 1, assign_rollback 1, controller_restart 0", got)
	}
}

// failSecondFetch fails the second read of each job: the first is the FIFO
// scan, the second the read back after the assignment (ADR-0961).
func failSecondFetch() func(string) error {
	calls := map[string]int{}
	return func(id string) error {
		calls[id]++
		if calls[id] >= 2 {
			return errors.New("injected fetch failure")
		}
		return nil
	}
}

// TestStatsCountsRestartRequeues: a job running when the queue closed comes
// back PENDING when it reopens, and counts as a controller_restart requeue.
func TestStatsCountsRestartRequeues(t *testing.T) {
	dbPath := filepath.Join(t.TempDir(), "restart.db")
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	q, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	submitFor(t, q, "t")
	if _, err := q.PullWork(context.Background(), "node-a", "t", queue.NodeCapacity{Slots: 1}); err != nil {
		t.Fatalf("PullWork: %v", err)
	}
	if err := q.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	reopened, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	t.Cleanup(func() { _ = reopened.Close() })
	st := statsOf(t, reopened)
	if st.Requeued[queue.RequeueRestart] != 1 || len(st.Tenants) != 1 || st.Tenants[0].Pending != 1 {
		t.Errorf("after restart: %+v, want one restart requeue and one pending job", st)
	}
}

// TestStatsHonoursTheContext is the negative case: a cancelled context fails
// the read instead of blocking the scrape.
func TestStatsHonoursTheContext(t *testing.T) {
	q := newTestQueue(t)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := q.Stats(ctx); err == nil {
		t.Fatal("Stats with a cancelled context succeeded")
	}
}
