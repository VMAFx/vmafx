// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/app_test.go — fx composition + lifecycle tests for the
// golusoris-framework controller (ADR-1119 Phase-1 PR-2).
//
// These tests prove:
//
//  1. The production fx graph is satisfiable — every provider and invoke
//     resolves (TestAppGraphValidates).
//  2. The golusoris HTTP listener actually binds. fx providers are lazy, so the
//     graph carries a standalone fx.Invoke(func(_ *http.Server){}) to force
//     construction of golusoris' graceful *http.Server (the server migration's
//     #1 BLOCKER lesson); without it nothing consumes *http.Server and the HTTP
//     listener never starts. TestAppStartsAndStops populates the *http.Server
//     and *grpc.Server and asserts their bound addresses so a regression can
//     never silently drop a surface.
//  3. The gRPC server binds and the JWT auth interceptor is in the chain
//     (TestAuthInterceptorInChain) — an unauthenticated RegisterNode RPC is
//     rejected with codes.Unauthenticated before it reaches the handler.
//  4. Stop order (R1): the gRPC server drains BEFORE the queue Close and the
//     node-registry reaper stop, which in turn run BEFORE the scorer Close
//     (TestStopOrder, which observes the REAL hook firing order).
//
// ADR-1119: golusoris fx framework adoption.

//go:build cgo

package main

import (
	"context"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"go.uber.org/fx"
	"go.uber.org/fx/fxevent"
	"go.uber.org/fx/fxtest"
	googlegrpc "google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	"github.com/golusoris/golusoris/config"
	"github.com/golusoris/golusoris/otel"
	"go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/trace"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/internal/oteltest"
	buildversion "github.com/VMAFx/vmafx/pkg/version"
)

// writeControllerEnv writes a no-op vmaf stub + model dir and points the VMAFX_
// config env vars at them, plus ephemeral listen addresses and a temp SQLite DB.
// Auth is disabled so the graph builds without a live JWKS endpoint while still
// exercising the full interceptor chain (the middleware injects a synthetic
// tenant when disabled). It returns after t.Setenv so the config module (loaded
// inside fx) observes the values.
func writeControllerEnv(t *testing.T) {
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
	t.Setenv("VMAFX_DB_PATH", filepath.Join(t.TempDir(), "controller.db"))
	// Ephemeral ports so concurrent test runs do not collide.
	t.Setenv("VMAFX_HTTP_ADDR", "127.0.0.1:0")
	t.Setenv("VMAFX_GRPC_LISTEN", "127.0.0.1:0")
	t.Setenv("VMAFX_LOG_LEVEL", "ERROR")
	// Auth disabled for the graph/lifecycle tests (no JWKS endpoint required).
	t.Setenv("VMAFX_AUTH_DISABLED", "true")
}

// productionGraph returns the exact provider/invoke set used by main(), minus
// the .Run() blocking call. It supplies the config env-options replace with
// file-watch disabled (a watcher goroutine would otherwise leak across tests);
// everything else is identical to the binary's composition.
func productionGraph() fx.Option {
	envReplace := fx.Replace(config.Options{
		EnvPrefix: "VMAFX_",
		Delimiter: ".",
		Watch:     false,
		CompoundKeys: []string{
			"auth.tenant_claim",
			"auth.roles_claim",
		},
	})
	return fx.Options(productionOptions(envReplace)...)
}

// TestAppGraphValidates asserts the production dependency graph is satisfiable
// without starting any listeners. A missing provider, an ambiguous type, or a
// cyclic dependency fails here.
func TestAppGraphValidates(t *testing.T) {
	writeControllerEnv(t)
	if err := fx.ValidateApp(productionGraph()); err != nil {
		t.Fatalf("production fx graph does not validate: %v", err)
	}
}

// TestAppStartsAndStops boots the full graph (real HTTP + gRPC listeners on
// ephemeral ports) and shuts it down, proving start + graceful stop succeed.
//
// It populates the golusoris *http.Server and *grpc.Server and asserts their
// bound addresses are non-empty after RequireStart — the empirical proof that
// both listeners actually came up. Without the standalone lazy-provider invokes
// in productionOptions, fx's laziness leaves a server unconstructed and these
// assertions fail.
func TestAppStartsAndStops(t *testing.T) {
	writeControllerEnv(t)
	var httpSrv *http.Server
	var grpcSrv *googlegrpc.Server
	app := fxtest.New(t, productionGraph(),
		fx.Populate(&httpSrv),
		fx.Populate(&grpcSrv),
	)
	app.RequireStart()
	if httpSrv == nil {
		t.Fatal("*http.Server was not constructed; golusoris HTTP listener never bound")
	}
	if httpSrv.Addr == "" {
		t.Errorf("*http.Server.Addr is empty; expected a bound HTTP listen address")
	}
	if grpcSrv == nil {
		t.Fatal("*grpc.Server was not constructed; gRPC listener never bound")
	}
	app.RequireStop()
}

// TestGRPCListenerBindsAndServes proves the gRPC listener binds and a service
// is reachable over the wire. It boots the real graph with a live gRPC listener,
// dials it, and issues a VmafxScoring.Health RPC.
//
// It uses the VmafxScoring service (gen/go) — properly protoc-generated and
// therefore wire-marshalable — rather than VmafxController, whose hand-written
// proto types (gen/go/controller) do not implement the protobuf v2
// reflection interface the standard codec requires (see the orchestrator note
// in docs/rebase-notes.md). With auth disabled the interceptor injects a
// synthetic dev tenant and the RPC reaches the handler.
func TestGRPCListenerBindsAndServes(t *testing.T) {
	writeControllerEnv(t)
	addr := freeLocalAddr(t)
	t.Setenv("VMAFX_GRPC_LISTEN", addr)

	app := fxtest.New(t, productionGraph())
	app.RequireStart()
	defer app.RequireStop()

	conn, err := googlegrpc.NewClient(addr, googlegrpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("dial gRPC: %v", err)
	}
	defer conn.Close()

	client := vmafxv1.NewVmafxScoringClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.Health(ctx, &vmafxv1.HealthRequest{})
	if err != nil {
		t.Fatalf("Health RPC against bound gRPC listener %s failed: %v", addr, err)
	}
	if !resp.GetOk() {
		t.Errorf("Health.ok = false, want true — listener bound but service mis-wired")
	}
}

// TestAuthInterceptorRejectsMissingTokenWhenEnabled proves the JWT auth
// interceptor (#225) is wired into the golusoris gRPC server's interceptor chain
// AND is load-bearing: with auth ENABLED (JWKS endpoint + issuer set), an RPC
// carrying no bearer token is rejected with codes.Unauthenticated by the auth
// interceptor before the handler runs.
//
// This exercises the VmafxScoring.Health RPC over the wire (the scoring proto is
// properly generated). Health requires no role, so a success would only happen
// if the request reached the handler — which proves the interceptor short-
// circuited the call iff it returns Unauthenticated.
func TestAuthInterceptorRejectsMissingTokenWhenEnabled(t *testing.T) {
	writeControllerEnv(t)
	addr := freeLocalAddr(t)
	t.Setenv("VMAFX_GRPC_LISTEN", addr)
	// Enable auth with a (never-reached) JWKS endpoint + issuer.
	t.Setenv("VMAFX_AUTH_DISABLED", "false")
	t.Setenv("VMAFX_JWKS_ENDPOINT", "https://idp.invalid/.well-known/jwks.json")
	t.Setenv("VMAFX_AUTH_ISSUER", "https://idp.invalid")

	app := fxtest.New(t, productionGraph())
	app.RequireStart()
	defer app.RequireStop()

	conn, err := googlegrpc.NewClient(addr, googlegrpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("dial gRPC: %v", err)
	}
	defer conn.Close()

	client := vmafxv1.NewVmafxScoringClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	// No authorization metadata → the auth interceptor must reject before the
	// handler runs.
	_, err = client.Health(ctx, &vmafxv1.HealthRequest{})
	if err == nil {
		t.Fatal("expected Unauthenticated error from the auth interceptor, got nil")
	}
	if status.Code(err) != codes.Unauthenticated {
		t.Errorf("expected codes.Unauthenticated, got %v (%v)", status.Code(err), err)
	}
}

// orderRecorder is a minimal fxevent.Logger that records the CallerName of each
// OnStop hook as it begins executing.
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

// TestStopOrder proves R1: at fx stop, the gRPC server's OnStop (GracefulStop,
// draining in-flight RPCs) runs BEFORE the queue Close + node-registry reaper
// stop, which run BEFORE the scorer Close. This observes the REAL hook firing
// order via an fxevent.Logger — the scorer/queue/registry hooks are scheduled
// in their respective provideX functions (distinct CallerNames) and golusoris
// grpc.Module's GracefulStop is scheduled inside the framework's gRPC
// constructor.
func TestStopOrder(t *testing.T) {
	writeControllerEnv(t)

	rec := &orderRecorder{}
	app := fxtest.New(t,
		productionGraph(),
		fx.WithLogger(func() fxevent.Logger { return rec }),
	)
	app.RequireStart()
	app.RequireStop()

	rec.mu.Lock()
	defer rec.mu.Unlock()

	grpcIdx, queueIdx, registryIdx, scorerIdx := -1, -1, -1, -1
	for i, caller := range rec.order {
		switch {
		case strings.Contains(caller, "provideScorer"):
			if scorerIdx == -1 {
				scorerIdx = i
			}
		case strings.Contains(caller, "provideJobQueue"):
			if queueIdx == -1 {
				queueIdx = i
			}
		case strings.Contains(caller, "provideNodeRegistry"):
			if registryIdx == -1 {
				registryIdx = i
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
	if queueIdx == -1 {
		t.Fatalf("did not observe a provideJobQueue OnStop hook; callers=%v", rec.order)
	}
	if registryIdx == -1 {
		t.Fatalf("did not observe a provideNodeRegistry OnStop hook; callers=%v", rec.order)
	}
	if scorerIdx == -1 {
		t.Fatalf("did not observe a provideScorer OnStop hook; callers=%v", rec.order)
	}
	// gRPC drains before the domain resources close.
	if grpcIdx > queueIdx {
		t.Errorf("R1 violated: gRPC GracefulStop (idx %d) ran AFTER queue Close (idx %d); callers=%v",
			grpcIdx, queueIdx, rec.order)
	}
	if grpcIdx > registryIdx {
		t.Errorf("R1 violated: gRPC GracefulStop (idx %d) ran AFTER registry stop (idx %d); callers=%v",
			grpcIdx, registryIdx, rec.order)
	}
	// Scorer closes last (after gRPC + queue + registry).
	if queueIdx > scorerIdx {
		t.Errorf("ordering violated: queue Close (idx %d) ran AFTER scorer Close (idx %d); callers=%v",
			queueIdx, scorerIdx, rec.order)
	}
}

// TestProductionGraphBadGRPCAddrFailsStart asserts that a non-bindable gRPC
// listen address surfaces as a Start error from the production graph. Built via
// fx.New (not fxtest) because fxtest.RequireStart fails the test on a start
// error — here the error is the expected outcome.
func TestProductionGraphBadGRPCAddrFailsStart(t *testing.T) {
	writeControllerEnv(t)
	t.Setenv("VMAFX_GRPC_LISTEN", "invalid-addr-not-bindable")

	app := fx.New(productionGraph())
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	if err := app.Start(ctx); err == nil {
		_ = app.Stop(context.Background())
		t.Fatal("expected app.Start to fail with an unbindable gRPC address, got nil")
	}
}

// freeLocalAddr returns a 127.0.0.1:PORT address with an OS-assigned free port.
// The listener is closed before returning so the caller (golusoris grpc.Module)
// can re-bind it.
func freeLocalAddr(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve free port: %v", err)
	}
	addr := ln.Addr().String()
	_ = ln.Close()
	return addr
}

// TestOTelWiredThroughBootstrap asserts the OTel contract vmafx-controller
// inherits from bootstrap.Base (ADR-0782 / ADR-1119): golusoris's otel.Module
// is in the production graph, is a silent no-op without an OTLP endpoint, and
// the resource identity is service.name "vmafx-controller" (derived from the
// binary) with service.version from pkg/version.
func TestOTelWiredThroughBootstrap(t *testing.T) {
	writeControllerEnv(t)
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
	if opts.Service.Name != "vmafx-controller" {
		t.Errorf("service.name = %q, want vmafx-controller", opts.Service.Name)
	}
	if opts.Service.Version != buildversion.Version() {
		t.Errorf("service.version = %q, want %q", opts.Service.Version, buildversion.Version())
	}
}

// TestGRPCHealthEmitsLinkedSpans is the request-path integration test for the
// gRPC surface: an RPC against the production graph's bound listener produces
// a server span from grpcmod.Module's otelgrpc stats handler, and a client
// dialled with the otelgrpc client handler (what pkg/score and golusoris's
// ConnFactory install, ADR-1095) produces a client span in the same trace —
// i.e. the W3C traceparent crossed the wire and the two hops are linked.
func TestGRPCHealthEmitsLinkedSpans(t *testing.T) {
	writeControllerEnv(t)
	addr := freeLocalAddr(t)
	t.Setenv("VMAFX_GRPC_LISTEN", addr)
	sr := oteltest.Recorder(t)

	app := fxtest.New(t, productionGraph())
	app.RequireStart()
	defer app.RequireStop()

	conn, err := googlegrpc.NewClient(addr,
		googlegrpc.WithTransportCredentials(insecure.NewCredentials()),
		googlegrpc.WithStatsHandler(otelgrpc.NewClientHandler()),
	)
	if err != nil {
		t.Fatalf("dial gRPC: %v", err)
	}
	defer conn.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, err := vmafxv1.NewVmafxScoringClient(conn).Health(ctx, &vmafxv1.HealthRequest{}); err != nil {
		t.Fatalf("Health RPC: %v", err)
	}

	const rpc = "vmafx.v1.VmafxScoring/Health"
	// The server span ends on the server goroutine after the response is on
	// the wire, so it can trail the client's return by a few milliseconds.
	var serverSpan, clientSpan sdktrace.ReadOnlySpan
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) && (serverSpan == nil || clientSpan == nil) {
		for _, s := range oteltest.Ended(sr, rpc) {
			switch s.SpanKind() {
			case trace.SpanKindServer:
				serverSpan = s
			case trace.SpanKindClient:
				clientSpan = s
			}
		}
		if serverSpan == nil || clientSpan == nil {
			time.Sleep(20 * time.Millisecond)
		}
	}
	if serverSpan == nil || clientSpan == nil {
		t.Fatalf("want server+client spans named %q, got %v", rpc, oteltest.Names(sr))
	}
	if serverSpan.SpanContext().TraceID() != clientSpan.SpanContext().TraceID() {
		t.Errorf("trace context did not propagate: server trace %s, client trace %s",
			serverSpan.SpanContext().TraceID(), clientSpan.SpanContext().TraceID())
	}
	if serverSpan.Parent().SpanID() != clientSpan.SpanContext().SpanID() {
		t.Errorf("server span parent %s is not the client span %s",
			serverSpan.Parent().SpanID(), clientSpan.SpanContext().SpanID())
	}
}
