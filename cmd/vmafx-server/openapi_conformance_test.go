// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/openapi_conformance_test.go — OpenAPI conformance tests.
//
// Verifies that every endpoint declared in api/openapi/vmafx-server-v1.yaml
// behaves according to the spec, including:
//   - Correct HTTP method routing (GET vs POST).
//   - JSON response schema matching generated oapi types.
//   - Status codes for happy path and error cases.
//   - Swagger UI availability at /swagger and /swagger/spec.json.
//
// Tests use net/http/httptest (no real ports) with the same stub scorer as
// the existing main_test.go helpers.
//
// ADR-0797: vmafx-server OpenAPI REST contract.

//go:build cgo

package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/gen/go/oapi"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// ---------------------------------------------------------------------------
// /v1/health conformance
// ---------------------------------------------------------------------------

// TestOpenAPIHealthOK verifies GET /v1/health returns 200 with a valid
// HealthResponse body (status == "ok").
func TestOpenAPIHealthOK(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/v1/health")
	if err != nil {
		t.Fatalf("GET /v1/health: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}
	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		t.Errorf("Content-Type: want application/json, got %q", ct)
	}

	var body oapi.HealthResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode HealthResponse: %v", err)
	}
	if body.Status != oapi.Ok {
		t.Errorf("status: got %q, want %q", body.Status, oapi.Ok)
	}
}

// TestOpenAPIHealthMethodNotAllowed verifies that POST /v1/health is rejected.
func TestOpenAPIHealthMethodNotAllowed(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Post(ts.URL+"/v1/health", "application/json", nil)
	if err != nil {
		t.Fatalf("POST /v1/health: %v", err)
	}
	defer resp.Body.Close()

	// net/http ServeMux returns 405 for method mismatches when the route was
	// registered with a method pattern (Go 1.22+).
	if resp.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("expected 405, got %d", resp.StatusCode)
	}
}

// ---------------------------------------------------------------------------
// /v1/ready conformance
// ---------------------------------------------------------------------------

// TestOpenAPIReadyOK verifies GET /v1/ready returns 200 with status "ready"
// when the scorer is initialised.
func TestOpenAPIReadyOK(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/v1/ready")
	if err != nil {
		t.Fatalf("GET /v1/ready: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}

	var body oapi.ReadyResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode ReadyResponse: %v", err)
	}
	if body.Status != oapi.Ready {
		t.Errorf("status: got %q, want %q", body.Status, oapi.Ready)
	}
}

// TestOpenAPIReadyNotReady verifies GET /v1/ready returns 503 with status
// "not ready" when the grpcServer has a nil scorer.
func TestOpenAPIReadyNotReady(t *testing.T) {
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	// nil scorer → not ready
	grpc := newGRPCServer(nil, metrics, log)
	hs := newHTTPServer(nil, metrics, reg, log, grpc)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/v1/ready")
	if err != nil {
		t.Fatalf("GET /v1/ready: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusServiceUnavailable {
		t.Errorf("expected 503, got %d", resp.StatusCode)
	}

	var body oapi.ReadyResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode ReadyResponse: %v", err)
	}
	if body.Status != oapi.NotReady {
		t.Errorf("status: got %q, want %q", body.Status, oapi.NotReady)
	}
	if body.Reason == nil || *body.Reason == "" {
		t.Error("expected non-empty reason field in not-ready response")
	}
}

// ---------------------------------------------------------------------------
// /swagger and /swagger/spec.json conformance
// ---------------------------------------------------------------------------

// TestSwaggerUIAvailable verifies GET /swagger returns a 200 HTML page
// containing the swagger-ui-bundle.js CDN reference.
func TestSwaggerUIAvailable(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/swagger")
	if err != nil {
		t.Fatalf("GET /swagger: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}
	ct := resp.Header.Get("Content-Type")
	if !strings.HasPrefix(ct, "text/html") {
		t.Errorf("Content-Type: want text/html, got %q", ct)
	}
	body, _ := io.ReadAll(resp.Body)
	if !bytes.Contains(body, []byte("swagger-ui-bundle.js")) {
		t.Error("swagger UI page does not reference swagger-ui-bundle.js")
	}
}

// TestSwaggerSpecJSON verifies GET /swagger/spec.json returns valid JSON with
// the "openapi" key and the expected API title.
func TestSwaggerSpecJSON(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	resp, err := ts.Client().Get(ts.URL + "/swagger/spec.json")
	if err != nil {
		t.Fatalf("GET /swagger/spec.json: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("expected 200, got %d", resp.StatusCode)
	}
	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		t.Errorf("Content-Type: want application/json, got %q", ct)
	}

	var spec map[string]any
	if err := json.NewDecoder(resp.Body).Decode(&spec); err != nil {
		t.Fatalf("decode spec.json: %v", err)
	}
	if _, ok := spec["openapi"]; !ok {
		t.Error("spec.json missing 'openapi' key")
	}
	info, ok := spec["info"].(map[string]any)
	if !ok {
		t.Fatal("spec.json 'info' missing or wrong type")
	}
	if title, _ := info["title"].(string); !strings.Contains(title, "VMAFX") {
		t.Errorf("spec title: want string containing 'VMAFX', got %q", title)
	}
}

// TestSwaggerRedirect verifies that GET /swagger/ (trailing slash) redirects
// to /swagger.
func TestSwaggerRedirect(t *testing.T) {
	hs, _ := newTestHTTPServer(t)
	mux := http.NewServeMux()
	hs.routes(mux)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// Use a client that does not follow redirects so we can inspect the 301.
	client := &http.Client{
		CheckRedirect: func(_ *http.Request, _ []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
	resp, err := client.Get(ts.URL + "/swagger/")
	if err != nil {
		t.Fatalf("GET /swagger/: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusMovedPermanently {
		t.Errorf("expected 301, got %d", resp.StatusCode)
	}
	loc := resp.Header.Get("Location")
	if loc != "/swagger" {
		t.Errorf("Location: want /swagger, got %q", loc)
	}
}

// ---------------------------------------------------------------------------
// Embedded spec roundtrip
// ---------------------------------------------------------------------------

// TestEmbeddedSpecRoundtrip verifies that oapi.GetSpecJSON decodes without
// error and the result is valid JSON.
func TestEmbeddedSpecRoundtrip(t *testing.T) {
	raw, err := oapi.GetSpecJSON()
	if err != nil {
		t.Fatalf("oapi.GetSpecJSON: %v", err)
	}
	if len(raw) == 0 {
		t.Fatal("oapi.GetSpecJSON returned empty bytes")
	}
	var m map[string]any
	if err := json.Unmarshal(raw, &m); err != nil {
		t.Fatalf("spec JSON is not valid JSON: %v", err)
	}
	if _, ok := m["openapi"]; !ok {
		t.Error("embedded spec missing 'openapi' key")
	}
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
