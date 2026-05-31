// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/score/grpc_client_test.go — smoke tests for the score client wrapper.
//
// These tests exercise the framing surface of ScoreStream against an in-process
// gRPC server. They do not require a real libvmaf scoring backend because the
// Phase 1 server stub returns codes.Unimplemented after validating the opening
// StreamConfig (ADR-0933).

package score

import (
	"context"
	"errors"
	"net"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
)

// stubServer is a minimal VmafxScoringServer that mimics the Phase 1
// behaviour of cmd/vmafx-server/grpc_server.go without dragging in cgo.
type stubServer struct {
	vmafxv1.UnimplementedVmafxScoringServer
}

func (stubServer) ScoreStream(stream vmafxv1.VmafxScoring_ScoreStreamServer) error {
	first, err := stream.Recv()
	if err != nil {
		return status.Errorf(codes.InvalidArgument, "recv: %v", err)
	}
	cfg := first.GetConfig()
	if cfg == nil {
		return status.Errorf(codes.InvalidArgument, "first message must set the config oneof")
	}
	if cfg.GetWidth() == 0 || cfg.GetHeight() == 0 {
		return status.Errorf(codes.InvalidArgument, "non-zero width and height required")
	}
	if cfg.GetPixelFormat() == vmafxv1.PixelFormat_PIXEL_FORMAT_UNSPECIFIED {
		return status.Errorf(codes.InvalidArgument, "pixel_format must be set")
	}
	return status.Errorf(codes.Unimplemented, "Phase 2")
}

// startStub spins up the stub server on an ephemeral port and returns
// the dial-able address plus a cleanup function.
func startStub(t *testing.T) (string, func()) {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	srv := grpc.NewServer()
	vmafxv1.RegisterVmafxScoringServer(srv, stubServer{})
	go func() { _ = srv.Serve(lis) }()
	return lis.Addr().String(), func() { srv.Stop() }
}

func TestScoreStream_RejectsZeroDimensions(t *testing.T) {
	addr, stop := startStub(t)
	defer stop()

	cli, err := Dial(addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer cli.Close()

	// Manually open a raw stream and send a deliberately invalid config so
	// we exercise the server's framing validation, not OpenScoreStream's.
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	raw, err := cli.api.ScoreStream(ctx)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	if err := raw.Send(&vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_Config{
			Config: &vmafxv1.StreamConfig{
				Width:       0,
				Height:      0,
				PixelFormat: vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P,
			},
		},
	}); err != nil {
		t.Fatalf("send: %v", err)
	}
	if err := raw.CloseSend(); err != nil {
		t.Fatalf("closeSend: %v", err)
	}
	_, recvErr := raw.Recv()
	if recvErr == nil {
		t.Fatal("expected an error for zero dimensions, got nil")
	}
	if got := status.Code(recvErr); got != codes.InvalidArgument {
		t.Fatalf("expected codes.InvalidArgument, got %v (%v)", got, recvErr)
	}
}

func TestScoreStream_ValidConfigReturnsUnimplemented(t *testing.T) {
	addr, stop := startStub(t)
	defer stop()

	cli, err := Dial(addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer cli.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	stream, err := cli.OpenScoreStream(ctx, 64, 48, vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P, "", 0)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	if err := stream.CloseSend(); err != nil {
		t.Fatalf("closeSend: %v", err)
	}
	_, _, recvErr := stream.Recv()
	if recvErr == nil {
		t.Fatal("expected Phase 1 Unimplemented, got nil")
	}
	// Recv wraps the gRPC error in fmt.Errorf, so unwrap before checking.
	var st interface{ GRPCStatus() *status.Status }
	if !errors.As(recvErr, &st) {
		t.Fatalf("expected wrapped gRPC status, got %T: %v", recvErr, recvErr)
	}
	if got := st.GRPCStatus().Code(); got != codes.Unimplemented {
		t.Fatalf("expected codes.Unimplemented, got %v (%v)", got, recvErr)
	}
}

// Compile-time check that OpenScoreStream signature stays in sync with the
// generated client.
var _ = func(c *Client) (*ScoreStream, error) {
	return c.OpenScoreStream(context.Background(), 0, 0, vmafxv1.PixelFormat_PIXEL_FORMAT_UNSPECIFIED, "", 0)
}
