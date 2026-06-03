// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/rest_adapter_extra_test.go — coverage for restAdapter paths
// not exercised by the existing openapi_conformance_test.go:
//
//   - ScoreVideoPair: happy path (POST /v1/score via the adapter).
//   - ScoreVideoPair: invalid JSON body → 400.
//   - ScoreVideoPair: scorer error → 500.
//   - ScoreVideoPair: method not allowed (GET) → 405.
//   - GetReady: not-ready path when grpcServer.scorer is nil.
//
// ADR-0797: vmafx-server OpenAPI REST contract.

//go:build cgo

package main

import (
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

// newTestRestAdapter creates a restAdapter backed by the stub scorer.
func newTestRestAdapter(t *testing.T) *restAdapter {
	t.Helper()
	stub := writeVmafStub(t, vmafGoldenJSON)
	modelDir := writeModelFile(t)
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	grpcSrv := newGRPCServer(scorer, metrics, log)
	return newRestAdapter(grpcSrv, log)
}

// TestRestAdapter_ScoreVideoPair_HappyPath verifies that a valid POST to
// ScoreVideoPair returns 200 with a non-zero score.
func TestRestAdapter_ScoreVideoPair_HappyPath(t *testing.T) {
	t.Parallel()
	adapter := newTestRestAdapter(t)

	body := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv","model":"vmaf_v0.6.1"}`
	req := httptest.NewRequest(http.MethodPost, "/v1/score", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()

	adapter.ScoreVideoPair(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d; body: %s", rr.Code, rr.Body.String())
	}

	var resp struct {
		Score    float64            `json:"score"`
		Features map[string]float64 `json:"features"`
	}
	if err := json.NewDecoder(rr.Body).Decode(&resp); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	const wantScore = 76.6683
	if resp.Score != wantScore {
		t.Errorf("score: got %v, want %v", resp.Score, wantScore)
	}
}

// TestRestAdapter_ScoreVideoPair_InvalidJSON verifies that a malformed JSON
// body returns 400.
func TestRestAdapter_ScoreVideoPair_InvalidJSON(t *testing.T) {
	t.Parallel()
	adapter := newTestRestAdapter(t)

	req := httptest.NewRequest(http.MethodPost, "/v1/score", strings.NewReader(`{bad json`))
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()

	adapter.ScoreVideoPair(rr, req)

	if rr.Code != http.StatusBadRequest {
		t.Errorf("expected 400, got %d", rr.Code)
	}
}

// TestRestAdapter_ScoreVideoPair_MethodNotAllowed verifies that GET returns 405.
func TestRestAdapter_ScoreVideoPair_MethodNotAllowed(t *testing.T) {
	t.Parallel()
	adapter := newTestRestAdapter(t)

	req := httptest.NewRequest(http.MethodGet, "/v1/score", nil)
	rr := httptest.NewRecorder()

	adapter.ScoreVideoPair(rr, req)

	if rr.Code != http.StatusMethodNotAllowed {
		t.Errorf("expected 405, got %d", rr.Code)
	}
}

// TestRestAdapter_ScoreVideoPair_ScorerError verifies that a scorer failure
// (e.g. binary exits non-zero) maps to 500.
func TestRestAdapter_ScoreVideoPair_ScorerError(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	stub := filepath.Join(dir, "vmaf")
	const script = `#!/bin/sh
echo "deliberate error" >&2
exit 1
`
	if err := os.WriteFile(stub, []byte(script), 0o700); err != nil {
		t.Fatalf("write stub: %v", err)
	}
	modelDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(modelDir, "vmaf_v0.6.1.json"), []byte("{}"), 0o600); err != nil {
		t.Fatalf("write model: %v", err)
	}
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	grpcSrv := newGRPCServer(scorer, metrics, log)
	adapter := newRestAdapter(grpcSrv, log)

	body := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv"}`
	req := httptest.NewRequest(http.MethodPost, "/v1/score", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()

	adapter.ScoreVideoPair(rr, req)

	if rr.Code != http.StatusInternalServerError {
		b, _ := io.ReadAll(rr.Body)
		t.Errorf("expected 500, got %d; body: %s", rr.Code, b)
	}
}

// TestRestAdapter_ScoreVideoPair_WithOptionalModel verifies that an explicit
// model name is forwarded correctly.
func TestRestAdapter_ScoreVideoPair_WithOptionalModel(t *testing.T) {
	t.Parallel()
	adapter := newTestRestAdapter(t)

	body := `{"reference":"/tmp/ref.yuv","distorted":"/tmp/dis.yuv","model":"vmaf_v0.6.1"}`
	req := httptest.NewRequest(http.MethodPost, "/v1/score", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()

	adapter.ScoreVideoPair(rr, req)

	if rr.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rr.Code)
	}
}

// TestRestAdapter_GetHealth_OK verifies GetHealth returns 200 with status "ok".
func TestRestAdapter_GetHealth_OK(t *testing.T) {
	t.Parallel()
	adapter := newTestRestAdapter(t)

	req := httptest.NewRequest(http.MethodGet, "/v1/health", nil)
	rr := httptest.NewRecorder()
	adapter.GetHealth(rr, req)

	if rr.Code != http.StatusOK {
		t.Errorf("expected 200, got %d", rr.Code)
	}
}

// TestRestAdapter_GetReady_NotReady verifies GetReady returns 503 when scorer is nil.
func TestRestAdapter_GetReady_NotReady(t *testing.T) {
	t.Parallel()
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	// nil scorer → not ready
	grpcSrv := newGRPCServer(nil, metrics, log)
	adapter := newRestAdapter(grpcSrv, log)

	req := httptest.NewRequest(http.MethodGet, "/v1/ready", nil)
	rr := httptest.NewRecorder()
	adapter.GetReady(rr, req)

	if rr.Code != http.StatusServiceUnavailable {
		t.Errorf("expected 503, got %d", rr.Code)
	}
}
