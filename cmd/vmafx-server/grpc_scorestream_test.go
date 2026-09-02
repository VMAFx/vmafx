// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-server/grpc_scorestream_test.go — end-to-end test for the
// ScoreStream bidirectional RPC against the real in-process libvmaf engine
// (ADR-0933 Phase 2).
//
// Unlike grpc_server_handler_test.go (which uses a stub vmaf binary + a
// placeholder model for the unary path), ScoreStream drives the cgo streaming
// scorer, so this test wires a Scorer pointed at the real model/ directory and
// feeds the real 576x324 / 48-frame YUV fixtures over the wire. Skipped when
// the fixtures or model are unavailable.

//go:build cgo

package main

import (
	"context"
	"io"
	"net"
	"os"
	"path/filepath"
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

// repoRoot walks up from the working directory to the repository root (marked
// by CLAUDE.md), mirroring libvmaf.RepoRoot without importing an unexported
// helper.
func repoRoot(t *testing.T) string {
	t.Helper()
	dir, err := os.Getwd()
	if err != nil {
		t.Fatalf("getwd: %v", err)
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "CLAUDE.md")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			t.Fatalf("repo root (CLAUDE.md) not found from %s", dir)
		}
		dir = parent
	}
}

// startStreamTestServer starts an in-process gRPC server whose Scorer resolves
// models from the real model/ directory, so ScoreStream can load vmaf_v0.6.1.
func startStreamTestServer(t *testing.T, modelDir string) (vmafxv1.VmafxScoringClient, func()) {
	t.Helper()
	// The binary path is only used by the unary Score path; ScoreStream uses
	// the cgo engine directly. A harmless stub keeps libvmaf.New happy.
	stub := writeVmafStub(t, vmafGoldenJSON)
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
	return vmafxv1.NewVmafxScoringClient(conn), func() {
		_ = conn.Close()
		srv.GracefulStop()
	}
}

// streamE2EFixture resolves the model dir + YUV fixtures or skips the test.
func streamE2EFixture(t *testing.T) (modelDir, ref, dis string) {
	t.Helper()
	if testing.Short() {
		t.Skip("skipping end-to-end ScoreStream test in -short mode")
	}
	root := repoRoot(t)
	modelDir = filepath.Join(root, "model")
	ref = filepath.Join(root, "testdata", "ref_576x324_48f.yuv")
	dis = filepath.Join(root, "testdata", "dis_576x324_48f.yuv")
	for _, p := range []string{filepath.Join(modelDir, "vmaf_v0.6.1.json"), ref, dis} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("fixture missing (%s); skipping end-to-end", p)
		}
	}
	return modelDir, ref, dis
}

// TestGRPCScoreStream_EndToEnd opens a bidirectional stream, pushes the 48
// real frames, half-closes, and asserts the server streams back one FrameScore
// per frame followed by exactly one terminal AggregateScore with a sane pooled
// VMAF.
func TestGRPCScoreStream_EndToEnd(t *testing.T) {
	modelDir, ref, dis := streamE2EFixture(t)
	client, stop := startStreamTestServer(t, modelDir)
	defer stop()

	rb, err := os.ReadFile(ref)
	if err != nil {
		t.Fatalf("read ref: %v", err)
	}
	db, err := os.ReadFile(dis)
	if err != nil {
		t.Fatalf("read dis: %v", err)
	}
	// 576x324 yuv420p 8-bit frame size.
	const w, h = 576, 324
	frameSize := w*h + 2*(w/2)*(h/2)
	if len(rb)%frameSize != 0 {
		t.Fatalf("ref length %d not a multiple of frame size %d", len(rb), frameSize)
	}
	nFrames := len(rb) / frameSize

	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	stream, err := client.ScoreStream(ctx)
	if err != nil {
		t.Fatalf("ScoreStream open: %v", err)
	}

	// Opening config.
	if err := stream.Send(&vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_Config{
			Config: &vmafxv1.StreamConfig{
				Width: w, Height: h,
				PixelFormat:    vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P,
				Model:          "vmaf_v0.6.1",
				FrameCountHint: uint32(nFrames),
			},
		},
	}); err != nil {
		t.Fatalf("send config: %v", err)
	}

	// Push frames from a goroutine so a slow consumer cannot deadlock the
	// single-threaded send loop against gRPC flow control.
	sendErr := make(chan error, 1)
	go func() {
		for i := 0; i < nFrames; i++ {
			lo, hi := i*frameSize, (i+1)*frameSize
			if err := stream.Send(&vmafxv1.ScoreStreamRequest{
				Payload: &vmafxv1.ScoreStreamRequest_FramePair{
					FramePair: &vmafxv1.FramePair{
						FrameIndex:   uint32(i),
						RawReference: rb[lo:hi],
						RawDistorted: db[lo:hi],
					},
				},
			}); err != nil {
				sendErr <- err
				return
			}
		}
		sendErr <- stream.CloseSend()
	}()

	// Consume the response stream: N FrameScore then exactly one Aggregate.
	var frameScores int
	var aggregate *vmafxv1.AggregateScore
	for {
		resp, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("Recv: %v", err)
		}
		switch {
		case resp.GetFrameScore() != nil:
			if aggregate != nil {
				t.Fatal("received FrameScore after Aggregate")
			}
			fs := resp.GetFrameScore()
			if int(fs.GetFrameIndex()) != frameScores {
				t.Errorf("frame index out of order: got %d want %d", fs.GetFrameIndex(), frameScores)
			}
			if fs.GetScore() < 0 || fs.GetScore() > 100 {
				t.Errorf("frame %d score %f out of (0,100]", fs.GetFrameIndex(), fs.GetScore())
			}
			frameScores++
		case resp.GetAggregate() != nil:
			if aggregate != nil {
				t.Fatal("received more than one Aggregate")
			}
			aggregate = resp.GetAggregate()
		default:
			t.Fatalf("response had neither frame_score nor aggregate: %+v", resp)
		}
	}

	if err := <-sendErr; err != nil {
		t.Fatalf("send loop: %v", err)
	}

	if frameScores != nFrames {
		t.Errorf("got %d FrameScore messages, want %d", frameScores, nFrames)
	}
	if aggregate == nil {
		t.Fatal("never received terminal AggregateScore")
	}
	if int(aggregate.GetFramesProcessed()) != nFrames {
		t.Errorf("AggregateScore.frames_processed = %d, want %d", aggregate.GetFramesProcessed(), nFrames)
	}
	if aggregate.GetScore() <= 0 || aggregate.GetScore() > 100 {
		t.Errorf("aggregate VMAF %f out of (0,100]", aggregate.GetScore())
	}
	if len(aggregate.GetFeatures()) == 0 {
		t.Error("expected non-empty aggregate feature map")
	}
}

// TestGRPCScoreStream_FrameSizeMismatch verifies the server rejects a FramePair
// whose payload length does not match the configured geometry with
// codes.InvalidArgument rather than corrupting the picture planes.
func TestGRPCScoreStream_FrameSizeMismatch(t *testing.T) {
	modelDir, _, _ := streamE2EFixture(t)
	client, stop := startStreamTestServer(t, modelDir)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
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
		t.Fatalf("send config: %v", err)
	}
	// 64x64 yuv420p needs 6144 bytes; send a deliberately short buffer.
	if err := stream.Send(&vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_FramePair{
			FramePair: &vmafxv1.FramePair{
				FrameIndex:   0,
				RawReference: make([]byte, 10),
				RawDistorted: make([]byte, 10),
			},
		},
	}); err != nil {
		t.Fatalf("send frame: %v", err)
	}
	_ = stream.CloseSend()

	_, err = stream.Recv()
	if err == nil {
		t.Fatal("expected error for frame-size mismatch, got nil")
	}
	if got := status.Code(err); got != codes.InvalidArgument {
		t.Errorf("expected InvalidArgument, got %v (err: %v)", got, err)
	}
}
