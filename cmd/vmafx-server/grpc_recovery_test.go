// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/grpc_recovery_test.go — regression test for ADR-0978:
// a panic inside a gRPC handler must surface to the client as
// codes.Internal, not crash the server process.

//go:build cgo

package main

import (
	"context"
	"net"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/pkg/observability"
)

// panickyServer is a stub that panics on Score and ScoreStream. Without the
// recovery interceptors from ADR-0978 the panic would tear down the gRPC
// worker goroutine and crash the process.
type panickyServer struct {
	vmafxv1.UnimplementedVmafxScoringServer
}

func (panickyServer) Score(_ context.Context, _ *vmafxv1.ScoreRequest) (*vmafxv1.ScoreResponse, error) {
	panic("test panic: nil-pointer dereference simulation (ADR-0978)")
}

func (panickyServer) ScoreStream(stream vmafxv1.VmafxScoring_ScoreStreamServer) error {
	// Read one message so we exercise the stream interceptor path, then
	// panic exactly the way a cgo libvmaf crash would.
	_, _ = stream.Recv()
	panic("test panic: stream handler (ADR-0978)")
}

// TestPanicRecovery_Unary verifies a panic inside the unary Score handler
// converts to codes.Internal without crashing the server.
func TestPanicRecovery_Unary(t *testing.T) {
	log := observability.NewLogger("ERROR")

	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}

	srv := grpc.NewServer(
		grpc.UnaryInterceptor(recoveryUnaryInterceptor(log)),
		grpc.StreamInterceptor(recoveryStreamInterceptor(log)),
	)
	vmafxv1.RegisterVmafxScoringServer(srv, panickyServer{})
	go func() { _ = srv.Serve(lis) }()
	defer srv.Stop()

	conn, err := grpc.NewClient(lis.Addr().String(),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	client := vmafxv1.NewVmafxScoringClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	_, err = client.Score(ctx, &vmafxv1.ScoreRequest{
		Reference: "/dev/null",
		Distorted: "/dev/null",
	})
	if err == nil {
		t.Fatal("expected an error from panicking handler, got nil")
	}
	if got := status.Code(err); got != codes.Internal {
		t.Errorf("expected codes.Internal, got %v (err: %v)", got, err)
	}

	// The server must still be alive — a follow-up RPC should also reach
	// the panicky handler and return Internal, not connection-closed.
	_, err = client.Score(ctx, &vmafxv1.ScoreRequest{
		Reference: "/dev/null",
		Distorted: "/dev/null",
	})
	if err == nil {
		t.Fatal("second call: expected error from panicking handler, got nil")
	}
	if got := status.Code(err); got != codes.Internal {
		t.Errorf("second call: expected codes.Internal, got %v (err: %v)", got, err)
	}
}

// TestPanicRecovery_Stream verifies a panic inside the streaming ScoreStream
// handler converts to codes.Internal without crashing the server.
func TestPanicRecovery_Stream(t *testing.T) {
	log := observability.NewLogger("ERROR")

	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}

	srv := grpc.NewServer(
		grpc.UnaryInterceptor(recoveryUnaryInterceptor(log)),
		grpc.StreamInterceptor(recoveryStreamInterceptor(log)),
	)
	vmafxv1.RegisterVmafxScoringServer(srv, panickyServer{})
	go func() { _ = srv.Serve(lis) }()
	defer srv.Stop()

	conn, err := grpc.NewClient(lis.Addr().String(),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	client := vmafxv1.NewVmafxScoringClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	stream, err := client.ScoreStream(ctx)
	if err != nil {
		t.Fatalf("ScoreStream open: %v", err)
	}
	// Send a config to give the panicky handler something to Recv before
	// the panic fires.
	if err := stream.Send(&vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_Config{
			Config: &vmafxv1.StreamConfig{
				Width: 64, Height: 48,
				PixelFormat: vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P,
			},
		},
	}); err != nil {
		t.Fatalf("Send: %v", err)
	}
	_ = stream.CloseSend()

	_, err = stream.Recv()
	if err == nil {
		t.Fatal("expected an error from panicking stream handler, got nil")
	}
	if got := status.Code(err); got != codes.Internal {
		t.Errorf("expected codes.Internal, got %v (err: %v)", got, err)
	}
}
