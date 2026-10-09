// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/hardening_test.go — the effective HTTP and gRPC server
// limits of the production graph (issue #1251).

//go:build cgo

package main

import (
	"bytes"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"go.uber.org/fx"
	"go.uber.org/fx/fxtest"

	grpcmod "github.com/golusoris/golusoris/grpc"
	httpserver "github.com/golusoris/golusoris/httpx/server"
)

// populateLimits builds the production graph (no listeners started) and
// returns the effective gRPC config and HTTP server options.
func populateLimits(t *testing.T) (grpcmod.Config, httpserver.Options, *http.Server) {
	t.Helper()
	var (
		gcfg grpcmod.Config
		hopt httpserver.Options
		srv  *http.Server
	)
	app := fxtest.New(t, productionGraph(), fx.Populate(&gcfg, &hopt, &srv))
	app.RequireStart()
	t.Cleanup(app.RequireStop)
	return gcfg, hopt, srv
}

// TestServerDefaultsFitScoring pins the limits an unconfigured server runs
// with: every HTTP timeout and size guard is set, the gRPC receive cap holds a
// 4K frame pair, and the write deadline outlasts a minute of scoring.
func TestServerDefaultsFitScoring(t *testing.T) {
	writeVmafStubForApp(t)
	gcfg, hopt, srv := populateLimits(t)

	if gcfg.MaxRecvSize != defaultGRPCMaxRecvSize {
		t.Errorf("grpc MaxRecvSize = %d, want %d", gcfg.MaxRecvSize, defaultGRPCMaxRecvSize)
	}
	const fourKPair16Bit = 2 * 3840 * 2160 * 3 // reference + distorted, 16-bit 4:2:0
	if gcfg.MaxRecvSize < fourKPair16Bit {
		t.Errorf("grpc MaxRecvSize %d cannot carry a 4K 16-bit frame pair (%d bytes)", gcfg.MaxRecvSize, fourKPair16Bit)
	}
	if gcfg.MaxSendSize <= 0 {
		t.Errorf("grpc MaxSendSize = %d, want > 0", gcfg.MaxSendSize)
	}
	if hopt.Timeouts.Write != defaultHTTPWriteTimeout || srv.WriteTimeout != defaultHTTPWriteTimeout {
		t.Errorf("http write timeout = %v / %v, want %v", hopt.Timeouts.Write, srv.WriteTimeout, defaultHTTPWriteTimeout)
	}
	for name, d := range map[string]time.Duration{
		"read": srv.ReadTimeout, "header": srv.ReadHeaderTimeout, "idle": srv.IdleTimeout,
	} {
		if d <= 0 {
			t.Errorf("http %s timeout = %v, want > 0", name, d)
		}
	}
	if srv.MaxHeaderBytes <= 0 || hopt.Limits.Body <= 0 {
		t.Errorf("http size guards unset: header=%d body=%d", srv.MaxHeaderBytes, hopt.Limits.Body)
	}
}

// TestServerDefaultsYieldToOperator proves the operator value wins over the
// vmafx default for both keys.
func TestServerDefaultsYieldToOperator(t *testing.T) {
	writeVmafStubForApp(t)
	t.Setenv("VMAFX_GRPC_MAX_RECV_SIZE", "1048576")
	t.Setenv("VMAFX_HTTP_TIMEOUTS_WRITE", "90s")
	gcfg, hopt, _ := populateLimits(t)

	if gcfg.MaxRecvSize != 1048576 {
		t.Errorf("grpc MaxRecvSize = %d, want operator value 1048576", gcfg.MaxRecvSize)
	}
	if hopt.Timeouts.Write != 90*time.Second {
		t.Errorf("http write timeout = %v, want operator value 90s", hopt.Timeouts.Write)
	}
}

// bodyStatus serves one POST of n bytes through an http.Server built from opt
// (golusoris applies its body limit there) to a handler that reads the whole
// body, and returns the status: 200 when the body was read, 413 when the limit
// cut it.
func bodyStatus(t *testing.T, opt httpserver.Options, n int) int {
	t.Helper()
	readAll := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if _, err := io.Copy(io.Discard, r.Body); err != nil {
			w.WriteHeader(http.StatusRequestEntityTooLarge)
			return
		}
		w.WriteHeader(http.StatusOK)
	})
	srv := httpserver.New(readAll, opt)
	rec := httptest.NewRecorder()
	req := httptest.NewRequestWithContext(t.Context(), http.MethodPost, "/", bytes.NewReader(make([]byte, n)))
	srv.Handler.ServeHTTP(rec, req)
	return rec.Code
}

// frameworkBodyLimit is golusoris httpx/server's default request-body cap.
const frameworkBodyLimit = 10 << 20

// TestBodyLimitZeroKeepsTheDefault pins the docs of VMAFX_HTTP_LIMITS_BODY:
// since golusoris v0.13.0 `0` keeps the 10 MiB cap instead of removing it.
func TestBodyLimitZeroKeepsTheDefault(t *testing.T) {
	writeVmafStubForApp(t)
	t.Setenv("VMAFX_HTTP_LIMITS_BODY", "0")
	_, hopt, _ := populateLimits(t)

	if got := bodyStatus(t, hopt, frameworkBodyLimit); got != http.StatusOK {
		t.Errorf("body at the cap: status %d, want 200", got)
	}
	if got := bodyStatus(t, hopt, frameworkBodyLimit+1); got != http.StatusRequestEntityTooLarge {
		t.Errorf("body one byte over the cap: status %d, want 413", got)
	}
}

// TestBodyLimitUnlimitedRemovesTheCap pins VMAFX_HTTP_LIMITS_UNLIMITED: the
// variable reaches golusoris's http.limits.unlimited and no body is cut.
func TestBodyLimitUnlimitedRemovesTheCap(t *testing.T) {
	writeVmafStubForApp(t)
	t.Setenv("VMAFX_HTTP_LIMITS_UNLIMITED", "true")
	_, hopt, _ := populateLimits(t)

	if !hopt.Limits.AllowUnlimitedBody {
		t.Fatal("VMAFX_HTTP_LIMITS_UNLIMITED=true did not reach http.limits.unlimited")
	}
	if got := bodyStatus(t, hopt, frameworkBodyLimit+1); got != http.StatusOK {
		t.Errorf("body over the default cap: status %d, want 200", got)
	}
}
