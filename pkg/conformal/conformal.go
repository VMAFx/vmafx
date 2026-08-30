// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package conformal is the Go port of the split-conformal half of
// tools/vmaf-tune/src/vmaftune/conformal.py (ADR-0279 / PR #488).
//
// Split conformal prediction wraps a point VMAF estimate in an interval
// [point - q, point + q] whose half-width q is the finite-sample-corrected
// (1 - alpha) empirical quantile of the calibration set's absolute residuals.
// Under exchangeability of the calibration and test points, marginal coverage
// is at least 1 - alpha (Lei et al. 2018 Theorem 2.2; Vovk et al. 2005
// Proposition 2.2 for the tie-admitting rank-based statement).
//
// Scope note: only the split-conformal calibration is ported here, because
// that is the only variant the `predict --with-uncertainty` path consumes
// (it loads a sidecar written by save_split_calibration and re-derives the
// interval from the residual quantile alone). The CV+ calibration
// (Barber et al. 2021) is used only by the offline training pipeline in
// ai/ and predictor_train.py, neither of which is in the Go port's scope.
package conformal

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"sort"
)

// DefaultAlpha is the nominal miscoverage level (0.05 = 95 % coverage),
// mirroring vmaftune.conformal.default_alpha().
const DefaultAlpha = 0.05

// AbsoluteResidualScore is the default non-conformity score |target - pred|.
// Any permutation-invariant score satisfies the coverage proof; the absolute
// residual is the one analysed in Lei et al. 2018 Theorem 2.2.
func AbsoluteResidualScore(prediction, target float64) float64 {
	return math.Abs(target - prediction)
}

// EmpiricalQuantile computes the type-7 (Hyndman-Fan) empirical quantile,
// matching numpy's np.quantile(..., method="linear") for any non-empty input.
func EmpiricalQuantile(values []float64, q float64) (float64, error) {
	if len(values) == 0 {
		return 0, errors.New("conformal: empirical quantile of an empty sample is undefined")
	}
	if q < 0.0 || q > 1.0 {
		return 0, fmt.Errorf("conformal: q must be in [0, 1]; got %v", q)
	}
	sorted := make([]float64, len(values))
	copy(sorted, values)
	sort.Float64s(sorted)
	n := len(sorted)
	if n == 1 {
		return sorted[0], nil
	}
	// Type-7 plotting position: h = (n-1) * q.
	h := float64(n-1) * q
	lo := int(math.Floor(h))
	hi := int(math.Ceil(h))
	if lo == hi {
		return sorted[lo], nil
	}
	frac := h - float64(lo)
	return sorted[lo] + frac*(sorted[hi]-sorted[lo]), nil
}

// SplitCalibration holds the absolute residuals from a held-out calibration
// set plus the nominal miscoverage level.
type SplitCalibration struct {
	Residuals []float64
	Alpha     float64
}

// Interval is a (point, low, high) conformal prediction interval.
type Interval struct {
	Point float64
	Low   float64
	High  float64
	Alpha float64
}

// Validate rejects an out-of-range alpha and any non-finite or negative
// residual.
func (c SplitCalibration) Validate() error {
	if !(c.Alpha > 0.0 && c.Alpha < 1.0) {
		return fmt.Errorf("conformal: alpha must be in (0, 1); got %v", c.Alpha)
	}
	for _, r := range c.Residuals {
		if r < 0.0 || math.IsNaN(r) || math.IsInf(r, 0) {
			return fmt.Errorf(
				"conformal: residuals must be non-negative finite floats; got %v", r)
		}
	}
	return nil
}

// N returns the calibration-set size.
func (c SplitCalibration) N() int { return len(c.Residuals) }

// IsEmpty reports whether the calibration set has zero points.
func (c SplitCalibration) IsEmpty() bool { return c.N() == 0 }

// Quantile returns q_{1-alpha} — the half-width of the interval.
//
// The level is finite-sample corrected as min(1, ceil((n+1)(1-alpha)) / n)
// per Lei et al. 2018 §2.2 / Romano 2019 §3 so marginal coverage >= 1 - alpha
// survives at small n.
//
// An empty calibration set returns 0 with ok=false; callers then see
// low == high == point and must flag the result as uncalibrated rather than
// pretending to a coverage guarantee they do not have.
func (c SplitCalibration) Quantile() (halfWidth float64, ok bool) {
	if c.IsEmpty() {
		return 0.0, false
	}
	n := float64(c.N())
	level := math.Min(1.0, math.Ceil((n+1)*(1.0-c.Alpha))/n)
	q, err := EmpiricalQuantile(c.Residuals, level)
	if err != nil {
		return 0.0, false
	}
	return q, true
}

// IntervalFor wraps a point estimate in the calibrated interval, clamped to
// the VMAF [0, 100] scale.
func (c SplitCalibration) IntervalFor(point float64) Interval {
	q, ok := c.Quantile()
	if !ok {
		return Interval{Point: point, Low: point, High: point, Alpha: c.Alpha}
	}
	return Interval{
		Point: point,
		Low:   clamp(point-q, 0.0, 100.0),
		High:  clamp(point+q, 0.0, 100.0),
		Alpha: c.Alpha,
	}
}

// WithAlpha returns a copy of the calibration with an overridden alpha,
// mirroring the Python dataclasses.replace(cal, alpha=...) override.
func (c SplitCalibration) WithAlpha(alpha float64) SplitCalibration {
	return SplitCalibration{Residuals: c.Residuals, Alpha: alpha}
}

// CalibrateSplit builds a calibration from paired predictions and targets.
// The pairs are assumed exchangeable with future test points — the only
// assumption of the coverage proof.
func CalibrateSplit(predictions, targets []float64, alpha float64) (SplitCalibration, error) {
	if len(predictions) != len(targets) {
		return SplitCalibration{}, fmt.Errorf(
			"conformal: predictions and targets must be the same length; got %d vs %d",
			len(predictions), len(targets))
	}
	residuals := make([]float64, len(predictions))
	for i := range predictions {
		residuals[i] = AbsoluteResidualScore(predictions[i], targets[i])
	}
	cal := SplitCalibration{Residuals: residuals, Alpha: alpha}
	if err := cal.Validate(); err != nil {
		return SplitCalibration{}, err
	}
	return cal, nil
}

// sidecar is the on-disk JSON schema written by
// vmaftune.conformal.save_split_calibration.
type sidecar struct {
	Method    string    `json:"method"`
	Alpha     float64   `json:"alpha"`
	N         int       `json:"n"`
	Residuals []float64 `json:"residuals"`
}

// LoadSplitCalibration reads a split-conformal sidecar from disk.
func LoadSplitCalibration(path string) (SplitCalibration, error) {
	// #nosec G304 -- path comes from an operator-supplied CLI flag.
	data, err := os.ReadFile(path)
	if err != nil {
		return SplitCalibration{}, fmt.Errorf("conformal: read sidecar %q: %w", path, err)
	}
	var doc sidecar
	if unmarshalErr := json.Unmarshal(data, &doc); unmarshalErr != nil {
		return SplitCalibration{}, fmt.Errorf(
			"conformal: parse sidecar %q: %w", path, unmarshalErr)
	}
	cal := SplitCalibration{Residuals: doc.Residuals, Alpha: doc.Alpha}
	if validateErr := cal.Validate(); validateErr != nil {
		return SplitCalibration{}, fmt.Errorf("conformal: sidecar %q: %w", path, validateErr)
	}
	return cal, nil
}

// clamp bounds v to [lo, hi].
func clamp(v, lo, hi float64) float64 {
	return math.Max(lo, math.Min(hi, v))
}
