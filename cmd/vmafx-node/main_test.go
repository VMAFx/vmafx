// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/main_test.go — lifecycle tests for vmafx-node.
//
// Tests exercise the executor dispatch path via the mock controller server.
// Config-loading and node-registration tests were removed in the go-workspace
// audit (2026-05-29): the controller-registration path was replaced by the
// ffmpeg-probe / gRPC server model in the current main.go; those symbols
// (loadConfig, node, register, executeAndReport) no longer exist.
//
// ADR-0713: vmafx-node Go worker binary.

package main

import (
	"context"
	"log/slog"
	"net"
	"testing"

	"google.golang.org/grpc"

	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/ai"
)

// ---------------------------------------------------------------------------
// Mock controller server
// ---------------------------------------------------------------------------

// mockController implements a minimal controllerv1.VmafxControllerServer for
// testing the node lifecycle.
type mockController struct {
	controllerv1.UnimplementedVmafxControllerServer

	// registered tracks whether RegisterNode was called.
	registered chan struct{}
	// heartbeats counts received heartbeats.
	heartbeats chan struct{}
	// pullCount tracks PullWork calls.
	pullCount int
	// jobToReturn is the job returned on the first PullWork call.
	jobToReturn *controllerv1.Job
	// reportedResult holds the last ReportResult call.
	reportedResult chan *controllerv1.ReportResultRequest
}

func newMockController() *mockController {
	return &mockController{
		registered:     make(chan struct{}, 1),
		heartbeats:     make(chan struct{}, 10),
		reportedResult: make(chan *controllerv1.ReportResultRequest, 1),
	}
}

func (m *mockController) RegisterNode(_ context.Context, req *controllerv1.RegisterNodeRequest) (*controllerv1.RegisterNodeResponse, error) {
	select {
	case m.registered <- struct{}{}:
	default:
	}
	return &controllerv1.RegisterNodeResponse{
		NodeId:       req.GetName() + "-assigned",
		SessionToken: "test-token",
	}, nil
}

func (m *mockController) Heartbeat(_ context.Context, _ *controllerv1.HeartbeatRequest) (*controllerv1.HeartbeatResponse, error) {
	select {
	case m.heartbeats <- struct{}{}:
	default:
	}
	return &controllerv1.HeartbeatResponse{Ok: true}, nil
}

func (m *mockController) PullWork(_ context.Context, _ *controllerv1.PullWorkRequest) (*controllerv1.PullWorkResponse, error) {
	m.pullCount++
	if m.pullCount == 1 && m.jobToReturn != nil {
		return &controllerv1.PullWorkResponse{Job: m.jobToReturn}, nil
	}
	return &controllerv1.PullWorkResponse{}, nil
}

func (m *mockController) ReportResult(_ context.Context, req *controllerv1.ReportResultRequest) (*controllerv1.ReportResultResponse, error) {
	select {
	case m.reportedResult <- req:
	default:
	}
	return &controllerv1.ReportResultResponse{Ok: true}, nil
}

// startMockController starts the mock server on a random port and returns
// (server, address, cleanup).
func startMockController(t *testing.T, mc *mockController) (*grpc.Server, string) {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	srv := grpc.NewServer()
	controllerv1.RegisterVmafxControllerServer(srv, mc)
	go func() {
		if serveErr := srv.Serve(lis); serveErr != nil {
			// Expect ErrServerStopped on test teardown.
		}
	}()
	t.Cleanup(srv.GracefulStop)
	return srv, lis.Addr().String()
}

// ---------------------------------------------------------------------------
// Executor tests
// ---------------------------------------------------------------------------

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
