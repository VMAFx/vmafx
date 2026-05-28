// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package ladder implements the per-title ABR bitrate-ladder generator for
// vmafx-tune Stage 2 + Stage 3.
//
// Algorithm (mirrors tools/vmaf-tune/src/vmaftune/ladder.py Phase E):
//
//  1. For each (resolution, VMAF target) cell in the requested grid, run the
//     existing CRF bisect (pkg/bisect) to find the CRF whose measured VMAF
//     meets the target.  This produces a cloud of (bitrate, vmaf) points.
//  2. Compute the upper convex hull of the (bitrate, vmaf) cloud.  The hull
//     is the Pareto frontier: no point above the hull achieves higher VMAF at
//     lower bitrate.
//  3. Select a small set of "knee" renditions along the hull by detecting
//     curvature maxima (Kneedle-style inflection detection on the normalised
//     curve).  The caller controls the maximum rung count via MaxRungs.
//  4. Emit a LadderResult that carries the raw cloud, the hull points, and
//     the selected renditions.  The JSON schema is a superset of the Python
//     ladder.py output, maintaining schema-forward compatibility.
//
// Stage-3 additions (ADR-0734):
//   - Workers: concurrent grid sampling bounded by a semaphore of size Workers
//     (default = NumCPU/2 clamped to [1,8]).  The goroutine fan-out is safe
//     because each cell writes to a pre-allocated slot in the cloud slice
//     (indexed by cellIdx = resIdx*len(targets) + targetIdx) so there are no
//     slice races.
//   - Conformal intervals: see ConformalSamplerFn and ConformalPoint.  The
//     ladder CLI exposes uncertainty metadata in the JSON output when the
//     conformal sampler is wired in.
//
// Python parity:
//   - The convex hull, knee selection, and rung ordering all mirror the Python
//     Phase E implementation in ladder.py.
//   - The SamplerFn seam mirrors ladder.py's SamplerFn type alias so callers
//     (including tests) can inject stub samplers.
//   - The default sampler is NOT ported in Stage 2; callers that need a live
//     encoder sweep use pkg/bisect.Run directly.  The CLI wires bisect.Run as
//     the production sampler.
//
// See ADR-0730 for Stage-2 design rationale and ADR-0734 for Stage-3.
package ladder

import (
	"cmp"
	"fmt"
	"math"
	"runtime"
	"slices"
	"sync"
)

// DefaultMaxRungs is the default maximum number of ABR ladder rungs selected
// from the convex hull.
const DefaultMaxRungs = 6

// DefaultMinBitrateGapKbps is the minimum bitrate gap between adjacent rungs
// in the emitted ladder.  Rungs closer than this (after rounding) are merged
// with their lower-bitrate neighbour.
const DefaultMinBitrateGapKbps = 100.0

// DefaultWorkers is the default number of concurrent grid-sampling goroutines.
// It is NumCPU/2 clamped to [1, 8] to avoid overwhelming the encoder queue on
// large machines while still providing meaningful parallelism on typical
// workstations (ADR-0734).
func DefaultWorkers() int {
	n := runtime.NumCPU() / 2
	if n < 1 {
		n = 1
	}
	if n > 8 {
		n = 8
	}
	return n
}

// Point is one sampled (resolution, bitrate, vmaf, crf) data point from the
// (resolution × VMAF-target) grid.
//
// The JSON tags mirror the Python LadderPoint dataclass field names so the
// existing Python tooling can consume Go ladder output without modification.
type Point struct {
	Width       int     `json:"width"`
	Height      int     `json:"height"`
	BitratekBps float64 `json:"bitrate_kbps"`
	VMAF        float64 `json:"vmaf"`
	CRF         int     `json:"crf"`
	// TargetVMAF records the bisect target this point was sampled against.
	// Not present in the Python schema but added as an optional field (never
	// removed — ADR-0705 JSON-schema-forward invariant).
	TargetVMAF float64 `json:"target_vmaf,omitempty"`
	// OK is false when the bisect could not satisfy the target VMAF.
	OK bool `json:"ok"`
	// Error is non-empty when OK is false.
	Error string `json:"error,omitempty"`
}

// Rendition is one rung of the emitted ABR ladder.  Fields mirror the Python
// Rendition dataclass so the existing HLS/DASH manifest renderer can ingest
// the JSON unchanged.
type Rendition struct {
	Width       int     `json:"width"`
	Height      int     `json:"height"`
	BitratekBps float64 `json:"bitrate_kbps"`
	VMAF        float64 `json:"vmaf"`
	CRF         int     `json:"crf"`
}

// LadderResult is the complete output of Build.  Fields mirror the Python
// Ladder + rendition picks so callers can drive HLS/DASH manifest generation
// from the JSON output.
type LadderResult struct {
	// Src is the path to the source file used for sampling.
	Src string `json:"src"`
	// Encoder is the codec name used for sampling (e.g. "libx264").
	Encoder string `json:"encoder"`
	// Cloud is every sampled (resolution, bitrate, vmaf, crf) point,
	// including failed bisects.  Plotters use this as the raw data cloud.
	Cloud []Point `json:"cloud"`
	// Hull is the upper convex hull of the cloud's successful (bitrate, vmaf)
	// points, sorted by ascending bitrate.
	Hull []Point `json:"hull"`
	// Renditions is the selected subset of hull knees, sorted by ascending
	// bitrate, capped at MaxRungs.
	Renditions []Rendition `json:"renditions"`
}

// SamplerFn produces one Point for a given (src, encoder, width, height,
// targetVMAF) request.  The production sampler in the CLI wires bisect.Run;
// tests inject a mock.  The function signature mirrors the Python SamplerFn
// type alias in ladder.py.
type SamplerFn func(src, encoder string, width, height int, targetVMAF float64) (Point, error)

// Params configures a Build call.
type Params struct {
	// Resolutions is the grid of (width, height) pairs to sample.
	// At least one resolution must be provided.
	Resolutions [][2]int

	// TargetVMAFs is the list of VMAF targets to bisect for.
	// At least one target must be provided.
	TargetVMAFs []float64

	// Sampler is the function used to produce a Point for each grid cell.
	// When nil, Build returns an error — callers must supply the sampler.
	Sampler SamplerFn

	// MaxRungs caps the number of renditions selected from the hull.
	// Defaults to DefaultMaxRungs.
	MaxRungs int

	// MinBitrateGapKbps is the minimum bitrate gap between adjacent
	// renditions.  Defaults to DefaultMinBitrateGapKbps.
	MinBitrateGapKbps float64

	// Workers is the maximum number of concurrent grid-sampling goroutines
	// (Stage-3, ADR-0734).  The semaphore bounds encoder queue pressure.
	// Defaults to DefaultWorkers() when ≤ 0.
	Workers int
}

func (p *Params) applyDefaults() {
	if p.MaxRungs <= 0 {
		p.MaxRungs = DefaultMaxRungs
	}
	if p.MinBitrateGapKbps <= 0 {
		p.MinBitrateGapKbps = DefaultMinBitrateGapKbps
	}
	if p.Workers <= 0 {
		p.Workers = DefaultWorkers()
	}
}

// Build samples the (resolution × VMAF-target) grid using params.Sampler,
// computes the upper convex hull of the successful points, selects knee
// renditions from the hull, and returns the complete LadderResult.
//
// Stage-3 (ADR-0734): sampling is concurrent.  Up to params.Workers goroutines
// run in parallel, bounded by a semaphore channel of size Workers.  Each cell
// writes to a pre-allocated slot in the cloud slice (indexed by
// resIdx*len(TargetVMAFs)+targetIdx) so there are no slice-growth races.
// Error values from cells are captured in-place as failed Points; Build does
// not abort on individual cell failures.
func Build(src, encoder string, params Params) (LadderResult, error) {
	params.applyDefaults()

	if params.Sampler == nil {
		return LadderResult{}, fmt.Errorf("ladder.Build: Sampler must not be nil")
	}
	if len(params.Resolutions) == 0 {
		return LadderResult{}, fmt.Errorf("ladder.Build: Resolutions must not be empty")
	}
	if len(params.TargetVMAFs) == 0 {
		return LadderResult{}, fmt.Errorf("ladder.Build: TargetVMAFs must not be empty")
	}
	for _, t := range params.TargetVMAFs {
		if t <= 0 || t > 100 {
			return LadderResult{}, fmt.Errorf("ladder.Build: TargetVMAF %g out of range (0,100]", t)
		}
	}

	nRes := len(params.Resolutions)
	nTgt := len(params.TargetVMAFs)
	totalCells := nRes * nTgt

	// Pre-allocate the cloud slice so concurrent writes are index-safe.
	cloud := make([]Point, totalCells)

	// Semaphore: a buffered channel of size Workers bounds concurrency.
	sem := make(chan struct{}, params.Workers)
	var wg sync.WaitGroup

	for ri, res := range params.Resolutions {
		for ti, tv := range params.TargetVMAFs {
			cellIdx := ri*nTgt + ti
			width, height, targetVMAF := res[0], res[1], tv

			wg.Add(1)
			sem <- struct{}{} // acquire slot
			go func(idx, w, h int, tgt float64) {
				defer wg.Done()
				defer func() { <-sem }() // release slot

				pt, err := params.Sampler(src, encoder, w, h, tgt)
				if err != nil {
					cloud[idx] = Point{
						Width:      w,
						Height:     h,
						TargetVMAF: tgt,
						OK:         false,
						Error:      err.Error(),
					}
					return
				}
				cloud[idx] = pt
			}(cellIdx, width, height, targetVMAF)
		}
	}
	wg.Wait()

	// Compute the upper convex hull from the successful points.
	hull := upperConvexHull(cloud)

	// Select knee renditions from the hull.
	renditions := selectRenditions(hull, params.MaxRungs, params.MinBitrateGapKbps)

	return LadderResult{
		Src:        src,
		Encoder:    encoder,
		Cloud:      cloud,
		Hull:       hull,
		Renditions: renditions,
	}, nil
}

// upperConvexHull computes the upper convex hull of the successful (bitrate,
// vmaf) points in cloud, sorted by ascending bitrate.
//
// The algorithm mirrors ladder.py's convex_hull: it uses the standard
// Graham-scan upper hull on the (bitrate, vmaf) plane, keeping only the
// Pareto-dominant points (no point with higher VMAF at equal or lower
// bitrate is discarded).
//
// Returns the hull points (subset of cloud) sorted by ascending bitrate.
// Points where OK is false are excluded.
func upperConvexHull(cloud []Point) []Point {
	// Filter to successful points with valid (bitrate, vmaf).
	ok := make([]Point, 0, len(cloud))
	for _, p := range cloud {
		if p.OK && p.BitratekBps > 0 && p.VMAF > 0 {
			ok = append(ok, p)
		}
	}
	if len(ok) == 0 {
		return nil
	}

	// Sort by bitrate ascending; break ties by VMAF descending.
	slices.SortFunc(ok, func(a, b Point) int {
		if c := cmp.Compare(a.BitratekBps, b.BitratekBps); c != 0 {
			return c
		}
		return cmp.Compare(b.VMAF, a.VMAF)
	})

	// Upper hull: keep a point if it improves VMAF over the running maximum.
	// This is the "staircase" / Pareto-dominant upper hull on (bitrate,vmaf).
	// The Graham-scan cross-product removes clockwise turns so no point below
	// the hull is retained.
	if len(ok) == 1 {
		return ok
	}

	hull := make([]Point, 0, len(ok))
	for _, p := range ok {
		// Remove points that form a non-left turn (clockwise or collinear).
		for len(hull) >= 2 {
			a, b, c := hull[len(hull)-2], hull[len(hull)-1], p
			// Cross product of vectors (b-a) and (c-a).
			cross := (b.BitratekBps-a.BitratekBps)*(c.VMAF-a.VMAF) -
				(b.VMAF-a.VMAF)*(c.BitratekBps-a.BitratekBps)
			if cross <= 0 {
				hull = hull[:len(hull)-1]
			} else {
				break
			}
		}
		hull = append(hull, p)
	}

	return hull
}

// selectRenditions picks up to maxRungs renditions from hull using Kneedle-
// style inflection detection on the normalised (bitrate, vmaf) curve.
//
// Strategy (mirrors ladder.py select_knees):
//
//  1. Normalise bitrate and vmaf to [0,1] across the hull.
//  2. Compute the "knee score" for each interior point as the perpendicular
//     distance from the chord connecting the first and last hull point to the
//     point.  The point with the maximum distance is the primary knee.
//  3. Recurse on each sub-segment (left/right of the primary knee) until
//     maxRungs is reached.
//  4. Apply the min-bitrate-gap filter: if two adjacent selected renditions
//     are within minBitrateGapKbps of each other, drop the lower-vmaf one.
func selectRenditions(hull []Point, maxRungs int, minBitrateGapKbps float64) []Rendition {
	if len(hull) == 0 {
		return nil
	}
	if len(hull) == 1 {
		return []Rendition{toRendition(hull[0])}
	}
	if maxRungs <= 0 {
		maxRungs = DefaultMaxRungs
	}

	// Normalisation bounds.
	minB, maxB := hull[0].BitratekBps, hull[len(hull)-1].BitratekBps
	minV := hull[0].VMAF
	maxV := hull[0].VMAF
	for _, p := range hull {
		if p.VMAF < minV {
			minV = p.VMAF
		}
		if p.VMAF > maxV {
			maxV = p.VMAF
		}
	}
	normB := func(b float64) float64 {
		if maxB == minB {
			return 0
		}
		return (b - minB) / (maxB - minB)
	}
	normV := func(v float64) float64 {
		if maxV == minV {
			return 0
		}
		return (v - minV) / (maxV - minV)
	}

	// Recursive knee search over an index range [lo, hi].
	type indexRange struct{ lo, hi int }
	selected := make(map[int]bool)
	// Always include the endpoints.
	selected[0] = true
	selected[len(hull)-1] = true

	queue := []indexRange{{0, len(hull) - 1}}
	for len(queue) > 0 && len(selected) < maxRungs {
		r := queue[0]
		queue = queue[1:]
		if r.hi-r.lo <= 1 {
			continue
		}

		// Find interior point with maximum perpendicular distance from
		// the chord [r.lo, r.hi].
		ax, ay := normB(hull[r.lo].BitratekBps), normV(hull[r.lo].VMAF)
		bx, by := normB(hull[r.hi].BitratekBps), normV(hull[r.hi].VMAF)
		dx, dy := bx-ax, by-ay
		chordLen := math.Hypot(dx, dy)

		bestIdx := -1
		bestDist := -1.0
		for i := r.lo + 1; i < r.hi; i++ {
			px, py := normB(hull[i].BitratekBps), normV(hull[i].VMAF)
			var dist float64
			if chordLen < 1e-12 {
				dist = math.Hypot(px-ax, py-ay)
			} else {
				// Perpendicular distance from point to the chord line.
				dist = math.Abs(dy*(px-ax)-dx*(py-ay)) / chordLen
			}
			if dist > bestDist {
				bestDist = dist
				bestIdx = i
			}
		}
		if bestIdx < 0 {
			continue
		}

		selected[bestIdx] = true
		queue = append(queue, indexRange{r.lo, bestIdx})
		queue = append(queue, indexRange{bestIdx, r.hi})
	}

	// Collect and sort selected indices.
	indices := make([]int, 0, len(selected))
	for idx := range selected {
		indices = append(indices, idx)
	}
	slices.Sort(indices)

	renditions := make([]Rendition, 0, len(indices))
	for _, idx := range indices {
		renditions = append(renditions, toRendition(hull[idx]))
	}

	// Apply min-bitrate-gap filter: remove rungs too close to their
	// lower-bitrate neighbour.
	renditions = applyBitrateGap(renditions, minBitrateGapKbps)

	// Re-sort by ascending bitrate after any gap-filter removals.
	slices.SortFunc(renditions, func(a, b Rendition) int {
		return cmp.Compare(a.BitratekBps, b.BitratekBps)
	})

	return renditions
}

// toRendition converts a Point to a Rendition.
func toRendition(p Point) Rendition {
	return Rendition{
		Width:       p.Width,
		Height:      p.Height,
		BitratekBps: p.BitratekBps,
		VMAF:        p.VMAF,
		CRF:         p.CRF,
	}
}

// applyBitrateGap removes renditions whose bitrate is within minGap of their
// lower-bitrate neighbour.  When two renditions are within the gap, the one
// with lower VMAF is removed.
func applyBitrateGap(renditions []Rendition, minGap float64) []Rendition {
	if len(renditions) <= 1 || minGap <= 0 {
		return renditions
	}
	out := []Rendition{renditions[0]}
	for i := 1; i < len(renditions); i++ {
		prev := out[len(out)-1]
		cur := renditions[i]
		if cur.BitratekBps-prev.BitratekBps < minGap {
			// Keep whichever has higher VMAF.
			if cur.VMAF > prev.VMAF {
				out[len(out)-1] = cur
			}
			continue
		}
		out = append(out, cur)
	}
	return out
}
