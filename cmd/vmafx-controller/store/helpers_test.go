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
	"log/slog"
	"math"
	"net/url"
	"testing"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/golusoris/golusoris/testutil/pg"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
)

// testImage is the PostgreSQL release the chart deploys (ADR-2350 D1).
const testImage = "postgres:18.6-alpine"

// oldestImage is the oldest release external servers may run.
const oldestImage = "postgres:16.15-alpine"

// appRole is the role the store connects as. It owns the tables, like the
// application user of a CloudNativePG cluster, and is not a superuser, so
// row-level security applies to it (FORCE ROW LEVEL SECURITY).
const appRole = "vmafx_app"

// testDB is a migrated database and the store on it.
type testDB struct {
	store *store.Postgres
	pool  *pgxpool.Pool // the application role's pool
	dsn   string
}

// newTestDB starts PostgreSQL (image), creates the application role, migrates
// as that role, and returns the store.
func newTestDB(t *testing.T, image string) testDB {
	t.Helper()
	admin := pg.Start(t, pg.Options{Image: image})
	ctx := context.Background()
	for _, stmt := range []string{
		"CREATE ROLE " + appRole + " LOGIN PASSWORD 'app'",
		"GRANT CREATE ON SCHEMA public TO " + appRole,
	} {
		if _, err := admin.Exec(ctx, stmt); err != nil {
			t.Fatalf("%s: %v", stmt, err)
		}
	}
	dsn := roleDSN(t, admin.Config().ConnString())
	if err := store.MigratePostgres(dsn, slog.New(slog.DiscardHandler)); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	pool, err := pgxpool.New(ctx, dsn)
	if err != nil {
		t.Fatalf("app pool: %v", err)
	}
	t.Cleanup(pool.Close)
	return testDB{store: store.NewPostgres(pool), pool: pool, dsn: dsn}
}

// roleDSN rewrites the container's superuser DSN to the application role.
func roleDSN(t *testing.T, dsn string) string {
	t.Helper()
	u, err := url.Parse(dsn)
	if err != nil {
		t.Fatalf("parse DSN: %v", err)
	}
	u.User = url.UserPassword(appRole, "app")
	return u.String()
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

// expireLease moves a job's lease into the past, as if its node fell silent.
func expireLease(t *testing.T, db testDB, id uuid.UUID) {
	t.Helper()
	ctx := context.Background()
	tx, err := db.pool.Begin(ctx)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if _, err := tx.Exec(ctx, "SELECT set_config('vmafx.maintenance', 'on', true)"); err != nil {
		t.Fatalf("maintenance: %v", err)
	}
	tag, err := tx.Exec(ctx,
		"UPDATE jobs SET lease_expires_at = now() - interval '1 second' WHERE id = $1 AND status = 'running'", id)
	if err != nil || tag.RowsAffected() != 1 {
		t.Fatalf("expire lease of %s: rows=%d err=%v", id, tag.RowsAffected(), err)
	}
	if err := tx.Commit(ctx); err != nil {
		t.Fatalf("commit: %v", err)
	}
}

// noBackoff makes an expired job claimable again at once.
func noBackoff(int32) time.Duration { return 0 }

// nan is a feature value JSON cannot carry.
func nan() float64 { return math.NaN() }
