// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/store_wiring_test.go — the controller's fx graph on
// the PostgreSQL backend, and the refusals of a bad store configuration.

//go:build cgo

package main

import (
	"context"
	"net/http"
	"testing"
	"time"

	"go.uber.org/fx"
	"go.uber.org/fx/fxtest"

	"github.com/golusoris/golusoris/jobs"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/store/storetest"
)

// readyz returns the status of GET /readyz at addr.
func readyz(t *testing.T, addr string) int {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://"+addr+"/readyz", nil)
	if err != nil {
		t.Fatalf("request: %v", err)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("GET /readyz: %v", err)
	}
	if cerr := resp.Body.Close(); cerr != nil {
		t.Fatalf("close body: %v", cerr)
	}
	return resp.StatusCode
}

func TestControllerRunsOnThePostgresBackend(t *testing.T) {
	db := storetest.New(t, storetest.Image)
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	if err := jobs.Migrate(ctx, db.Pool); err != nil {
		t.Fatalf("River migrations: %v", err)
	}
	app, httpAddr := startPostgresController(t, db.DSN)
	// River starts in the background, so readiness follows within moments.
	waitFor(t, 15*time.Second, "/readyz 200 on a migrated database", func() bool {
		return readyz(t, httpAddr) == http.StatusOK
	})
	if _, err := db.Pool.Exec(ctx, "UPDATE schema_migrations SET dirty = true"); err != nil {
		t.Fatalf("mark the schema dirty: %v", err)
	}
	if code := readyz(t, httpAddr); code != http.StatusServiceUnavailable {
		t.Fatalf("/readyz on a dirty schema: %d, want 503", code)
	}
	app.RequireStop()
}

// startPostgresController starts the controller on db with a 1 s sweep and
// returns the address of its HTTP server.
func startPostgresController(t *testing.T, dsn string) (*fxtest.App, string) {
	t.Helper()
	writeControllerEnv(t)
	httpAddr := freeLocalAddr(t)
	t.Setenv("VMAFX_HTTP_ADDR", httpAddr)
	t.Setenv("VMAFX_STORE_BACKEND", "postgres")
	t.Setenv("VMAFX_DB_DSN", dsn)
	t.Setenv("VMAFX_STORE_SWEEP_INTERVAL", "1s")
	app := fxtest.New(t, productionGraph())
	app.RequireStart()
	return app, httpAddr
}

// completedSweeps counts the lease sweeps River has worked.
func completedSweeps(t *testing.T, ctx context.Context, db storetest.DB) int {
	t.Helper()
	var n int
	if err := db.Pool.QueryRow(ctx,
		"SELECT count(*) FROM river_job WHERE kind = 'vmafx_controller_lease_sweep' AND state = 'completed'",
	).Scan(&n); err != nil {
		t.Fatalf("count sweeps: %v", err)
	}
	return n
}

// waitFor polls cond every 250 ms for at most d.
func waitFor(t *testing.T, d time.Duration, what string, cond func() bool) {
	t.Helper()
	for range int(d / (250 * time.Millisecond)) {
		if cond() {
			return
		}
		time.Sleep(250 * time.Millisecond)
	}
	t.Fatalf("%s: not within %v", what, d)
}

// River must outlive the start hook: the context fx hands a start hook ends
// when the start does (fxtest cancels it on return, fx at its start timeout),
// and River stops with the context it was started with.
func TestRiverKeepsSweepingAfterTheStart(t *testing.T) {
	db := storetest.New(t, storetest.Image)
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	if err := jobs.Migrate(ctx, db.Pool); err != nil {
		t.Fatalf("River migrations: %v", err)
	}
	app, _ := startPostgresController(t, db.DSN)
	waitFor(t, 10*time.Second, "first sweep", func() bool { return completedSweeps(t, ctx, db) > 0 })
	first := completedSweeps(t, ctx, db)
	waitFor(t, 10*time.Second, "sweeps after the start", func() bool { return completedSweeps(t, ctx, db) >= first+2 })
	app.RequireStop()
}

// A controller started before River's migrations waits unready instead of
// exiting, and becomes ready once they are applied.
func TestControllerStartedBeforeItsMigrationsWaitsUnready(t *testing.T) {
	db := storetest.New(t, storetest.Image)
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	app, httpAddr := startPostgresController(t, db.DSN)
	if code := readyz(t, httpAddr); code != http.StatusServiceUnavailable {
		t.Fatalf("/readyz before River's migrations: %d, want 503", code)
	}
	if err := jobs.Migrate(ctx, db.Pool); err != nil {
		t.Fatalf("River migrations: %v", err)
	}
	waitFor(t, 15*time.Second, "ready after the migrations", func() bool {
		return readyz(t, httpAddr) == http.StatusOK
	})
	app.RequireStop()
}

func TestControllerRefusesABadStoreConfiguration(t *testing.T) {
	cases := map[string]map[string]string{
		"unknown backend":  {"VMAFX_STORE_BACKEND": "etcd"},
		"postgres, no DSN": {"VMAFX_STORE_BACKEND": "postgres", "VMAFX_DB_DSN": ""},
		"negative lease":   {"VMAFX_STORE_LEASE_TTL": "-1s"},
		"unparsable sweep": {"VMAFX_STORE_SWEEP_INTERVAL": "soon"},
	}
	for name, env := range cases {
		t.Run(name, func(t *testing.T) {
			writeControllerEnv(t)
			for k, v := range env {
				t.Setenv(k, v)
			}
			app := fx.New(productionGraph(), fx.NopLogger)
			if err := app.Err(); err == nil {
				if serr := app.Start(t.Context()); serr == nil {
					_ = app.Stop(context.Background())
					t.Fatal("the controller started")
				}
			}
		})
	}
}
