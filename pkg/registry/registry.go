// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/registry/registry.go — generic in-memory keyed store with RWMutex
// concurrency, snapshot-copy reads, predicate-bulk eviction, and a
// caller-provided counter constraint for observability hooks.
//
// Use case: any controller-style subsystem that needs the Add/Get/All/Delete
// pattern with safe snapshot semantics (no aliasing leaks back to callers).
// The original motivation was collapsing the duplicated map + sync.RWMutex
// + map-copy plumbing in cmd/vmafx-controller/nodes/registry.go (and
// avoiding two near-identical narrow interfaces in pkg/observability —
// see Counter below).
//
// Design notes:
//
//   - Store snapshots values on read via the user-supplied Cloner[V] to keep
//     callers race-free.  The original nodes.Registry used `cp := *n` for
//     this; the generic Store hoists that pattern.
//   - Mutating helpers (Update, EvictWhere) take callbacks that run under the
//     write lock so domain logic can compose its own atomic transitions
//     (heartbeats, status updates) without exposing the lock.
//   - The SQLite-backed job queue is intentionally NOT a Store consumer; its
//     storage paradigm is durable, not in-memory.  Forcing both behind one
//     interface would over-abstract.  See ADR-0925 §Alternatives considered.
//
// ADR-0925: Generic in-memory registry for vmafx-controller subsystems.
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion (origin).

package registry

import (
	"sync"
)

// Cloner returns an independent copy of v.  Callers supply it so the Store
// can hand out snapshots without leaking internal aliasing back through the
// API.  For value-semantic V (struct copy is sufficient) a one-liner
// `func(v V) V { return v }` works.
type Cloner[V any] func(v V) V

// Counter is the constraint used by pkg/observability to register
// GaugeFunc instruments backed by any registry-shaped subsystem.  It
// replaces the two single-method narrow interfaces (jobQueueSource,
// nodeRegistrySource) with one generic mechanism.
type Counter interface {
	Count() int
}

// Store is a thread-safe keyed map with snapshot reads, predicate eviction,
// and write-side update callbacks.
//
// K must be comparable (map key constraint).  V is unconstrained; pass a
// Cloner that performs the deepest copy callers will see (shallow struct
// copy is fine when V holds no internal pointers callers can mutate).
type Store[K comparable, V any] struct {
	mu    sync.RWMutex
	items map[K]V
	clone Cloner[V]
}

// New returns an empty Store backed by an initialised map.  clone must not
// be nil; pass a `func(v V) V { return v }` for value-only V.
func New[K comparable, V any](clone Cloner[V]) *Store[K, V] {
	return &Store[K, V]{
		items: make(map[K]V),
		clone: clone,
	}
}

// Put inserts or replaces the value at key k.  The stored value is the
// caller-supplied v as-is; if v contains pointers that the caller will keep
// mutating, the caller is responsible for deep-copying before Put.
func (s *Store[K, V]) Put(k K, v V) {
	s.mu.Lock()
	s.items[k] = v
	s.mu.Unlock()
}

// Get returns a snapshot copy of the value at k.  The second return is false
// when k is absent.
func (s *Store[K, V]) Get(k K) (V, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	v, ok := s.items[k]
	if !ok {
		var zero V
		return zero, false
	}
	return s.clone(v), true
}

// All returns a snapshot copy of every value, in unspecified order.
func (s *Store[K, V]) All() []V {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]V, 0, len(s.items))
	for _, v := range s.items {
		out = append(out, s.clone(v))
	}
	return out
}

// Count returns the number of items.  Satisfies the Counter constraint.
func (s *Store[K, V]) Count() int {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return len(s.items)
}

// Delete removes the entry at k.  Returns true if an entry was removed.
func (s *Store[K, V]) Delete(k K) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.items[k]; !ok {
		return false
	}
	delete(s.items, k)
	return true
}

// Update applies fn to the value at k under the write lock and writes the
// returned value back.  fn must NOT call other Store methods (it runs with
// the write lock held — re-entry would deadlock).
//
// Returns (zero, false) when k is absent; the caller's fn is not invoked.
// Returns the post-update value (copied via the Cloner) when fn ran.
func (s *Store[K, V]) Update(k K, fn func(v V) V) (V, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	v, ok := s.items[k]
	if !ok {
		var zero V
		return zero, false
	}
	updated := fn(v)
	s.items[k] = updated
	return s.clone(updated), true
}

// Read applies fn to the value at k under the read lock.  Use when a caller
// needs to inspect multiple fields atomically without committing a write.
// Returns false when k is absent (fn is not invoked).
//
// fn must NOT call other Store methods.
func (s *Store[K, V]) Read(k K, fn func(v V)) bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	v, ok := s.items[k]
	if !ok {
		return false
	}
	fn(v)
	return true
}

// EvictWhere removes every entry for which pred returns true.  The visit
// function (may be nil) is invoked for each evicted entry under the write
// lock — useful for logging the deletion in the same critical section.
//
// Returns the number of entries removed.
func (s *Store[K, V]) EvictWhere(pred func(k K, v V) bool, visit func(k K, v V)) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	removed := 0
	for k, v := range s.items {
		if !pred(k, v) {
			continue
		}
		if visit != nil {
			visit(k, v)
		}
		delete(s.items, k)
		removed++
	}
	return removed
}
