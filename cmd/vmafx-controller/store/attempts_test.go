// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/attempts_test.go — attempt lookup and counts.

package store_test

import (
	"context"
	"errors"
	"testing"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
)

func TestAttemptOfFindsTheSessionsAttempt(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	id := submit(t, db.store, "t1", "cpu", 0)
	mine := register(t, db.store, "t1", "n1")
	other := register(t, db.store, "t1", "n2")
	if _, err := db.store.AttemptOf(ctx, mine, id); !errors.Is(err, store.ErrFenced) {
		t.Fatalf("before any claim: err = %v, want ErrFenced", err)
	}
	claim(t, db.store, mine, "cpu")
	if a, err := db.store.AttemptOf(ctx, mine, id); err != nil || a != 1 {
		t.Fatalf("running attempt: %d %v", a, err)
	}
	if _, err := db.store.AttemptOf(ctx, other, id); !errors.Is(err, store.ErrFenced) {
		t.Fatalf("another session: err = %v, want ErrFenced", err)
	}
	if _, err := db.store.Report(ctx, store.AttemptRef{Session: mine, JobID: id, Attempt: 1}, store.Result{Score: 50}); err != nil {
		t.Fatalf("report: %v", err)
	}
	if a, err := db.store.AttemptOf(ctx, mine, id); err != nil || a != 1 {
		t.Fatalf("after the job ended, the last attempt of the session: %d %v", a, err)
	}
	foreign := register(t, db.store, "t2", "n3")
	if _, err := db.store.AttemptOf(ctx, foreign, id); !errors.Is(err, store.ErrNotFound) {
		t.Fatalf("another tenant: err = %v, want ErrNotFound", err)
	}
}

func TestStatsCountPerTenantAndLiveSessions(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	submit(t, db.store, "t1", "cpu", 0)
	submit(t, db.store, "t1", "cpu", 0)
	submit(t, db.store, "t2", "cpu", 0)
	ref := register(t, db.store, "t1", "n1")
	claim(t, db.store, ref, "cpu")
	st, err := db.store.Stats(ctx)
	if err != nil {
		t.Fatalf("stats: %v", err)
	}
	if st.LiveNodes != 1 || len(st.Tenants) != 2 {
		t.Fatalf("stats: %+v", st)
	}
	t1, t2 := st.Tenants[0], st.Tenants[1]
	if t1.TenantID != "t1" || t1.Pending != 1 || t1.Running != 1 || t1.OldestPending.IsZero() {
		t.Fatalf("t1: %+v", t1)
	}
	if t2.TenantID != "t2" || t2.Pending != 1 || t2.Running != 0 {
		t.Fatalf("t2: %+v", t2)
	}
}

func TestRunningAttemptOnlyWhileHeld(t *testing.T) {
	t.Parallel()
	db := newTestDB(t, testImage)
	ctx := context.Background()
	id := submit(t, db.store, "t1", "cpu", 0)
	ref := register(t, db.store, "t1", "n1")
	if _, held, err := db.store.RunningAttempt(ctx, ref, id); err != nil || held {
		t.Fatalf("before the claim: held=%v err=%v", held, err)
	}
	claim(t, db.store, ref, "cpu")
	if a, held, err := db.store.RunningAttempt(ctx, ref, id); err != nil || !held || a != 1 {
		t.Fatalf("while running: %d %v %v", a, held, err)
	}
	if done, err := db.store.Cancel(ctx, "t1", id); err != nil || !done {
		t.Fatalf("cancel: %v %v", done, err)
	}
	if _, held, err := db.store.RunningAttempt(ctx, ref, id); err != nil || held {
		t.Fatalf("after cancel: held=%v err=%v", held, err)
	}
}
