// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// Golden values in this file were generated with the same scipy the
// Python MCP server uses (scipy 1.18.1 / numpy 2.5.2) via
// scipy.stats.pearsonr / spearmanr / rankdata and
// np.sqrt(((x-y)**2).mean()) on float64 inputs. Regenerate rather than
// hand-edit if a case ever needs to change.

package modeleval

import (
	"errors"
	"math"
	"testing"
)

// tol is the agreement required against scipy. scipy's spearmanr returns
// 0.99999999999999989 for a perfectly monotonic pair where the centred
// Pearson form here returns exactly 1, so exact equality is the wrong
// gate; 1e-12 is far tighter than any meaningful drift.
const tol = 1e-12

func TestPearsonSpearmanRMSEAgainstScipy(t *testing.T) {
	cases := []struct {
		name              string
		x, y              []float64
		plcc, srocc, rmse float64
	}{
		{
			name: "perfectly linear", x: []float64{1, 2, 3, 4, 5}, y: []float64{2, 4, 6, 8, 10},
			plcc: 1, srocc: 0.99999999999999989, rmse: 3.3166247903553998,
		},
		{
			name: "perfectly inverse", x: []float64{1, 2, 3, 4, 5}, y: []float64{5, 4, 3, 2, 1},
			plcc: -1, srocc: -0.99999999999999989, rmse: 2.8284271247461903,
		},
		{
			name: "ties in x only", x: []float64{1, 2, 2, 3, 4, 4}, y: []float64{1, 2, 3, 4, 5, 6},
			plcc: 0.97100831245522445, srocc: 0.97100831245522456, rmse: 1.0801234497346435,
		},
		{
			name: "ties in both", x: []float64{1, 1, 2, 2, 3, 3}, y: []float64{2, 2, 4, 4, 6, 6},
			plcc: 1, srocc: 1, rmse: 2.1602468994692869,
		},
		{
			// Monotonic but not linear: SROCC saturates where PLCC does not.
			name: "nonlinear monotonic", x: []float64{1, 2, 3, 4, 5}, y: []float64{1, 4, 9, 16, 25},
			plcc: 0.98110491025159297, srocc: 0.99999999999999989, rmse: 10.807404868885037,
		},
		{
			name: "noisy", x: []float64{3.0, 1.5, 4.25, 1.0, 5.5}, y: []float64{2.5, 2.0, 4.0, 1.25, 5.0},
			plcc: 0.98526979177467244, srocc: 0.99999999999999989, rmse: 0.41833001326703778,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			plcc, err := Pearson(tc.x, tc.y)
			if err != nil {
				t.Fatalf("Pearson: %v", err)
			}
			if math.Abs(plcc-tc.plcc) > tol {
				t.Errorf("Pearson = %.17g, want %.17g (delta %g)", plcc, tc.plcc, math.Abs(plcc-tc.plcc))
			}
			srocc, err := Spearman(tc.x, tc.y)
			if err != nil {
				t.Fatalf("Spearman: %v", err)
			}
			if math.Abs(srocc-tc.srocc) > tol {
				t.Errorf("Spearman = %.17g, want %.17g (delta %g)", srocc, tc.srocc, math.Abs(srocc-tc.srocc))
			}
			rmse, err := RMSE(tc.x, tc.y)
			if err != nil {
				t.Fatalf("RMSE: %v", err)
			}
			if math.Abs(rmse-tc.rmse) > tol {
				t.Errorf("RMSE = %.17g, want %.17g (delta %g)", rmse, tc.rmse, math.Abs(rmse-tc.rmse))
			}
		})
	}
}

// TestRankAverage pins the tie-averaging rule against scipy's
// rankdata(method="average"), which spearmanr depends on.
func TestRankAverage(t *testing.T) {
	cases := []struct {
		name string
		in   []float64
		want []float64
	}{
		{"no ties, sorted", []float64{1, 2, 3}, []float64{1, 2, 3}},
		{"no ties, unsorted", []float64{3.0, 1.5, 4.25, 1.0, 5.5}, []float64{3, 2, 4, 1, 5}},
		{"pairs tied", []float64{1, 2, 2, 3, 4, 4}, []float64{1, 2.5, 2.5, 4, 5.5, 5.5}},
		{"all tied groups", []float64{1, 1, 2, 2, 3, 3}, []float64{1.5, 1.5, 3.5, 3.5, 5.5, 5.5}},
		{"every value identical", []float64{7, 7, 7, 7}, []float64{2.5, 2.5, 2.5, 2.5}},
		{"descending", []float64{5, 4, 3, 2, 1}, []float64{5, 4, 3, 2, 1}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := rankAverage(tc.in)
			if len(got) != len(tc.want) {
				t.Fatalf("length = %d, want %d", len(got), len(tc.want))
			}
			for i := range got {
				if got[i] != tc.want[i] {
					t.Errorf("rank[%d] = %v, want %v (got %v)", i, got[i], tc.want[i], got)
				}
			}
		})
	}
}

func TestStatsRejectBadInput(t *testing.T) {
	cases := []struct {
		name    string
		x, y    []float64
		wantErr error
	}{
		{"length mismatch", []float64{1, 2, 3}, []float64{1, 2}, nil},
		{"too few samples", []float64{1}, []float64{2}, nil},
		{"empty", []float64{}, []float64{}, nil},
		{"NaN in x", []float64{1, math.NaN(), 3}, []float64{1, 2, 3}, nil},
		{"Inf in y", []float64{1, 2, 3}, []float64{1, math.Inf(1), 3}, nil},
		{"constant x is degenerate", []float64{2, 2, 2}, []float64{1, 2, 3}, ErrDegenerate},
		{"constant y is degenerate", []float64{1, 2, 3}, []float64{5, 5, 5}, ErrDegenerate},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := Pearson(tc.x, tc.y)
			if err == nil {
				t.Fatalf("Pearson accepted invalid input")
			}
			if tc.wantErr != nil && !errors.Is(err, tc.wantErr) {
				t.Errorf("Pearson error = %v, want %v", err, tc.wantErr)
			}
		})
	}
}

// TestRMSERejectsBadInput guards the shared validation path, since a
// non-finite RMSE would fail json.Marshal in the MCP layer.
func TestRMSERejectsBadInput(t *testing.T) {
	if _, err := RMSE([]float64{1, 2}, []float64{1}); err == nil {
		t.Error("RMSE accepted mismatched lengths")
	}
	if _, err := RMSE([]float64{math.NaN(), 1}, []float64{1, 2}); err == nil {
		t.Error("RMSE accepted NaN")
	}
}

func TestPearsonClampsToUnitRange(t *testing.T) {
	// A large-magnitude perfectly-correlated pair is where accumulated
	// rounding would otherwise push |r| a few ULP past 1.
	x := make([]float64, 500)
	y := make([]float64, 500)
	for i := range x {
		x[i] = float64(i) * 1e6
		y[i] = float64(i) * 1e6
	}
	r, err := Pearson(x, y)
	if err != nil {
		t.Fatalf("Pearson: %v", err)
	}
	if r > 1 || r < -1 {
		t.Fatalf("Pearson = %.17g, outside [-1, 1]", r)
	}
}

func TestToFloat64(t *testing.T) {
	in := []float32{0, 1.5, -2.25, 3.125}
	got := toFloat64(in)
	want := []float64{0, 1.5, -2.25, 3.125}
	if len(got) != len(want) {
		t.Fatalf("length = %d, want %d", len(got), len(want))
	}
	for i := range got {
		if got[i] != want[i] {
			t.Errorf("toFloat64[%d] = %v, want %v", i, got[i], want[i])
		}
	}
}
