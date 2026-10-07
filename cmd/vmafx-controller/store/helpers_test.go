// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/helpers_test.go — a migrated PostgreSQL store for
// each test.

package store_test

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"testing"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store/storetest"
)

// testImage and oldestImage are the releases the tests run on.
const (
	testImage   = storetest.Image
	oldestImage = storetest.OldestImage
)

// testDB is a migrated database and the store on it.
type testDB struct {
	store *store.Postgres
	pool  *pgxpool.Pool // the application role's pool
	dsn   string
}

// newTestDB starts PostgreSQL (image) and migrates it (storetest.New).
func newTestDB(t *testing.T, image string) testDB {
	t.Helper()
	db := storetest.New(t, image)
	return testDB{store: db.Store, pool: db.Pool, dsn: db.DSN}
}

// spec is a job specification for tests.
func spec(t *testing.T, ref string) json.RawMessage {
	t.Helper()
	b, err := json.Marshal(map[string]string{"reference": ref, "distorted": ref + ".dis"})
	if err != nil {
		t.Fatalf("spec: %v", err)
	}
	return b
}

// submit enqueues a job of tenant on backend and returns its ID.
func submit(t *testing.T, s *store.Postgres, tenant, backend string, priority int32) uuid.UUID {
	t.Helper()
	job, created, err := s.Submit(context.Background(), store.SubmitParams{
		TenantID: tenant, Spec: spec(t, fmt.Sprintf("%s-%s-%d", tenant, backend, time.Now().UnixNano())),
		Backend: backend, Priority: priority,
	})
	if err != nil || !created {
		t.Fatalf("submit: created=%v err=%v", created, err)
	}
	return job.ID
}

// register opens a session for node of tenant.
func register(t *testing.T, s *store.Postgres, tenant, node string) store.SessionRef {
	t.Helper()
	sess, err := s.RegisterSession(context.Background(), store.RegisterParams{
		TenantID: tenant, NodeID: node, TTL: time.Minute,
	})
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	return store.SessionRef{ID: sess.ID, TenantID: tenant, Token: sess.Token}
}

// claim leases the next job of ref's tenant for backends.
func claim(t *testing.T, s *store.Postgres, ref store.SessionRef, backends ...string) *store.Claim {
	t.Helper()
	c, err := s.Claim(context.Background(), store.ClaimParams{Session: ref, Backends: backends, LeaseTTL: time.Minute})
	if err != nil {
		t.Fatalf("claim: %v", err)
	}
	return c
}

// expireLease moves a job's lease into the past (storetest.ExpireLease).
func expireLease(t *testing.T, db testDB, id uuid.UUID) {
	t.Helper()
	storetest.ExpireLease(t, db.pool, id.String())
}

// noBackoff makes an expired job claimable again at once.
func noBackoff(int32) time.Duration { return 0 }

// nan is a feature value JSON cannot carry.
func nan() float64 { return math.NaN() }
