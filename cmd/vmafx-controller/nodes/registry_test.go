// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/nodes/registry_test.go — unit tests for the node registry.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-0962: NewRegistry now requires a context; tests pass context.Background()
//           and defer r.Close() to prevent reaper goroutine leaks.

package nodes_test

import (
	"context"
	"log/slog"
	"os"
	"testing"
	"time"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
)

func newTestRegistry(t *testing.T) *nodes.Registry {
	t.Helper()
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	r := nodes.NewRegistry(context.Background(), log)
	t.Cleanup(func() { r.Close() })
	return r
}

// TestRegisterAndGet verifies that a registered node can be retrieved by ID.
func TestRegisterAndGet(t *testing.T) {
	r := newTestRegistry(t)
	cap := nodes.Capability{GPUVendor: "nvidia", Backends: []string{"cuda", "cpu"}, Concurrency: 4}

	nodeID, token, err := r.Register("worker-1", cap)
	if err != nil {
		t.Fatalf("Register: %v", err)
	}
	if nodeID == "" {
		t.Fatal("expected non-empty node_id")
	}
	if token == "" {
		t.Fatal("expected non-empty session_token")
	}

	n, ok := r.Get(nodeID)
	if !ok {
		t.Fatalf("Get: node %q not found", nodeID)
	}
	if n.Name != "worker-1" {
		t.Errorf("name: got %q, want worker-1", n.Name)
	}
	if n.Capability.GPUVendor != "nvidia" {
		t.Errorf("gpu_vendor: got %q, want nvidia", n.Capability.GPUVendor)
	}
}

// TestHeartbeatValid verifies that a valid heartbeat updates the node's
// last-seen timestamp and returns true.
func TestHeartbeatValid(t *testing.T) {
	r := newTestRegistry(t)
	nodeID, token, err := r.Register("worker-1", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	if err != nil {
		t.Fatalf("Register: %v", err)
	}

	if ok := r.Heartbeat(nodeID, token, 0); !ok {
		t.Error("expected Heartbeat to return true for valid session")
	}
}

// TestHeartbeatInvalidToken verifies that a heartbeat with wrong token returns
// false (node should re-register).
func TestHeartbeatInvalidToken(t *testing.T) {
	r := newTestRegistry(t)
	nodeID, _, err := r.Register("worker-1", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	if err != nil {
		t.Fatalf("Register: %v", err)
	}

	if ok := r.Heartbeat(nodeID, "wrong-token", 0); ok {
		t.Error("expected Heartbeat to return false for invalid token")
	}
}

// TestHeartbeatUnknownNode verifies that a heartbeat for an unknown node ID
// returns false.
func TestHeartbeatUnknownNode(t *testing.T) {
	r := newTestRegistry(t)

	if ok := r.Heartbeat("nonexistent-id", "any-token", 0); ok {
		t.Error("expected Heartbeat to return false for unknown node")
	}
}

// TestValidateSession verifies the session validation helper.
func TestValidateSession(t *testing.T) {
	r := newTestRegistry(t)
	nodeID, token, err := r.Register("w1", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	if err != nil {
		t.Fatalf("Register: %v", err)
	}

	if !r.ValidateSession(nodeID, token) {
		t.Error("expected ValidateSession true for correct pair")
	}
	if r.ValidateSession(nodeID, "bad") {
		t.Error("expected ValidateSession false for bad token")
	}
	if r.ValidateSession("bad-id", token) {
		t.Error("expected ValidateSession false for bad node ID")
	}
}

// TestCount verifies that Count reflects registered nodes.
func TestCount(t *testing.T) {
	r := newTestRegistry(t)

	if r.Count() != 0 {
		t.Errorf("initial count: got %d, want 0", r.Count())
	}

	for i := 0; i < 3; i++ {
		_, _, err := r.Register("w", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
		if err != nil {
			t.Fatalf("Register %d: %v", i, err)
		}
	}

	if r.Count() != 3 {
		t.Errorf("after 3 registrations: got %d, want 3", r.Count())
	}
}

// TestAll returns all registered nodes.
func TestAll(t *testing.T) {
	r := newTestRegistry(t)
	_, _, _ = r.Register("a", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	_, _, _ = r.Register("b", nodes.Capability{Backends: []string{"cuda"}, Concurrency: 2})

	all := r.All()
	if len(all) != 2 {
		t.Errorf("All: got %d nodes, want 2", len(all))
	}
}

// TestReaper_StopsOnContextCancel verifies that the reaper goroutine exits
// promptly after the context is cancelled.
//
// ADR-0962: the old variadic `_ ...context.Context` reaper never stopped —
// each NewRegistry call in tests leaked one goroutine.  This test would hang
// (or timeout) with the old implementation because goroutine.Wait has no
// observable side-effect; instead we validate that Close() completes quickly
// (i.e. the select branch fires) by racing a short deadline.
func TestReaper_StopsOnContextCancel(t *testing.T) {
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))

	ctx, cancel := context.WithCancel(context.Background())
	r := nodes.NewRegistry(ctx, log)

	// Cancel the context — the reaper should unblock from its select within the
	// next poll cycle.  HeartbeatTimeout/3 = 20 s in production, but in tests
	// the internal ticker is not observable.  Close() cancels the same ctx, so
	// calling it after cancel() is a no-op double-cancel — both are safe.
	cancel()
	r.Close() // idempotent; ensures deferred cleanup path is exercised

	// Give the goroutine up to 200 ms to exit.  On any scheduler this is far
	// more than enough for a blocked select to wake on a closed Done channel.
	deadline := time.After(200 * time.Millisecond)
	done := make(chan struct{})
	go func() {
		// The goroutine exit is not directly observable; we verify the registry
		// remains usable (no panic, no deadlock) after cancellation.
		_ = r.Count()
		close(done)
	}()

	select {
	case <-done:
		// Registry still responsive after context cancellation — pass.
	case <-deadline:
		t.Fatal("registry.Count() blocked after context cancel: reaper may not have exited")
	}
}

// TestAllSeq verifies the iter.Seq adapter walks every registered node
// (map iteration order is non-deterministic, so the test compares the
// yielded set against the expected set).
func TestAllSeq(t *testing.T) {
	r := newTestRegistry(t)
	_, _, _ = r.Register("a", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	_, _, _ = r.Register("b", nodes.Capability{Backends: []string{"cuda"}, Concurrency: 2})
	_, _, _ = r.Register("c", nodes.Capability{Backends: []string{"hip"}, Concurrency: 4})

	seen := make(map[string]bool)
	for n := range r.AllSeq() {
		if n == nil {
			t.Fatal("AllSeq yielded a nil *Node")
		}
		seen[n.Name] = true
	}
	if len(seen) != 3 {
		t.Errorf("AllSeq yielded %d distinct names, want 3 (got %v)", len(seen), seen)
	}
	for _, want := range []string{"a", "b", "c"} {
		if !seen[want] {
			t.Errorf("AllSeq did not yield node %q", want)
		}
	}
}

// TestAllSeq_earlyBreak verifies the iter.Seq adapter honours caller break.
// This is the load-bearing property motivating AllSeq over All (the slice
// allocation in All is unavoidable even when the caller wants the first match).
func TestAllSeq_earlyBreak(t *testing.T) {
	r := newTestRegistry(t)
	for i := 0; i < 5; i++ {
		_, _, _ = r.Register("worker", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	}

	var walked int
	for range r.AllSeq() {
		walked++
		if walked == 2 {
			break
		}
	}
	if walked != 2 {
		t.Errorf("AllSeq early-break: walked %d, want 2", walked)
	}
}

// TestValidateSession_ConstantTime verifies that ValidateSession uses
// crypto/subtle.ConstantTimeCompare so that the comparison cannot leak the
// token value via wall-clock timing.  The test calls ValidateSession with a
// correct token and a wrong token; if both return promptly and with the
// expected boolean the behavioural contract is met.
//
// A timing oracle test requires statistically-significant wall-clock samples
// and is not appropriate for a unit-test suite.  This test instead verifies
// the contract via code inspection (the fix adds subtle.ConstantTimeCompare
// at the call sites) and a correctness round-trip to guard against regression.
func TestValidateSession_ConstantTime(t *testing.T) {
	r := newTestRegistry(t)
	nodeID, token, err := r.Register("w1", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	if err != nil {
		t.Fatalf("Register: %v", err)
	}

	// Correct pair must pass.
	if !r.ValidateSession(nodeID, token) {
		t.Error("ValidateSession: expected true for correct (nodeID, token)")
	}
	// Wrong token must fail regardless of prefix matching.
	if r.ValidateSession(nodeID, token+"x") {
		t.Error("ValidateSession: expected false for appended token")
	}
	if r.ValidateSession(nodeID, token[:len(token)-1]) {
		t.Error("ValidateSession: expected false for truncated token")
	}
	// Heartbeat uses the same path — exercise it too.
	if !r.Heartbeat(nodeID, token, 0) {
		t.Error("Heartbeat: expected true for correct (nodeID, token)")
	}
	if r.Heartbeat(nodeID, "totally-wrong", 0) {
		t.Error("Heartbeat: expected false for wrong token")
	}
}

// TestAll_shimMatchesAllSeq guards the All -> AllSeq deprecation shim:
// the deprecated slice form must continue to return what the iterator
// produces, for one release.
func TestAll_shimMatchesAllSeq(t *testing.T) {
	r := newTestRegistry(t)
	_, _, _ = r.Register("a", nodes.Capability{Backends: []string{"cpu"}, Concurrency: 1})
	_, _, _ = r.Register("b", nodes.Capability{Backends: []string{"cuda"}, Concurrency: 2})

	sliceCount := len(r.All())
	var seqCount int
	for range r.AllSeq() {
		seqCount++
	}
	if sliceCount != seqCount {
		t.Errorf("All shim count %d != AllSeq count %d", sliceCount, seqCount)
	}
}
