// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// server_grpc_test.go — verifies that vmafx-node's Serve() registers the
// VmafxScoring gRPC service and that the service is reachable, with and without
// a configured scorer. The end-to-end ScoreStream path is covered against the
// real libvmaf engine when the model + YUV fixtures are present.

//go:build cgo

package server_test

import (
	"context"
	"io"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	"github.com/VMAFx/vmafx/cmd/vmafx-node/probe"
	"github.com/VMAFx/vmafx/cmd/vmafx-node/server"
	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
	"github.com/VMAFx/vmafx/pkg/libvmaf"
)

// startNode starts a node Server on a random port and returns a connected
// VmafxScoring client plus a stop function. scorer may be nil to exercise the
// Health-only path.
func startNode(t *testing.T, scorer *libvmaf.Scorer) (vmafxv1.VmafxScoringClient, func()) {
	t.Helper()
	// Bind a random port, then release it so Serve can re-bind the same addr.
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	addr := lis.Addr().String()
	_ = lis.Close()

	srv, err := server.New(server.Config{
		Addr:      addr,
		FFmpegBin: "ffmpeg",
		Encoders:  probe.EmptyInventory(),
		Scorer:    scorer,
	})
	if err != nil {
		t.Fatalf("server.New: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	serveErr := make(chan error, 1)
	go func() { serveErr <- srv.Serve(ctx) }()

	// Wait for the listener to accept connections.
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if c, derr := net.Dial("tcp", addr); derr == nil {
			_ = c.Close()
			break
		}
		time.Sleep(10 * time.Millisecond)
	}

	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		cancel()
		t.Fatalf("dial: %v", err)
	}

	stop := func() {
		_ = conn.Close()
		cancel()
		select {
		case err := <-serveErr:
			if err != nil {
				t.Errorf("Serve returned error: %v", err)
			}
		case <-time.After(5 * time.Second):
			t.Error("Serve did not return within 5s of cancel")
		}
	}
	return vmafxv1.NewVmafxScoringClient(conn), stop
}

// TestNodeServe_HealthReachable verifies that Serve registers VmafxScoring and
// the Health RPC answers even without a scorer configured.
func TestNodeServe_HealthReachable(t *testing.T) {
	client, stop := startNode(t, nil)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.Health(ctx, &vmafxv1.HealthRequest{})
	if err != nil {
		t.Fatalf("Health: %v", err)
	}
	if !resp.GetOk() {
		t.Errorf("Health.ok = false, want true")
	}
}

// TestNodeServe_ScoreWithoutScorer verifies that a node without a scorer
// rejects scoring RPCs with codes.FailedPrecondition (the service is still
// registered and reachable — only the engine is absent).
func TestNodeServe_ScoreWithoutScorer(t *testing.T) {
	client, stop := startNode(t, nil)
	defer stop()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	_, err := client.Score(ctx, &vmafxv1.ScoreRequest{
		Reference: "/tmp/r.yuv", Distorted: "/tmp/d.yuv",
	})
	if err == nil {
		t.Fatal("expected FailedPrecondition without a scorer, got nil")
	}
	if got := status.Code(err); got != codes.FailedPrecondition {
		t.Errorf("expected FailedPrecondition, got %v (err: %v)", got, err)
	}
}

// nodeRepoRoot walks up to the repo root (marked by CLAUDE.md).
func nodeRepoRoot(t *testing.T) string {
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
			t.Fatalf("repo root not found from %s", dir)
		}
		dir = parent
	}
}

// TestNodeServe_ScoreStreamEndToEnd drives the node's registered ScoreStream
// service against the real libvmaf engine and the 48-frame fixtures. Skipped
// when fixtures are missing.
func TestNodeServe_ScoreStreamEndToEnd(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping node ScoreStream end-to-end in -short mode")
	}
	root := nodeRepoRoot(t)
	modelDir := filepath.Join(root, "model")
	ref := filepath.Join(root, "testdata", "ref_576x324_48f.yuv")
	dis := filepath.Join(root, "testdata", "dis_576x324_48f.yuv")
	for _, p := range []string{filepath.Join(modelDir, "vmaf_v0.6.1.json"), ref, dis} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("fixture missing (%s); skipping", p)
		}
	}

	scorer, err := libvmaf.New("/bin/true", modelDir) // binary unused by ScoreStream
	if err != nil {
		t.Fatalf("libvmaf.New: %v", err)
	}
	client, stop := startNode(t, scorer)
	defer stop()

	rb, err := os.ReadFile(ref)
	if err != nil {
		t.Fatalf("read ref: %v", err)
	}
	db, err := os.ReadFile(dis)
	if err != nil {
		t.Fatalf("read dis: %v", err)
	}
	const w, h = 576, 324
	frameSize := w*h + 2*(w/2)*(h/2)
	nFrames := len(rb) / frameSize

	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	stream, err := client.ScoreStream(ctx)
	if err != nil {
		t.Fatalf("ScoreStream open: %v", err)
	}
	if err := stream.Send(&vmafxv1.ScoreStreamRequest{
		Payload: &vmafxv1.ScoreStreamRequest_Config{
			Config: &vmafxv1.StreamConfig{
				Width: w, Height: h,
				PixelFormat: vmafxv1.PixelFormat_PIXEL_FORMAT_YUV420P,
				Model:       "vmaf_v0.6.1",
			},
		},
	}); err != nil {
		t.Fatalf("send config: %v", err)
	}

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
		if fs := resp.GetFrameScore(); fs != nil {
			frameScores++
		} else if agg := resp.GetAggregate(); agg != nil {
			aggregate = agg
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
	if aggregate.GetScore() <= 0 || aggregate.GetScore() > 100 {
		t.Errorf("aggregate VMAF %f out of (0,100]", aggregate.GetScore())
	}
}
