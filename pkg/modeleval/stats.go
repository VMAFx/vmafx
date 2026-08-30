// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/modeleval/stats.go — correlation / error statistics for model
// evaluation, ported from the scipy calls the Python MCP server makes.
//
// # Numerical-parity note (read before "fixing" a diff against Python)
//
// The Python reference (`_eval_model_on_split`) hands scipy *float32*
// arrays, and scipy does not upcast:
//
//   - pearsonr(float32) accumulates in float32. Measured against the
//     float64 result on 256 correlated samples the two agree to ~1e-7
//     (0.9447892904281616 vs 0.9447893906810056).
//   - spearmanr is bit-identical either way: it correlates *ranks*,
//     which are integers or exact halves and so are represented
//     exactly in both widths.
//   - RMSE via np.sqrt(((p-y)**2).mean()) on float32 lands within
//     ~1e-9 of the float64 value.
//
// This package deliberately computes in float64. The float32
// accumulation on the Python side is an artefact of numpy's default
// dtype propagation, not a contract worth reproducing, and float64 is
// the strictly more accurate answer. Callers comparing Go output to
// Python output should therefore expect agreement to ~1e-7 on PLCC,
// ~1e-9 on RMSE, and exact agreement on SROCC — not bit-equality.
//
// # Non-finite policy
//
// scipy returns NaN for a degenerate (zero-variance) input and Python's
// json.dumps happily emits a bare `NaN`, which is not valid JSON. Go's
// encoding/json refuses to marshal it, so the MCP layer would surface
// an opaque "failed to marshal result". These helpers therefore fail
// loudly and specifically instead, which is a deliberate improvement
// over the Python behaviour rather than an accidental divergence.

package modeleval

import (
	"errors"
	"fmt"
	"math"
	"sort"
)

// ErrDegenerate is returned when a correlation is mathematically
// undefined because one of the inputs has zero variance (every value
// identical). scipy yields NaN here; see the package note above.
var ErrDegenerate = errors.New("correlation undefined: input has zero variance")

// checkPair validates that two samples are the same length, long enough
// to correlate, and free of non-finite values.
func checkPair(x, y []float64) error {
	if len(x) != len(y) {
		return fmt.Errorf("length mismatch: %d vs %d", len(x), len(y))
	}
	if len(x) < 2 {
		return fmt.Errorf("need >=2 samples, got %d", len(x))
	}
	for i, v := range x {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return fmt.Errorf("non-finite value at index %d of first sample", i)
		}
	}
	for i, v := range y {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return fmt.Errorf("non-finite value at index %d of second sample", i)
		}
	}
	return nil
}

// Pearson returns the Pearson product-moment correlation coefficient
// (PLCC) of x and y using the numerically stable two-pass centred form.
//
// The result is clamped to [-1, 1]: accumulated rounding can push a
// perfect correlation a few ULP outside the mathematically valid range,
// and scipy applies the same clamp.
func Pearson(x, y []float64) (float64, error) {
	if err := checkPair(x, y); err != nil {
		return 0, err
	}
	n := float64(len(x))

	var sumX, sumY float64
	for i := range x {
		sumX += x[i]
		sumY += y[i]
	}
	meanX, meanY := sumX/n, sumY/n

	var cov, varX, varY float64
	for i := range x {
		dx := x[i] - meanX
		dy := y[i] - meanY
		cov += dx * dy
		varX += dx * dx
		varY += dy * dy
	}
	if varX == 0 || varY == 0 {
		return 0, ErrDegenerate
	}

	r := cov / math.Sqrt(varX*varY)
	return math.Max(-1, math.Min(1, r)), nil
}

// Spearman returns the Spearman rank correlation coefficient (SROCC):
// the Pearson correlation of the average-tied ranks of x and y. This
// matches scipy.stats.spearmanr, which ranks via rankdata(method="average").
func Spearman(x, y []float64) (float64, error) {
	if err := checkPair(x, y); err != nil {
		return 0, err
	}
	return Pearson(rankAverage(x), rankAverage(y))
}

// rankAverage assigns 1-based ranks to v, giving every member of a group
// of tied values the arithmetic mean of the ranks that group spans.
// Equivalent to scipy.stats.rankdata(v, method="average").
//
// Input must be free of NaN — checkPair guarantees that for every
// caller in this package, and a NaN would otherwise make the sort
// comparator non-transitive.
func rankAverage(v []float64) []float64 {
	n := len(v)
	idx := make([]int, n)
	for i := range idx {
		idx[i] = i
	}
	sort.SliceStable(idx, func(a, b int) bool { return v[idx[a]] < v[idx[b]] })

	ranks := make([]float64, n)
	for i := 0; i < n; {
		// Extend j over the run of values tied with v[idx[i]].
		j := i
		for j+1 < n && v[idx[j+1]] == v[idx[i]] {
			j++
		}
		// Ranks are 1-based, so the run spans i+1 .. j+1 and their
		// mean is ((i+1)+(j+1))/2.
		avg := float64(i+j+2) / 2
		for k := i; k <= j; k++ {
			ranks[idx[k]] = avg
		}
		i = j + 1
	}
	return ranks
}

// RMSE returns the root-mean-square error between predictions and
// targets: sqrt(mean((pred-target)^2)).
func RMSE(pred, target []float64) (float64, error) {
	if err := checkPair(pred, target); err != nil {
		return 0, err
	}
	var acc float64
	for i := range pred {
		d := pred[i] - target[i]
		acc += d * d
	}
	return math.Sqrt(acc / float64(len(pred))), nil
}

// toFloat64 widens a float32 sample to float64 for the statistics above.
func toFloat64(in []float32) []float64 {
	out := make([]float64, len(in))
	for i, v := range in {
		out[i] = float64(v)
	}
	return out
}
