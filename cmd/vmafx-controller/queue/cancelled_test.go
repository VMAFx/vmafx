// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/queue/cancelled_test.go — CancelledAmong names only
// the given IDs of one tenant's CANCELLED jobs, in the given order (ADR-1567).

package queue_test

import (
	"context"
	"slices"
	"testing"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
)

func TestCancelledAmong(t *testing.T) {
	q := newTestQueue(t)
	ctx := context.Background()
	submit := func(tenant string) string {
		t.Helper()
		id, err := q.Submit(ctx, &queue.Job{TenantID: tenant, Scoring: queue.ScoringParams{Reference: "/r", Distorted: "/d"}})
		if err != nil {
			t.Fatalf("Submit: %v", err)
		}
		return id
	}
	first, second, pending, rival := submit("acme"), submit("acme"), submit("acme"), submit("rival")
	for _, id := range []string{first, second, rival} {
		if _, err := q.Cancel(ctx, id); err != nil {
			t.Fatalf("Cancel %s: %v", id, err)
		}
	}

	got, err := q.CancelledAmong(ctx, "acme", []string{second, pending, "unknown", rival, first})
	if err != nil {
		t.Fatalf("CancelledAmong: %v", err)
	}
	if want := []string{second, first}; !slices.Equal(got, want) {
		t.Fatalf("CancelledAmong = %v, want %v (input order, acme's cancelled jobs only)", got, want)
	}
	if got, err := q.CancelledAmong(ctx, "acme", nil); err != nil || len(got) != 0 {
		t.Fatalf("no IDs: %v, %v; want nothing", got, err)
	}
	if got, err := q.CancelledAmong(ctx, "rival", []string{first, rival}); err != nil || !slices.Equal(got, []string{rival}) {
		t.Fatalf("rival: %v, %v; want only its own job", got, err)
	}
}
