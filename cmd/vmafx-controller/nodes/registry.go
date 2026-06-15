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
// Storage: a plain map[string]*Node guarded by RWMutex.  Snapshots are
// returned by value so callers can mutate without holding the lock.  On
// controller restart, nodes must re-register (their heartbeat loop does
// this automatically); there is no SQLite persistence for nodes.
//
// Thread safety: all exported methods are safe for concurrent use.
//
// Lifecycle (ADR-1119): NewRegistry does NOT spawn the reaper goroutine.
// Call Start(ctx) (wired to an fx OnStart hook) to launch it and Close()
// (wired to an fx OnStop hook) to stop and await it.  The reaper is bound
// to a Close-owned context, so its lifetime is governed entirely by the
// Start/Close pair and no goroutine leaks past Close().  This mirrors the
// vmafx-node FeedbackClient Start/Close lifecycle pattern.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-0962: fix reaper goroutine stop signal (round-25 audit B.4).
// ADR-1119: reaper bound to fx.Lifecycle Start/Close instead of a caller ctx.

package nodes

import (
	"context"
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"fmt"
	"iter"
	"log/slog"
	"sync"
	"sync/atomic"
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

// Registry tracks live vmafx-node instances.  The reaper goroutine is
// launched by Start(ctx) and stopped by Close(); its lifetime is owned by
// an internal, Close-cancelled context rather than a caller-supplied one.
type Registry struct {
	mu    sync.RWMutex
	nodes map[string]*Node // keyed by node ID
	log   *slog.Logger

	// reaperCtx / cancel own the reaper goroutine's lifetime.  cancel is
	// wired in Start() and fired by Close() to stop the reaper after the
	// current tick completes.
	reaperCtx context.Context
	cancel    context.CancelFunc

	// startOnce / closeOnce make Start and Close idempotent: Start launches
	// the reaper at most once, Close stops it at most once.
	startOnce sync.Once
	closeOnce sync.Once
	started   atomic.Bool

	// done is closed when the reaper goroutine has returned.  Close blocks
	// on this so callers observe a synchronous shutdown.
	done chan struct{}
}

// NewRegistry creates an empty Registry.  It does NOT launch the background
// reaper — call Start(ctx) (typically from an fx OnStart hook) to launch it
// and Close() (from an fx OnStop hook) to stop and await it.
//
// ADR-1119: the reaper is bound to the Start/Close pair driven by
// fx.Lifecycle, replacing the prior NewRegistry(ctx) signature that tied the
// goroutine to a caller-supplied context and spawned it at construction.
func NewRegistry(log *slog.Logger) *Registry {
	if log == nil {
		log = slog.Default()
	}
	reaperCtx, cancel := context.WithCancel(context.Background())
	return &Registry{
		nodes:     make(map[string]*Node),
		log:       log,
		reaperCtx: reaperCtx,
		cancel:    cancel,
		done:      make(chan struct{}),
	}
}

// Start launches the background reaper goroutine.  It is idempotent: the
// reaper is launched at most once.  The supplied ctx is observed for
// cancellation in addition to Close(), so either stops the reaper; Close()
// additionally awaits the goroutine's return.
//
// Wire Start to an fx OnStart hook.
func (r *Registry) Start(ctx context.Context) {
	r.startOnce.Do(func() {
		// If Close already cancelled our context (Start-after-Close), do not
		// launch a goroutine; close done so Close's wait stays correct.
		if r.reaperCtx.Err() != nil {
			close(r.done)
			return
		}
		// Propagate cancellation of the caller's ctx into the reaper ctx so an
		// fx OnStart ctx cancel also stops the reaper.
		if ctx != nil {
			go func() {
				select {
				case <-ctx.Done():
					r.cancel()
				case <-r.reaperCtx.Done():
				}
			}()
		}
		r.started.Store(true)
		go func() {
			defer close(r.done)
			r.reaper(r.reaperCtx)
		}()
	})
}

// Close stops the background reaper goroutine and waits for it to return.
// It is safe to call multiple times; subsequent calls are no-ops.  Close is
// correct whether or not Start was ever called: if the reaper was never
// started, Close cancels the context and returns without blocking.
//
// Wire Close to an fx OnStop hook.
func (r *Registry) Close() {
	r.cancel()
	r.closeOnce.Do(func() {
		if r.started.Load() {
			<-r.done
		}
	})
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
	if !ok || subtle.ConstantTimeCompare([]byte(n.SessionToken), []byte(sessionToken)) != 1 {
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
	cp := *n
	if n.Capability.Backends != nil {
		cp.Capability.Backends = append([]string(nil), n.Capability.Backends...)
	}
	return &cp, true
}

// ValidateSession returns true if the node_id and session_token are both
// present and match.
func (r *Registry) ValidateSession(nodeID, sessionToken string) bool {
	r.mu.RLock()
	defer r.mu.RUnlock()
	n, ok := r.nodes[nodeID]
	return ok && subtle.ConstantTimeCompare([]byte(n.SessionToken), []byte(sessionToken)) == 1
}

// All returns a snapshot of all live nodes.
//
// Deprecated: prefer AllSeq for new code.  All retains a slice allocation
// proportional to the registered-node count even when the caller breaks
// out of the range early (e.g. "find first NVIDIA node").  AllSeq is the
// streaming-friendly equivalent and avoids the snapshot.  All remains
// supported for one release as a shim and will be removed in
// v3.x.y-lusoris.N+2 (see ADR-0932).
func (r *Registry) All() []*Node {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]*Node, 0, len(r.nodes))
	for _, n := range r.nodes {
		cp := *n
		if n.Capability.Backends != nil {
			cp.Capability.Backends = append([]string(nil), n.Capability.Backends...)
		}
		out = append(out, &cp)
	}
	return out
}

// AllSeq returns a single-shot iterator over a snapshot of all live nodes.
// Each yielded *Node is a defensive copy (same semantics as Get), safe for
// the caller to mutate without holding the registry lock.
//
// The iterator holds the registry RLock for its full lifetime — callers
// must not call back into Registry methods that take the write lock from
// within the yield function, or they will deadlock.  The common pattern
// (filter, accumulate, dispatch) is safe.  If a caller needs to mutate
// the registry mid-walk, snapshot via AllSeq + slices.Collect first.
//
// Added in v3.x.y-lusoris.N+1 (ADR-0932).  Callers iterating linearly
// with no random access or re-iteration should prefer this over All.
func (r *Registry) AllSeq() iter.Seq[*Node] {
	return func(yield func(*Node) bool) {
		r.mu.RLock()
		defer r.mu.RUnlock()
		for _, n := range r.nodes {
			cp := *n
			if n.Capability.Backends != nil {
				cp.Capability.Backends = append([]string(nil), n.Capability.Backends...)
			}
			if !yield(&cp) {
				return
			}
		}
	}
}

// Count returns the number of currently registered nodes.  Also satisfies
// the registry.Counter constraint consumed by pkg/observability.
func (r *Registry) Count() int {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.nodes)
}

// reaper runs in the background and evicts nodes that have not sent a
// heartbeat within HeartbeatTimeout.  It exits when ctx is cancelled (via
// Close() or a cancelled Start ctx).
//
// ADR-1119: the reaper context is owned by the Start/Close pair (fx
// lifecycle) rather than a caller-supplied context, so Close() both stops
// the loop and awaits this goroutine's return — no goroutine leaks.
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
