// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/grpc_server_handler_test.go — unit tests for the gRPC
// handlers: Score, Health, ScoreStream, and the runGRPC/runGRPCWithServer
// lifecycle helpers.
//
// Tests use a real in-process gRPC listener (random port) and a stub
// libvmaf.Scorer backed by a small shell script, so no GPU or real vmaf
// binary is required.
//
// ADR-0703: vmafx-server Go gRPC + HTTP service.
// ADR-0933: ScoreStream Phase 1 stub.

//go:build cgo

package main

import (
	"context"
	"net"
	"os"
	"testing"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// ---------------------------------------------------------------------------
// Test helper: in-process gRPC server
// ---------------------------------------------------------------------------

// startGRPCTestServer starts a real in-process gRPC server backed by the stub
// scorer and returns a client and a stop function.  The server installs the
// recovery interceptors so handler tests cover the production code path.
func startGRPCTestServer(t *testing.T) (vmafxv1.VmafxScoringClient, func()) {
	t.Helper()
	stub := writeVmafStub(t, vmafGoldenJSON)
	modelDir := writeModelFile(t)
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}

	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	impl := newGRPCServer(scorer, metrics, log)

	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	srv := grpc.NewServer(
		grpc.UnaryInterceptor(recoveryUnaryInterceptor(log)),
		grpc.StreamInterceptor(recoveryStreamInterceptor(log)),
	)
	vmafxv1.RegisterVmafxScoringServer(srv, impl)
	go func() { _ = srv.Serve(lis) }()

	conn, err := grpc.NewClient(lis.Addr().String(),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		srv.Stop()
		t.Fatalf("dial: %v", err)
	}

	stop := func() {
		_ = conn.Close()
		srv.GracefulStop()
	}
	return vmafxv1.NewVmafxScoringClient(conn), stop
}

// ---------------------------------------------------------------------------
// Score handler
// ---------------------------------------------------------------------------

// TestGRPCScore_HappyPath verifies that a well-formed Score request returns
// the expected score from the stub scorer.
func TestGRPCScore_HappyPath(t *testing.T) {
	t.Parallel()
	client, stop := startGRPCTestServer(t)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.Score(ctx, &vmafxv1.ScoreRequest{
		Reference: "/tmp/ref.yuv",
		Distorted: "/tmp/dis.yuv",
		Model:     "vmaf_v0.6.1",
	})
	if err != nil {
		t.Fatalf("Score: %v", err)
	}
	const wantScore = 76.6683
	if resp.GetScore() != wantScore {
		t.Errorf("score: got %v, want %v", resp.GetScore(), wantScore)
	}
	if len(resp.GetFeatures()) == 0 {
		t.Error("expected non-empty features map")
	}
}

// TestGRPCScore_MissingReference verifies that a Score request missing the
// reference path returns codes.InvalidArgument.
func TestGRPCScore_MissingReference(t *testing.T) {
	t.Parallel()
	client, stop := startGRPCTestServer(t)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	_, err := client.Score(ctx, &vmafxv1.ScoreRequest{
		Distorted: "/tmp/dis.yuv",
	})
	if err == nil {
		t.Fatal("expected error for missing reference, got nil")
	}
	if got := status.Code(err); got != codes.InvalidArgument {
		t.Errorf("expected InvalidArgument, got %v", got)
	}
}

// TestGRPCScore_MissingDistorted verifies that a Score request missing the
// distorted path returns codes.InvalidArgument.
func TestGRPCScore_MissingDistorted(t *testing.T) {
	t.Parallel()
	client, stop := startGRPCTestServer(t)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	_, err := client.Score(ctx, &vmafxv1.ScoreRequest{
		Reference: "/tmp/ref.yuv",
	})
	if err == nil {
		t.Fatal("expected error for missing distorted, got nil")
	}
	if got := status.Code(err); got != codes.InvalidArgument {
		t.Errorf("expected InvalidArgument, got %v", got)
	}
}

// ---------------------------------------------------------------------------
// Health handler
// ---------------------------------------------------------------------------

// TestGRPCHealth verifies the Health RPC returns ok=true and a non-empty message.
func TestGRPCHealth(t *testing.T) {
	t.Parallel()
	client, stop := startGRPCTestServer(t)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.Health(ctx, &vmafxv1.HealthRequest{})
	if err != nil {
		t.Fatalf("Health: %v", err)
	}
	if !resp.GetOk() {
		t.Errorf("Health.ok: got false, want true")
	}
	if resp.GetMessage() == "" {
		t.Error("Health.message: expected non-empty")
	}
}

// ---------------------------------------------------------------------------
// ScoreStream handler (Phase 1 stub — ADR-0933)
// ---------------------------------------------------------------------------

// TestGRPCScoreStream_NoMessage verifies that closing the stream without
// sending any message causes the handler to return InvalidArgument (EOF on
// Recv maps to that).
func TestGRPCScoreStream_NoMessage(t *testing.T) {
	t.Parallel()
	client, stop := startGRPCTestServer(t)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	stream, err := client.ScoreStream(ctx)
	if err != nil {
		t.Fatalf("ScoreStream open: %v", err)
	}
	// Close without sending any message — handler Recv gets EOF.
	if err := stream.CloseSend(); err != nil {
		t.Fatalf("CloseSend: %v", err)
	}
	_, err = stream.Recv()
	if err == nil {
		t.Fatal("expected error from ScoreStream without config, got nil")
	}
	if got := status.Code(err); got != codes.InvalidArgument {
		t.Errorf("expected InvalidArgument, got %v (err: %v)", got, err)
	}
}

// TestGRPCScoreStream_NonConfigFirstMessage verifies that sending a FramePair
// as the first message (no Config oneof) returns codes.InvalidArgument.
func TestGRPCScoreStream_NonConfigFirstMessage(t *testing.T) {
	t.Parallel()
	client, stop := startGRPCTestServer(t)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	stream, err := client.ScoreStream(ctx)
	if err != nil {
		t.Fatalf("ScoreStream open: %v", err)
	}
	// Send a FramePair as the first message (Config oneof not set).
	if err := stream.Send(&vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_FramePair{
			FramePair: &vmafxv1.FramePair{
				FrameIndex:   0,
				RawReference: []byte("ref"),
				RawDistorted: []byte("dis"),
			},
		},
	}); err != nil {
		t.Fatalf("Send frame pair: %v", err)
	}
	_ = stream.CloseSend()

	_, err = stream.Recv()
	if err == nil {
		t.Fatal("expected error from non-config first message, got nil")
	}
	if got := status.Code(err); got != codes.InvalidArgument {
		t.Errorf("expected InvalidArgument, got %v (err: %v)", got, err)
	}
}

// TestGRPCScoreStream_ZeroDimensions verifies that a StreamConfig with zero
// width or height returns codes.InvalidArgument.
func TestGRPCScoreStream_ZeroDimensions(t *testing.T) {
	t.Parallel()
	client, stop := startGRPCTestServer(t)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	stream, err := client.ScoreStream(ctx)
	if err != nil {
		t.Fatalf("ScoreStream open: %v", err)
	}
	if err := stream.Send(&vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_Config{
			Config: &vmafxv1.StreamConfig{
				Width: 0, Height: 0,
				PixelFormat: vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P,
			},
		},
	}); err != nil {
		t.Fatalf("Send: %v", err)
	}
	_ = stream.CloseSend()

	_, err = stream.Recv()
	if err == nil {
		t.Fatal("expected error for zero dimensions, got nil")
	}
	if got := status.Code(err); got != codes.InvalidArgument {
		t.Errorf("expected InvalidArgument, got %v (err: %v)", got, err)
	}
}

// TestGRPCScoreStream_UnspecifiedPixelFormat verifies that a StreamConfig with
// PIXEL_FORMAT_UNSPECIFIED returns codes.InvalidArgument.
func TestGRPCScoreStream_UnspecifiedPixelFormat(t *testing.T) {
	t.Parallel()
	client, stop := startGRPCTestServer(t)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	stream, err := client.ScoreStream(ctx)
	if err != nil {
		t.Fatalf("ScoreStream open: %v", err)
	}
	if err := stream.Send(&vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_Config{
			Config: &vmafxv1.StreamConfig{
				Width: 1920, Height: 1080,
				PixelFormat: vmafxv1.PixelFormat_PIXEL_FORMAT_UNSPECIFIED,
			},
		},
	}); err != nil {
		t.Fatalf("Send: %v", err)
	}
	_ = stream.CloseSend()

	_, err = stream.Recv()
	if err == nil {
		t.Fatal("expected error for unspecified pixel format, got nil")
	}
	if got := status.Code(err); got != codes.InvalidArgument {
		t.Errorf("expected InvalidArgument, got %v (err: %v)", got, err)
	}
}

// TestGRPCScoreStream_BadModelRejected verifies that a well-formed StreamConfig
// whose resolved model file is not a loadable VMAF model is rejected with a
// non-Unimplemented status (Phase 2 wires real scoring per ADR-0933, so the
// engine now actually attempts to load the model). startGRPCTestServer uses a
// placeholder "{}" model file, which libvmaf cannot parse — the handler must
// surface that as an error code, never codes.Unimplemented.
func TestGRPCScoreStream_BadModelRejected(t *testing.T) {
	t.Parallel()
	client, stop := startGRPCTestServer(t)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	stream, err := client.ScoreStream(ctx)
	if err != nil {
		t.Fatalf("ScoreStream open: %v", err)
	}
	if err := stream.Send(&vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_Config{
			Config: &vmafxv1.StreamConfig{
				Width: 64, Height: 64,
				PixelFormat: vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P,
				Model:       "vmaf_v0.6.1",
			},
		},
	}); err != nil {
		t.Fatalf("Send: %v", err)
	}
	_ = stream.CloseSend()

	_, err = stream.Recv()
	if err == nil {
		t.Fatal("expected an error for an unloadable model, got nil")
	}
	if got := status.Code(err); got == codes.Unimplemented {
		t.Errorf("ScoreStream must not return Unimplemented in Phase 2; got %v", err)
	}
}

// ---------------------------------------------------------------------------
// Score handler — scorer-failure path
// ---------------------------------------------------------------------------

// TestGRPCScore_ScorerError verifies that when the libvmaf scorer returns an
// error (e.g. the vmaf subprocess exits non-zero), the Score handler returns
// codes.Internal to the caller. This covers grpc_server.go:74-78, the branch
// that was previously exercised only for the HTTP path (TestScoreInternalServerError)
// and the REST adapter (TestRestAdapter_ScoreVideoPair_ScorerError) but not for
// the gRPC handler itself.
func TestGRPCScore_ScorerError(t *testing.T) {
	t.Parallel()

	// Build a stub vmaf binary that always exits non-zero so scorer.Score fails.
	dir := t.TempDir()
	stub := dir + "/vmaf"
	const failScript = "#!/bin/sh\necho 'stub: deliberate scorer failure' >&2\nexit 1\n"
	if err := os.WriteFile(stub, []byte(failScript), 0o700); err != nil {
		t.Fatalf("write stub: %v", err)
	}
	modelDir := writeModelFile(t)
	scorer, err := libvmaf.New(stub, modelDir)
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}

	reg := prometheus.NewRegistry()
	metrics := observability.NewMetrics(reg)
	log := observability.NewLogger("ERROR")
	impl := newGRPCServer(scorer, metrics, log)

	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	srv := grpc.NewServer(
		grpc.UnaryInterceptor(recoveryUnaryInterceptor(log)),
		grpc.StreamInterceptor(recoveryStreamInterceptor(log)),
	)
	vmafxv1.RegisterVmafxScoringServer(srv, impl)
	go func() { _ = srv.Serve(lis) }()
	defer srv.GracefulStop()

	conn, err := grpc.NewClient(lis.Addr().String(),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		srv.Stop()
		t.Fatalf("dial: %v", err)
	}
	defer func() { _ = conn.Close() }()

	client := vmafxv1.NewVmafxScoringClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	_, err = client.Score(ctx, &vmafxv1.ScoreRequest{
		Reference: "/tmp/ref.yuv",
		Distorted: "/tmp/dis.yuv",
		Model:     "vmaf_v0.6.1",
	})
	if err == nil {
		t.Fatal("expected codes.Internal when scorer fails, got nil")
	}
	if got := status.Code(err); got != codes.Internal {
		t.Errorf("expected codes.Internal, got %v (err: %v)", got, err)
	}
}
