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
	writeControllerEnv(t)
	httpAddr := freeLocalAddr(t)
	t.Setenv("VMAFX_HTTP_ADDR", httpAddr)
	t.Setenv("VMAFX_STORE_BACKEND", "postgres")
	t.Setenv("VMAFX_DB_DSN", db.DSN)
	t.Setenv("VMAFX_STORE_SWEEP_INTERVAL", "1s")

	app := fxtest.New(t, productionGraph())
	app.RequireStart()
	if code := readyz(t, httpAddr); code != http.StatusOK {
		t.Fatalf("/readyz on a migrated database: %d, want 200", code)
	}
	if _, err := db.Pool.Exec(ctx, "UPDATE schema_migrations SET dirty = true"); err != nil {
		t.Fatalf("mark the schema dirty: %v", err)
	}
	if code := readyz(t, httpAddr); code != http.StatusServiceUnavailable {
		t.Fatalf("/readyz on a dirty schema: %d, want 503", code)
	}
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
