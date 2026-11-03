// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/libvmaf/collapse_test.go — tests of the collapse of the direct, stream
// and subprocess paths onto pkg/vmafx (#2434). This file holds the binding
// equivalence checks; collapse_stream_test.go, collapse_errors_test.go and
// collapse_subprocess_test.go hold the rest, collapse_fixtures_test.go the
// shared fixtures.
//
// The pooled score of a pair is computed four ways and compared by IEEE-754
// bits: the libvmaf C API (testdata/c_api_pooled_mean.c, compiled and run by
// the test), pkg/vmafx driven directly by the test, ScoreDirect, and the
// StreamScorer; the vmaf CLI run through Scorer.Run joins them at
// --precision=max (%.17g round-trips). Every test runs against the
// Meson-built library the CGO flags point at.
//
// Fixtures: the Netflix 576x324 pair (python/test/resource/yuv, untracked;
// VMAFX_YUV_REF and VMAFX_YUV_DIS override the paths as in the Rust binding
// tests) and the tracked 48-frame pair under testdata/. A missing fixture
// skips the test with the path named; nothing else is substituted.

//go:build cgo

package libvmaf

import (
	"context"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/internal/vmaftest"
)

// TestPooledMeanBitExactAcrossBindings is the acceptance check of #2434: the
// pooled mean of vmaf_v1.0.16_3d0h through the direct path equals the value
// through pkg/vmafx and the libvmaf C API bit for bit, on the Netflix pair and
// on the tracked 48-frame pair.
func TestPooledMeanBitExactAcrossBindings(t *testing.T) {
	model := bitExactModelPath(t)
	exe := buildCAPIHarness(t)
	fixtures := map[string]func(*testing.T) pairFixture{
		"netflix": netflixFixture, "testdata48": trackedFixture,
	}
	for name, load := range fixtures {
		t.Run(name, func(t *testing.T) {
			fx := load(t)
			want := cAPIPooled(t, exe, fx.scoring(model, 8, PoolMethodMean))
			requireSameBits(t, "pkg/vmafx vs C API", vmafxPooled(t, fx.scoring(model, 8, PoolMethodMean)), want)
			res := directPooled(t, fx.scoring(model, 8, PoolMethodMean))
			requireSameBits(t, "ScoreDirect vs C API", res.VMAF, want)
			t.Logf("%s pooled mean %x (%.17g), %d frames", fx.name, want, want, res.FrameCount)
		})
	}
}

// TestPooledMeanBitExactEveryPoolMethod covers the boundary of the pooling
// enum: every method of PoolMethod agrees between the C API and ScoreDirect.
func TestPooledMeanBitExactEveryPoolMethod(t *testing.T) {
	model := bitExactModelPath(t)
	exe := buildCAPIHarness(t)
	fx := trackedFixture(t)
	for pool := PoolMethodMin; pool <= PoolMethodPerc20; pool++ {
		t.Run(pool.String(), func(t *testing.T) {
			want := cAPIPooled(t, exe, fx.scoring(model, 8, pool))
			got := directPooled(t, fx.scoring(model, 8, pool))
			requireSameBits(t, "ScoreDirect vs C API", got.VMAF, want)
			requireSameBits(t, "pkg/vmafx vs C API",
				vmafxPooled(t, fx.scoring(model, 8, pool)), want)
		})
	}
}

// TestPooledMeanBitExactBoundaryGeometries covers a single frame and the
// two-byte sample path (10-bit samples widened from the 8-bit fixture).
func TestPooledMeanBitExactBoundaryGeometries(t *testing.T) {
	model := bitExactModelPath(t)
	exe := buildCAPIHarness(t)
	fx := trackedFixture(t)
	cases := []struct {
		name   string
		frames int
		shift  int
		bpc    int
	}{
		{"single_frame_8bit", 1, 0, 8},
		{"twelve_frames_10bit", 12, 2, 10},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			ref := firstFrames(t, fx.ref, tc.frames, tc.shift)
			dis := firstFrames(t, fx.dis, tc.frames, tc.shift)
			sc := scoringCase{ref: ref, dis: dis, model: model, w: fx.w, h: fx.h, bpc: tc.bpc, pool: PoolMethodMean}
			want := cAPIPooled(t, exe, sc)
			got := directPooled(t, sc)
			if got.FrameCount != tc.frames {
				t.Fatalf("FrameCount = %d, want %d", got.FrameCount, tc.frames)
			}
			requireSameBits(t, "ScoreDirect vs C API", got.VMAF, want)
			requireSameBits(t, "pkg/vmafx vs C API", vmafxPooled(t, sc), want)
		})
	}
}

// TestScorerRunBitExactWithCAPI joins the subprocess path: the vmaf CLI run
// by Scorer.Run at --precision=max reports the pooled mean the C API computes,
// to the bit.
func TestScorerRunBitExactWithCAPI(t *testing.T) {
	model := bitExactModelPath(t)
	exe := buildCAPIHarness(t)
	fx := trackedFixture(t)
	want := cAPIPooled(t, exe, fx.scoring(model, 8, PoolMethodMean))

	scorer, err := New(vmaftest.Binary(t), filepath.Join(RepoRoot(), "model"))
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	res, err := scorer.Run(context.Background(), Request{
		Reference: fx.ref, Distorted: fx.dis, Model: bitExactModel,
		Args: []string{
			"--precision=max", "--width", "576", "--height", "324", "--pixel_format", "420",
			"--bitdepth", "8", "--threads", "1", "--backend", "cpu",
		},
	})
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	requireSameBits(t, "Scorer.Run (vmaf CLI) vs C API", res.Score, want)
}
