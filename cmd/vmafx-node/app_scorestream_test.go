// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-node/app_scorestream_test.go — end-to-end ScoreStream test driving
// the fx-composed node's registered VmafxScoring service against the real
// libvmaf engine and the 48-frame fixtures. Replaces the equivalent coverage
// that lived in the (now-removed) cmd/vmafx-node/server package, but exercises
// the production fx graph + golusoris gRPC server instead of the hand-rolled
// server.Serve. Skipped when the model + YUV fixtures are missing or in -short.
//
// ADR-0933: in-memory per-frame ScoreStream.
// ADR-1119: golusoris fx framework adoption.

//go:build cgo

package main

import (
	"context"
	"io"
	"os"
	"path/filepath"
	"testing"
	"time"

	"go.uber.org/fx/fxtest"
	googlegrpc "google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	vmafxv1 "github.com/VMAFx/vmafx/gen/go"
)

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

// TestAppScoreStreamEndToEnd boots the production fx graph with the in-tree
// model dir and drives the node's registered ScoreStream service against the
// real libvmaf engine and the 48-frame fixtures. This proves the golusoris gRPC
// server serves the full streaming scoring contract, not just Health.
func TestAppScoreStreamEndToEnd(t *testing.T) {
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

	addr := freeLoopbackAddr(t)
	// /bin/true stands in for the vmaf CLI binary: ScoreStream uses libvmaf
	// directly (cgo), not the subprocess, so the binary is never executed.
	t.Setenv("VMAFX_VMAF_BINARY", "/bin/true")
	t.Setenv("VMAFX_MODEL_DIR", modelDir)
	t.Setenv("VMAFX_GRPC_LISTEN", addr)
	t.Setenv("VMAFX_LOG_LEVEL", "error")
	t.Setenv("VMAFX_FFMPEG_BIN", "/bin/true")
	t.Setenv("VMAFX_SIDECAR_SOCKET", filepath.Join(t.TempDir(), "sidecar.sock"))

	app := fxtest.New(t, productionGraph())
	app.RequireStart()
	defer app.RequireStop()

	conn, err := googlegrpc.NewClient(addr, googlegrpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("dial %s: %v", addr, err)
	}
	defer conn.Close()
	client := vmafxv1.NewVmafxScoringClient(conn)

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
