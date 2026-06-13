// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/nodes/registry_edge_test.go — edge-case + concurrency
// tests for the node registry.
//
// These extend the happy-path coverage in registry_test.go to exercise:
//   - Concurrent Register / Heartbeat / Get under -race
//   - Get on an unknown node returns (nil, false)
//   - All() and Count() reflect dedup of stale Register calls (each Register
//     produces a fresh node_id, so there is no "same name = same node" dedup;
//     the test pins that contract so it isn't broken inadvertently).
//   - Heartbeat updates LastHeartbeat and JobsRunning observably.
//
// We deliberately do NOT exercise the reaper goroutine's actual eviction
// timing (HeartbeatTimeout / 3 ≈ 20 s) — that would make the test slow.
// The reaper's logic is exercised indirectly via the timing-independent
// state machine (LastHeartbeat updates).
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.

package nodes_test

import (
	"sync"
	"testing"
	"time"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
)

// TestGetUnknownNode verifies that Get on a non-existent ID returns (nil, false).
func TestGetUnknownNode(t *testing.T) {
	t.Parallel()
	r := newTestRegistry(t)

	n, ok := r.Get("nonexistent")
	if ok {
		t.Errorf("Get(unknown): expected ok=false, got ok=true, node=%+v", n)
	}
	if n != nil {
		t.Errorf("Get(unknown): expected nil node, got %+v", n)
	}
}

// TestRegisterReturnsDistinctIDsForSameName verifies that Register does NOT
// dedup by name — each call mints a fresh node_id. This pins the current
// contract; if a future ADR changes it, this test must be updated.
func TestRegisterReturnsDistinctIDsForSameName(t *testing.T) {
	t.Parallel()
	r := newTestRegistry(t)
	cap := nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1}

	id1, _, err := r.Register("worker", cap)
	if err != nil {
		t.Fatalf("Register #1: %v", err)
	}
	id2, _, err := r.Register("worker", cap)
	if err != nil {
		t.Fatalf("Register #2: %v", err)
	}
	if id1 == id2 {
		t.Errorf("expected distinct IDs for same name, both got %q", id1)
	}
	if got := r.Count(); got != 2 {
		t.Errorf("Count after 2 same-name registrations: got %d, want 2", got)
	}
}

// TestHeartbeatUpdatesJobsRunning verifies that Heartbeat propagates the
// jobs-running counter to the registered node.
func TestHeartbeatUpdatesJobsRunning(t *testing.T) {
	t.Parallel()
	r := newTestRegistry(t)
	id, tok, err := r.Register("w", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 4})
	if err != nil {
		t.Fatalf("Register: %v", err)
	}

	if ok := r.Heartbeat(id, tok, 3); !ok {
		t.Fatal("Heartbeat returned false")
	}
	n, ok := r.Get(id)
	if !ok {
		t.Fatalf("Get(%q): not found", id)
	}
	if n.JobsRunning != 3 {
		t.Errorf("JobsRunning: got %d, want 3", n.JobsRunning)
	}
}

// TestHeartbeatAdvancesTimestamp verifies that Heartbeat moves LastHeartbeat
// forward in time (the reaper's eviction predicate).
func TestHeartbeatAdvancesTimestamp(t *testing.T) {
	t.Parallel()
	r := newTestRegistry(t)
	id, tok, err := r.Register("w", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	if err != nil {
		t.Fatalf("Register: %v", err)
	}
	before, _ := r.Get(id)
	prev := before.LastHeartbeat

	// Sleep a small amount so the timestamp delta is observable.
	time.Sleep(2 * time.Millisecond)
	if ok := r.Heartbeat(id, tok, 0); !ok {
		t.Fatal("Heartbeat returned false")
	}
	after, _ := r.Get(id)
	if !after.LastHeartbeat.After(prev) {
		t.Errorf("LastHeartbeat did not advance: before=%v after=%v", prev, after.LastHeartbeat)
	}
}

// TestConcurrentRegisterHeartbeat exercises the registry under -race with
// concurrent Register / Heartbeat / Get / Count callers.
func TestConcurrentRegisterHeartbeat(t *testing.T) {
	t.Parallel()
	r := newTestRegistry(t)

	const (
		workers   = 16
		opsPerW   = 32
		cpuCnt    = 1
		jobsBurst = 7
	)
	var wg sync.WaitGroup
	wg.Add(workers)

	for w := 0; w < workers; w++ {
		go func() {
			defer wg.Done()
			id, tok, err := r.Register("worker", nodes.Capability{Backends: []string{"cpu"}, Concurrency: cpuCnt})
			if err != nil {
				t.Errorf("Register: %v", err)
				return
			}
			for i := 0; i < opsPerW; i++ {
				if ok := r.Heartbeat(id, tok, jobsBurst); !ok {
					t.Errorf("Heartbeat returned false for %q", id)
					return
				}
				_, _ = r.Get(id)
				_ = r.Count()
				_ = r.ValidateSession(id, tok)
			}
		}()
	}
	wg.Wait()

	if got := r.Count(); got != workers {
		t.Errorf("Count after concurrent registers: got %d, want %d", got, workers)
	}
	all := r.All()
	if len(all) != workers {
		t.Errorf("All() len: got %d, want %d", len(all), workers)
	}
	// Spot-check that the per-node copies returned by All() are independent
	// (mutating one must not affect the registry).
	if len(all) > 0 {
		all[0].JobsRunning = -999
		again, _ := r.Get(all[0].ID)
		if again != nil && again.JobsRunning == -999 {
			t.Errorf("All() did not return a defensive copy: mutation leaked back into registry")
		}
	}
}
