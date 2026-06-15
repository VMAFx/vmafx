// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/scheduler/scheduler_test.go — unit tests for the scheduler.
//
// Tests use real queue + registry instances backed by an in-memory SQLite DB.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-1119: NewRegistry(log) no longer takes a context; the reaper is launched
//           by Start(ctx).  These tests exercise Register/ValidateSession only
//           (the reaper is immaterial) and defer Close() for leak-free cleanup.

package scheduler_test

import (
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/scheduler"
)

// fixture wires up a Scheduler with a real SQLite queue and an in-memory
// node registry.
type fixture struct {
	q   *queue.SQLiteQueue
	r   *nodes.Registry
	s   *scheduler.Scheduler
	log *slog.Logger
}

func newFixture(t *testing.T) *fixture {
	t.Helper()
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "sched.db")
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))

	q, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("queue.New: %v", err)
	}
	t.Cleanup(func() { _ = q.Close() })

	r := nodes.NewRegistry(log)
	t.Cleanup(func() { r.Close() })
	s := scheduler.New(q, r, log)
	return &fixture{q: q, r: r, s: s, log: log}
}

// TestAssignHappyPath verifies that a registered node with matching capabilities
// receives the oldest PENDING job.
func TestAssignHappyPath(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()

	nodeID, token, err := f.r.Register("node-1", nodes.Capability{
		GPUVendor:   "nvidia",
		Backends:    []string{"cuda", "cpu"},
		Concurrency: 2,
	})
	if err != nil {
		t.Fatalf("Register: %v", err)
	}

	jobID, err := f.q.Submit(ctx, &queue.Job{
		Scoring: queue.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv", Backend: "cuda"},
	})
	if err != nil {
		t.Fatalf("Submit: %v", err)
	}

	cap := nodes.Capability{GPUVendor: "nvidia", Backends: []string{"cuda", "cpu"}, Concurrency: 2}
	job, err := f.s.Assign(ctx, nodeID, token, cap)
	if err != nil {
		t.Fatalf("Assign: %v", err)
	}
	if job == nil {
		t.Fatal("expected a job, got nil")
	}
	if job.ID != jobID {
		t.Errorf("job ID: got %q, want %q", job.ID, jobID)
	}
}

// TestAssignInvalidSession verifies that Assign returns an error for an unknown
// or expired session.
func TestAssignInvalidSession(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()

	cap := nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1}
	_, err := f.s.Assign(ctx, "nonexistent-node", "bad-token", cap)
	if err == nil {
		t.Error("expected error for invalid session, got nil")
	}
}

// TestAssignNoMatchingJob verifies that Assign returns (nil, nil) when no job
// in the queue matches the node's capabilities.
func TestAssignNoMatchingJob(t *testing.T) {
	f := newFixture(t)
	ctx := context.Background()

	nodeID, token, err := f.r.Register("cpu-node", nodes.Capability{
		GPUVendor:   "cpu",
		Backends:    []string{"cpu"},
		Concurrency: 1,
	})
	if err != nil {
		t.Fatalf("Register: %v", err)
	}

	// Submit a CUDA job; the cpu-only node should not receive it.
	_, err = f.q.Submit(ctx, &queue.Job{
		Scoring: queue.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv", Backend: "cuda"},
	})
	if err != nil {
		t.Fatalf("Submit: %v", err)
	}

	cpuCap := nodes.Capability{GPUVendor: "cpu", Backends: []string{"cpu"}, Concurrency: 1}
	job, err := f.s.Assign(ctx, nodeID, token, cpuCap)
	if err != nil {
		t.Fatalf("Assign: %v", err)
	}
	if job != nil {
		t.Errorf("cpu-only node should not receive cuda job, got %s", job.ID)
	}
}

// TestAssignJobBackToQueueOnNodeDisconnect verifies that a running job is
// returned to PENDING when the controller restarts (queue reload behaviour
// tested in queue_test.go; here we verify the scheduler sees it again).
func TestAssignJobBackToQueueOnNodeDisconnect(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "sched.db")
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	ctx := context.Background()

	// First controller instance: submit, pull (node "crashes").
	q1, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("q1 New: %v", err)
	}
	r1 := nodes.NewRegistry(log)
	// Stop the reaper goroutine before the test exits — otherwise it
	// leaks past the test's lifetime and trips goroutine-leak detectors.
	defer r1.Close()
	s1 := scheduler.New(q1, r1, log)

	nodeID1, token1, err := r1.Register("crash-node", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	if err != nil {
		t.Fatalf("Register: %v", err)
	}

	jobID, err := q1.Submit(ctx, &queue.Job{
		Scoring: queue.ScoringParams{Reference: "/r.yuv", Distorted: "/d.yuv"},
	})
	if err != nil {
		t.Fatalf("Submit: %v", err)
	}

	cap := nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1}
	job, err := s1.Assign(ctx, nodeID1, token1, cap)
	if err != nil || job == nil {
		t.Fatalf("Assign (pre-crash): err=%v job=%v", err, job)
	}
	_ = q1.Close()

	// Second controller instance: the job should be back to PENDING.
	q2, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("q2 New: %v", err)
	}
	defer q2.Close()
	r2 := nodes.NewRegistry(log)
	defer r2.Close()
	s2 := scheduler.New(q2, r2, log)

	// Register a new node.
	nodeID2, token2, err := r2.Register("new-node", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	if err != nil {
		t.Fatalf("Register new-node: %v", err)
	}

	newJob, err := s2.Assign(ctx, nodeID2, token2, cap)
	if err != nil {
		t.Fatalf("Assign (post-restart): %v", err)
	}
	if newJob == nil {
		t.Fatal("expected job to be re-queued after node disconnect, got nil")
	}
	if newJob.ID != jobID {
		t.Errorf("job ID: got %q, want %q", newJob.ID, jobID)
	}
}
