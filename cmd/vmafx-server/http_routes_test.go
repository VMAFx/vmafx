// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/http_routes_test.go — request/response coverage for the
// PRODUCTION chi route surface (DTL-1).
//
// The other HTTP tests in this package exercise httpServer.routes(*http.ServeMux)
// — the legacy net/http registration. After the golusoris migration (ADR-1119)
// production serves its HTTP surface through mountHTTPRoutes(chi.Router, ...),
// which is a DIFFERENT registration path (chi router, golusoris health handlers,
// the "/swagger/*" subtree wildcard). This file pins that production path: it
// builds a real chi.NewRouter(), supplies the same domain deps mountHTTPRoutes
// consumes, mounts the routes, and asserts the key endpoints over a real
// httptest server.

//go:build cgo

package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/go-chi/chi/v5"
	"github.com/prometheus/client_golang/prometheus"

	"github.com/golusoris/golusoris/clock"
	"github.com/golusoris/golusoris/observability/statuspage"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// newProductionRoutesServer wires the exact dependency set mountHTTPRoutes needs
// (the same shapes main() supplies in production) onto a fresh chi router and
// returns an httptest server over it. A real stub scorer is used so the
// readiness route reports ready.
func newProductionRoutesServer(t *testing.T) *httptest.Server {
	t.Helper()

	stub := writeVmafStub(t, vmafGoldenJSON)
	modelDir := writeModelFile(t)
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}

	registry := prometheus.NewRegistry()
	metrics := observability.NewMetrics(registry)
	log := observability.NewLogger("ERROR")

	limiter, err := NewScoreLimiter(2)
	if err != nil {
		t.Fatalf("NewScoreLimiter: %v", err)
	}
	impl := newGRPCServerWithLimiter(scorer, metrics, log, limiter)
	statusReg := statuspage.NewRegistry(clock.NewFake())

	r := chi.NewRouter()
	mountHTTPRoutes(r, scorer, metrics, registry, impl, limiter, statusReg, log)

	ts := httptest.NewServer(r)
	t.Cleanup(ts.Close)
	return ts
}

// getStatus issues a GET and returns the status code plus the body.
func getStatus(t *testing.T, url string) (int, string) {
	t.Helper()
	resp, err := http.Get(url) //nolint:noctx // test helper; short-lived in-process call
	if err != nil {
		t.Fatalf("GET %s: %v", url, err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(body)
}

// TestProductionRoutes_Metrics asserts GET /metrics returns 200 and Prometheus
// exposition text containing a known vmafx metric name.
func TestProductionRoutes_Metrics(t *testing.T) {
	ts := newProductionRoutesServer(t)
	code, body := getStatus(t, ts.URL+"/metrics")
	if code != http.StatusOK {
		t.Fatalf("GET /metrics: expected 200, got %d", code)
	}
	if !strings.Contains(body, "vmafx_server_score_requests_total") {
		t.Errorf("GET /metrics: expected Prometheus text with vmafx_server_score_requests_total, got:\n%s", body)
	}
}

// TestProductionRoutes_Health asserts the health/probe endpoints return 200.
// /healthz and /readyz are the legacy JSON aliases; /livez and /startupz are the
// golusoris canonical probes backed by the status registry.
func TestProductionRoutes_Health(t *testing.T) {
	ts := newProductionRoutesServer(t)
	for _, path := range []string{"/healthz", "/readyz", "/livez", "/startupz"} {
		code, body := getStatus(t, ts.URL+path)
		if code != http.StatusOK {
			t.Errorf("GET %s: expected 200, got %d (body: %s)", path, code, body)
		}
	}
}

// TestProductionRoutes_Swagger asserts GET /swagger returns 200 and that the
// "/swagger/*" subtree wildcard serves a deeper path too (a chi exact-match
// "/swagger/" would 404 the subpath — see registerSwaggerUIChi).
func TestProductionRoutes_Swagger(t *testing.T) {
	ts := newProductionRoutesServer(t)

	code, _ := getStatus(t, ts.URL+"/swagger")
	if code != http.StatusOK {
		t.Errorf("GET /swagger: expected 200, got %d", code)
	}

	// Subtree path must also reach the index handler via the "/swagger/*"
	// wildcard rather than 404ing.
	subCode, _ := getStatus(t, ts.URL+"/swagger/index.html")
	if subCode != http.StatusOK {
		t.Errorf("GET /swagger/index.html: expected 200 via subtree, got %d", subCode)
	}
}
