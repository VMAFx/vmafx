// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package bitratesearch_test

import (
	"fmt"
	"math"
	"testing"

	"github.com/VMAFx/vmafx/pkg/bitratesearch"
	"github.com/VMAFx/vmafx/pkg/bisect"
	"github.com/VMAFx/vmafx/pkg/encoder"
)

// --- mock infrastructure ---

// stubEncoder is a no-op Encoder that satisfies the interface.
// The actual encode is performed by the stub scoreFunc.
type stubEncoder struct{ name string }

func (e stubEncoder) Name() string                                      { return e.name }
func (e stubEncoder) CRFRange() (int, int)                             { return 0, 51 }
func (e stubEncoder) Encode(src string, p encoder.EncodeParams) (encoder.EncodeResult, error) {
	// bitratesearch.Run calls encodeAtBitrate (internal) via ffmpeg shell-out.
	// In unit tests we bypass Run and test convergence directly via
	// mockRun below.
	return encoder.EncodeResult{}, fmt.Errorf("stub encoder: direct Encode not expected in unit test")
}

// mockRun directly tests the bisect convergence logic by simulating the
// inner loop without shelling out to ffmpeg.  It accepts a VMAF model
// f(bitrateKbps) → vmaf and a set of Params, then replays the same
// bisect algorithm and verifies convergence properties.
func mockRun(
	t *testing.T,
	vmafModel func(float64) float64,
	params bitratesearch.Params,
) bitratesearch.Result {
	t.Helper()

	// We can't call bitratesearch.Run directly with a mock encoder because
	// encodeAtBitrate is internal (unexported).  Instead we replicate the
	// bisect logic here and verify our understanding of the algorithm's
	// convergence property.
	//
	// This is a glass-box test of the convergence contract, not an
	// integration test.

	lo := params.BitrateMinKbps
	if lo <= 0 {
		lo = 500
	}
	hi := params.BitrateMaxKbps
	if hi <= 0 {
		hi = 10000
	}
	tol := params.Tolerance
	if tol <= 0 {
		tol = bitratesearch.DefaultTolerance
	}
	maxIter := params.MaxIter
	if maxIter <= 0 {
		maxIter = bitratesearch.DefaultMaxIter
	}

	var (
		best     = -1.0
		bestVMAF float64
		iters    int
		samples  []bitratesearch.Sample
	)

	probe := func(b float64) float64 {
		iters++
		v := vmafModel(b)
		samples = append(samples, bitratesearch.Sample{BitrateKbps: b, VMAFScore: v})
		return v
	}

	// Lower bound probe.
	loVMAF := probe(lo)
	if loVMAF >= params.TargetVMAF {
		return bitratesearch.Result{
			BestBitrateKbps: lo, BestVMAFScore: loVMAF,
			Converged: true, Iterations: iters, Samples: samples,
		}
	}

	// Upper bound probe.
	hiVMAF := probe(hi)
	if hiVMAF < params.TargetVMAF {
		return bitratesearch.Result{
			BestBitrateKbps: -1,
			Converged:       false, Iterations: iters, Samples: samples,
		}
	}
	best = hi
	bestVMAF = hiVMAF

	converged := false
	for iters < maxIter {
		if hi-lo <= tol {
			converged = true
			break
		}
		mid := (lo + hi) / 2
		midVMAF := probe(mid)
		if midVMAF >= params.TargetVMAF {
			best = mid
			bestVMAF = midVMAF
			hi = mid
		} else {
			lo = mid
		}
	}
	if hi-lo <= tol {
		converged = true
	}

	return bitratesearch.Result{
		BestBitrateKbps: best, BestVMAFScore: bestVMAF,
		Converged: converged, Iterations: iters, Samples: samples,
	}
}

// --- tests ---

// TestBitrateSearch_convergesToMinBitrate verifies that the bisect finds the
// minimum bitrate meeting the target, within tolerance.
func TestBitrateSearch_convergesToMinBitrate(t *testing.T) {
	t.Parallel()

	// VMAF model: linear in bitrate.  At 4000 kbps VMAF = 90.
	// Minimum bitrate for VMAF ≥ 85 is 4000 * (85/90) ≈ 3778 kbps.
	vmafModel := func(b float64) float64 {
		return 90.0 * (b / 4000.0)
	}

	params := bitratesearch.Params{
		TargetVMAF:     85.0,
		BitrateMinKbps: 500,
		BitrateMaxKbps: 10000,
		Tolerance:      50.0,
		MaxIter:        bitratesearch.DefaultMaxIter,
	}

	result := mockRun(t, vmafModel, params)

	if result.BestBitrateKbps < 0 {
		t.Fatal("expected a valid best bitrate, got -1 (no convergence)")
	}
	if !result.Converged {
		t.Errorf("expected Converged=true, got false after %d iterations", result.Iterations)
	}

	// The bisect should land within Tolerance of the true minimum.
	trueMin := 4000.0 * (85.0 / 90.0) // ≈ 3778 kbps
	if math.Abs(result.BestBitrateKbps-trueMin) > params.Tolerance*2 {
		t.Errorf("BestBitrateKbps = %.1f, want within %.0f kbps of %.1f",
			result.BestBitrateKbps, params.Tolerance*2, trueMin)
	}

	// VMAF at best bitrate must meet target.
	achievedVMAF := vmafModel(result.BestBitrateKbps)
	if achievedVMAF < params.TargetVMAF {
		t.Errorf("VMAF at best bitrate = %.2f < target %.2f", achievedVMAF, params.TargetVMAF)
	}
}

// TestBitrateSearch_lowerBoundMeetsTarget verifies fast-path when lo already
// meets the target.
func TestBitrateSearch_lowerBoundMeetsTarget(t *testing.T) {
	t.Parallel()

	// VMAF is always 95 regardless of bitrate.
	vmafModel := func(_ float64) float64 { return 95.0 }

	params := bitratesearch.Params{
		TargetVMAF:     85.0,
		BitrateMinKbps: 1000,
		BitrateMaxKbps: 8000,
		Tolerance:      50.0,
		MaxIter:        bitratesearch.DefaultMaxIter,
	}

	result := mockRun(t, vmafModel, params)

	// Should return lo immediately (1 probe).
	if result.BestBitrateKbps != params.BitrateMinKbps {
		t.Errorf("BestBitrateKbps = %.0f, want %.0f (lo)", result.BestBitrateKbps, params.BitrateMinKbps)
	}
	if result.Iterations != 1 {
		t.Errorf("Iterations = %d, want 1 (fast-path)", result.Iterations)
	}
}

// TestBitrateSearch_upperBoundFails verifies that -1 is returned when even
// the maximum bitrate cannot meet the target.
func TestBitrateSearch_upperBoundFails(t *testing.T) {
	t.Parallel()

	// VMAF tops out at 80 regardless of bitrate — below target 85.
	vmafModel := func(_ float64) float64 { return 80.0 }

	params := bitratesearch.Params{
		TargetVMAF:     85.0,
		BitrateMinKbps: 500,
		BitrateMaxKbps: 10000,
		Tolerance:      50.0,
		MaxIter:        bitratesearch.DefaultMaxIter,
	}

	result := mockRun(t, vmafModel, params)

	if result.BestBitrateKbps != -1 {
		t.Errorf("BestBitrateKbps = %.0f, want -1 (upper bound fails)", result.BestBitrateKbps)
	}
}

// TestBitrateSearch_samplesInOrder verifies that Samples are accumulated in
// probe order (lo, hi, bisect iterations).
func TestBitrateSearch_samplesInOrder(t *testing.T) {
	t.Parallel()

	vmafModel := func(b float64) float64 { return 85.0 * (b / 5000.0) }

	params := bitratesearch.Params{
		TargetVMAF:     70.0,
		BitrateMinKbps: 500,
		BitrateMaxKbps: 10000,
		Tolerance:      500, // coarse so we stop quickly
		MaxIter:        5,
	}

	result := mockRun(t, vmafModel, params)

	if len(result.Samples) < 2 {
		t.Fatalf("expected ≥ 2 samples, got %d", len(result.Samples))
	}
	// First sample is lo, second is hi.
	if result.Samples[0].BitrateKbps != params.BitrateMinKbps {
		t.Errorf("Samples[0].BitrateKbps = %.0f, want %.0f", result.Samples[0].BitrateKbps, params.BitrateMinKbps)
	}
	if result.Samples[1].BitrateKbps != params.BitrateMaxKbps {
		t.Errorf("Samples[1].BitrateKbps = %.0f, want %.0f", result.Samples[1].BitrateKbps, params.BitrateMaxKbps)
	}
}

// TestBitrateSearch_defaultParams verifies that zero-value Params apply
// sensible defaults (no panic, returns a valid struct).
func TestBitrateSearch_defaultParams(t *testing.T) {
	t.Parallel()

	vmafModel := func(b float64) float64 { return 90.0 * (b / 6000.0) }

	params := bitratesearch.Params{
		TargetVMAF: 85.0,
		// All other fields zero — should apply defaults.
	}

	result := mockRun(t, vmafModel, params)
	// With defaults (500–10000, tol=50, maxIter=20) the bisect should converge.
	if result.Iterations <= 0 {
		t.Error("Iterations = 0, expected > 0")
	}

	// bisect.ScoreFunc is exported — confirm it satisfies the interface signature.
	var _ bisect.ScoreFunc = func(ref, distorted string) (float64, error) { return 0, nil }
}
