// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/nodes/registry.go — in-memory node registry with
// heartbeat-based liveness tracking.
//
// Every vmafx-node instance registers with the controller on startup,
// then sends a Heartbeat RPC every ~10 s.  A node that misses heartbeats
// for more than HeartbeatTimeout (60 s) is considered dead and removed.
//
// Registered nodes are stored in an in-memory map keyed by node_id.
// There is no SQLite persistence for nodes: on controller restart, nodes
// must re-register (their heartbeat loop does this automatically).
//
// Thread safety: all exported methods are safe for concurrent use.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.

package nodes

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"log/slog"
	"sync"
	"time"
)

const (
	// HeartbeatTimeout is the duration after which a silent node is evicted.
	HeartbeatTimeout = 60 * time.Second

	// sessionTokenLen is the number of random bytes in a session token.
	sessionTokenLen = 16
)

// Capability describes what a vmafx-node can run.
type Capability struct {
	// GPUVendor is one of "nvidia", "amd", "intel", "cpu".
	GPUVendor string
	// Backends is the list of available backend strings, e.g. ["cuda", "cpu"].
	Backends []string
	// Concurrency is the number of concurrent scoring slots the node offers.
	Concurrency int
}

// Node represents an active vmafx-node session.
type Node struct {
	ID            string
	Name          string
	SessionToken  string
	Capability    Capability
	LastHeartbeat time.Time
	JobsRunning   int
}

// Registry tracks live vmafx-node instances.
type Registry struct {
	mu    sync.RWMutex
	nodes map[string]*Node // keyed by node ID
	log   *slog.Logger
}

// NewRegistry creates an empty Registry and starts the reaper goroutine.
func NewRegistry(log *slog.Logger) *Registry {
	r := &Registry{
		nodes: make(map[string]*Node),
		log:   log,
	}
	go r.reaper()
	return r
}

// Register adds (or replaces) a node.  Returns the assigned node_id and
// session_token.
func (r *Registry) Register(name string, cap Capability) (nodeID, sessionToken string, err error) {
	nodeID, err = generateID()
	if err != nil {
		return "", "", fmt.Errorf("registry: generate node ID: %w", err)
	}
	sessionToken, err = generateID()
	if err != nil {
		return "", "", fmt.Errorf("registry: generate session token: %w", err)
	}

	r.mu.Lock()
	r.nodes[nodeID] = &Node{
		ID:            nodeID,
		Name:          name,
		SessionToken:  sessionToken,
		Capability:    cap,
		LastHeartbeat: time.Now(),
	}
	r.mu.Unlock()

	r.log.Info("node registered",
		"node_id", nodeID,
		"name", name,
		"gpu_vendor", cap.GPUVendor,
		"backends", cap.Backends,
		"concurrency", cap.Concurrency,
	)
	return nodeID, sessionToken, nil
}

// Heartbeat updates the last-seen timestamp for a node.  Returns false if
// the node_id / session_token pair is unknown (caller should re-register).
func (r *Registry) Heartbeat(nodeID, sessionToken string, jobsRunning int) bool {
	r.mu.Lock()
	defer r.mu.Unlock()

	n, ok := r.nodes[nodeID]
	if !ok || n.SessionToken != sessionToken {
		return false
	}
	n.LastHeartbeat = time.Now()
	n.JobsRunning = jobsRunning
	return true
}

// Get returns a snapshot of a node by ID.  Returns (nil, false) if not found.
func (r *Registry) Get(nodeID string) (*Node, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	n, ok := r.nodes[nodeID]
	if !ok {
		return nil, false
	}
	// Return a copy to avoid data races on the caller side.
	cp := *n
	return &cp, true
}

// ValidateSession returns true if the node_id and session_token are both
// present and match.
func (r *Registry) ValidateSession(nodeID, sessionToken string) bool {
	r.mu.RLock()
	defer r.mu.RUnlock()
	n, ok := r.nodes[nodeID]
	return ok && n.SessionToken == sessionToken
}

// All returns a snapshot of all live nodes.
func (r *Registry) All() []*Node {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]*Node, 0, len(r.nodes))
	for _, n := range r.nodes {
		cp := *n
		out = append(out, &cp)
	}
	return out
}

// Count returns the number of currently registered nodes.
func (r *Registry) Count() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.nodes)
}

// reaper runs in the background and evicts nodes that have not sent a
// heartbeat within HeartbeatTimeout.
func (r *Registry) reaper(_ ...context.Context) {
	ticker := time.NewTicker(HeartbeatTimeout / 3)
	defer ticker.Stop()
	for range ticker.C {
		deadline := time.Now().Add(-HeartbeatTimeout)
		r.mu.Lock()
		for id, n := range r.nodes {
			if n.LastHeartbeat.Before(deadline) {
				r.log.Warn("node evicted (heartbeat timeout)",
					"node_id", id,
					"name", n.Name,
					"last_heartbeat", n.LastHeartbeat,
				)
				delete(r.nodes, id)
			}
		}
		r.mu.Unlock()
	}
}

// generateID returns a 32-char hex string from 16 crypto-random bytes.
func generateID() (string, error) {
	buf := make([]byte, sessionTokenLen)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	return hex.EncodeToString(buf), nil
}
