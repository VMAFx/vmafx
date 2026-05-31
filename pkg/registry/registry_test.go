// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/registry/registry_test.go — unit tests for the generic in-memory store.
//
// ADR-0925: Generic in-memory registry for vmafx-controller subsystems.

package registry_test

import (
	"sync"
	"testing"

	"github.com/VMAFx/vmafx/pkg/registry"
)

// item is a simple value type used by the table-driven tests.  It carries
// no internal pointers, so a shallow struct copy is a valid clone.
type item struct {
	Name   string
	Count  int
	Active bool
}

func cloneItem(v item) item { return v }

func newTestStore() *registry.Store[string, item] {
	return registry.New[string, item](cloneItem)
}

func TestPutAndGet(t *testing.T) {
	s := newTestStore()
	s.Put("a", item{Name: "alpha", Count: 1})

	got, ok := s.Get("a")
	if !ok {
		t.Fatal("Get: expected ok=true")
	}
	if got.Name != "alpha" || got.Count != 1 {
		t.Errorf("Get: got %+v, want {alpha 1 false}", got)
	}
}

func TestGetMissing(t *testing.T) {
	s := newTestStore()
	got, ok := s.Get("missing")
	if ok {
		t.Errorf("Get missing: expected ok=false, got %+v", got)
	}
	if got != (item{}) {
		t.Errorf("Get missing: expected zero value, got %+v", got)
	}
}

func TestGetReturnsSnapshot(t *testing.T) {
	s := newTestStore()
	s.Put("a", item{Name: "alpha", Count: 1})

	got, _ := s.Get("a")
	got.Name = "MUTATED"

	again, _ := s.Get("a")
	if again.Name != "alpha" {
		t.Errorf("snapshot leak: caller mutation propagated to store: got %q", again.Name)
	}
}

func TestAll(t *testing.T) {
	s := newTestStore()
	s.Put("a", item{Name: "alpha"})
	s.Put("b", item{Name: "beta"})

	all := s.All()
	if len(all) != 2 {
		t.Errorf("All: got %d, want 2", len(all))
	}
}

func TestCount(t *testing.T) {
	s := newTestStore()
	if s.Count() != 0 {
		t.Errorf("initial Count: got %d, want 0", s.Count())
	}
	s.Put("a", item{})
	s.Put("b", item{})
	if s.Count() != 2 {
		t.Errorf("Count after 2 Put: got %d, want 2", s.Count())
	}
}

func TestDelete(t *testing.T) {
	s := newTestStore()
	s.Put("a", item{})

	if !s.Delete("a") {
		t.Error("Delete present: got false, want true")
	}
	if s.Delete("a") {
		t.Error("Delete absent: got true, want false")
	}
	if _, ok := s.Get("a"); ok {
		t.Error("Get after Delete: still present")
	}
}

func TestUpdateExisting(t *testing.T) {
	s := newTestStore()
	s.Put("a", item{Count: 1})

	updated, ok := s.Update("a", func(v item) item {
		v.Count++
		v.Active = true
		return v
	})
	if !ok {
		t.Fatal("Update existing: ok=false")
	}
	if updated.Count != 2 || !updated.Active {
		t.Errorf("Update return: got %+v, want {Count:2 Active:true}", updated)
	}

	got, _ := s.Get("a")
	if got.Count != 2 || !got.Active {
		t.Errorf("Update persisted: got %+v, want {Count:2 Active:true}", got)
	}
}

func TestUpdateMissing(t *testing.T) {
	s := newTestStore()
	called := false
	_, ok := s.Update("missing", func(v item) item {
		called = true
		return v
	})
	if ok {
		t.Error("Update missing: ok=true, want false")
	}
	if called {
		t.Error("Update missing: callback ran (should be skipped)")
	}
}

func TestRead(t *testing.T) {
	s := newTestStore()
	s.Put("a", item{Name: "alpha", Count: 7})

	var observed item
	ok := s.Read("a", func(v item) { observed = v })
	if !ok {
		t.Fatal("Read existing: ok=false")
	}
	if observed.Name != "alpha" || observed.Count != 7 {
		t.Errorf("Read observation: got %+v", observed)
	}

	called := false
	ok = s.Read("missing", func(v item) { called = true })
	if ok || called {
		t.Errorf("Read missing: ok=%v called=%v, want false false", ok, called)
	}
}

func TestEvictWhere(t *testing.T) {
	s := newTestStore()
	for _, k := range []string{"keep1", "evict1", "keep2", "evict2"} {
		s.Put(k, item{Name: k})
	}

	visited := make([]string, 0)
	removed := s.EvictWhere(
		func(k string, _ item) bool { return k[0] == 'e' },
		func(k string, _ item) { visited = append(visited, k) },
	)
	if removed != 2 {
		t.Errorf("EvictWhere removed: got %d, want 2", removed)
	}
	if len(visited) != 2 {
		t.Errorf("visit callback: got %d invocations, want 2", len(visited))
	}
	if s.Count() != 2 {
		t.Errorf("post-evict count: got %d, want 2", s.Count())
	}
}

// TestCounterConstraint verifies the *Store[K, V] type satisfies the
// Counter constraint used by pkg/observability gauge wiring.
func TestCounterConstraint(t *testing.T) {
	s := newTestStore()
	s.Put("a", item{})
	s.Put("b", item{})
	s.Put("c", item{})

	var c registry.Counter = s
	if c.Count() != 3 {
		t.Errorf("Counter view: got %d, want 3", c.Count())
	}
}

// TestConcurrentWrites exercises the Store under -race to catch any locking
// regression.
func TestConcurrentWrites(t *testing.T) {
	s := registry.New[int, int](func(v int) int { return v })

	var wg sync.WaitGroup
	for i := 0; i < 100; i++ {
		wg.Add(1)
		go func(k int) {
			defer wg.Done()
			s.Put(k, k*2)
			_, _ = s.Get(k)
			_, _ = s.Update(k, func(v int) int { return v + 1 })
		}(i)
	}
	wg.Wait()

	if s.Count() != 100 {
		t.Errorf("post-concurrent count: got %d, want 100", s.Count())
	}
}
