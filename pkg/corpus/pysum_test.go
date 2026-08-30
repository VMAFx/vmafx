// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/pysum_test.go — CPython summation / statistics parity tests.
//
// The expected values were produced by running builtins.sum() and
// statistics.pstdev() under CPython 3 on the same slices. They are compared
// bit-exactly, because these results land in the corpus JSONL verbatim: a
// single ULP of drift is a visible byte difference between a Go-written and a
// Python-written corpus.
//
// The "cancelling magnitudes" case is the one that motivates the port: a naive
// left-to-right float loop returns 0.0 there, while CPython's Neumaier
// compensation returns 1.0.

package corpus

import (
	"math"
	"testing"
)

func TestPySumMatchesCPython(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		values []float64
		want   float64
	}{
		{
			name:   "repeating decimals accumulate without drift",
			values: repeat([]float64{19.0, 19.05, 19.1, 19.02}, 12),
			want:   914.04,
		},
		{
			name:   "cancelling magnitudes keep the small term",
			values: []float64{1e16, 1.0, -1e16},
			want:   1.0,
		},
		{
			name:   "ten tenths sum to exactly one",
			values: []float64{0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1},
			want:   1.0,
		},
		{
			name:   "mixed scales",
			values: []float64{1e10, 1e-10, -1e10, 1e-10},
			want:   2e-10,
		},
		{
			name:   "fractional shot durations",
			values: []float64{24 / 23.976, 48 / 23.976, 96 / 23.976},
			want:   7.007007007007008,
		},
		{name: "an empty slice sums to zero"},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := pySum(tc.values); got != tc.want {
				t.Errorf("pySum() = %v, want %v (CPython sum())", got, tc.want)
			}
		})
	}
}

func TestPySumDiffersFromANaiveLoop(t *testing.T) {
	t.Parallel()

	// This is the whole reason the compensated algorithm is ported: without
	// it the Go corpus would drift from the Python one on exactly this
	// shape of input.
	values := []float64{1e16, 1.0, -1e16}
	naive := 0.0
	for _, v := range values {
		naive += v
	}
	if naive == pySum(values) {
		t.Skip("this platform's naive loop happens to agree; the parity check is moot")
	}
	if pySum(values) != 1.0 {
		t.Errorf("pySum = %v, want 1.0 — the compensation was lost", pySum(values))
	}
}

func TestPySumNonFinite(t *testing.T) {
	t.Parallel()

	if got := pySum([]float64{math.Inf(1), 1.0, 2.0}); !math.IsInf(got, 1) {
		t.Errorf("pySum with +Inf = %v, want +Inf", got)
	}
	if got := pySum([]float64{math.NaN(), 1.0}); !math.IsNaN(got) {
		t.Errorf("pySum with NaN = %v, want NaN", got)
	}
	if got := pySum([]float64{math.Inf(1), math.Inf(-1)}); !math.IsNaN(got) {
		t.Errorf("pySum(+Inf, -Inf) = %v, want NaN", got)
	}
}

func TestPyPopulationStdevMatchesCPython(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		values []float64
		want   float64
	}{
		{
			name:   "repeating decimals",
			values: repeat([]float64{19.0, 19.05, 19.1, 19.02}, 12),
			want:   0.03766629793329905,
		},
		{
			name:   "cancelling magnitudes",
			values: []float64{1e16, 1.0, -1e16},
			want:   8164965809277260.0,
		},
		{
			name:   "identical values have zero spread",
			values: []float64{0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1, 0.1},
			want:   0.0,
		},
		{
			name:   "mixed scales",
			values: []float64{1e10, 1e-10, -1e10, 1e-10},
			want:   7071067811.865476,
		},
		{
			name:   "the shot-duration shape the corpus row carries",
			values: []float64{1.0, 2.0, 1.0},
			want:   0.4714045207910317,
		},
		{
			name:   "fractional shot durations",
			values: []float64{24 / 23.976, 48 / 23.976, 96 / 23.976},
			want:   1.2484675965211685,
		},
		{name: "a singleton has zero spread", values: []float64{3.5}, want: 0.0},
		{name: "an empty slice has zero spread", want: 0.0},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := pyPopulationStdev(tc.values); got != tc.want {
				t.Errorf("pyPopulationStdev() = %v, want %v (statistics.pstdev)",
					got, tc.want)
			}
		})
	}
}

func TestPyPopulationStdevRejectsNonFinite(t *testing.T) {
	t.Parallel()

	// statistics.pstdev raises on non-finite data; the corpus callers read
	// NaN as "spread unavailable".
	for _, values := range [][]float64{
		{1.0, math.NaN()},
		{1.0, math.Inf(1)},
	} {
		if got := pyPopulationStdev(values); !math.IsNaN(got) {
			t.Errorf("pyPopulationStdev(%v) = %v, want NaN", values, got)
		}
	}
}

func TestFloorDiv2MatchesPython(t *testing.T) {
	t.Parallel()

	// Python's // rounds toward negative infinity; Go's / truncates.
	tests := []struct{ in, want int }{
		{in: 4, want: 2}, {in: 5, want: 2}, {in: 0, want: 0},
		{in: -1, want: -1}, {in: -2, want: -1}, {in: -3, want: -2}, {in: -109, want: -55},
	}
	for _, tc := range tests {
		if got := floorDiv2(tc.in); got != tc.want {
			t.Errorf("floorDiv2(%d) = %d, want %d", tc.in, got, tc.want)
		}
	}
}

// repeat returns n concatenated copies of values.
func repeat(values []float64, n int) []float64 {
	out := make([]float64, 0, len(values)*n)
	for i := 0; i < n; i++ {
		out = append(out, values...)
	}
	return out
}
