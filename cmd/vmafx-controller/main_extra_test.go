// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/main_extra_test.go — error-path + lifecycle tests
// that extend main_test.go's happy-path coverage.
//
// What this file adds (vs. main_test.go):
//   - version() returns the buildVersion ldflag (or "dev")
//   - 405 method-not-allowed on /healthz, /readyz, /v1/score
//   - 400 invalid-JSON body on /v1/score
//   - 500 scorer-error mapping on /v1/score (via stub that exits non-zero)
//
// ADR-0703: vmafx-server Go gRPC + HTTP service (origin).
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-1119: envOr / runHTTP removed — config + HTTP serving are owned by
//           golusoris now; their dedicated tests moved to app_test.go.

//go:build cgo

package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// TestVersion verifies version() returns the buildVersion ldflag (or "dev").
func TestVersion(t *testing.T) {
	t.Parallel()
	if v := version(); v == "" {
		t.Error("version() returned empty string")
	}
}

// TestHealthzMethodNotAllowed verifies that a non-GET request to /healthz
// returns 405.
func TestHealthzMethodNotAllowed(t *testing.T) {
	t.Parallel()
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Post(ts.URL+"/healthz", "text/plain", strings.NewReader("x"))
	if err != nil {
		t.Fatalf("POST /healthz: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("expected 405, got %d", resp.StatusCode)
	}
}

// TestReadyzMethodNotAllowed verifies that a non-GET request to /readyz
// returns 405.
func TestReadyzMethodNotAllowed(t *testing.T) {
	t.Parallel()
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Post(ts.URL+"/readyz", "text/plain", strings.NewReader("x"))
	if err != nil {
		t.Fatalf("POST /readyz: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("expected 405, got %d", resp.StatusCode)
	}
}

// TestScoreMethodNotAllowed verifies that a GET request to /v1/score returns 405.
func TestScoreMethodNotAllowed(t *testing.T) {
	t.Parallel()
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/v1/score")
	if err != nil {
		t.Fatalf("GET /v1/score: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("expected 405, got %d", resp.StatusCode)
	}
}

// TestScoreInvalidJSON verifies that a malformed JSON body returns 400 with
// a JSON error payload.
func TestScoreInvalidJSON(t *testing.T) {
	t.Parallel()
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Post(ts.URL+"/v1/score", "application/json", strings.NewReader(`{"reference":`))
	if err != nil {
		t.Fatalf("POST /v1/score: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	if !strings.Contains(string(body), "invalid JSON") {
		t.Errorf("expected body to mention invalid JSON, got: %s", body)
	}
}

// TestScoreInternalServerError verifies that a scorer that exits non-zero
// surfaces as a 500 to the caller.
func TestScoreInternalServerError(t *testing.T) {
	t.Parallel()

	// Stub vmaf binary that always exits 1 with a diagnostic on stderr.
	dir := t.TempDir()
	stub := filepath.Join(dir, "vmaf")
	const script = `#!/bin/sh
echo "stub: deliberate failure" >&2
exit 1
`
	if err := os.WriteFile(stub, []byte(script), 0o700); err != nil {
		t.Fatalf("write stub: %v", err)
	}

	modelDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(modelDir, "m.json"), []byte("{}"), 0o600); err != nil {
		t.Fatalf("write model: %v", err)
	}
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	hs := newHTTPServer(scorer, metrics, reg, nil, log)

	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	reqBody := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv"}`
	resp, err := ts.Client().Post(ts.URL+"/v1/score", "application/json", strings.NewReader(reqBody))
	if err != nil {
		t.Fatalf("POST /v1/score: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusInternalServerError {
		body, _ := io.ReadAll(resp.Body)
		t.Errorf("expected 500, got %d; body: %s", resp.StatusCode, body)
	}
}
