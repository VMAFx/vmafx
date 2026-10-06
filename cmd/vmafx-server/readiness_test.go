// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/readiness_test.go — /readyz must go 503 when the vmaf
// binary or model directory the scorer depends on is unusable (issue #1251).

//go:build cgo

package main

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus"

	"github.com/golusoris/golusoris/core/clock"
	"github.com/golusoris/golusoris/core/config"
	"github.com/golusoris/golusoris/k8s/health"
	"github.com/golusoris/golusoris/observability/statuspage"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

func TestCheckScorerHost(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	good := filepath.Join(dir, "vmaf")
	if err := os.WriteFile(good, []byte("#!/bin/sh\n"), 0o755); err != nil { //nolint:gosec // test stub must be executable
		t.Fatal(err)
	}
	plain := filepath.Join(dir, "plain")
	if err := os.WriteFile(plain, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	cancelled, cancel := context.WithCancel(context.Background())
	cancel()

	cases := []struct {
		name     string
		ctx      context.Context
		binary   string
		modelDir string
		wantErr  bool
	}{
		{"usable", context.Background(), good, dir, false},
		{"usable without model dir", context.Background(), good, "", false},
		{"binary missing", context.Background(), filepath.Join(dir, "gone"), "", true},
		{"binary not executable", context.Background(), plain, "", true},
		{"binary is a directory", context.Background(), dir, "", true},
		{"model dir missing", context.Background(), good, filepath.Join(dir, "nomodels"), true},
		{"model dir is a file", context.Background(), good, plain, true},
		{"cancelled context", cancelled, good, "", true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			err := checkScorerHost(tc.ctx, tc.binary, tc.modelDir)
			if (err != nil) != tc.wantErr {
				t.Fatalf("checkScorerHost err = %v, wantErr %v", err, tc.wantErr)
			}
		})
	}
}

// TestReadyzFollowsBinaryAvailability drives the real /readyz route: 200 while
// the binary exists, 503 once it is removed, 200 again when it returns.
func TestReadyzFollowsBinaryAvailability(t *testing.T) {
	stub := writeVmafStub(t, vmafGoldenJSON)
	modelDir := writeModelFile(t)
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	t.Setenv("VMAFX_VMAF_BINARY", stub)
	t.Setenv("VMAFX_MODEL_DIR", modelDir)
	cfg, err := config.New(serverEnvOptions(false))
	if err != nil {
		t.Fatalf("config.New: %v", err)
	}

	registry := prometheus.NewRegistry()
	metrics := observability.NewMetrics(registry)
	log := observability.NewLogger("ERROR")
	limiter, err := NewScoreLimiter(1)
	if err != nil {
		t.Fatal(err)
	}
	statusReg := statuspage.NewRegistry(clock.NewFake())
	registerHealthChecks(statusReg, scorer)
	registerBinaryReadiness(statusReg, cfg)

	r := chi.NewRouter()
	mountHTTPRoutes(r, scorer, metrics, registry,
		newGRPCServerWithLimiter(scorer, metrics, log, limiter), limiter, statusReg, log)
	ts := httptest.NewServer(r)
	t.Cleanup(ts.Close)

	if code, body := getStatus(t, ts.URL+"/readyz"); code != http.StatusOK {
		t.Fatalf("usable binary: GET /readyz = %d (%s), want 200", code, body)
	}
	moved := stub + ".moved"
	if err := os.Rename(stub, moved); err != nil {
		t.Fatal(err)
	}
	if code, body := getStatus(t, ts.URL+"/readyz"); code != http.StatusServiceUnavailable {
		t.Fatalf("binary removed: GET /readyz = %d (%s), want 503", code, body)
	}
	if results := statusReg.RunTagged(context.Background(), health.TagReadiness); !hasDown(results) {
		t.Errorf("registry readiness has no down check: %+v", results)
	}
	if err := os.Rename(moved, stub); err != nil {
		t.Fatal(err)
	}
	if code, _ := getStatus(t, ts.URL+"/readyz"); code != http.StatusOK {
		t.Fatalf("binary restored: GET /readyz = %d, want 200", code)
	}
}

func hasDown(results []statuspage.Result) bool {
	for _, r := range results {
		if r.Status == statuspage.StatusDown {
			return true
		}
	}
	return false
}
