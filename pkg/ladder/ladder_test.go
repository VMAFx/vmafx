// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package ladder_test

import (
	"fmt"
	"math"
	"testing"

	"github.com/VMAFx/vmafx/pkg/ladder"
)

// syntheticSampler returns a SamplerFn that maps (width, height, targetVMAF)
// to a deterministic Point without running any encoder.
//
// Model (mirrors a plausible rate-quality curve):
//   - bitrate = baseKbps * (width * height / (320 * 240)) * scale(targetVMAF)
//   - vmaf    = targetVMAF + small noise (always meets target)
//   - crf     = 28 (constant for testing)
//
// where scale(t) = 1 + (t-75)/100 — higher targets need more bits.
func syntheticSampler(baseKbps float64) ladder.SamplerFn {
	return func(src, encoder string, width, height int, targetVMAF float64) (ladder.Point, error) {
		if width <= 0 || height <= 0 {
			return ladder.Point{}, fmt.Errorf("invalid resolution %dx%d", width, height)
		}
		scale := 1.0 + (targetVMAF-75.0)/100.0
		bitrate := baseKbps * float64(width*height) / float64(320*240) * scale
		vmaf := targetVMAF + 0.5 // bisect lands slightly above target
		return ladder.Point{
			Width:       width,
			Height:      height,
			BitratekBps: bitrate,
			VMAF:        vmaf,
			CRF:         28,
			TargetVMAF:  targetVMAF,
			OK:          true,
		}, nil
	}
}

// failSampler returns a SamplerFn that always returns an error.
func failSampler() ladder.SamplerFn {
	return func(src, encoder string, width, height int, targetVMAF float64) (ladder.Point, error) {
		return ladder.Point{}, fmt.Errorf("synthetic sampler failure for %dx%d @ %.1f", width, height, targetVMAF)
	}
}

// TestBuild_singleResolutionSingleTarget verifies that Build with one
// resolution and one VMAF target returns a ladder with one cloud point, one
// hull point, and one rendition.
func TestBuild_singleResolutionSingleTarget(t *testing.T) {
	t.Parallel()

	result, err := ladder.Build("src.mp4", "libx264", ladder.Params{
		Resolutions: [][2]int{{1280, 720}},
		TargetVMAFs: []float64{85.0},
		Sampler:     syntheticSampler(1000.0),
		MaxRungs:    4,
	})
	if err != nil {
		t.Fatalf("Build: %v", err)
	}
	if len(result.Cloud) != 1 {
		t.Errorf("Cloud len = %d, want 1", len(result.Cloud))
	}
	if !result.Cloud[0].OK {
		t.Errorf("Cloud[0].OK = false, want true")
	}
	if len(result.Hull) != 1 {
		t.Errorf("Hull len = %d, want 1", len(result.Hull))
	}
	if len(result.Renditions) != 1 {
		t.Errorf("Renditions len = %d, want 1", len(result.Renditions))
	}
	r := result.Renditions[0]
	if r.Width != 1280 || r.Height != 720 {
		t.Errorf("Rendition resolution = %dx%d, want 1280x720", r.Width, r.Height)
	}
	if r.BitratekBps <= 0 {
		t.Errorf("Rendition bitrate = %.1f, want > 0", r.BitratekBps)
	}
}

// TestBuild_multiResolutionMultiTarget verifies that Build with a 3×3
// (resolution × target) grid produces a cloud of 9 points, a non-empty hull,
// and renditions sorted by ascending bitrate.
func TestBuild_multiResolutionMultiTarget(t *testing.T) {
	t.Parallel()

	resolutions := [][2]int{{320, 240}, {640, 480}, {1280, 720}}
	targets := []float64{75.0, 85.0, 95.0}

	result, err := ladder.Build("src.mp4", "libx264", ladder.Params{
		Resolutions: resolutions,
		TargetVMAFs: targets,
		Sampler:     syntheticSampler(500.0),
		MaxRungs:    ladder.DefaultMaxRungs,
	})
	if err != nil {
		t.Fatalf("Build: %v", err)
	}

	wantCloud := len(resolutions) * len(targets)
	if len(result.Cloud) != wantCloud {
		t.Errorf("Cloud len = %d, want %d", len(result.Cloud), wantCloud)
	}

	if len(result.Hull) == 0 {
		t.Error("Hull is empty, want at least 1 point")
	}

	// All hull points must be successful.
	for i, p := range result.Hull {
		if !p.OK {
			t.Errorf("Hull[%d].OK = false", i)
		}
	}

	// Renditions must be sorted by ascending bitrate.
	for i := 1; i < len(result.Renditions); i++ {
		if result.Renditions[i].BitratekBps < result.Renditions[i-1].BitratekBps {
			t.Errorf("Renditions not sorted: [%d]=%.1f < [%d]=%.1f",
				i, result.Renditions[i].BitratekBps,
				i-1, result.Renditions[i-1].BitratekBps)
		}
	}

	// Renditions must be capped at MaxRungs.
	if len(result.Renditions) > ladder.DefaultMaxRungs {
		t.Errorf("Renditions len = %d, want <= %d", len(result.Renditions), ladder.DefaultMaxRungs)
	}

	t.Logf("cloud=%d hull=%d renditions=%d", len(result.Cloud), len(result.Hull), len(result.Renditions))
}

// TestBuild_allSamplersFail verifies that Build with a failing sampler returns
// a result with all cloud points marked as failed, an empty hull, and no
// renditions.
func TestBuild_allSamplersFail(t *testing.T) {
	t.Parallel()

	result, err := ladder.Build("src.mp4", "libx264", ladder.Params{
		Resolutions: [][2]int{{640, 480}, {1280, 720}},
		TargetVMAFs: []float64{80.0, 90.0},
		Sampler:     failSampler(),
		MaxRungs:    4,
	})
	if err != nil {
		// Build itself should not error even when all samplers fail.
		t.Fatalf("Build returned error for all-fail sampler: %v", err)
	}
	for i, p := range result.Cloud {
		if p.OK {
			t.Errorf("Cloud[%d].OK = true, want false for fail sampler", i)
		}
	}
	if len(result.Hull) != 0 {
		t.Errorf("Hull len = %d, want 0 (all points failed)", len(result.Hull))
	}
	if len(result.Renditions) != 0 {
		t.Errorf("Renditions len = %d, want 0 (no hull)", len(result.Renditions))
	}
}

// TestBuild_invalidParams verifies that Build returns errors for invalid
// parameter combinations without panicking.
func TestBuild_invalidParams(t *testing.T) {
	t.Parallel()

	validSampler := syntheticSampler(500.0)

	cases := []struct {
		name   string
		params ladder.Params
	}{
		{
			name: "nil sampler",
			params: ladder.Params{
				Resolutions: [][2]int{{640, 480}},
				TargetVMAFs: []float64{85.0},
				Sampler:     nil,
			},
		},
		{
			name: "empty resolutions",
			params: ladder.Params{
				Resolutions: [][2]int{},
				TargetVMAFs: []float64{85.0},
				Sampler:     validSampler,
			},
		},
		{
			name: "empty targets",
			params: ladder.Params{
				Resolutions: [][2]int{{640, 480}},
				TargetVMAFs: []float64{},
				Sampler:     validSampler,
			},
		},
		{
			name: "target out of range",
			params: ladder.Params{
				Resolutions: [][2]int{{640, 480}},
				TargetVMAFs: []float64{0.0},
				Sampler:     validSampler,
			},
		},
		{
			name: "target over 100",
			params: ladder.Params{
				Resolutions: [][2]int{{640, 480}},
				TargetVMAFs: []float64{101.0},
				Sampler:     validSampler,
			},
		},
	}

	for _, tc := range cases {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			_, err := ladder.Build("src.mp4", "libx264", tc.params)
			if err == nil {
				t.Errorf("expected error for %q, got nil", tc.name)
			}
		})
	}
}

// TestBitrateGap_minGapFiltersClose verifies that closely-spaced hull points
// do not produce renditions within the min bitrate gap.
func TestBitrateGap_minGapFiltersClose(t *testing.T) {
	t.Parallel()

	// Use a tight gap (500 kbps) and a sampler that produces three resolutions
	// at one target, so the resulting bitrates are closely spaced.
	result, err := ladder.Build("src.mp4", "libx264", ladder.Params{
		Resolutions: [][2]int{
			{320, 240}, {352, 264}, {384, 288}, // very similar pixel counts → close bitrates
		},
		TargetVMAFs:       []float64{85.0},
		Sampler:           syntheticSampler(200.0),
		MaxRungs:          6,
		MinBitrateGapKbps: 500.0, // large gap to force filtering
	})
	if err != nil {
		t.Fatalf("Build: %v", err)
	}

	// With a 500 kbps gap and similar resolutions, most renditions should be
	// pruned. Verify adjacents satisfy the gap.
	for i := 1; i < len(result.Renditions); i++ {
		gap := result.Renditions[i].BitratekBps - result.Renditions[i-1].BitratekBps
		if gap < 500.0-1e-6 { // 1e-6 tolerance for float arithmetic
			t.Errorf("adjacent renditions [%d,%d] bitrate gap = %.1f kbps, want >= 500",
				i-1, i, gap)
		}
	}
}

// TestBuild_hullMonotonicity verifies that the convex hull is strictly
// increasing in both bitrate and VMAF.  (No point on the hull should have a
// lower VMAF than a lower-bitrate neighbour.)
func TestBuild_hullMonotonicity(t *testing.T) {
	t.Parallel()

	result, err := ladder.Build("src.mp4", "libx264", ladder.Params{
		Resolutions: [][2]int{
			{320, 240}, {640, 480}, {1280, 720}, {1920, 1080},
		},
		TargetVMAFs: []float64{75.0, 80.0, 85.0, 90.0, 95.0},
		Sampler:     syntheticSampler(800.0),
		MaxRungs:    ladder.DefaultMaxRungs,
	})
	if err != nil {
		t.Fatalf("Build: %v", err)
	}

	// Hull should be monotone: increasing bitrate AND non-decreasing VMAF.
	for i := 1; i < len(result.Hull); i++ {
		if result.Hull[i].BitratekBps <= result.Hull[i-1].BitratekBps {
			t.Errorf("hull[%d].bitrate=%.1f <= hull[%d].bitrate=%.1f (should be strictly increasing)",
				i, result.Hull[i].BitratekBps, i-1, result.Hull[i-1].BitratekBps)
		}
		if result.Hull[i].VMAF < result.Hull[i-1].VMAF-1e-9 {
			t.Errorf("hull[%d].vmaf=%.2f < hull[%d].vmaf=%.2f (should be non-decreasing)",
				i, result.Hull[i].VMAF, i-1, result.Hull[i-1].VMAF)
		}
	}
}

// TestBuild_renditionsSubsetOfHull verifies that every emitted rendition is a
// point that was present in the hull (same bitrate and vmaf, within float
// tolerance).
func TestBuild_renditionsSubsetOfHull(t *testing.T) {
	t.Parallel()

	result, err := ladder.Build("src.mp4", "libx264", ladder.Params{
		Resolutions: [][2]int{{320, 240}, {640, 480}, {1280, 720}, {1920, 1080}},
		TargetVMAFs: []float64{75.0, 85.0, 95.0},
		Sampler:     syntheticSampler(1000.0),
		MaxRungs:    4,
	})
	if err != nil {
		t.Fatalf("Build: %v", err)
	}

	for _, rend := range result.Renditions {
		found := false
		for _, hp := range result.Hull {
			if math.Abs(hp.BitratekBps-rend.BitratekBps) < 1e-6 &&
				math.Abs(hp.VMAF-rend.VMAF) < 1e-6 {
				found = true
				break
			}
		}
		if !found {
			t.Errorf("rendition (%.1f kbps, vmaf %.2f) not found in hull",
				rend.BitratekBps, rend.VMAF)
		}
	}
}

// TestLadderResult_IterCloudAndHull verifies the iter.Seq adapters walk
// every Cloud / Hull point in order with the same payload as ranging over
// the field directly.
func TestLadderResult_IterCloudAndHull(t *testing.T) {
	t.Parallel()

	result, err := ladder.Build("src.mp4", "libx264", ladder.Params{
		Resolutions: [][2]int{{640, 480}, {1280, 720}, {1920, 1080}},
		TargetVMAFs: []float64{75.0, 85.0, 95.0},
		Sampler:     syntheticSampler(1000.0),
	})
	if err != nil {
		t.Fatalf("Build: %v", err)
	}

	var walkedCloud []ladder.Point
	for p := range result.IterCloud() {
		walkedCloud = append(walkedCloud, p)
	}
	if len(walkedCloud) != len(result.Cloud) {
		t.Fatalf("IterCloud yielded %d, want %d", len(walkedCloud), len(result.Cloud))
	}
	for i, p := range walkedCloud {
		if p != result.Cloud[i] {
			t.Errorf("IterCloud[%d] = %+v, want %+v", i, p, result.Cloud[i])
		}
	}

	var walkedHull []ladder.Point
	for p := range result.IterHull() {
		walkedHull = append(walkedHull, p)
	}
	if len(walkedHull) != len(result.Hull) {
		t.Fatalf("IterHull yielded %d, want %d", len(walkedHull), len(result.Hull))
	}
	for i, p := range walkedHull {
		if p != result.Hull[i] {
			t.Errorf("IterHull[%d] = %+v, want %+v", i, p, result.Hull[i])
		}
	}
}

// TestLadderResult_IterEarlyBreak verifies both iter.Seq adapters honour
// caller break — required by the iter.Seq contract.
func TestLadderResult_IterEarlyBreak(t *testing.T) {
	t.Parallel()

	result, err := ladder.Build("src.mp4", "libx264", ladder.Params{
		Resolutions: [][2]int{{640, 480}, {1280, 720}, {1920, 1080}},
		TargetVMAFs: []float64{75.0, 85.0, 95.0},
		Sampler:     syntheticSampler(1000.0),
	})
	if err != nil {
		t.Fatalf("Build: %v", err)
	}

	var n int
	for range result.IterCloud() {
		n++
		if n == 2 {
			break
		}
	}
	if n != 2 {
		t.Errorf("IterCloud early-break: walked %d, want 2", n)
	}

	n = 0
	for range result.IterHull() {
		n++
		break
	}
	if n != 1 {
		t.Errorf("IterHull early-break: walked %d, want 1", n)
	}
}
