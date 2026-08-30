// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/coarse_to_fine.go — 2-pass coarse-to-fine CRF search (ADR-0296 /
// ADR-0306).
//
// A full-grid sweep over CRF 0..51 step 1 is 52 encodes per (source, preset).
// When the caller only wants "the smallest CRF that meets a VMAF target" the
// bracket can be found in two passes:
//
//  1. a coarse pass at every coarseStep over the CRF range;
//  2. a fine pass at fineStep within +/- fineRadius of the best coarse point.
//
// The defaults (10 / 5 / 1 over 0..51) visit 5 + 10 = 15 unique encodes — a
// 3.5x wall-time speedup with no measurable quality loss on the Netflix Public
// corpus (docs/research/0067 + ADR-0296).
//
// When no target is supplied the orchestrator still runs both passes and
// refines around the highest-VMAF coarse point.

package corpus

import (
	"context"
	"fmt"
	"math"
	"sort"
)

// crfClamp constrains a CRF candidate to the libx264 0..51 valid range.
func crfClamp(crf int) int {
	switch {
	case crf < 0:
		return 0
	case crf > 51:
		return 51
	default:
		return crf
	}
}

// CoarseGridCRFs returns the coarse-pass CRF grid as a deduped, sorted slice.
//
// The defaults yield (10, 20, 30, 40, 50) — five points spanning the
// practically useful CRF range for libx264. CRF below 10 is visually lossless
// on most content (huge bitrate, no perceptual gain) and CRF 51 is the codec
// floor; the coarse pass intentionally skips both. Override crfMin / crfMax for
// codecs with different quality-knob ranges.
func CoarseGridCRFs(crfMin, crfMax, coarseStep int) ([]int, error) {
	if coarseStep <= 0 {
		return nil, fmt.Errorf("coarse_step must be positive, got %d", coarseStep)
	}
	if crfMin > crfMax {
		return nil, fmt.Errorf("crf_min (%d) > crf_max (%d)", crfMin, crfMax)
	}
	n := (crfMax-crfMin)/coarseStep + 1
	seen := map[int]bool{}
	var grid []int
	for i := 0; i < n; i++ {
		c := crfClamp(crfMin + i*coarseStep)
		if !seen[c] {
			seen[c] = true
			grid = append(grid, c)
		}
	}
	sort.Ints(grid)
	return grid, nil
}

// FineGridCRFs returns the CRF candidates in [best-radius, best+radius] at
// fineStep, minus the excluded cells (typically the coarse grid) so the second
// pass only visits points that have not been measured already.
func FineGridCRFs(bestCRF, fineRadius, fineStep, crfMin, crfMax int, exclude []int) ([]int, error) {
	if fineRadius < 0 {
		return nil, fmt.Errorf("fine_radius must be non-negative, got %d", fineRadius)
	}
	if fineStep <= 0 {
		return nil, fmt.Errorf("fine_step must be positive, got %d", fineStep)
	}
	excluded := make(map[int]bool, len(exclude))
	for _, c := range exclude {
		excluded[c] = true
	}
	candidates := map[int]bool{}
	for delta := -fineRadius; delta <= fineRadius; delta += fineStep {
		candidates[crfClamp(bestCRF+delta)] = true
	}
	var out []int
	for c := range candidates {
		if excluded[c] || c < crfMin || c > crfMax {
			continue
		}
		out = append(out, c)
	}
	sort.Ints(out)
	return out, nil
}

// rowScore reads a row's vmaf_score as a float, returning NaN for missing or
// non-numeric values.
func rowScore(row map[string]any) float64 {
	v, ok := row["vmaf_score"]
	if !ok {
		return math.NaN()
	}
	f, ok := toFloat(v)
	if !ok {
		return math.NaN()
	}
	return f
}

// rowCRF reads a row's crf column as an int.
func rowCRF(row map[string]any) int {
	v, ok := row["crf"]
	if !ok {
		return 0
	}
	switch t := v.(type) {
	case int:
		return t
	case float64:
		return int(t)
	default:
		return 0
	}
}

// PickBestCRF identifies the "best" coarse CRF for refinement.
//
// With a target: the highest CRF whose vmaf_score meets targetVMAF — the
// smallest-quality candidate that still passes the gate, so refining around it
// locates the smallest acceptable CRF. Without a target (targetVMAF == nil):
// the CRF with the highest VMAF. NaN / failed rows are ignored; ok is false
// when no row carries a measurable score.
func PickBestCRF(rows []map[string]any, targetVMAF *float64) (int, bool) {
	var valid []map[string]any
	for _, r := range rows {
		if !math.IsNaN(rowScore(r)) {
			valid = append(valid, r)
		}
	}
	if len(valid) == 0 {
		return 0, false
	}
	maxByScore := func(rs []map[string]any) map[string]any {
		best := rs[0]
		for _, r := range rs[1:] {
			if rowScore(r) > rowScore(best) {
				best = r
			}
		}
		return best
	}
	if targetVMAF == nil {
		return rowCRF(maxByScore(valid)), true
	}
	var passing []map[string]any
	for _, r := range valid {
		if rowScore(r) >= *targetVMAF {
			passing = append(passing, r)
		}
	}
	if len(passing) > 0 {
		// Highest CRF that still passes — refining around it finds the
		// smallest CRF that still meets the target.
		best := passing[0]
		for _, r := range passing[1:] {
			if rowCRF(r) > rowCRF(best) {
				best = r
			}
		}
		return rowCRF(best), true
	}
	// Nothing met the target on the coarse pass. Fall back to the
	// highest-VMAF coarse point so the fine pass at least probes near the
	// achievable ceiling.
	return rowCRF(maxByScore(valid)), true
}

// shouldSkipRefinement decides whether the coarse pass alone is enough.
//
// The fine pass is skipped when the coarse pass produced no measurable rows, or
// when a target is set, the best-coarse CRF meets it, and refining higher
// cannot help (the best-coarse is already the largest CRF in the coarse grid or
// is pinned at crfMax). In that case there are no larger CRF candidates to
// probe, so the existing best already minimises bitrate at the gate.
func shouldSkipRefinement(
	bestCRF int, haveBest bool, coarseGrid []int, targetVMAF *float64,
	bestScore float64, crfMax int,
) bool {
	if !haveBest {
		return true
	}
	if targetVMAF == nil {
		return false
	}
	if math.IsNaN(bestScore) {
		return false
	}
	if bestScore < *targetVMAF {
		return false
	}
	maxCoarse := coarseGrid[0]
	for _, c := range coarseGrid[1:] {
		if c > maxCoarse {
			maxCoarse = c
		}
	}
	return bestCRF >= maxCoarse || bestCRF >= crfMax
}

// CoarseToFineParams carries the tunables for CoarseToFineSearch.
type CoarseToFineParams struct {
	// TargetVMAF is the quality gate. Nil refines around the highest-VMAF
	// coarse point instead.
	TargetVMAF *float64
	CoarseStep int
	FineRadius int
	FineStep   int
	CRFMin     int
	CRFMax     int
}

// DefaultCoarseToFineParams mirrors the Python keyword defaults.
func DefaultCoarseToFineParams() CoarseToFineParams {
	return CoarseToFineParams{
		CoarseStep: 10,
		FineRadius: 5,
		FineStep:   1,
		CRFMin:     10,
		CRFMax:     50,
	}
}

// CoarseToFineSearch runs the 2-pass coarse-to-fine CRF search.
//
// It emits the same rows IterRows does — the coarse pass first, then the fine
// pass when it is not skipped. The caller selects the chosen CRF from the rows;
// this function only drives the encodes.
//
// The presets in job.Cells are honoured: the search runs once per distinct
// preset, with the CRF axis replaced by the coarse-then-fine sweep.
func CoarseToFineSearch(
	ctx context.Context, job Job, opts Options, runners Runners,
	params CoarseToFineParams, emit func(map[string]any) error,
) error {
	// Distinct presets in first-seen order (dict.fromkeys semantics).
	var presets []string
	seen := map[string]bool{}
	for _, cell := range job.Cells {
		if !seen[cell.Preset] {
			seen[cell.Preset] = true
			presets = append(presets, cell.Preset)
		}
	}
	if len(presets) == 0 {
		return nil
	}

	coarseGrid, err := CoarseGridCRFs(params.CRFMin, params.CRFMax, params.CoarseStep)
	if err != nil {
		return err
	}

	for _, preset := range presets {
		coarseJob := job
		coarseJob.Cells = cellsFor(preset, coarseGrid)

		var coarseRows []map[string]any
		coarseErr := IterRows(ctx, coarseJob, opts, runners, func(row map[string]any) error {
			coarseRows = append(coarseRows, row)
			return emit(row)
		})
		if coarseErr != nil {
			return coarseErr
		}

		bestCRF, haveBest := PickBestCRF(coarseRows, params.TargetVMAF)
		bestScore := math.NaN()
		if haveBest {
			for _, r := range coarseRows {
				if rowCRF(r) == bestCRF {
					bestScore = rowScore(r)
					break
				}
			}
		}

		if shouldSkipRefinement(bestCRF, haveBest, coarseGrid, params.TargetVMAF,
			bestScore, params.CRFMax) {
			continue
		}

		fineCRFs, fineErr := FineGridCRFs(bestCRF, params.FineRadius, params.FineStep,
			params.CRFMin, params.CRFMax, coarseGrid)
		if fineErr != nil {
			return fineErr
		}
		if len(fineCRFs) == 0 {
			continue
		}

		fineJob := job
		fineJob.Cells = cellsFor(preset, fineCRFs)
		if err := IterRows(ctx, fineJob, opts, runners, emit); err != nil {
			return err
		}
	}
	return nil
}

// cellsFor pairs a preset with each CRF in the grid.
func cellsFor(preset string, crfs []int) []Cell {
	out := make([]Cell, len(crfs))
	for i, c := range crfs {
		out[i] = Cell{Preset: preset, CRF: c}
	}
	return out
}
