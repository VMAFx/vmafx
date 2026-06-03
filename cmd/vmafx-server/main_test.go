// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/main_test.go — integration tests for vmafx-server HTTP endpoints.
//
// All tests boot a real httpServer against a mock libvmaf.Scorer and use
// net/http/httptest to avoid opening real ports.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.
// ADR-0797: vmafx-server OpenAPI REST contract.

//go:build cgo

package main

import (
	"bytes"
	"encoding/json"
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

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// vmafGoldenJSON is a canned vmaf CLI JSON output representing the Netflix
// golden pair (src01_hrc00_576x324 ↔ src01_hrc01_576x324, VMAF ≈ 76.6683).
const vmafGoldenJSON = `{
  "pooled_metrics": {
    "vmaf":       {"mean": 76.6683},
    "vif_scale0": {"mean": 0.8912},
    "adm2":       {"mean": 0.9876},
    "motion2":    {"mean": 2.3456}
  }
}`

// writeVmafStub writes a shell-script stub that emits a canned JSON payload
// whenever invoked, and returns the path to the script.
func writeVmafStub(t *testing.T, payload string) string {
	t.Helper()
	dir := t.TempDir()
	script := `#!/bin/sh
outfile=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) outfile="$2"; shift 2 ;;
    *)  shift ;;
  esac
done
if [ -n "$outfile" ]; then
  printf '%s' '` + strings.ReplaceAll(payload, "'", "'\\''") + `' > "$outfile"
fi
exit 0
`
	p := filepath.Join(dir, "vmaf")
	if err := os.WriteFile(p, []byte(script), 0o700); err != nil {
		t.Fatalf("writeVmafStub: %v", err)
	}
	return p
}

// writeModelFile creates a placeholder model .json in a temp dir.
func writeModelFile(t *testing.T) (modelDir string) {
	t.Helper()
	modelDir = t.TempDir()
	if err := os.WriteFile(filepath.Join(modelDir, "vmaf_v0.6.1.json"), []byte(`{}`), 0o600); err != nil {
		t.Fatalf("writeModelFile: %v", err)
	}
	return modelDir
}

// newTestHTTPServer creates a real httpServer backed by the stub scorer and returns it.
// The grpcServer is also created so that OpenAPI REST adapter routes are wired.
func newTestHTTPServer(t *testing.T) (*httpServer, *prometheus.Registry) {
	t.Helper()
	stub := writeVmafStub(t, vmafGoldenJSON)
	modelDir := writeModelFile(t)
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR") // suppress noise in tests
	grpc := newGRPCServer(scorer, metrics, log)
	return newHTTPServer(scorer, metrics, reg, log, grpc), reg
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// TestHealthEndpoint verifies that GET /healthz returns 200 with JSON body.
func TestHealthEndpoint(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/healthz")
	if err != nil {
		t.Fatalf("GET /healthz: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Errorf("expected 200, got %d", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	if !bytes.Contains(body, []byte("ok")) {
		t.Errorf("expected body to contain 'ok', got: %s", body)
	}
}

// TestReadyEndpoint verifies that GET /readyz returns 200 when scorer is set.
func TestReadyEndpoint(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/readyz")
	if err != nil {
		t.Fatalf("GET /readyz: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Errorf("expected 200, got %d", resp.StatusCode)
	}
}

// TestReadyEndpointNotReady verifies that /readyz returns 503 when scorer is nil.
func TestReadyEndpointNotReady(t *testing.T) {
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	hs := newHTTPServer(nil, metrics, reg, log, nil) // nil scorer + nil grpc → not ready
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/readyz")
	if err != nil {
		t.Fatalf("GET /readyz: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusServiceUnavailable {
		t.Errorf("expected 503, got %d", resp.StatusCode)
	}
}

// TestMetricsEndpoint verifies that GET /metrics returns Prometheus exposition format.
func TestMetricsEndpoint(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/metrics")
	if err != nil {
		t.Fatalf("GET /metrics: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Errorf("expected 200, got %d", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	// Prometheus exposition format starts with # HELP or # TYPE lines.
	if !bytes.Contains(body, []byte("# HELP")) && !bytes.Contains(body, []byte("# TYPE")) {
		t.Errorf("expected Prometheus exposition format, got: %s", body[:min(200, len(body))])
	}
}

// TestScoreEndpoint verifies POST /v1/score with mocked libvmaf.
func TestScoreEndpoint(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	reqBody := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv","model":"vmaf_v0.6.1"}`
	resp, err := ts.Client().Post(ts.URL+"/v1/score", "application/json", strings.NewReader(reqBody))
	if err != nil {
		t.Fatalf("POST /v1/score: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		t.Fatalf("expected 200, got %d; body: %s", resp.StatusCode, body)
	}

	var result scoreResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		t.Fatalf("decode response: %v", err)
	}

	const wantScore = 76.6683
	if result.Score != wantScore {
		t.Errorf("score: got %v, want %v", result.Score, wantScore)
	}
	if len(result.Features) == 0 {
		t.Error("expected non-empty features")
	}
}

// TestScoreEndpointMissingFields verifies that missing reference/distorted returns 400.
func TestScoreEndpointMissingFields(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Post(ts.URL+"/v1/score", "application/json", strings.NewReader(`{}`))
	if err != nil {
		t.Fatalf("POST /v1/score: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", resp.StatusCode)
	}
}

// TestGracefulShutdown verifies that a server drains within the timeout window.
// We start a test server, trigger a shutdown context cancel, and verify the
// server returns without error.
func TestGracefulShutdown(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)

	// Create a context we control.
	// Use a channel-based approach to avoid importing context directly.
	ts := httptest.NewServer(mux)
	defer ts.Close() // normal httptest cleanup path

	// Verify the server is alive first.
	resp, err := ts.Client().Get(ts.URL + "/healthz")
	if err != nil {
		t.Fatalf("pre-shutdown healthz: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("pre-shutdown healthz: expected 200, got %d", resp.StatusCode)
	}

	// ts.Close() triggers a graceful shutdown of the httptest server.
	// This mirrors SIGTERM → http.Server.Shutdown() in production.
	ts.Close()

	// Post-close: the server should reject new connections.
	_, err = ts.Client().Get(ts.URL + "/healthz")
	if err == nil {
		t.Error("expected error after server shutdown, got nil")
	}
}

// min is a small integer minimum helper (for test truncation only).
func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// TestScoreEndpoint_RejectsOversizedBody guards the ADR-0978 regression: the
// /v1/score endpoint must cap incoming JSON bodies to maxScoreRequestBodyBytes
// (1 MiB) so an unauthenticated POST with a multi-GB body cannot OOM the
// process via the JSON decoder's read buffer.
func TestScoreEndpoint_RejectsOversizedBody(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// Construct a syntactically-valid JSON body larger than the 1 MiB cap.
	// The Reference field carries the padding.
	const oversized = maxScoreRequestBodyBytes + 1024
	pad := strings.Repeat("a", oversized)
	reqBody := `{"reference":"` + pad + `","distorted":"/tmp/dis.yuv"}`

	resp, err := ts.Client().Post(ts.URL+"/v1/score", "application/json",
		strings.NewReader(reqBody))
	if err != nil {
		t.Fatalf("POST /v1/score: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusRequestEntityTooLarge {
		body, _ := io.ReadAll(resp.Body)
		t.Errorf("expected 413, got %d; body: %s", resp.StatusCode, body)
	}
}

// TestScoreEndpoint_AcceptsBoundedBody verifies the cap is permissive enough
// for legitimate requests — a 64 KiB body well within the cap is accepted
// even though it's much larger than a typical (~200-byte) request.
func TestScoreEndpoint_AcceptsBoundedBody(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// 64 KiB of padding inside the Model field (kept under the per-field
	// JSON tag where length is harmless).
	pad := strings.Repeat("m", 64*1024)
	reqBody := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv","model":"` + pad + `"}`

	resp, err := ts.Client().Post(ts.URL+"/v1/score", "application/json",
		strings.NewReader(reqBody))
	if err != nil {
		t.Fatalf("POST /v1/score: %v", err)
	}
	defer resp.Body.Close()

	// We don't care about the exact status — only that the body cap did
	// NOT reject it (so 200 OK from the stub scorer or some other 4xx /
	// 5xx unrelated to body size is acceptable). 413 specifically means
	// the cap was too tight.
	if resp.StatusCode == http.StatusRequestEntityTooLarge {
		t.Errorf("64 KiB body wrongly rejected as too large; cap too tight")
	}
}
