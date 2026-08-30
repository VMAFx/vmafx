// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/coarse_to_fine_test.go — 2-pass CRF search tests.
//
// The expected grids and picks were produced by calling
// vmaftune.corpus.coarse_grid_crfs / fine_grid_crfs / _pick_best_crf /
// _should_skip_refinement on the same inputs.

package corpus

import (
	"context"
	"math"
	"reflect"
	"testing"
)

func TestCoarseGridCRFs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name                       string
		crfMin, crfMax, coarseStep int
		want                       []int
		wantErr                    bool
	}{
		{
			name: "the documented defaults", crfMin: 10, crfMax: 50, coarseStep: 10,
			want: []int{10, 20, 30, 40, 50},
		},
		{
			name: "a full 0..51 window", crfMin: 0, crfMax: 51, coarseStep: 10,
			want: []int{0, 10, 20, 30, 40, 50},
		},
		{
			name:   "a step wider than the window still yields two points",
			crfMin: 10, crfMax: 50, coarseStep: 25,
			want: []int{10, 35},
		},
		{name: "a non-positive step is rejected", crfMin: 10, crfMax: 50, coarseStep: 0, wantErr: true},
		{name: "an inverted window is rejected", crfMin: 50, crfMax: 10, coarseStep: 10, wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := CoarseGridCRFs(tc.crfMin, tc.crfMax, tc.coarseStep)
			if (err != nil) != tc.wantErr {
				t.Fatalf("CoarseGridCRFs error = %v, wantErr = %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("CoarseGridCRFs(%d, %d, %d) = %v, want %v",
					tc.crfMin, tc.crfMax, tc.coarseStep, got, tc.want)
			}
		})
	}
}

func TestFineGridCRFs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name                                    string
		bestCRF, fineRadius, fineStep, min, max int
		exclude                                 []int
		want                                    []int
		wantErr                                 bool
	}{
		{
			name:    "the defaults around a mid-grid pick, minus the coarse cells",
			bestCRF: 30, fineRadius: 5, fineStep: 1, min: 10, max: 50,
			exclude: []int{10, 20, 30, 40, 50},
			want:    []int{25, 26, 27, 28, 29, 31, 32, 33, 34, 35},
		},
		{
			name:    "a step of 2 at the top of the window clips to crfMax",
			bestCRF: 50, fineRadius: 5, fineStep: 2, min: 10, max: 50,
			exclude: []int{10, 20, 30, 40, 50},
			want:    []int{45, 47, 49},
		},
		{
			name:    "candidates below zero clamp rather than going negative",
			bestCRF: 0, fineRadius: 3, fineStep: 1, min: 0, max: 51,
			want: []int{0, 1, 2, 3},
		},
		{
			name: "a negative radius is rejected", bestCRF: 30, fineRadius: -1,
			fineStep: 1, min: 10, max: 50, wantErr: true,
		},
		{
			name: "a non-positive step is rejected", bestCRF: 30, fineRadius: 5,
			fineStep: 0, min: 10, max: 50, wantErr: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := FineGridCRFs(tc.bestCRF, tc.fineRadius, tc.fineStep,
				tc.min, tc.max, tc.exclude)
			if (err != nil) != tc.wantErr {
				t.Fatalf("FineGridCRFs error = %v, wantErr = %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("FineGridCRFs() = %v, want %v", got, tc.want)
			}
		})
	}
}

// coarseRows is the sample coarse-pass outcome the pick tests run against.
func coarseRows() []map[string]any {
	return []map[string]any{
		{"crf": 10, "vmaf_score": 99.0},
		{"crf": 20, "vmaf_score": 96.0},
		{"crf": 30, "vmaf_score": 93.5},
		{"crf": 40, "vmaf_score": 82.0},
		{"crf": 50, "vmaf_score": math.NaN()},
	}
}

func TestPickBestCRF(t *testing.T) {
	t.Parallel()

	target93 := 93.0
	target999 := 99.9

	tests := []struct {
		name   string
		rows   []map[string]any
		target *float64
		want   int
		wantOK bool
	}{
		{
			name: "without a target the highest-VMAF point wins",
			rows: coarseRows(), want: 10, wantOK: true,
		},
		{
			name: "with a target the highest passing CRF wins",
			rows: coarseRows(), target: &target93, want: 30, wantOK: true,
		},
		{
			name: "an unmet target falls back to the achievable ceiling",
			rows: coarseRows(), target: &target999, want: 10, wantOK: true,
		},
		{
			name: "no measurable rows",
			rows: []map[string]any{{"crf": 1, "vmaf_score": math.NaN()}},
		},
		{name: "no rows at all"},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, ok := PickBestCRF(tc.rows, tc.target)
			if ok != tc.wantOK {
				t.Fatalf("PickBestCRF ok = %v, want %v", ok, tc.wantOK)
			}
			if ok && got != tc.want {
				t.Errorf("PickBestCRF = %d, want %d", got, tc.want)
			}
		})
	}
}

func TestShouldSkipRefinement(t *testing.T) {
	t.Parallel()

	target := 93.0
	grid := []int{10, 20, 30, 40, 50}

	tests := []struct {
		name      string
		bestCRF   int
		haveBest  bool
		target    *float64
		bestScore float64
		want      bool
	}{
		{
			name:     "no measurable coarse rows means nothing to refine around",
			haveBest: false, want: true,
		},
		{
			name:    "without a target the fine pass always runs",
			bestCRF: 30, haveBest: true, bestScore: 93.5, want: false,
		},
		{
			name:    "a met target at the top of the grid has nowhere higher to probe",
			bestCRF: 50, haveBest: true, target: &target, bestScore: 95.0, want: true,
		},
		{
			name:    "a met target mid-grid still refines upward",
			bestCRF: 30, haveBest: true, target: &target, bestScore: 93.5, want: false,
		},
		{
			name:    "an unmet target always refines",
			bestCRF: 30, haveBest: true, target: &target, bestScore: 82.0, want: false,
		},
		{
			name:    "a NaN best score refines",
			bestCRF: 30, haveBest: true, target: &target, bestScore: math.NaN(), want: false,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := shouldSkipRefinement(tc.bestCRF, tc.haveBest, grid, tc.target,
				tc.bestScore, 50)
			if got != tc.want {
				t.Errorf("shouldSkipRefinement = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestCRFClamp(t *testing.T) {
	t.Parallel()

	for _, tc := range []struct{ in, want int }{
		{in: -5, want: 0}, {in: 0, want: 0}, {in: 26, want: 26},
		{in: 51, want: 51}, {in: 99, want: 51},
	} {
		if got := crfClamp(tc.in); got != tc.want {
			t.Errorf("crfClamp(%d) = %d, want %d", tc.in, got, tc.want)
		}
	}
}

func TestCoarseToFineSearchVisitOrder(t *testing.T) {
	t.Parallel()

	// The search must emit the coarse grid first, then only the fine cells
	// that were not already measured. The scores are shaped so CRF 30 is the
	// highest passing coarse point at a target of 93.
	scores := map[int]float64{
		10: 99.0, 20: 96.0, 30: 93.5, 40: 82.0, 50: 70.0,
	}
	target := 93.0

	job := Job{
		Source: "clip.yuv", Width: 320, Height: 240, PixFmt: "yuv420p",
		Framerate: 24, DurationS: 1,
		Cells: []Cell{{Preset: "medium", CRF: 0}},
	}
	opts := NewOptions()
	opts.EncodeDir = t.TempDir()
	opts.SrcSHA256 = false
	opts.HDRMode = HDRModeForceSDR

	var visited []int
	emit := func(row map[string]any) error {
		visited = append(visited, rowCRF(row))
		return nil
	}
	runners := scriptedRunners(scores)

	params := DefaultCoarseToFineParams()
	params.TargetVMAF = &target
	if err := CoarseToFineSearch(context.Background(), job, opts, runners,
		params, emit); err != nil {
		t.Fatalf("CoarseToFineSearch: %v", err)
	}

	wantCoarse := []int{10, 20, 30, 40, 50}
	if len(visited) < len(wantCoarse) {
		t.Fatalf("visited %d cells, want at least the %d coarse ones",
			len(visited), len(wantCoarse))
	}
	if !reflect.DeepEqual(visited[:len(wantCoarse)], wantCoarse) {
		t.Errorf("coarse pass visited %v, want %v", visited[:len(wantCoarse)], wantCoarse)
	}
	wantFine := []int{25, 26, 27, 28, 29, 31, 32, 33, 34, 35}
	if !reflect.DeepEqual(visited[len(wantCoarse):], wantFine) {
		t.Errorf("fine pass visited %v, want %v", visited[len(wantCoarse):], wantFine)
	}
}

func TestCoarseToFineSearchSkipsRefinementAtTheGridTop(t *testing.T) {
	t.Parallel()

	// Every coarse point clears the target, so the highest coarse CRF wins
	// and there is nothing higher to probe: the fine pass is skipped.
	scores := map[int]float64{10: 99, 20: 98, 30: 97, 40: 96, 50: 95}
	target := 90.0

	job := Job{
		Source: "clip.yuv", Width: 320, Height: 240, PixFmt: "yuv420p",
		Framerate: 24, DurationS: 1,
		Cells: []Cell{{Preset: "medium", CRF: 0}},
	}
	opts := NewOptions()
	opts.EncodeDir = t.TempDir()
	opts.SrcSHA256 = false
	opts.HDRMode = HDRModeForceSDR

	count := 0
	emit := func(map[string]any) error {
		count++
		return nil
	}
	params := DefaultCoarseToFineParams()
	params.TargetVMAF = &target
	if err := CoarseToFineSearch(context.Background(), job, opts,
		scriptedRunners(scores), params, emit); err != nil {
		t.Fatalf("CoarseToFineSearch: %v", err)
	}
	if count != 5 {
		t.Errorf("emitted %d rows, want just the 5 coarse cells", count)
	}
}

func TestCoarseToFineSearchWithNoPresetsIsANoOp(t *testing.T) {
	t.Parallel()

	job := Job{Source: "clip.yuv", Width: 320, Height: 240, PixFmt: "yuv420p"}
	opts := NewOptions()
	opts.EncodeDir = t.TempDir()

	called := false
	err := CoarseToFineSearch(context.Background(), job, opts, Runners{},
		DefaultCoarseToFineParams(), func(map[string]any) error {
			called = true
			return nil
		})
	if err != nil {
		t.Fatalf("CoarseToFineSearch: %v", err)
	}
	if called {
		t.Error("a job with no cells emitted rows")
	}
}
