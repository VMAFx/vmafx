// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-controller/grpc_server_test.go — unit tests for the gRPC server.
//
// Tests use real SQLite-backed queue instances to avoid mock drift.  The
// stream is exercised through a minimal in-process mock that satisfies the
// VmafxController_StreamJobsServer interface without a live gRPC connection.
//
// ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
// ADR-0962: StreamJobs was a no-op; these tests pin the fixed behaviour.

//go:build cgo

package main

import (
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"testing"

	"google.golang.org/grpc"
	"google.golang.org/grpc/metadata"

	"github.com/VMAFx/vmafx/cmd/vmafx-controller/nodes"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/queue"
	"github.com/VMAFx/vmafx/cmd/vmafx-controller/scheduler"
	controllerv1 "github.com/VMAFx/vmafx/gen/go/controller"
	"github.com/VMAFx/vmafx/pkg/observability"

	"github.com/prometheus/client_golang/prometheus"
)

// ---------------------------------------------------------------------------
// In-process mock stream
// ---------------------------------------------------------------------------

// mockStreamJobsServer implements controllerv1.VmafxController_StreamJobsServer
// for unit testing without a live gRPC connection.
type mockStreamJobsServer struct {
	grpc.ServerStream
	ctx  context.Context
	sent []*controllerv1.Job
}

func (m *mockStreamJobsServer) Send(j *controllerv1.Job) error {
	m.sent = append(m.sent, j)
	return nil
}

func (m *mockStreamJobsServer) Context() context.Context { return m.ctx }

// The remaining grpc.ServerStream methods are satisfied by the embedded
// grpc.ServerStream (zero value — panics are safe here since tests never
// exercise header/trailer paths).

// Satisfy the grpc.ServerStream methods that are not provided by the zero-value embed.
func (m *mockStreamJobsServer) SetHeader(metadata.MD) error  { return nil }
func (m *mockStreamJobsServer) SendHeader(metadata.MD) error { return nil }
func (m *mockStreamJobsServer) SetTrailer(metadata.MD)       {}
func (m *mockStreamJobsServer) SendMsg(any) error            { return nil }
func (m *mockStreamJobsServer) RecvMsg(any) error            { return nil }

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

func newTestControllerServer(t *testing.T) *controllerServer {
	t.Helper()
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "test.db")
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelError}))

	q, err := queue.New(dbPath, log)
	if err != nil {
		t.Fatalf("queue.New: %v", err)
	}
	t.Cleanup(func() { _ = q.Close() })

	r := nodes.NewRegistry(context.Background(), log)
	t.Cleanup(func() { r.Close() })

	s := scheduler.New(q, r, log)
	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)

	return newControllerServer(q, r, s, metrics, log)
}

func newTestStream(ctx context.Context) *mockStreamJobsServer {
	return &mockStreamJobsServer{ctx: ctx}
}

// submitTestJob submits a job directly to the queue inside a controllerServer.
func submitTestJob(t *testing.T, cs *controllerServer, ref, dis string) string {
	t.Helper()
	id, err := cs.queue.Submit(context.Background(), &queue.Job{
		Scoring: queue.ScoringParams{Reference: ref, Distorted: dis},
	})
	if err != nil {
		t.Fatalf("Submit: %v", err)
	}
	return id
}

// ---------------------------------------------------------------------------
// Tests — Bug B.3: StreamJobs must not be a no-op
// ---------------------------------------------------------------------------

// TestStreamJobs_EmptyQueue_ReturnsOK verifies that StreamJobs on an empty
// queue completes with no error and sends zero messages.
func TestStreamJobs_EmptyQueue_ReturnsOK(t *testing.T) {
	cs := newTestControllerServer(t)
	stream := newTestStream(context.Background())

	err := cs.StreamJobs(&controllerv1.StreamJobsRequest{}, stream)
	if err != nil {
		t.Fatalf("StreamJobs on empty queue: unexpected error: %v", err)
	}
	if len(stream.sent) != 0 {
		t.Errorf("sent: got %d messages, want 0", len(stream.sent))
	}
}

// TestStreamJobs_WithJobs_StreamsSnapshot verifies that 3 submitted jobs are
// all streamed by StreamJobs (no filter) before it closes.
func TestStreamJobs_WithJobs_StreamsSnapshot(t *testing.T) {
	cs := newTestControllerServer(t)
	stream := newTestStream(context.Background())

	// Submit 3 jobs.
	ids := make(map[string]bool)
	for i := range 3 {
		id := submitTestJob(t, cs, "/ref.yuv", "/dis.yuv")
		ids[id] = true
		_ = i
	}

	err := cs.StreamJobs(&controllerv1.StreamJobsRequest{}, stream)
	if err != nil {
		t.Fatalf("StreamJobs: unexpected error: %v", err)
	}
	if len(stream.sent) != 3 {
		t.Fatalf("sent: got %d messages, want 3", len(stream.sent))
	}

	// Verify the IDs match what we submitted.
	for _, j := range stream.sent {
		if !ids[j.GetId()] {
			t.Errorf("received unexpected job ID %q", j.GetId())
		}
	}
}

// TestStreamJobs_WithStatusFilter verifies that a PENDING-only filter returns
// only the pending jobs (not completed ones).
func TestStreamJobs_WithStatusFilter_PendingOnly(t *testing.T) {
	cs := newTestControllerServer(t)
	stream := newTestStream(context.Background())

	// Submit 2 jobs.
	submitTestJob(t, cs, "/ref1.yuv", "/dis1.yuv")
	submitTestJob(t, cs, "/ref2.yuv", "/dis2.yuv")

	req := &controllerv1.StreamJobsRequest{
		StatusFilter: []controllerv1.JobStatus{controllerv1.JobStatus_PENDING},
	}
	if err := cs.StreamJobs(req, stream); err != nil {
		t.Fatalf("StreamJobs with filter: unexpected error: %v", err)
	}
	if len(stream.sent) != 2 {
		t.Errorf("sent with PENDING filter: got %d, want 2", len(stream.sent))
	}
	for _, j := range stream.sent {
		if j.GetStatus() != controllerv1.JobStatus_PENDING {
			t.Errorf("job %q has status %v, want PENDING", j.GetId(), j.GetStatus())
		}
	}
}
