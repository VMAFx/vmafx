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
// Storage: pkg/registry.Store[string, Node] supplies the keyed map,
// RWMutex, snapshot copying, and predicate eviction (ADR-0925).  This
// file holds only the heartbeat / session / capability-specific logic.
// On controller restart, nodes must re-register (their heartbeat loop
// does this automatically); there is no SQLite persistence for nodes.
//
// Thread safety: all exported methods are safe for concurrent use.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
<<<<<<< ours
// ADR-0962: fix reaper goroutine stop signal (round-25 audit B.4).
=======
// ADR-0925: Generic in-memory registry for vmafx-controller subsystems.
>>>>>>> theirs

package nodes

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"log/slog"
	"time"

	"github.com/VMAFx/vmafx/pkg/registry"
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

// Registry tracks live vmafx-node instances.  It composes a generic
// registry.Store[string, Node] with the heartbeat-eviction reaper and
// session-token semantics specific to node management.
type Registry struct {
<<<<<<< ours
	mu     sync.RWMutex
	nodes  map[string]*Node // keyed by node ID
	log    *slog.Logger
	cancel context.CancelFunc // stops the reaper goroutine
=======
	store *registry.Store[string, Node]
	log   *slog.Logger
>>>>>>> theirs
}

// NewRegistry creates an empty Registry, accepts a context for lifetime
// control, and starts the reaper goroutine.  The reaper stops when ctx is
// cancelled or when Close is called.
//
// ADR-0962: ctx propagation replaces the old variadic no-op signature,
// ensuring tests and callers can stop the goroutine cleanly.
func NewRegistry(ctx context.Context, log *slog.Logger) *Registry {
	reaperCtx, cancel := context.WithCancel(ctx)
	r := &Registry{
<<<<<<< ours
		nodes:  make(map[string]*Node),
		log:    log,
		cancel: cancel,
=======
		store: registry.New[string, Node](cloneNode),
		log:   log,
>>>>>>> theirs
	}
	go r.reaper(reaperCtx)
	return r
}

<<<<<<< ours
// Close stops the background reaper goroutine.  It is safe to call multiple
// times; subsequent calls are no-ops.
func (r *Registry) Close() {
	r.cancel()
=======
// cloneNode is the snapshot copier handed to the generic Store.  Node
// contains a Capability which itself has a Backends slice; a shallow
// struct copy would alias that slice into snapshots.  Copy it explicitly.
func cloneNode(n Node) Node {
	cp := n
	if n.Capability.Backends != nil {
		cp.Capability.Backends = append([]string(nil), n.Capability.Backends...)
	}
	return cp
>>>>>>> theirs
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

	r.store.Put(nodeID, Node{
		ID:            nodeID,
		Name:          name,
		SessionToken:  sessionToken,
		Capability:    cap,
		LastHeartbeat: time.Now(),
	})

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
	tokenOK := false
	_, ok := r.store.Update(nodeID, func(n Node) Node {
		if n.SessionToken != sessionToken {
			return n // leave untouched; tokenOK stays false
		}
		n.LastHeartbeat = time.Now()
		n.JobsRunning = jobsRunning
		tokenOK = true
		return n
	})
	return ok && tokenOK
}

// Get returns a snapshot of a node by ID.  Returns (nil, false) if not found.
func (r *Registry) Get(nodeID string) (*Node, bool) {
	n, ok := r.store.Get(nodeID)
	if !ok {
		return nil, false
	}
	return &n, true
}

// ValidateSession returns true if the node_id and session_token are both
// present and match.
func (r *Registry) ValidateSession(nodeID, sessionToken string) bool {
	match := false
	r.store.Read(nodeID, func(n Node) {
		match = n.SessionToken == sessionToken
	})
	return match
}

// All returns a snapshot of all live nodes.
func (r *Registry) All() []*Node {
	snapshots := r.store.All()
	out := make([]*Node, len(snapshots))
	for i := range snapshots {
		n := snapshots[i]
		out[i] = &n
	}
	return out
}

// Count returns the number of currently registered nodes.  Also satisfies
// the registry.Counter constraint consumed by pkg/observability.
func (r *Registry) Count() int {
	return r.store.Count()
}

// reaper runs in the background and evicts nodes that have not sent a
<<<<<<< ours
// heartbeat within HeartbeatTimeout.  It exits when ctx is cancelled.
//
// ADR-0962: required non-variadic ctx replaces the old `_ ...context.Context`
// no-op that left each NewRegistry call leaking one goroutine.
func (r *Registry) reaper(ctx context.Context) {
	ticker := time.NewTicker(HeartbeatTimeout / 3)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
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
=======
// heartbeat within HeartbeatTimeout.
func (r *Registry) reaper() {
	ticker := time.NewTicker(HeartbeatTimeout / 3)
	defer ticker.Stop()
	for range ticker.C {
		deadline := time.Now().Add(-HeartbeatTimeout)
		r.store.EvictWhere(
			func(_ string, n Node) bool { return n.LastHeartbeat.Before(deadline) },
			func(id string, n Node) {
				r.log.Warn("node evicted (heartbeat timeout)",
					"node_id", id,
					"name", n.Name,
					"last_heartbeat", n.LastHeartbeat,
				)
			},
		)
>>>>>>> theirs
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
