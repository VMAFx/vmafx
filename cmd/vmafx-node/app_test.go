// SPDX-License-Identifier: EUPL-1.2
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/app_test.go — fx composition + lifecycle tests for the
// golusoris-framework node (ADR-1119 Phase-1 PR-3).
//
// These tests prove:
//
//  1. The production fx graph is satisfiable — every provider and invoke
//     resolves (TestAppGraphValidates / TestAppStartsAndBinds).
//  2. The golusoris gRPC listener actually binds and serves the registered
//     VmafxScoring service. fx providers are lazy, so the graph carries a
//     standalone fx.Invoke(func(_ *grpc.Server){}) to force construction of the
//     golusoris *grpc.Server; without it nothing consumes *grpc.Server and the
//     listener never starts. TestAppStartsAndBinds boots the real graph on a
//     fixed loopback port, dials it, and calls Health — the empirical proof the
//     listener came up and the service is wired.
//  3. R-node (cgo lifetime + drainer lifetime): at stop, the gRPC server drains
//     BEFORE the FeedbackClient drainer stops, which is BEFORE the libvmaf
//     scorer is closed. TestStopOrderNode observes the REAL OnStop hook firing
//     order via an fxevent.Logger.
//
// ADR-1119: golusoris fx framework adoption.

//go:build cgo

package main

import (
	"context"
	"fmt"
	"net"
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
	"google.golang.org/grpc/credentials/insecure"

	"github.com/golusoris/golusoris/otel"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/internal/app/bootstrap"
	"github.com/VMAFx/vmafx/internal/execstub"
	"github.com/VMAFx/vmafx/internal/oteltest"
	buildversion "github.com/VMAFx/vmafx/pkg/version"
)

// freeLoopbackAddr binds an OS-assigned loopback port, then releases it so the
// fx graph can re-bind the same address. This avoids a hard-coded port that
// could collide with a parallel run.
func freeLoopbackAddr(t *testing.T) string {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	addr := lis.Addr().String()
	_ = lis.Close()
	return addr
}

// writeNodeEnv writes a no-op vmaf stub + model dir and points the VMAFX_ config
// env vars at them, plus ephemeral loopback addresses (127.0.0.1:0) for gRPC and
// HTTP, letting the kernel assign free ports. Tests that need to dial must use
// writeDialableNodeEnv instead.
func writeNodeEnv(t *testing.T) {
	t.Helper()
	dir := t.TempDir()
	stub := filepath.Join(dir, "vmaf")
	execstub.Write(t, stub, []byte("#!/bin/sh\nexit 0\n"))
	modelDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(modelDir, "vmaf_v0.6.1.json"), []byte("{}"), 0o600); err != nil {
		t.Fatalf("write model: %v", err)
	}
	t.Setenv("VMAFX_VMAF_BINARY", stub)
	t.Setenv("VMAFX_MODEL_DIR", modelDir)
	t.Setenv("VMAFX_GRPC_LISTEN", "127.0.0.1:0")
	t.Setenv("VMAFX_HTTP_ADDR", "127.0.0.1:0")
	t.Setenv("VMAFX_LOG_LEVEL", "error")
	// /bin/true stands in for ffmpeg so the encoder probe runs (and degrades to
	// an empty inventory) without needing a real ffmpeg binary.
	t.Setenv("VMAFX_FFMPEG_BIN", "/bin/true")
	t.Setenv("VMAFX_SIDECAR_SOCKET", filepath.Join(t.TempDir(), "sidecar.sock"))
}

// writeDialableNodeEnv is only for a test that dials the listener, since the
// graph does not report the port it bound; such a test keeps the small
// reservation race.
func writeDialableNodeEnv(t *testing.T) string {
	t.Helper()
	writeNodeEnv(t)
	grpcAddr := freeLoopbackAddr(t)
	t.Setenv("VMAFX_GRPC_LISTEN", grpcAddr)
	t.Setenv("VMAFX_HTTP_ADDR", freeLoopbackAddr(t))
	return grpcAddr
}

// startAppWithRetry boots an fx graph, retrying up to 5 times if Start fails
// with "address already in use" by invoking setEnv before each attempt to pick
// fresh ports.
func startAppWithRetry(tb fxtest.TB, setEnv func(), opts ...fx.Option) (*fxtest.App, error) {
	var lastErr error
	for attempt := 1; attempt <= 5; attempt++ {
		if setEnv != nil {
			setEnv()
		}
		app := fxtest.New(tb, opts...)
		ctx, cancel := context.WithTimeout(context.Background(), app.StartTimeout())
		err := app.Start(ctx)
		cancel()
		if err == nil {
			return app, nil
		}
		lastErr = err
		if !strings.Contains(err.Error(), "address already in use") {
			return nil, err
		}
	}
	return nil, fmt.Errorf("start app after 5 attempts: %w", lastErr)
}

// startDialableNodeApp boots productionGraph() with fresh dialable addresses,
// retrying up to 5 times if binding fails with "address already in use".
func startDialableNodeApp(t *testing.T, opts ...fx.Option) *fxtest.App {
	t.Helper()
	allOpts := append([]fx.Option{productionGraph()}, opts...)
	app, err := startAppWithRetry(t, func() { writeDialableNodeEnv(t) }, allOpts...)
	if err != nil {
		t.Fatalf("start dialable node app: %v", err)
	}
	return app
}

// startNodeAppWithRetry boots opts with custom environment preparation, retrying
// up to 5 times if binding fails with "address already in use".
func startNodeAppWithRetry(t *testing.T, setEnv func(), opts ...fx.Option) *fxtest.App {
	t.Helper()
	app, err := startAppWithRetry(t, setEnv, opts...)
	if err != nil {
		t.Fatalf("start node app: %v", err)
	}
	return app
}

// TestWriteNodeEnvUsesKernelPorts asserts that writeNodeEnv configures
// kernel-assigned ephemeral ports (127.0.0.1:0), whereas writeDialableNodeEnv
// sets nonzero dialable loopback ports.
func TestWriteNodeEnvUsesKernelPorts(t *testing.T) {
	writeNodeEnv(t)
	if got := os.Getenv("VMAFX_GRPC_LISTEN"); got != "127.0.0.1:0" {
		t.Errorf("VMAFX_GRPC_LISTEN = %q, want 127.0.0.1:0", got)
	}
	if got := os.Getenv("VMAFX_HTTP_ADDR"); got != "127.0.0.1:0" {
		t.Errorf("VMAFX_HTTP_ADDR = %q, want 127.0.0.1:0", got)
	}

	grpcAddr := writeDialableNodeEnv(t)
	for _, envVar := range []string{"VMAFX_GRPC_LISTEN", "VMAFX_HTTP_ADDR"} {
		val := os.Getenv(envVar)
		_, portStr, err := net.SplitHostPort(val)
		if err != nil {
			t.Fatalf("split host port %s=%q: %v", envVar, val, err)
		}
		if portStr == "0" || portStr == "" {
			t.Errorf("%s port = %q, want nonzero port", envVar, portStr)
		}
	}
	if got := os.Getenv("VMAFX_GRPC_LISTEN"); got != grpcAddr {
		t.Errorf("VMAFX_GRPC_LISTEN = %q, want returned addr %q", got, grpcAddr)
	}
}

// productionGraph returns the exact provider/invoke set used by main(), minus
// the .Run() blocking call.
//
// The provider set and the ordering invokes come from main()'s own option
// constructors rather than a copy of them, so the tests exercise the real
// composition — including the standalone *grpc.Server invoke (lazy-provider
// guard) in the same relative position — and cannot drift out of lockstep with
// it. Only the foundation differs: the watch flag is off and no fx logger is
// installed, because a test app must not tail a ConfigMap or write startup
// chatter into the test output.
func productionGraph() fx.Option {
	return fx.Options(
		bootstrap.Base,
		fx.Replace(nodeEnvOptions(false)),
		nodeServerOptions(),
		nodeDomainOptions(),
		nodeLifecycleOptions(),
	)
}

// TestAppGraphValidates asserts the production dependency graph is satisfiable
// without starting any listeners. A missing provider, an ambiguous type, or a
// cyclic dependency fails here.
func TestAppGraphValidates(t *testing.T) {
	writeNodeEnv(t)
	if err := fx.ValidateApp(productionGraph()); err != nil {
		t.Fatalf("production fx graph does not validate: %v", err)
	}
}

// TestAppStartsAndBinds boots the full graph (real gRPC listener on a free
// loopback port) and shuts it down, proving start + graceful stop succeed.
//
// It then dials the configured gRPC address and calls Health — the empirical
// proof that the golusoris gRPC listener actually came up AND the VmafxScoring
// service is registered. Without the standalone *grpc.Server invoke in
// productionGraph, fx's laziness leaves the server unconstructed and the dial
// fails.
func TestAppStartsAndBinds(t *testing.T) {
	app := startDialableNodeApp(t)
	defer app.RequireStop()
	addr := os.Getenv("VMAFX_GRPC_LISTEN")

	conn, err := googlegrpc.NewClient(addr, googlegrpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("dial %s: %v", addr, err)
	}
	defer conn.Close()

	client := vmafxv1.NewVmafxScoringClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resp, err := client.Health(ctx, &vmafxv1.HealthRequest{})
	if err != nil {
		t.Fatalf("Health RPC against bound listener %s failed: %v", addr, err)
	}
	if !resp.GetOk() {
		t.Errorf("Health.ok = false, want true — listener bound but service mis-wired")
	}
}

// TestDialableNodeAppRetriesAddressInUse verifies that a port collision on
// start triggers a retry with fresh ports and succeeds.
func TestDialableNodeAppRetriesAddressInUse(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	occupiedAddr := ln.Addr().String()

	attempt := 0
	app, err := startAppWithRetry(t, func() {
		attempt++
		writeDialableNodeEnv(t)
		switch attempt {
		case 1:
			t.Setenv("VMAFX_HTTP_ADDR", occupiedAddr)
		case 2:
			_ = ln.Close()
		}
	}, productionGraph())
	if err != nil {
		_ = ln.Close()
		t.Fatalf("expected retry to succeed, got error: %v", err)
	}
	defer app.RequireStop()

	if attempt != 2 {
		t.Errorf("expected 2 attempts, got %d", attempt)
	}
}

// TestDialableNodeAppPlantedExhaustionFails is the planted negative case:
// when the address collision persists across all attempts, startAppWithRetry
// exhausts attempts and fails with the last error.
func TestDialableNodeAppPlantedExhaustionFails(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer func() { _ = ln.Close() }()
	occupiedAddr := ln.Addr().String()

	attempts := 0
	app, err := startAppWithRetry(t, func() {
		attempts++
		writeDialableNodeEnv(t)
		t.Setenv("VMAFX_HTTP_ADDR", occupiedAddr)
	}, productionGraph())
	if app != nil {
		defer app.RequireStop()
		t.Fatal("expected failure on exhausted attempts, got running app")
	}
	if err == nil {
		t.Fatal("expected error on exhausted attempts, got nil")
	}
	if !strings.Contains(err.Error(), "after 5 attempts") {
		t.Errorf("error %q does not mention 5 attempts", err)
	}
	if !strings.Contains(err.Error(), "address already in use") {
		t.Errorf("error %q does not contain 'address already in use'", err)
	}
}

// TestDialableNodeAppNonRetryableErrorFailsImmediately asserts that a startup
// error not caused by address collision fails immediately without retrying.
func TestDialableNodeAppNonRetryableErrorFailsImmediately(t *testing.T) {
	attempts := 0
	app, err := startAppWithRetry(t, func() {
		attempts++
		writeNodeEnv(t)
		t.Setenv("VMAFX_GRPC_LISTEN", "invalid-addr-not-bindable")
	}, productionGraph())
	if app != nil {
		defer app.RequireStop()
		t.Fatal("expected failure on invalid address, got running app")
	}
	if err == nil {
		t.Fatal("expected error on invalid address, got nil")
	}
	if attempts != 1 {
		t.Errorf("expected exactly 1 attempt on non-retryable error, got %d", attempts)
	}
	if strings.Contains(err.Error(), "after 5 attempts") {
		t.Errorf("expected immediate failure without attempt exhaustion, got %v", err)
	}
}

// orderRecorder is a minimal fxevent.Logger that records the CallerName of each
// OnStop hook as it begins executing. CallerName is the name of the function
// that *scheduled* the hook, so the scorer's Close hook (scheduled inside
// provideScorer), the FeedbackClient stop hook (scheduled inside
// provideFeedbackClient), and golusoris grpc.Module's GracefulStop hook
// (scheduled inside the framework's newServer) are distinguishable without
// injecting proxy hooks.
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

// TestStopOrderNode proves R-node: at fx stop, the gRPC server's OnStop
// (GracefulStop, draining in-flight Score / ScoreStream calls) runs BEFORE the
// FeedbackClient drainer stop, which runs BEFORE the scorer's Close (releasing
// cgo C resources).
//
// This observes the REAL hook firing order: an fxevent.Logger records the
// CallerName of every OnStop hook as it fires. A regression that constructs the
// gRPC server before the scorer/feedback client flips the sequence and fails the
// inversion checks below.
func TestStopOrderNode(t *testing.T) {
	writeNodeEnv(t)

	rec := &orderRecorder{}
	app := fxtest.New(t,
		productionGraph(),
		// Replace the fx event logger with our recorder so we observe the real
		// OnStop hook order. The last WithLogger wins.
		fx.WithLogger(func() fxevent.Logger { return rec }),
	)
	app.RequireStart()
	app.RequireStop()

	rec.mu.Lock()
	defer rec.mu.Unlock()

	grpcIdx, feedbackIdx, scorerIdx := -1, -1, -1
	for i, caller := range rec.order {
		switch {
		case strings.Contains(caller, "provideScorer"):
			if scorerIdx == -1 {
				scorerIdx = i
			}
		case strings.Contains(caller, "provideFeedbackClient"):
			if feedbackIdx == -1 {
				feedbackIdx = i
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
	if feedbackIdx == -1 {
		t.Fatalf("did not observe a provideFeedbackClient OnStop hook; callers=%v", rec.order)
	}
	if scorerIdx == -1 {
		t.Fatalf("did not observe a provideScorer OnStop hook; callers=%v", rec.order)
	}
	if grpcIdx > feedbackIdx {
		t.Errorf("R-node violated: gRPC GracefulStop (idx %d) ran AFTER FeedbackClient stop (idx %d); callers=%v",
			grpcIdx, feedbackIdx, rec.order)
	}
	if feedbackIdx > scorerIdx {
		t.Errorf("R-node violated: FeedbackClient stop (idx %d) ran AFTER scorer Close (idx %d); callers=%v",
			feedbackIdx, scorerIdx, rec.order)
	}
}

// TestProductionGraphBadGRPCAddrFailsStart asserts that a non-bindable gRPC
// listen address surfaces as a Start error from the production graph. Built via
// fx.New (not fxtest) because fxtest.RequireStart fails the test on a start
// error — here the error is the expected outcome and must be inspected.
func TestProductionGraphBadGRPCAddrFailsStart(t *testing.T) {
	writeNodeEnv(t)
	// Override only the gRPC address with something that cannot be bound.
	t.Setenv("VMAFX_GRPC_LISTEN", "invalid-addr-not-bindable")

	app := fx.New(productionGraph())
	ctx := t.Context()

	if err := app.Start(ctx); err == nil {
		_ = app.Stop(context.Background())
		t.Fatal("expected app.Start to fail with an unbindable gRPC address, got nil")
	}
}

// TestOTelWiredThroughBootstrap asserts the OTel contract vmafx-node inherits
// from bootstrap.Base (ADR-0782 / ADR-1119): golusoris's otel.Module is in the
// production graph, is a silent no-op without an OTLP endpoint, and the
// resource identity is service.name "vmafx-node" (derived from the binary)
// with service.version from pkg/version. The gRPC server span itself comes
// from grpcmod.Module's otelgrpc stats handler, exercised end to end by
// cmd/vmafx-controller's TestGRPCHealthEmitsLinkedSpans on the same module.
func TestOTelWiredThroughBootstrap(t *testing.T) {
	writeNodeEnv(t)
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
	if opts.Service.Name != "vmafx-node" {
		t.Errorf("service.name = %q, want vmafx-node", opts.Service.Name)
	}
	if opts.Service.Version != buildversion.Version() {
		t.Errorf("service.version = %q, want %q", opts.Service.Version, buildversion.Version())
	}
}
