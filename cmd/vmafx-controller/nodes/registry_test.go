// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/nodes/registry_test.go — unit tests for the node registry.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.

package nodes_test

import (
	"log/slog"
	"os"
	"testing"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
)

func newTestRegistry(t *testing.T) *nodes.Registry {
	t.Helper()
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	return nodes.NewRegistry(log)
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
