// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/app_test.go — fx composition + lifecycle tests for the
// golusoris-framework server (ADR-1119).
//
// These tests prove:
//
//  1. The production fx graph is satisfiable — every provider and invoke
//     resolves (TestAppGraphValidates / TestAppStartsAndStops).
//  2. The golusoris HTTP listener actually binds. fx providers are lazy, so the
//     graph carries a standalone fx.Invoke(func(_ *http.Server){}) to force
//     construction of golusoris' graceful *http.Server (F1 / DTL-2); without it
//     nothing consumes *http.Server and the HTTP listener never starts.
//     TestAppStartsAndStops populates the *http.Server and asserts its bound
//     Addr is non-empty so a regression can never silently drop the HTTP surface.
//  3. R1 (cgo lifetime): the gRPC server drains BEFORE the libvmaf scorer is
//     closed. The scorer-close OnStop hook is appended inside provideScorer; a
//     standalone fx.Invoke(func(_ *libvmaf.Scorer){}) placed BEFORE the gRPC
//     registration invoke forces the scorer to be constructed (and its hook
//     appended) ahead of golusoris grpc.Module's GracefulStop hook. fx runs
//     OnStop hooks in reverse of append order, so GracefulStop fires before the
//     scorer Close (TestStopOrderScorerAfterGRPC, which observes the REAL hook
//     firing order via an fxevent.Logger).
//
// ADR-1119: golusoris fx framework adoption.

//go:build cgo

package main

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"go.uber.org/fx"
	"go.uber.org/fx/fxevent"
	"go.uber.org/fx/fxtest"
	googlegrpc "google.golang.org/grpc"

	"github.com/golusoris/golusoris"
	"github.com/golusoris/golusoris/clock"
	grpcmod "github.com/golusoris/golusoris/grpc"
	"github.com/golusoris/golusoris/k8s/health"
	"github.com/golusoris/golusoris/observability/statuspage"
	"github.com/golusoris/golusoris/otel"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/internal/app/bootstrap"
	"github.com/VMAFx/vmafx/internal/oteltest"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// writeVmafStubForApp writes a no-op vmaf stub and a model dir, then points the
// VMAFX_ config env vars at them plus ephemeral listen addresses. It returns
// after t.Setenv so the config module (loaded inside fx) sees them.
func writeVmafStubForApp(t *testing.T) {
	t.Helper()
	dir := t.TempDir()
	stub := filepath.Join(dir, "vmaf")
	if err := os.WriteFile(stub, []byte("#!/bin/sh\nexit 0\n"), 0o700); err != nil {
		t.Fatalf("write vmaf stub: %v", err)
	}
	modelDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(modelDir, "vmaf_v0.6.1.json"), []byte("{}"), 0o600); err != nil {
		t.Fatalf("write model: %v", err)
	}
	t.Setenv("VMAFX_VMAF_BINARY", stub)
	t.Setenv("VMAFX_MODEL_DIR", modelDir)
	// Ephemeral ports so concurrent test runs do not collide and nothing leaks
	// a fixed listener.
	t.Setenv("VMAFX_HTTP_ADDR", "127.0.0.1:0")
	t.Setenv("VMAFX_GRPC_LISTEN", "127.0.0.1:0")
	t.Setenv("VMAFX_LOG_LEVEL", "ERROR")
}

// productionGraph returns the exact provider/invoke set used by main(), minus
// the .Run() blocking call. Kept in lockstep with main() so the tests exercise
// the real composition — including the standalone *http.Server invoke (F1) in
// the same relative position (after the gRPC registration invoke and after
// mountHTTPRoutes).
func productionGraph() fx.Option {
	return fx.Options(
		bootstrap.Base,
		fx.Replace(serverEnvOptions(false)),
		golusoris.HTTP,
		bootstrap.HTTPTracing,
		grpcmod.Module,
		fx.Provide(
			provideScorer,
			provideMetrics,
			provideScoreLimiter,
			provideStatusRegistry,
			newGRPCServerImpl,
		),
		// R1 ordering invoke: realise the Scorer before the gRPC server.
		fx.Invoke(func(_ *libvmaf.Scorer) {}),
		fx.Invoke(func(impl *grpcServer, s *googlegrpc.Server) {
			vmafxv1.RegisterVmafxScoringServer(s, impl)
		}),
		fx.Invoke(mountHTTPRoutes),
		fx.Invoke(registerHealthChecks),
		// F1 / DTL-2: force construction of the golusoris graceful *http.Server so
		// its OnStart listener binds. Placed last so its OnStop fires first.
		fx.Invoke(func(_ *http.Server) {}),
	)
}

// TestAppGraphValidates asserts the production dependency graph is satisfiable
// without starting any listeners. A missing provider, an ambiguous type, or a
// cyclic dependency fails here.
func TestAppGraphValidates(t *testing.T) {
	writeVmafStubForApp(t)
	if err := fx.ValidateApp(productionGraph()); err != nil {
		t.Fatalf("production fx graph does not validate: %v", err)
	}
}

// TestAppStartsAndStops boots the full graph (real HTTP + gRPC listeners on
// ephemeral ports) and shuts it down, proving start + graceful stop succeed.
//
// It additionally populates the golusoris *http.Server and asserts its bound
// Addr is non-empty after RequireStart — the empirical proof that the HTTP
// listener actually came up (F1 / DTL-2). Without the standalone *http.Server
// invoke in productionGraph, fx's laziness leaves the server unconstructed and
// this assertion fails.
func TestAppStartsAndStops(t *testing.T) {
	writeVmafStubForApp(t)
	var srv *http.Server
	app := fxtest.New(t, productionGraph(), fx.Populate(&srv))
	app.RequireStart()
	if srv == nil {
		t.Fatal("F1: *http.Server was not constructed; golusoris HTTP listener never bound")
	}
	if srv.Addr == "" {
		t.Errorf("F1: *http.Server.Addr is empty; expected a bound HTTP listen address")
	}
	app.RequireStop()
}

// orderRecorder is a minimal fxevent.Logger that records the CallerName of each
// OnStop hook as it begins executing. CallerName is the name of the function
// that *scheduled* the hook, so the scorer's real Close hook (scheduled inside
// provideScorer) and golusoris grpc.Module's GracefulStop hook (scheduled inside
// the framework's newServer) are distinguishable without injecting proxy hooks.
type orderRecorder struct {
	mu    sync.Mutex
	order []string
}

func (r *orderRecorder) LogEvent(e fxevent.Event) {
	if ev, ok := e.(*fxevent.OnStopExecuting); ok {
		r.mu.Lock()
		r.order = append(r.order, ev.CallerName)
		r.mu.Unlock()
	}
}

// TestStopOrderScorerAfterGRPC proves R1: at fx stop, the gRPC server's OnStop
// (GracefulStop, draining in-flight Score calls) runs BEFORE the scorer's Close
// OnStop (releasing cgo C resources).
//
// Unlike a proxy-hook approach, this observes the REAL hook firing order: an
// fxevent.Logger records the CallerName of every OnStop hook as it fires. The
// scorer's actual Close hook is scheduled inside provideScorer (CallerName
// contains "provideScorer"); golusoris grpc.Module's actual GracefulStop hook is
// scheduled inside the framework's gRPC server constructor (CallerName contains
// "golusoris/grpc"). A regression that constructs the gRPC server before the
// scorer flips the firing sequence and fails the inversion check below.
func TestStopOrderScorerAfterGRPC(t *testing.T) {
	writeVmafStubForApp(t)

	rec := &orderRecorder{}

	app := fxtest.New(t,
		productionGraph(),
		// Replace the fx event logger with our recorder so we observe the real
		// OnStop hook order. fxtest installs a TestLogger by default; the last
		// WithLogger wins.
		fx.WithLogger(func() fxevent.Logger { return rec }),
	)
	app.RequireStart()
	app.RequireStop()

	rec.mu.Lock()
	defer rec.mu.Unlock()

	grpcIdx, scorerIdx := -1, -1
	for i, caller := range rec.order {
		switch {
		case strings.Contains(caller, "provideScorer"):
			if scorerIdx == -1 {
				scorerIdx = i
			}
		case strings.Contains(caller, "golusoris/grpc"):
			if grpcIdx == -1 {
				grpcIdx = i
			}
		}
	}
	if grpcIdx == -1 {
		t.Fatalf("did not observe a golusoris/grpc OnStop hook; callers=%v", rec.order)
	}
	if scorerIdx == -1 {
		t.Fatalf("did not observe a provideScorer OnStop hook; callers=%v", rec.order)
	}
	if grpcIdx > scorerIdx {
		t.Errorf("R1 violated: gRPC GracefulStop (idx %d) ran AFTER scorer Close (idx %d); callers=%v",
			grpcIdx, scorerIdx, rec.order)
	}
}

// TestProductionGraphBadHTTPAddrFailsStart asserts that a non-bindable HTTP
// listen address surfaces as a Start error from the production graph (DTL-3).
// Built via fx.New (not fxtest) because fxtest.RequireStart fails the test on a
// start error — here the error is the expected outcome and must be inspected.
func TestProductionGraphBadHTTPAddrFailsStart(t *testing.T) {
	writeVmafStubForApp(t)
	// Override only the HTTP address with something that cannot be bound.
	t.Setenv("VMAFX_HTTP_ADDR", "invalid-addr-not-bindable")

	app := fx.New(productionGraph())
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	if err := app.Start(ctx); err == nil {
		// Make sure we don't leak a started app if Start unexpectedly succeeds.
		_ = app.Stop(context.Background())
		t.Fatal("expected app.Start to fail with an unbindable HTTP address, got nil")
	}
}

// TestRegisterHealthChecksReady verifies registerHealthChecks wires a readiness
// check that passes when the scorer is present and fails when it is nil.
func TestRegisterHealthChecksReady(t *testing.T) {
	t.Parallel()

	clk := clock.NewFake()
	regReady := statuspage.NewRegistry(clk)
	registerHealthChecks(regReady, &libvmaf.Scorer{})
	results := regReady.RunTagged(context.Background(), health.TagReadiness)
	if len(results) != 1 || results[0].Status != statuspage.StatusUp {
		t.Errorf("expected scorer readiness check Up, got %+v", results)
	}

	regNotReady := statuspage.NewRegistry(clk)
	registerHealthChecks(regNotReady, nil)
	results = regNotReady.RunTagged(context.Background(), health.TagReadiness)
	if len(results) != 1 || results[0].Status != statuspage.StatusDown {
		t.Errorf("expected scorer readiness check Down for nil scorer, got %+v", results)
	}
}

// TestOTelWiredThroughBootstrap asserts the OTel contract vmafx-server inherits
// from bootstrap.Base (ADR-0782 / ADR-1119): golusoris's otel.Module is in the
// production graph, is a silent no-op without an OTLP endpoint, and the
// resource identity is service.name "vmafx-server" (derived from the binary)
// with service.version from pkg/version.
func TestOTelWiredThroughBootstrap(t *testing.T) {
	writeVmafStubForApp(t)
	oteltest.NoopEnv(t)
	t.Setenv("OTEL_SERVICE_NAME", "")
	t.Setenv("VMAFX_OTEL_SERVICE_NAME", "")
	t.Setenv("VMAFX_OTEL_SERVICE_VERSION", "")

	var (
		providers *otel.Providers
		opts      otel.Options
	)
	app := fxtest.New(t, productionGraph(), fx.Populate(&providers, &opts))
	app.RequireStart()
	defer app.RequireStop()

	if providers == nil || providers.Tracer != nil || providers.Meter != nil || providers.Logger != nil {
		t.Fatalf("expected no-op OTel providers without an endpoint, got %+v", providers)
	}
	if opts.Service.Name != "vmafx-server" {
		t.Errorf("service.name = %q, want vmafx-server", opts.Service.Name)
	}
	if opts.Service.Version != version() {
		t.Errorf("service.version = %q, want %q", opts.Service.Version, version())
	}
}

// TestHTTPRouteEmitsServerSpan drives a request through the *http.Server
// golusoris.HTTP built for the production graph and asserts the otelhttp span
// bootstrap.HTTPTracing adds (ADR-0782 follow-up), plus the probe filter.
func TestHTTPRouteEmitsServerSpan(t *testing.T) {
	writeVmafStubForApp(t)
	sr := oteltest.Recorder(t)

	var srv *http.Server
	app := fxtest.New(t, productionGraph(), fx.Populate(&srv))
	app.RequireStart()
	defer app.RequireStop()

	rec := httptest.NewRecorder()
	srv.Handler.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/v1/health", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("GET /v1/health: status %d", rec.Code)
	}
	if got := oteltest.Ended(sr, "GET /v1/health"); len(got) != 1 {
		t.Fatalf("want one server span for GET /v1/health, got %v", oteltest.Names(sr))
	}

	rec = httptest.NewRecorder()
	srv.Handler.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/healthz", nil))
	if got := oteltest.Ended(sr, "GET /healthz"); len(got) != 0 {
		t.Errorf("probe endpoint must not be traced, got %v", oteltest.Names(sr))
	}
}
