// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store/storetest/storetest.go — a migrated PostgreSQL
// database for tests of the store and of the packages built on it.

// Package storetest starts PostgreSQL in a container (golusoris testutil/pg),
// creates the application role, migrates as that role and returns the store.
// It is imported by tests only.
package storetest

import (
	"context"
	"errors"
	"log/slog"
	"net/url"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/golusoris/golusoris/testutil/pg"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store"
)

// Image is the PostgreSQL release the chart deploys (ADR-2350 D1). golusoris
// testutil/pg refuses a test image without a digest (since v0.13.0), so both
// images are pinned as tag@sha256 of the multi-platform index.
const Image = "postgres:18.6-alpine@sha256:77f585114c32fbca283dc835b0596f4e52b51b4c6662d7810b2f4084f60a1873"

// OldestImage is the oldest release external servers may run.
const OldestImage = "postgres:16.15-alpine@sha256:721873c34ceb9f8d8fc265984940dc982404c105f19ad51be9fdc5970a6080ea"

// setupTimeout bounds every database call of the helpers (HISS-02).
const setupTimeout = 2 * time.Minute

// AppRole is the role the store connects as. It owns the tables, like the
// application user of a CloudNativePG cluster, and is not a superuser, so
// row-level security applies to it (FORCE ROW LEVEL SECURITY).
const AppRole = "vmafx_app"

// DB is a migrated database and the store on it.
type DB struct {
	Store *store.Postgres
	// Pool is the application role's pool.
	Pool *pgxpool.Pool
	// DSN connects as the application role.
	DSN string
}

// New starts PostgreSQL (image), creates the application role, migrates as
// that role, and returns the store. Everything is torn down with t.
func New(t *testing.T, image string) DB {
	t.Helper()
	admin := pg.Start(t, pg.Options{Image: image})
	ctx, cancel := context.WithTimeout(context.Background(), setupTimeout)
	defer cancel()
	for _, stmt := range []string{
		"CREATE ROLE " + AppRole + " LOGIN PASSWORD 'app'",
		"GRANT CREATE ON SCHEMA public TO " + AppRole,
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
	return DB{Store: store.NewPostgres(pool), Pool: pool, DSN: dsn}
}

// roleDSN rewrites the container's superuser DSN to the application role.
func roleDSN(t *testing.T, dsn string) string {
	t.Helper()
	u, err := url.Parse(dsn)
	if err != nil {
		t.Fatalf("parse DSN: %v", err)
	}
	u.User = url.UserPassword(AppRole, "app")
	return u.String()
}

// ExpireLease moves a running job's lease into the past, as if its node fell
// silent.
func ExpireLease(t *testing.T, pool *pgxpool.Pool, jobID string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), setupTimeout)
	defer cancel()
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	defer func() {
		if rerr := tx.Rollback(ctx); rerr != nil && !errors.Is(rerr, pgx.ErrTxClosed) {
			t.Errorf("rollback: %v", rerr)
		}
	}()
	if _, err := tx.Exec(ctx, "SELECT set_config('vmafx.maintenance', 'on', true)"); err != nil {
		t.Fatalf("maintenance: %v", err)
	}
	tag, err := tx.Exec(ctx,
		"UPDATE jobs SET lease_expires_at = now() - interval '1 second' WHERE id = $1 AND status = 'running'", jobID)
	if err != nil || tag.RowsAffected() != 1 {
		t.Fatalf("expire lease of %s: rows=%d err=%v", jobID, tag.RowsAffected(), err)
	}
	if err := tx.Commit(ctx); err != nil {
		t.Fatalf("commit: %v", err)
	}
}
