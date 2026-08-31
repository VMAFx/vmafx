// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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

	grpcmod "github.com/golusoris/golusoris/grpc"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/internal/app/bootstrap"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
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
// env vars at them, plus a fixed-but-free loopback gRPC address and a temp
// sidecar socket. It returns the gRPC address so the test can dial it.
func writeNodeEnv(t *testing.T) string {
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
	addr := freeLoopbackAddr(t)
	t.Setenv("VMAFX_VMAF_BINARY", stub)
	t.Setenv("VMAFX_MODEL_DIR", modelDir)
	t.Setenv("VMAFX_GRPC_LISTEN", addr)
	t.Setenv("VMAFX_LOG_LEVEL", "error")
	// /bin/true stands in for ffmpeg so the encoder probe runs (and degrades to
	// an empty inventory) without needing a real ffmpeg binary.
	t.Setenv("VMAFX_FFMPEG_BIN", "/bin/true")
	t.Setenv("VMAFX_SIDECAR_SOCKET", filepath.Join(t.TempDir(), "sidecar.sock"))
	return addr
}

// productionGraph returns the exact provider/invoke set used by main(), minus
// the .Run() blocking call. Kept in lockstep with main() so the tests exercise
// the real composition — including the standalone *grpc.Server invoke
// (lazy-provider guard) in the same relative position.
func productionGraph() fx.Option {
	return fx.Options(
		bootstrap.Base,
		fx.Replace(nodeEnvOptions(false)),
		grpcmod.Module,
		fx.Decorate(withNodeGRPCDefault),
		fx.Provide(
			provideEncoderInventory,
			provideScorer,
			provideExecutor,
			provideFeedbackClient,
			provideStatusRegistry,
			newScoringHandler,
		),
		// R-node ordering invoke: realise the Scorer before the gRPC server.
		fx.Invoke(func(_ *libvmaf.Scorer) {}),
		// Force the lifecycle-bearing domain objects between the scorer and the
		// gRPC server so their OnStop hooks land in the right slots.
		fx.Invoke(func(_ *FeedbackClient, _ *Executor) {}),
		fx.Invoke(func(s *googlegrpc.Server, h *scoringHandler) {
			vmafxv1.RegisterVmafxScoringServer(s, h)
		}),
		// Lazy-provider guard: force construction of the golusoris *grpc.Server so
		// its OnStart listener binds. Placed last so its OnStop fires first.
		fx.Invoke(func(_ *googlegrpc.Server) {}),
		fx.Invoke(mountNodeHealth),
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
	addr := writeNodeEnv(t)

	app := fxtest.New(t, productionGraph())
	app.RequireStart()
	defer app.RequireStop()

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
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	if err := app.Start(ctx); err == nil {
		_ = app.Stop(context.Background())
		t.Fatal("expected app.Start to fail with an unbindable gRPC address, got nil")
	}
}
