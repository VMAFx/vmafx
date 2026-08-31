// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/main_extra_test.go — error-path + lifecycle tests
// that extend main_test.go's happy-path coverage.
//
// Coverage additions (vs. main_test.go):
//   - version() returns the shared pkg/version ldflag (or "dev")
//   - 405 method-not-allowed on /healthz, /readyz, /v1/score
//   - 400 invalid-JSON body on /v1/score
//   - 500 scorer-error mapping on /v1/score (stub vmaf that exits non-zero)
//
// The pre-fx runHTTP / envOr lifecycle tests were removed when the composition
// root moved onto the golusoris fx framework (ADR-1119): signal handling and
// graceful shutdown are now owned by fx, and config parsing by golusoris/config.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.
// ADR-1119: golusoris fx framework adoption.

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

// TestVersion verifies version() returns a non-empty string.
func TestVersion(t *testing.T) {
	t.Parallel()
	if v := version(); v == "" {
		t.Error("version() returned empty string")
	}
}

func TestVersionRequest(t *testing.T) {
	t.Parallel()
	if !isVersionRequest([]string{"vmafx-server", "--version"}) {
		t.Fatal("--version must select the non-blocking version path")
	}
	if isVersionRequest([]string{"vmafx-server"}) {
		t.Fatal("normal startup must not select the version path")
	}
	if isVersionRequest([]string{"vmafx-server", "--version", "extra"}) {
		t.Fatal("version path must reject extra arguments")
	}
}

// TestHealthzMethodNotAllowed verifies that POST /healthz returns 405.
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

// TestReadyzMethodNotAllowed verifies that POST /readyz returns 405.
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

// TestScoreMethodNotAllowed verifies that GET /v1/score returns 405.
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

// TestScoreInvalidJSON verifies that a malformed JSON body returns 400.
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

// TestScoreInternalServerError verifies a non-zero-exit scorer maps to 500.
func TestScoreInternalServerError(t *testing.T) {
	t.Parallel()

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
	hs := newHTTPServer(scorer, metrics, reg, log, nil)

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
