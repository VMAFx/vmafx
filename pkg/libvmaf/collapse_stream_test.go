// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/libvmaf/collapse_stream_test.go — the stream path of the collapse
// (#2434): the StreamScorer against the C API and ScoreDirect, and the plane
// fill it shares with the engine.

//go:build cgo

package libvmaf

import (
	"bytes"
	"context"
	"errors"
	"testing"

	"github.com/VMAFx/vmafx/pkg/vmafx"
)

// TestStreamScorerBitExactWithCAPI pushes the tracked pair frame by frame and
// compares the pooled score with the C API by bits (stream path).
func TestStreamScorerBitExactWithCAPI(t *testing.T) {
	model := bitExactModelPath(t)
	exe := buildCAPIHarness(t)
	fx := trackedFixture(t)
	want := cAPIPooled(t, exe, fx.scoring(model, 8, PoolMethodMean))

	s, err := NewStreamScorer(StreamConfig{
		Width: fx.w, Height: fx.h, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: model,
	})
	if err != nil {
		t.Fatalf("NewStreamScorer: %v", err)
	}
	defer func() {
		if err := s.Close(); err != nil {
			t.Errorf("Close: %v", err)
		}
	}()
	if n := pushAll(t, s, fx.ref, fx.dis); n != 48 {
		t.Fatalf("pushed %d frames, want 48", n)
	}
	res, err := s.Finish(context.Background())
	if err != nil {
		t.Fatalf("Finish: %v", err)
	}
	requireSameBits(t, "StreamScorer vs C API", res.Score, want)
	record, digest, err := s.Provenance()
	if err != nil || len(record) == 0 || len(digest) != 64 {
		t.Fatalf("Provenance = (%d bytes, %q, %v), want a record and a SHA-256", len(record), digest, err)
	}
}

// TestStreamScorerBoundaryOneFrame is the boundary of the stream path: one
// pushed frame scores and equals ScoreDirect over the same single frame.
func TestStreamScorerBoundaryOneFrame(t *testing.T) {
	model := bitExactModelPath(t)
	fx := trackedFixture(t)
	ref := firstFrames(t, fx.ref, 1, 0)
	dis := firstFrames(t, fx.dis, 1, 0)
	s, err := NewStreamScorer(StreamConfig{
		Width: fx.w, Height: fx.h, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: model,
	})
	if err != nil {
		t.Fatalf("NewStreamScorer: %v", err)
	}
	defer func() { _ = s.Close() }()
	if n := pushAll(t, s, ref, dis); n != 1 {
		t.Fatalf("pushed %d frames, want 1", n)
	}
	res, err := s.Finish(context.Background())
	if err != nil {
		t.Fatalf("Finish: %v", err)
	}
	want := directPooled(t, scoringCase{ref: ref, dis: dis, model: model, w: fx.w, h: fx.h, bpc: 8, pool: PoolMethodMean})
	requireSameBits(t, "StreamScorer vs ScoreDirect", res.Score, want.VMAF)
	if res.FramesProcessed != 1 || len(res.Frames) != 1 {
		t.Fatalf("FramesProcessed=%d len(Frames)=%d, want 1/1", res.FramesProcessed, len(res.Frames))
	}
	requireSameBits(t, "frame 0 score vs pooled mean", res.Frames[0].Score, res.Score)
}

// TestFillFrameFromBytes covers the plane fill the stream path uses: exact
// length succeeds, a source one byte short or one byte long is ErrPictureRead.
func TestFillFrameFromBytes(t *testing.T) {
	desc := vmafx.FrameDesc{PixFmt: uint32(PixFmtYUV420P), Bpc: 8, W: 16, H: 8}
	size := frameBytes(16, 8, PixFmtYUV420P, 8)
	cases := []struct {
		name string
		n    int
		ok   bool
	}{
		{"exact", size, true},
		{"one_short", size - 1, false},
		{"one_long", size + 1, false},
		{"empty", 0, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			frame, err := vmafx.NewHostFrame(desc)
			if err != nil {
				t.Fatalf("NewHostFrame: %v", err)
			}
			defer frame.Close()
			err = fillFrameFromBytes(frame, bytes.Repeat([]byte{7}, tc.n))
			if tc.ok != (err == nil) {
				t.Fatalf("fillFrameFromBytes(%d bytes) = %v, ok want %v", tc.n, err, tc.ok)
			}
			if !tc.ok && !errors.Is(err, ErrPictureRead) {
				t.Fatalf("error %v is not ErrPictureRead", err)
			}
		})
	}
}

// TestEngineFillFrameReadsEveryRowOnce plants a stride-aware check: a frame
// filled from a counting pattern reads back byte for byte through PlaneBytes,
// so a fill that ignored the stride would shift the rows.
func TestEngineFillFrameReadsEveryRowOnce(t *testing.T) {
	desc := vmafx.FrameDesc{PixFmt: uint32(PixFmtYUV420P), Bpc: 8, W: 18, H: 6}
	size := frameBytes(18, 6, PixFmtYUV420P, 8)
	src := make([]byte, size)
	for i := range src {
		src[i] = byte(i)
	}
	frame, err := vmafx.NewHostFrame(desc)
	if err != nil {
		t.Fatalf("NewHostFrame: %v", err)
	}
	defer frame.Close()
	if err := fillFrameFromBytes(frame, src); err != nil {
		t.Fatalf("fillFrameFromBytes: %v", err)
	}
	planes, err := frame.Planes()
	if err != nil {
		t.Fatalf("Planes: %v", err)
	}
	var back []byte
	for p := uint32(0); p < planes.NPlanes; p++ {
		buf, err := frame.PlaneBytes(p)
		if err != nil {
			t.Fatalf("PlaneBytes(%d): %v", p, err)
		}
		for row := 0; row < int(planes.H[p]); row++ {
			off := row * int(planes.Stride[p])
			back = append(back, buf[off:off+int(planes.W[p])]...)
		}
	}
	if !bytes.Equal(back, src) {
		t.Fatal("planes read back differ from the source")
	}
}
