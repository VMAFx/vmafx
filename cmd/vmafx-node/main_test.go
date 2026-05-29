// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/main_test.go — lifecycle tests for vmafx-node.
//
// Tests exercise the config loading, GPU detection wiring, and the
// executor dispatch path.  The full lifecycle (Register → PullWork →
// Execute → ReportResult) is tested with a mock controller server.
//
// ADR-0713: vmafx-node Go worker binary.

package main

import (
	"context"
	"log/slog"

	"testing"
	"time"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/ai"
	"github.com/VMAFx/vmafx/pkg/gpu"
)

// ---------------------------------------------------------------------------

// Executor tests
// ---------------------------------------------------------------------------
//
// The previous mockController scaffolding (RegisterNode / Heartbeat /
// PullWork / ReportResult) was removed in the go-nilness audit (2026-05-30,
// staticcheck U1000) — it was orphaned when the controller-registration path
// was replaced by the ffmpeg-probe / gRPC server model documented in the
// header above. The remaining tests exercise the Executor directly.

func TestExecutor_NilScorer(t *testing.T) {
	t.Parallel()
	exec := NewExecutor(nil, nil, "cpu", slog.Default())
	job := &controllerv1.Job{
		Id: "test-job-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/ref.yuv",
			Distorted: "/dis.yuv",
		},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error when scorer is nil")
	}
}

func TestExecutor_UnsupportedCompareJob(t *testing.T) {
	t.Parallel()
	// A job with no reference+distorted does not classify as SCORING or AI.
	exec := NewExecutor(nil, nil, "cpu", slog.Default())
	job := &controllerv1.Job{
		Id:      "compare-job-1",
		Scoring: &controllerv1.ScoringParams{},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error for unclassifiable job")
	}
}

func TestExecutor_AIJobUnsupportedStage1(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	reg := ai.NewRegistry(dir)
	exec := NewExecutor(nil, reg, "cpu", slog.Default())
	// AI job: has reference but no distorted, model set.
	job := &controllerv1.Job{
		Id: "ai-job-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/features.json",
			Model:     "nr_metric_v1",
		},
	}
	result := exec.Execute(context.Background(), job)
	if result.Error == nil {
		t.Fatal("expected error — Stage 1 AI jobs have no input transport")
	}
}

// ---------------------------------------------------------------------------
// Lifecycle integration test with mock controller
// ---------------------------------------------------------------------------

// TestNodeLifecycle_RegisterHeartbeatPullReport runs the core lifecycle:
// register → heartbeat (at least one) → pull one SCORING job →
// report result (error expected since no vmaf binary in test env).
func TestNodeLifecycle_RegisterHeartbeatPullReport(t *testing.T) {
	if os.Getenv("VMAFX_INTEGRATION") == "" {
		t.Skip("set VMAFX_INTEGRATION=1 to run lifecycle integration test")
	}
	t.Parallel()

	mc := newMockController()
	mc.jobToReturn = &controllerv1.Job{
		Id: "integration-job-1",
		Scoring: &controllerv1.ScoringParams{
			Reference: "/nonexistent/ref.yuv",
			Distorted: "/nonexistent/dis.yuv",
			Model:     "vmaf_v0.6.1",
		},
	}

	_, addr := startMockController(t, mc)

	// Set env for the node.
	t.Setenv("VMAFX_CONTROLLER_ADDR", addr)
	t.Setenv("VMAFX_NODE_ID", "test-node")
	t.Setenv("VMAFX_LOG_LEVEL", "error")

	cfg, err := loadConfig()
	if err != nil {
		t.Fatalf("loadConfig: %v", err)
	}

	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))
	n := &node{cfg: cfg, log: log}
	n.gpuCap = gpu.Detect()
	if n.cfg.backend == "" {
		n.cfg.backend = n.gpuCap.PrimaryBackend()
	}

	conn, dialErr := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if dialErr != nil {
		t.Fatalf("dial: %v", dialErr)
	}
	t.Cleanup(func() { _ = conn.Close() })

	n.controllerCC = conn
	n.client = controllerv1.NewVmafxControllerClient(conn)
	n.executor = NewExecutor(nil, ai.NewRegistry(t.TempDir()), "cpu", log)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	// Register.
	if err := n.register(ctx); err != nil {
		t.Fatalf("register: %v", err)
	}
	// RegisterNode was called.
	select {
	case <-mc.registered:
	case <-time.After(2 * time.Second):
		t.Fatal("RegisterNode not called within 2s")
	}

	// Pull one job.
	pullCtx, pullCancel := context.WithTimeout(ctx, 5*time.Second)
	resp, pullErr := n.client.PullWork(pullCtx, &controllerv1.PullWorkRequest{
		NodeId:       n.cfg.nodeID,
		SessionToken: n.sessionToken,
		Capability: &controllerv1.NodeCapability{
			GpuVendor:   string(n.gpuCap.Vendor),
			Backends:    n.gpuCap.Backends,
			Concurrency: 1,
		},
	})
	pullCancel()
	if pullErr != nil {
		t.Fatalf("PullWork: %v", pullErr)
	}

	job := resp.GetJob()
	if job == nil {
		t.Fatal("expected a job from PullWork")
	}

	// Execute (will fail since /nonexistent/ref.yuv does not exist — that is expected).
	n.executeAndReport(ctx, job)

	// ReportResult must have been called.
	select {
	case reported := <-mc.reportedResult:
		if reported.GetJobId() != "integration-job-1" {
			t.Errorf("reported job_id = %q, want %q", reported.GetJobId(), "integration-job-1")
		}
		if !reported.GetFinal() {
			t.Error("expected final=true in ReportResult")
		}
	case <-time.After(3 * time.Second):
		t.Fatal("ReportResult not called within 3s")
	}
}
