// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/main_extra_test.go — error-path + lifecycle tests
// that extend main_test.go's happy-path coverage.
//
// Coverage additions (vs. main_test.go):
//   - envOr() default + override
//   - version() returns the buildVersion ldflag (or "dev")
//   - 405 method-not-allowed on /healthz, /readyz, /v1/score
//   - 400 invalid-JSON body on /v1/score
//   - 500 scorer-error mapping on /v1/score (stub vmaf that exits non-zero)
//   - runHTTP graceful shutdown on context cancel (bounded by
//     observability.GracefulShutdownTimeout)
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.

//go:build cgo

package main

import (
	"context"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"

	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// TestEnvOr verifies the default + override semantics of envOr.
func TestEnvOr(t *testing.T) {
	// Do not run parallel — uses os.Setenv.
	const key = "VMAFX_SERVER_TEST_ENVOR_KEY"

	t.Run("unset returns default", func(t *testing.T) {
		t.Setenv(key, "")
		if got := envOr(key, "default"); got != "default" {
			t.Errorf("envOr unset: got %q, want %q", got, "default")
		}
	})

	t.Run("set returns value", func(t *testing.T) {
		t.Setenv(key, "from-env")
		if got := envOr(key, "default"); got != "from-env" {
			t.Errorf("envOr set: got %q, want %q", got, "from-env")
		}
	})
}

// TestVersion verifies version() returns a non-empty string.
func TestVersion(t *testing.T) {
	t.Parallel()
	if v := version(); v == "" {
		t.Error("version() returned empty string")
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

// TestRunHTTP_BadAddress verifies that runHTTP returns a non-nil error when
// the TCP listen address is invalid.  Mirrors TestRunGRPCWithServer_BadAddress
// which tests the same invariant for the gRPC listener.
func TestRunHTTP_BadAddress(t *testing.T) {
	t.Parallel()
	hs, _ := newTestHTTPServer(t)
	log := observability.NewLogger("ERROR")

	ctx := context.Background()
	err := runHTTP(ctx, "invalid-addr-not-bindable", hs, log)
	if err == nil {
		t.Fatal("expected error for invalid listen address, got nil")
	}
}

// TestRunHTTPGracefulShutdown verifies runHTTP honours ctx cancellation and
// returns nil within GracefulShutdownTimeout. Mirrors the PR #300 invariant.
func TestRunHTTPGracefulShutdown(t *testing.T) {
	t.Parallel()
	hs, _ := newTestHTTPServer(t)
	log := observability.NewLogger("ERROR")

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	addr := ln.Addr().String()
	_ = ln.Close()

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- runHTTP(ctx, addr, hs, log) }()

	// Wait for the listener to come up.
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		conn, derr := net.Dial("tcp", addr)
		if derr == nil {
			_ = conn.Close()
			break
		}
		time.Sleep(10 * time.Millisecond)
	}

	cancel()

	select {
	case err := <-done:
		if err != nil {
			t.Errorf("runHTTP returned error: %v", err)
		}
	case <-time.After(observability.GracefulShutdownTimeout + 2*time.Second):
		t.Error("runHTTP did not return within graceful shutdown deadline")
	}
}
