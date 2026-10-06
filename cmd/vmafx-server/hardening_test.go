// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/hardening_test.go — the effective HTTP and gRPC server
// limits of the production graph (issue #1251).

//go:build cgo

package main

import (
	"net/http"
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
