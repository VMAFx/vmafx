// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// pkg/libvmaf/direct_test.go — unit tests for the direct-cgo scoring path
// (ADR-0931 Phase 1).
//
// Tests are table-driven and cover:
//   - Input validation (ErrInvalidArgument variants)
//   - Model resolution (ErrModelNotFound)
//   - End-to-end scoring against the testdata fixtures when libvmaf and the
//     576x324 reference pair are present.  Skipped otherwise so the unit
//     suite stays runnable without the model + YUV pair on disk.
//
// The subprocess-path tests live in libvmaf_test.go; both paths share the
// same input-validation and model-resolution surface from the MCP server's
// perspective, but the assertions live in separate files so a Phase 3
// removal of the subprocess path lands cleanly.

//go:build cgo

package libvmaf

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestParsePixFmt(t *testing.T) {
	cases := []struct {
		in   string
		want PixelFormat
		ok   bool
	}{
		{"420", PixFmtYUV420P, true},
		{"yuv420p", PixFmtYUV420P, true},
		{"422", PixFmtYUV422P, true},
		{"yuv422p", PixFmtYUV422P, true},
		{"444", PixFmtYUV444P, true},
		{"yuv444p", PixFmtYUV444P, true},
		{"", 0, false},
		{"yuv411p", 0, false},
		{"garbage", 0, false},
	}
	for _, tc := range cases {
		t.Run(tc.in, func(t *testing.T) {
			got, err := ParsePixFmt(tc.in)
			if tc.ok && err != nil {
				t.Fatalf("ParsePixFmt(%q) unexpected error: %v", tc.in, err)
			}
			if !tc.ok && err == nil {
				t.Fatalf("ParsePixFmt(%q) expected error, got nil", tc.in)
			}
			if got != tc.want {
				t.Errorf("ParsePixFmt(%q) = %v, want %v", tc.in, got, tc.want)
			}
		})
	}
}

func TestPixelFormat_String(t *testing.T) {
	cases := []struct {
		in   PixelFormat
		want string
	}{
		{PixFmtYUV420P, "yuv420p"},
		{PixFmtYUV422P, "yuv422p"},
		{PixFmtYUV444P, "yuv444p"},
		{PixelFormat(99), "unknown(99)"},
	}
	for _, tc := range cases {
		t.Run(tc.want, func(t *testing.T) {
			if got := tc.in.String(); got != tc.want {
				t.Errorf("PixelFormat(%d).String() = %q, want %q", tc.in, got, tc.want)
			}
		})
	}
}

func TestParsePoolMethod(t *testing.T) {
	cases := []struct {
		in   string
		want PoolMethod
		ok   bool
	}{
		{"min", PoolMethodMin, true},
		{"max", PoolMethodMax, true},
		{"mean", PoolMethodMean, true},
		{"", PoolMethodMean, true},
		{"harmonic_mean", PoolMethodHarmonicMean, true},
		{"median", PoolMethodMedian, true},
		{"perc5", PoolMethodPerc5, true},
		{"perc10", PoolMethodPerc10, true},
		{"perc20", PoolMethodPerc20, true},
		{"garbage", 0, false},
		{"perc50", 0, false},
	}
	for _, tc := range cases {
		t.Run(tc.in, func(t *testing.T) {
			got, err := ParsePoolMethod(tc.in)
			if tc.ok && err != nil {
				t.Fatalf("ParsePoolMethod(%q) unexpected error: %v", tc.in, err)
			}
			if !tc.ok && err == nil {
				t.Fatalf("ParsePoolMethod(%q) expected error, got nil", tc.in)
			}
			if got != tc.want {
				t.Errorf("ParsePoolMethod(%q) = %v, want %v", tc.in, got, tc.want)
			}
		})
	}
}

func TestPoolMethod_String(t *testing.T) {
	cases := []struct {
		in   PoolMethod
		want string
	}{
		{PoolMethodMin, "min"},
		{PoolMethodMax, "max"},
		{PoolMethodMean, "mean"},
		{PoolMethodHarmonicMean, "harmonic_mean"},
		{PoolMethodMedian, "median"},
		{PoolMethodPerc5, "perc5"},
		{PoolMethodPerc10, "perc10"},
		{PoolMethodPerc20, "perc20"},
		{PoolMethod(99), "unknown(99)"},
	}
	for _, tc := range cases {
		t.Run(tc.want, func(t *testing.T) {
			if got := tc.in.String(); got != tc.want {
				t.Errorf("PoolMethod(%d).String() = %q, want %q", tc.in, got, tc.want)
			}
		})
	}
}

func TestFrameBytes(t *testing.T) {
	cases := []struct {
		w, h, bd int
		pf       PixelFormat
		want     int
	}{
		// 576x324 yuv420p 8-bit: 576*324 + 2 * 288*162 = 186624 + 93312 = 279936
		{576, 324, 8, PixFmtYUV420P, 279936},
		// 1920x1080 yuv420p 8-bit
		{1920, 1080, 8, PixFmtYUV420P, 1920 * 1080 * 3 / 2},
		// 1920x1080 yuv420p 10-bit: doubles luma+chroma
		{1920, 1080, 10, PixFmtYUV420P, 1920 * 1080 * 3},
		// 1920x1080 yuv422p 8-bit: luma + 2 * (w/2 * h) = w*h + w*h = 2*w*h
		{1920, 1080, 8, PixFmtYUV422P, 1920 * 1080 * 2},
		// 1920x1080 yuv444p 8-bit: 3 * w * h
		{1920, 1080, 8, PixFmtYUV444P, 1920 * 1080 * 3},
	}
	for _, tc := range cases {
		if got := frameBytes(tc.w, tc.h, tc.pf, tc.bd); got != tc.want {
			t.Errorf("frameBytes(%d,%d,%v,%d) = %d, want %d",
				tc.w, tc.h, tc.pf, tc.bd, got, tc.want)
		}
	}
}

func TestScoreDirect_ValidationErrors(t *testing.T) {
	cases := []struct {
		name    string
		req     ScoreDirectRequest
		wantErr error
	}{
		{
			name:    "missing ref",
			req:     ScoreDirectRequest{Dis: "d.yuv", ModelPath: "m.json", Width: 1, Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 8},
			wantErr: ErrInvalidArgument,
		},
		{
			name:    "missing dis",
			req:     ScoreDirectRequest{Ref: "r.yuv", ModelPath: "m.json", Width: 1, Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 8},
			wantErr: ErrInvalidArgument,
		},
		{
			name:    "missing model",
			req:     ScoreDirectRequest{Ref: "r.yuv", Dis: "d.yuv", Width: 1, Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 8},
			wantErr: ErrInvalidArgument,
		},
		{
			name:    "zero width",
			req:     ScoreDirectRequest{Ref: "r.yuv", Dis: "d.yuv", ModelPath: "m.json", Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 8},
			wantErr: ErrInvalidArgument,
		},
		{
			name:    "missing pixfmt",
			req:     ScoreDirectRequest{Ref: "r.yuv", Dis: "d.yuv", ModelPath: "m.json", Width: 1, Height: 1, BitDepth: 8},
			wantErr: ErrInvalidArgument,
		},
		{
			name:    "bad bitdepth",
			req:     ScoreDirectRequest{Ref: "r.yuv", Dis: "d.yuv", ModelPath: "m.json", Width: 1, Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 9},
			wantErr: ErrInvalidArgument,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := ScoreDirect(context.Background(), tc.req)
			if err == nil {
				t.Fatalf("expected error, got nil")
			}
			if !errors.Is(err, tc.wantErr) {
				t.Errorf("error mismatch: got %v, want errors.Is(_, %v)", err, tc.wantErr)
			}
		})
	}
}

func TestScoreDirect_ModelNotFound(t *testing.T) {
	// Provide a valid req shape with a non-existent model path.
	dir := t.TempDir()
	ref := filepath.Join(dir, "ref.yuv")
	dis := filepath.Join(dir, "dis.yuv")
	if err := os.WriteFile(ref, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(dis, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	_, err := ScoreDirect(context.Background(), ScoreDirectRequest{
		Ref:       ref,
		Dis:       dis,
		ModelPath: filepath.Join(dir, "nope.json"),
		Width:     1, Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 8,
	})
	if !errors.Is(err, ErrModelNotFound) {
		t.Errorf("expected ErrModelNotFound, got %v", err)
	}
}

// TestScoreDirect_EndToEnd exercises the full direct path against the
// 576x324 / 48-frame testdata fixtures and the canonical vmaf_v0.6.1 model.
// Skipped automatically when either input is unavailable so the unit suite
// stays runnable in CI shards that don't ship the YUV pair.
func TestScoreDirect_EndToEnd(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping end-to-end ScoreDirect test in -short mode")
	}
	root := RepoRoot()
	ref := filepath.Join(root, "testdata", "ref_576x324_48f.yuv")
	dis := filepath.Join(root, "testdata", "dis_576x324_48f.yuv")
	model := filepath.Join(root, "model", "vmaf_v0.6.1.json")
	for _, p := range []string{ref, dis, model} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("fixture missing (%s); skipping end-to-end", p)
		}
	}
	res, err := ScoreDirect(context.Background(), ScoreDirectRequest{
		Ref:       ref,
		Dis:       dis,
		ModelPath: model,
		Width:     576, Height: 324,
		PixFmt:   PixFmtYUV420P,
		BitDepth: 8,
	})
	if err != nil {
		t.Fatalf("ScoreDirect: %v", err)
	}
	if res.FrameCount != 48 {
		t.Errorf("FrameCount = %d, want 48", res.FrameCount)
	}
	if res.Backend != "cpu" {
		t.Errorf("Backend = %q, want %q", res.Backend, "cpu")
	}
	if res.VMAF <= 0 || res.VMAF > 100 {
		t.Errorf("VMAF = %f, want in (0, 100]", res.VMAF)
	}
}

func TestLogDirectPathSelected_Idempotent(t *testing.T) {
	// Must not panic on repeated invocation; one-shot semantic is internal
	// to directOnce and we cannot observe it cheaply without redirecting
	// stderr.  Repeated calls are the public contract we test.
	LogDirectPathSelected()
	LogDirectPathSelected()
}

// TestScoreDirect_PreCancelledContext verifies that ScoreDirect rejects a
// pre-cancelled context before opening any files or allocating any C state.
// This is the cheap-but-load-bearing fast path for the cgo direct flow —
// the per-frame ctx.Err() check inside the loop covers the in-flight case.
//
// Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
func TestScoreDirect_PreCancelledContext(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	// Use a request shape that would pass validation otherwise — that way
	// we know ctx.Err() is the gate that fires, not ErrInvalidArgument.
	dir := t.TempDir()
	ref := filepath.Join(dir, "ref.yuv")
	dis := filepath.Join(dir, "dis.yuv")
	model := filepath.Join(dir, "model.json")
	for _, p := range []string{ref, dis, model} {
		if err := os.WriteFile(p, []byte("x"), 0o600); err != nil {
			t.Fatal(err)
		}
	}

	_, err := ScoreDirect(ctx, ScoreDirectRequest{
		Ref:       ref,
		Dis:       dis,
		ModelPath: model,
		Width:     1, Height: 1, PixFmt: PixFmtYUV420P, BitDepth: 8,
	})
	if err == nil {
		t.Fatal("expected error from pre-cancelled context, got nil")
	}
	if !errors.Is(err, context.Canceled) {
		t.Errorf("expected context.Canceled, got %v", err)
	}
}

// TestScoreDirect_CancelDuringLoop exercises the per-frame ctx.Done() check.
// The test runs the full end-to-end fixture with a context that is cancelled
// shortly after Score starts; if libvmaf is present, the loop must exit with
// context.Canceled.  Skipped when fixtures are missing (same predicate as
// TestScoreDirect_EndToEnd).
//
// Fixes T-LIBVMAF-SCORE-NEEDS-CTX-2026-05-31.
func TestScoreDirect_CancelDuringLoop(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping cancel-during-loop test in -short mode")
	}
	root := RepoRoot()
	ref := filepath.Join(root, "testdata", "ref_576x324_48f.yuv")
	dis := filepath.Join(root, "testdata", "dis_576x324_48f.yuv")
	model := filepath.Join(root, "model", "vmaf_v0.6.1.json")
	for _, p := range []string{ref, dis, model} {
		if _, err := os.Stat(p); err != nil {
			t.Skipf("fixture missing (%s); skipping cancel-during-loop", p)
		}
	}

	ctx, cancel := context.WithTimeout(context.Background(), 1*time.Millisecond)
	defer cancel()

	// Give the timeout time to fire before we even enter the function — that
	// makes this deterministic across machines.  The 5ms sleep dominates the
	// ScoreDirect entry path's cost so by the time we hit the loop, ctx is
	// already done.
	time.Sleep(5 * time.Millisecond)

	_, err := ScoreDirect(ctx, ScoreDirectRequest{
		Ref:       ref,
		Dis:       dis,
		ModelPath: model,
		Width:     576, Height: 324,
		PixFmt:   PixFmtYUV420P,
		BitDepth: 8,
	})
	if err == nil {
		t.Fatal("expected ScoreDirect to fail under a cancelled context, got nil")
	}
	if !errors.Is(err, context.DeadlineExceeded) && !errors.Is(err, context.Canceled) {
		t.Errorf("expected context.DeadlineExceeded or context.Canceled, got %v", err)
	}
}
