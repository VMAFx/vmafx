// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/libvmaf/stream_test.go — tests for the in-memory streaming scoring path
// (ADR-0933 Phase 2).  Validation-only cases run without libvmaf fixtures; the
// end-to-end case is skip-guarded on the 576x324 / 48-frame testdata fixtures
// plus the canonical vmaf_v0.6.1 model, mirroring TestScoreDirect_EndToEnd.

//go:build cgo

package libvmaf

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

// TestNewStreamScorer_ValidationErrors exercises the constructor's up-front
// validation without needing libvmaf to load a real model.
func TestNewStreamScorer_ValidationErrors(t *testing.T) {
	cases := []struct {
		name string
		cfg  StreamConfig
	}{
		{"zero width", StreamConfig{Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: "m.json"}},
		{"zero height", StreamConfig{Width: 1, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: "m.json"}},
		{"missing pixfmt", StreamConfig{Width: 1, Height: 1, BitDepth: 8, ModelPath: "m.json"}},
		{"bad bitdepth", StreamConfig{Width: 1, Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 9, ModelPath: "m.json"}},
		{"missing model", StreamConfig{Width: 1, Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 8}},
	}
	for _, tc := range cases {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			s, err := NewStreamScorer(tc.cfg)
			if err == nil {
				s.Close()
				t.Fatalf("expected error for %q, got nil", tc.name)
			}
			if !errors.Is(err, ErrInvalidArgument) {
				t.Errorf("expected ErrInvalidArgument, got %v", err)
			}
		})
	}
}

// TestNewStreamScorer_ModelNotFound verifies the missing-model path returns
// ErrModelNotFound (wrapping os.ErrNotExist) without needing a real model.
func TestNewStreamScorer_ModelNotFound(t *testing.T) {
	_, err := NewStreamScorer(StreamConfig{
		Width: 1, Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 8,
		ModelPath: filepath.Join(t.TempDir(), "nope.json"),
	})
	if err == nil {
		t.Fatal("expected error for missing model, got nil")
	}
	if !errors.Is(err, ErrModelNotFound) {
		t.Errorf("expected ErrModelNotFound, got %v", err)
	}
}

// streamFixture resolves the end-to-end fixtures or skips the test.
func streamFixture(t *testing.T) (ref, dis, model string) {
	t.Helper()
	if testing.Short() {
		t.Skip("skipping end-to-end StreamScorer test in -short mode")
	}
	root := RepoRoot()
	ref = filepath.Join(root, "testdata", "ref_576x324_48f.yuv")
	dis = filepath.Join(root, "testdata", "dis_576x324_48f.yuv")
	model = filepath.Join(root, "model", "vmaf_v0.6.1.json")
	for _, p := range []string{ref, dis, model} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("fixture missing (%s); skipping end-to-end", p)
		}
	}
	return ref, dis, model
}

// pushAll reads the YUV fixtures and pushes every frame into the scorer.
func pushAll(t *testing.T, s *StreamScorer, ref, dis string) int {
	t.Helper()
	rb, err := os.ReadFile(ref)
	if err != nil {
		t.Fatalf("read ref: %v", err)
	}
	db, err := os.ReadFile(dis)
	if err != nil {
		t.Fatalf("read dis: %v", err)
	}
	fs := s.FrameSize()
	if fs <= 0 || len(rb)%fs != 0 {
		t.Fatalf("frame size %d does not divide ref length %d", fs, len(rb))
	}
	n := len(rb) / fs
	for i := 0; i < n; i++ {
		lo, hi := i*fs, (i+1)*fs
		if err := s.PushFrame(i, rb[lo:hi], db[lo:hi]); err != nil {
			t.Fatalf("PushFrame %d: %v", i, err)
		}
	}
	return n
}

// TestStreamScorer_EndToEnd pushes all 48 frames in memory and asserts the
// per-frame and pooled scores are sane, and that the pooled VMAF matches the
// independent ScoreDirect path bit-for-bit (both drive the same C engine).
func TestStreamScorer_EndToEnd(t *testing.T) {
	ref, dis, model := streamFixture(t)

	s, err := NewStreamScorer(StreamConfig{
		Width: 576, Height: 324, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: model,
	})
	if err != nil {
		t.Fatalf("NewStreamScorer: %v", err)
	}
	defer s.Close()

	n := pushAll(t, s, ref, dis)
	if n != 48 {
		t.Fatalf("pushed %d frames, want 48", n)
	}

	res, err := s.Finish(context.Background())
	if err != nil {
		t.Fatalf("Finish: %v", err)
	}
	if res.FramesProcessed != 48 || len(res.Frames) != 48 {
		t.Fatalf("FramesProcessed=%d len(Frames)=%d, want 48/48", res.FramesProcessed, len(res.Frames))
	}
	for _, fr := range res.Frames {
		if fr.Score < 0 || fr.Score > 100 {
			t.Errorf("frame %d score %f out of (0,100]", fr.Index, fr.Score)
		}
	}
	if len(res.Frames[0].Features) == 0 {
		t.Error("expected non-empty per-frame feature map")
	}
	if res.Score <= 0 || res.Score > 100 {
		t.Errorf("pooled VMAF %f out of (0,100]", res.Score)
	}
	if len(res.Features) == 0 {
		t.Error("expected non-empty pooled feature map")
	}

	// Cross-check against ScoreDirect on the same inputs — the streaming
	// in-memory path and the file-reading direct path must agree exactly.
	dr, err := ScoreDirect(context.Background(), ScoreDirectRequest{
		Ref: ref, Dis: dis, ModelPath: model,
		Width: 576, Height: 324, PixFmt: PixFmtYUV420P, BitDepth: 8,
	})
	if err != nil {
		t.Fatalf("ScoreDirect: %v", err)
	}
	if res.Score != dr.VMAF {
		t.Errorf("pooled mismatch: stream=%.10f direct=%.10f", res.Score, dr.VMAF)
	}
}

// TestStreamScorer_OutOfOrderFrame verifies a non-monotonic frame index is
// rejected with ErrInvalidArgument.
func TestStreamScorer_OutOfOrderFrame(t *testing.T) {
	ref, dis, model := streamFixture(t)

	s, err := NewStreamScorer(StreamConfig{
		Width: 576, Height: 324, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: model,
	})
	if err != nil {
		t.Fatalf("NewStreamScorer: %v", err)
	}
	defer s.Close()

	rb, _ := os.ReadFile(ref)
	db, _ := os.ReadFile(dis)
	fs := s.FrameSize()
	// Push frame 0 OK, then push at index 2 (gap) — must fail.
	if err := s.PushFrame(0, rb[:fs], db[:fs]); err != nil {
		t.Fatalf("PushFrame 0: %v", err)
	}
	err = s.PushFrame(2, rb[fs:2*fs], db[fs:2*fs])
	if err == nil {
		t.Fatal("expected error for out-of-order frame index, got nil")
	}
	if !errors.Is(err, ErrInvalidArgument) {
		t.Errorf("expected ErrInvalidArgument, got %v", err)
	}
}

// TestStreamScorer_FrameSizeMismatch verifies a wrong-length frame buffer is
// rejected before it can corrupt the picture planes.
func TestStreamScorer_FrameSizeMismatch(t *testing.T) {
	_, _, model := streamFixture(t)

	s, err := NewStreamScorer(StreamConfig{
		Width: 576, Height: 324, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: model,
	})
	if err != nil {
		t.Fatalf("NewStreamScorer: %v", err)
	}
	defer s.Close()

	short := make([]byte, s.FrameSize()-1)
	err = s.PushFrame(0, short, short)
	if err == nil {
		t.Fatal("expected error for short frame, got nil")
	}
	if !errors.Is(err, ErrInvalidArgument) {
		t.Errorf("expected ErrInvalidArgument, got %v", err)
	}
}

// TestStreamScorer_FinishWithoutFrames verifies pooling an empty stream fails.
func TestStreamScorer_FinishWithoutFrames(t *testing.T) {
	_, _, model := streamFixture(t)

	s, err := NewStreamScorer(StreamConfig{
		Width: 576, Height: 324, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: model,
	})
	if err != nil {
		t.Fatalf("NewStreamScorer: %v", err)
	}
	defer s.Close()

	if _, err := s.Finish(context.Background()); !errors.Is(err, ErrPictureRead) {
		t.Errorf("expected ErrPictureRead for empty stream, got %v", err)
	}
}

// TestStreamScorer_CloseIdempotent verifies Close is safe to call twice and
// that PushFrame after Close is rejected.
func TestStreamScorer_CloseIdempotent(t *testing.T) {
	_, _, model := streamFixture(t)

	s, err := NewStreamScorer(StreamConfig{
		Width: 576, Height: 324, PixFmt: PixFmtYUV420P, BitDepth: 8, ModelPath: model,
	})
	if err != nil {
		t.Fatalf("NewStreamScorer: %v", err)
	}
	s.Close()
	s.Close() // must not panic

	buf := make([]byte, s.FrameSize())
	if err := s.PushFrame(0, buf, buf); !errors.Is(err, ErrInvalidArgument) {
		t.Errorf("expected ErrInvalidArgument after Close, got %v", err)
	}
}
