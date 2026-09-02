// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package conformal is the Go port of
// tools/vmaf-tune/src/vmaftune/conformal.py — conformal prediction for the
// VMAF predictor.
//
// Conformal prediction (CP) turns any point predictor into an interval
// predictor with a distribution-free, finite-sample coverage guarantee.
// Given a calibration set (X_i, y_i) for i = 1..n whose pairs are
// exchangeable with the test point (X_{n+1}, y_{n+1}), and a base predictor
// f trained on data disjoint from the calibration set, the split-conformal
// prediction interval at miscoverage level alpha is
//
//	C_alpha(X_{n+1}) = [ f(X_{n+1}) - q_{1-alpha}, f(X_{n+1}) + q_{1-alpha} ]
//
// where q_{1-alpha} is the ceil((n+1)*(1-alpha))/n empirical quantile of the
// absolute calibration residuals R_i = |y_i - f(X_i)|. The marginal coverage
// guarantee is P(y_{n+1} in C_alpha(X_{n+1})) >= 1 - alpha, with an upper
// bound of 1 - alpha + 1/(n+1) when the residuals are distinct (Lemma 1,
// Vovk et al. 2005; Theorem 2.2, Lei et al. 2018). The proof relies only on
// exchangeability — there is no Gaussian / i.i.d. assumption on either the
// residuals or the base predictor.
//
// References:
//
//   - Vovk, Gammerman, Shafer (2005), Algorithmic Learning in a Random World,
//     Springer. Chapter 2 establishes the conformal inductive inference
//     framework and proves marginal validity (Proposition 2.2) for the
//     inductive (split) variant under exchangeability.
//   - Lei, G'Sell, Rinaldo, Tibshirani, Wasserman (2018), Distribution-Free
//     Predictive Inference for Regression, JASA 113(523), 1094-1111.
//     Theorem 2.2 states the 1-alpha lower / 1-alpha+1/(n+1) upper coverage
//     bracket for split conformal.
//   - Romano, Patterson, Candes (2019), Conformalized Quantile Regression,
//     NeurIPS. Section 3 proves the analogous coverage bracket for the
//     normalised / locally-weighted residual score.
//   - Barber, Candes, Ramdas, Tibshirani (2021), Predictive Inference with the
//     Jackknife+, Annals of Statistics 49(1), 486-507. Theorem 1 proves the
//     CV+ / jackknife+ variant attains 1-2*alpha worst-case coverage with no
//     holdout split.
//
// The package ships two estimators:
//
//   - SplitCalibration — the Lei 2018 form. Requires a calibration set
//     disjoint from training. Cheap, the 1-alpha bound is tight, and the
//     produced quantile is a single scalar.
//   - CVPlusCalibration — the Barber 2021 jackknife+ / CV+ form. Cycles K
//     folds; every training point doubles as a calibration point. Coverage
//     degrades to 1-2*alpha (still distribution-free), useful when the
//     calibration corpus is too small to afford a holdout split.
//
// Predictor adapts a point predictor to an interval-returning interface
// without touching the underlying ONNX model or the analytical fallback.
// Calibration is opt-in and persists as a small JSON sidecar
// (calibration.json) next to the model file, byte-compatible with the
// Python SplitConformalCalibration.to_json() output.
//
// # Divergence from the Python original
//
// Python signals an empty / stale calibration through
// warnings.warn(MiscalibrationWarning). Go has no warning channel, so the
// equivalent conditions are returned as *advisory* errors — ErrEmptyCalibration
// and StaleCalibrationError — alongside a fully-usable value. Callers that
// want the Python "suppress the warning and continue" behaviour simply ignore
// the returned error; the numeric result is identical either way.
package conformal

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
)

// DefaultAlpha is the default nominal miscoverage level. alpha = 0.05
// corresponds to a 95 % prediction interval — the convention adopted by
// ADR-0279 (deep-ensemble + conformal scaffold) and by the
// `vmaf-tune --quality-confidence` consumer.
const DefaultAlpha = 0.05

// DefaultStaleThresholdPP is the default empirical-coverage shortfall, in
// percentage points below nominal, past which a calibration is reported
// stale by Predictor.CoverageProbe.
const DefaultStaleThresholdPP = 5.0

// SplitMethod is the "method" discriminator written into (and required by)
// the split-conformal JSON sidecar.
const SplitMethod = "split-conformal"

// ErrEmptyCalibration is the advisory error returned by Quantile when the
// calibration set has zero points. The returned quantile is 0.0, so
// predictions degrade to deterministic point estimates with width 0 — the
// same behaviour as the Python MiscalibrationWarning path. Callers that
// deliberately opt into the no-interval mode ignore this error.
var ErrEmptyCalibration = errors.New("conformal: calibration set is empty; " +
	"predictions degrade to deterministic point estimates with width=0")

// StaleCalibrationError reports that a coverage probe measured empirical
// coverage more than StaleThresholdPP percentage points below nominal.
// The probe result is still returned; the operator is expected to
// re-calibrate.
type StaleCalibrationError struct {
	// Coverage is the measured empirical coverage in [0, 1].
	Coverage float64
	// Nominal is 1 - alpha.
	Nominal float64
	// GapPP is (Nominal - Coverage) * 100, in percentage points.
	GapPP float64
	// Alpha is the calibration's nominal miscoverage level.
	Alpha float64
}

func (e *StaleCalibrationError) Error() string {
	return fmt.Sprintf("conformal: calibration appears stale: empirical coverage "+
		"%.3f is %.1f pp below nominal %.3f (alpha=%v)",
		e.Coverage, e.GapPP, e.Nominal, e.Alpha)
}

// Calibration is the behaviour shared by SplitCalibration and
// CVPlusCalibration: expose a half-width quantile plus the metadata the
// Predictor needs to build an interval.
type Calibration interface {
	// Quantile returns the interval half-width q_{1-alpha}. An empty
	// calibration returns (0, ErrEmptyCalibration).
	Quantile() (float64, error)
	// Alpha returns the nominal miscoverage level.
	Alpha() float64
	// N returns the calibration-set size.
	N() int
	// IsEmpty reports whether the calibration set has zero points.
	IsEmpty() bool
}

// AbsoluteResidualScore is the default conformity / non-conformity score:
// |target - prediction|. Larger values mean a worse fit. Conformal prediction
// theory accepts any score function invariant under permutation of the
// calibration set; the absolute residual is the simplest choice and the one
// analysed in Lei et al. (2018) Theorem 2.2.
func AbsoluteResidualScore(prediction, target float64) float64 {
	return math.Abs(target - prediction)
}

// EmpiricalQuantile computes the type-7 (Hyndman-Fan) empirical quantile.
// It matches numpy's np.quantile(..., method="linear") for any non-empty
// input, and therefore the Python _empirical_quantile helper exactly.
// Returns an error on empty input or on q outside [0, 1].
func EmpiricalQuantile(values []float64, q float64) (float64, error) {
	if len(values) == 0 {
		return 0, errors.New("conformal: empirical quantile of an empty sample is undefined")
	}
	if !(q >= 0.0 && q <= 1.0) {
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

// correctedLevel is the finite-sample-corrected quantile level
// min(1, ceil((n+1) * (1-alpha)) / n) per Lei et al. 2018 §2.2 /
// Romano 2019 §3, so the marginal coverage >= 1-alpha survives at small n.
func correctedLevel(n int, alpha float64) float64 {
	return math.Min(1.0, math.Ceil(float64(n+1)*(1.0-alpha))/float64(n))
}

// ---------------------------------------------------------------------
// Split conformal — single-quantile, simplest case.
// ---------------------------------------------------------------------

// SplitCalibration holds split-conformal calibration state: the per-point
// absolute residuals from a held-out calibration set, plus the nominal
// miscoverage level. Quantile returns the q_{1-alpha} that yields the
// symmetric interval [point - q, point + q].
//
// Per Lei et al. (2018) Theorem 2.2, q is the ceil((n+1)*(1-alpha))/n
// quantile of the residuals when they are distinct; Vovk et al. (2005)
// Proposition 2.2 (split variant) is the more general statement that admits
// ties via the rank-based score.
type SplitCalibration struct {
	residuals []float64
	alpha     float64
}

// NewSplitCalibration validates residuals and alpha and returns the
// calibration. Residuals must be non-negative and finite; alpha must be in
// the open interval (0, 1). Pass alpha <= 0 to select DefaultAlpha.
func NewSplitCalibration(residuals []float64, alpha float64) (*SplitCalibration, error) {
	if alpha == 0 {
		alpha = DefaultAlpha
	}
	if !(alpha > 0.0 && alpha < 1.0) {
		return nil, fmt.Errorf("conformal: alpha must be in (0, 1); got %v", alpha)
	}
	for _, r := range residuals {
		if r < 0.0 || math.IsNaN(r) || math.IsInf(r, 0) {
			return nil, fmt.Errorf(
				"conformal: residuals must be non-negative finite floats; got %v", r)
		}
	}
	cp := make([]float64, len(residuals))
	copy(cp, residuals)
	return &SplitCalibration{residuals: cp, alpha: alpha}, nil
}

// CalibrateSplit computes a SplitCalibration from a (predictions, targets)
// pair. Both slices must be the same length n; the i-th entry of each is one
// calibration point. The pairs are assumed exchangeable with future test
// points (the only assumption of the coverage proof). If they are not — e.g.
// the calibration corpus is drawn from a different distribution than the test
// corpus — the 1-alpha lower bound no longer holds.
func CalibrateSplit(predictions, targets []float64, alpha float64) (*SplitCalibration, error) {
	if len(predictions) != len(targets) {
		return nil, fmt.Errorf(
			"conformal: predictions and targets must be the same length; got %d vs %d",
			len(predictions), len(targets))
	}
	residuals := make([]float64, len(predictions))
	for i := range predictions {
		residuals[i] = AbsoluteResidualScore(predictions[i], targets[i])
	}
	return NewSplitCalibration(residuals, alpha)
}

// Residuals returns a copy of the calibration residuals.
func (c *SplitCalibration) Residuals() []float64 {
	out := make([]float64, len(c.residuals))
	copy(out, c.residuals)
	return out
}

// Alpha returns the nominal miscoverage level.
func (c *SplitCalibration) Alpha() float64 { return c.alpha }

// N returns the calibration-set size.
func (c *SplitCalibration) N() int { return len(c.residuals) }

// IsEmpty reports whether the calibration set has zero points.
func (c *SplitCalibration) IsEmpty() bool { return c.N() == 0 }

// Quantile returns q_{1-alpha} — the half-width of the interval. For an empty
// calibration set it returns (0, ErrEmptyCalibration) and callers see
// low == high == point.
func (c *SplitCalibration) Quantile() (float64, error) {
	if c.IsEmpty() {
		return 0.0, ErrEmptyCalibration
	}
	return EmpiricalQuantile(c.residuals, correctedLevel(c.N(), c.alpha))
}

// splitSidecar is the on-disk JSON shape. Field order is alphabetical so
// encoding/json reproduces Python's json.dumps(..., sort_keys=True) output
// byte-for-byte — the sidecar is read by both implementations during the
// migration.
type splitSidecar struct {
	Alpha     float64   `json:"alpha"`
	Method    string    `json:"method"`
	N         int       `json:"n"`
	Residuals []float64 `json:"residuals"`
}

// MarshalJSON serialises the calibration to the sidecar shape.
func (c *SplitCalibration) MarshalJSON() ([]byte, error) {
	residuals := c.residuals
	if residuals == nil {
		residuals = []float64{}
	}
	return json.Marshal(splitSidecar{
		Alpha:     c.alpha,
		Method:    SplitMethod,
		N:         c.N(),
		Residuals: residuals,
	})
}

// UnmarshalJSON parses a sidecar produced by MarshalJSON (or by the Python
// SplitConformalCalibration.to_json). A "method" other than
// SplitMethod is rejected.
func (c *SplitCalibration) UnmarshalJSON(data []byte) error {
	var doc splitSidecar
	if err := json.Unmarshal(data, &doc); err != nil {
		return fmt.Errorf("conformal: parse split-conformal sidecar: %w", err)
	}
	if doc.Method != SplitMethod {
		return fmt.Errorf("conformal: sidecar method mismatch: expected %q, got %q",
			SplitMethod, doc.Method)
	}
	parsed, err := NewSplitCalibration(doc.Residuals, doc.Alpha)
	if err != nil {
		return err
	}
	*c = *parsed
	return nil
}

// LoadSplitCalibration reads a split-conformal sidecar from disk.
func LoadSplitCalibration(path string) (*SplitCalibration, error) {
	// #nosec G304 -- path is an operator-supplied calibration sidecar, the
	// same trust level as the model file it sits next to.
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("conformal: read calibration %q: %w", path, err)
	}
	var cal SplitCalibration
	if err := json.Unmarshal(data, &cal); err != nil {
		return nil, err
	}
	return &cal, nil
}

// SaveSplitCalibration writes a split-conformal sidecar to disk, with the
// trailing newline the Python save_split_calibration emits.
func SaveSplitCalibration(cal *SplitCalibration, path string) error {
	data, err := json.Marshal(cal)
	if err != nil {
		return fmt.Errorf("conformal: marshal calibration: %w", err)
	}
	// G301: 0o750 keeps the sidecar directory owner/group-only.
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return fmt.Errorf("conformal: create calibration dir: %w", err)
	}
	// G306: 0o600 — the sidecar records residuals derived from a private
	// corpus; restrict to the owner.
	if err := os.WriteFile(path, append(data, '\n'), 0o600); err != nil {
		return fmt.Errorf("conformal: write calibration %q: %w", path, err)
	}
	return nil
}

// ---------------------------------------------------------------------
// CV+ / jackknife+ conformal — no holdout split, costlier.
// ---------------------------------------------------------------------

// CVPlusCalibration holds cross-validation+ (CV+) calibration state: the
// per-fold leave-out predictions and targets. The coverage guarantee degrades
// to 1-2*alpha (Barber et al. 2021, Theorem 1). The advantage over split
// conformal is that every calibration point also contributes to the model's
// training distribution — no holdout split is wasted — which is the right
// trade when the available labelled corpus is small.
type CVPlusCalibration struct {
	foldPredictions [][]float64
	foldTargets     [][]float64
	alpha           float64
}

// CalibrateCVPlus validates and returns a CV+ calibration. Each entry in
// foldPredictions / foldTargets corresponds to one of the K folds; the i-th
// fold's predictions are the leave-out predictions for the i-th fold's
// training points. Pass alpha <= 0 to select DefaultAlpha.
func CalibrateCVPlus(foldPredictions, foldTargets [][]float64, alpha float64) (*CVPlusCalibration, error) {
	if alpha == 0 {
		alpha = DefaultAlpha
	}
	if !(alpha > 0.0 && alpha < 1.0) {
		return nil, fmt.Errorf("conformal: alpha must be in (0, 1); got %v", alpha)
	}
	if len(foldPredictions) != len(foldTargets) {
		return nil, fmt.Errorf(
			"conformal: foldPredictions and foldTargets must have equal K; got %d vs %d",
			len(foldPredictions), len(foldTargets))
	}
	fp := make([][]float64, len(foldPredictions))
	ft := make([][]float64, len(foldTargets))
	for k := range foldPredictions {
		if len(foldPredictions[k]) != len(foldTargets[k]) {
			return nil, fmt.Errorf("conformal: fold %d length mismatch: %d vs %d",
				k, len(foldPredictions[k]), len(foldTargets[k]))
		}
		fp[k] = append([]float64(nil), foldPredictions[k]...)
		ft[k] = append([]float64(nil), foldTargets[k]...)
	}
	return &CVPlusCalibration{foldPredictions: fp, foldTargets: ft, alpha: alpha}, nil
}

// Alpha returns the nominal miscoverage level.
func (c *CVPlusCalibration) Alpha() float64 { return c.alpha }

// N returns the total calibration-point count across folds.
func (c *CVPlusCalibration) N() int {
	total := 0
	for _, p := range c.foldPredictions {
		total += len(p)
	}
	return total
}

// IsEmpty reports whether the calibration holds zero points.
func (c *CVPlusCalibration) IsEmpty() bool { return c.N() == 0 }

// PerPointResiduals flattens the leave-out residuals across folds, in fold
// order. Exported for diagnostics; Predictor uses it via Quantile.
func (c *CVPlusCalibration) PerPointResiduals() []float64 {
	out := make([]float64, 0, c.N())
	for k := range c.foldPredictions {
		for i := range c.foldPredictions[k] {
			out = append(out, AbsoluteResidualScore(c.foldPredictions[k][i], c.foldTargets[k][i]))
		}
	}
	return out
}

// Quantile returns the conservative q_{1-alpha} half-width so CV+ drops into
// the same plotting code as split conformal. The proper CV+ interval is
// asymmetric and is built at predict time. An empty calibration returns
// (0, ErrEmptyCalibration).
func (c *CVPlusCalibration) Quantile() (float64, error) {
	residuals := c.PerPointResiduals()
	if len(residuals) == 0 {
		return 0.0, ErrEmptyCalibration
	}
	return EmpiricalQuantile(residuals, correctedLevel(len(residuals), c.alpha))
}

// ---------------------------------------------------------------------
// Predictor wrapper — the public entry point.
// ---------------------------------------------------------------------

// Interval is a point estimate plus its prediction interval. The interval is
// closed on both ends. Low and High are clamped to the predictor's [0, 100]
// VMAF range at the call site (see Predictor.Predict); this struct itself does
// not clamp so that residual diagnostics survive intact.
//
// Alpha is NaN when the wrapper is uncalibrated — the downstream signal for
// "point estimate only, no interval".
type Interval struct {
	Point float64
	Low   float64
	High  float64
	Alpha float64
}

// Width returns the interval width High - Low (>= 0).
func (iv Interval) Width() float64 { return iv.High - iv.Low }

// intervalWire is the JSON shape matching the Python ConformalInterval.to_dict
// CLI schema: {"point": .., "interval": {"low": .., "high": .., "alpha": ..}}.
// Field order matches the Python dict insertion order.
type intervalWire struct {
	Point    float64 `json:"point"`
	Interval struct {
		Low   float64 `json:"low"`
		High  float64 `json:"high"`
		Alpha float64 `json:"alpha"`
	} `json:"interval"`
}

// MarshalJSON renders the interval in the CLI schema.
func (iv Interval) MarshalJSON() ([]byte, error) {
	var w intervalWire
	w.Point = iv.Point
	w.Interval.Low = iv.Low
	w.Interval.High = iv.High
	w.Interval.Alpha = iv.Alpha
	return json.Marshal(w)
}

// PointPredictor is the base point-estimate surface the conformal layer
// wraps — anything with a VMAF prediction for one shot at one (crf, codec).
// Mirrors vmaftune.predictor.Predictor.predict_vmaf.
type PointPredictor interface {
	PredictVMAF(features []float64, crf int, codec string) (float64, error)
}

// Predictor wraps a PointPredictor with conformal intervals.
//
// Calibration may be nil, which yields a no-op wrapper returning
// (point, point, point) — the degraded path for a --with-uncertainty run
// that ships no calibration sidecar.
//
// Coverage assumption (carried over from the Calibration): the
// (features, target_vmaf) pairs in the calibration set are exchangeable with
// the test inputs the wrapper sees. Distribution shift breaks the 1-alpha
// lower bound; CoverageProbe surfaces that as a StaleCalibrationError.
type Predictor struct {
	// Base is the underlying point predictor. Required.
	Base PointPredictor
	// Calibration is the conformal calibration state. nil disables intervals.
	Calibration Calibration
	// VMAFFloor / VMAFCeiling clamp the reported interval bounds. The zero
	// value selects [0, 100], matching the VMAF output range; widen when the
	// wrapper is used with non-VMAF regressors.
	VMAFFloor   float64
	VMAFCeiling float64
	// StaleThresholdPP is the coverage shortfall (percentage points below
	// nominal) past which CoverageProbe reports the calibration stale.
	// The zero value selects DefaultStaleThresholdPP.
	StaleThresholdPP float64
}

// floor returns the effective clamp floor.
func (p *Predictor) floor() float64 { return p.VMAFFloor }

// ceiling returns the effective clamp ceiling, defaulting to 100.
func (p *Predictor) ceiling() float64 {
	if p.VMAFCeiling == 0 {
		return 100.0
	}
	return p.VMAFCeiling
}

// staleThreshold returns the effective stale threshold in percentage points.
func (p *Predictor) staleThreshold() float64 {
	if p.StaleThresholdPP == 0 {
		return DefaultStaleThresholdPP
	}
	return p.StaleThresholdPP
}

// Predict returns (point, low, high) for one shot. The point estimate is
// exactly Base.PredictVMAF — the conformal layer never modifies it. Low and
// High are clamped to [VMAFFloor, VMAFCeiling].
//
// When the calibration is nil or empty, the interval collapses to the point
// and Alpha is NaN; ErrEmptyCalibration is returned alongside the usable
// Interval so callers may either surface or ignore it (the Python original
// raises a suppressible MiscalibrationWarning here).
func (p *Predictor) Predict(features []float64, crf int, codec string) (Interval, error) {
	if p.Base == nil {
		return Interval{}, errors.New("conformal: Predictor.Base is nil")
	}
	point, err := p.Base.PredictVMAF(features, crf, codec)
	if err != nil {
		return Interval{}, fmt.Errorf("conformal: base predictor: %w", err)
	}
	if p.Calibration == nil || p.Calibration.IsEmpty() {
		clamped := clamp(point, p.floor(), p.ceiling())
		return Interval{
			Point: point,
			Low:   clamped,
			High:  clamped,
			Alpha: math.NaN(),
		}, ErrEmptyCalibration
	}
	q, qErr := p.Calibration.Quantile()
	if qErr != nil {
		return Interval{}, fmt.Errorf("conformal: quantile: %w", qErr)
	}
	return Interval{
		Point: point,
		Low:   clamp(point-q, p.floor(), p.ceiling()),
		High:  clamp(point+q, p.floor(), p.ceiling()),
		Alpha: p.Calibration.Alpha(),
	}, nil
}

// CoverageProbe returns the empirical coverage of the calibrated interval on a
// probe set: the fraction of probe points whose target falls inside the
// symmetric interval [p - q, p + q].
//
// If the empirical coverage is more than StaleThresholdPP below the nominal
// 1-alpha, the coverage is still returned but paired with a
// *StaleCalibrationError.
//
// The probe must be distinct from the calibration set — re-using the
// calibration set as the probe gives optimistic coverage and defeats the
// diagnostic. An uncalibrated predictor or an empty probe returns NaN.
func (p *Predictor) CoverageProbe(predictions, targets []float64) (float64, error) {
	if p.Calibration == nil || p.Calibration.IsEmpty() {
		return math.NaN(), ErrEmptyCalibration
	}
	if len(predictions) != len(targets) {
		return math.NaN(), errors.New(
			"conformal: predictions and targets length mismatch in CoverageProbe")
	}
	if len(predictions) == 0 {
		return math.NaN(), nil
	}
	q, err := p.Calibration.Quantile()
	if err != nil {
		return math.NaN(), fmt.Errorf("conformal: quantile: %w", err)
	}
	hits := 0
	for i := range predictions {
		if math.Abs(targets[i]-predictions[i]) <= q {
			hits++
		}
	}
	coverage := float64(hits) / float64(len(predictions))
	nominal := 1.0 - p.Calibration.Alpha()
	gapPP := (nominal - coverage) * 100.0
	if gapPP > p.staleThreshold() {
		return coverage, &StaleCalibrationError{
			Coverage: coverage,
			Nominal:  nominal,
			GapPP:    gapPP,
			Alpha:    p.Calibration.Alpha(),
		}
	}
	return coverage, nil
}

// clamp constrains value to [lo, hi].
func clamp(value, lo, hi float64) float64 {
	return math.Max(lo, math.Min(hi, value))
}

// Compile-time checks that both estimators satisfy Calibration.
var (
	_ Calibration = (*SplitCalibration)(nil)
	_ Calibration = (*CVPlusCalibration)(nil)
)

// WithAlpha returns a copy of the calibration re-quantiled at a different
// miscoverage level, leaving the receiver untouched. The residual set is the
// expensive part of a calibration and does not depend on alpha, so re-deriving
// an interval at a different confidence level is a pure re-quantile rather than
// a re-fit.
//
// Added when the group-6 `predict --with-uncertainty` path was merged onto this
// (group-2) conformal implementation: `--alpha` overrides the alpha baked into
// the sidecar at calibration time.
func (c *SplitCalibration) WithAlpha(alpha float64) (*SplitCalibration, error) {
	if c == nil {
		return nil, errors.New("conformal: WithAlpha on a nil calibration")
	}
	return NewSplitCalibration(c.Residuals(), alpha)
}

// IntervalFor centres the calibrated interval on a point prediction.
//
// The half-width is the conformal quantile of the residual set, so the interval
// is symmetric about the prediction. An empty or unusable calibration yields a
// zero-width interval at the point, which callers must report as uncalibrated
// rather than as a coverage guarantee.
func (c *SplitCalibration) IntervalFor(point float64) Interval {
	if c == nil {
		return Interval{Point: point, Low: point, High: point}
	}
	q, err := c.Quantile()
	if err != nil {
		return Interval{Point: point, Low: point, High: point, Alpha: c.Alpha()}
	}
	// Clamp to the predictor's VMAF range, matching conformal.py's
	// _clamp(point, vmaf_floor, vmaf_ceiling) and the group-6 implementation
	// this bridge replaced. Without it `predict --with-uncertainty` reports
	// interval bounds outside [0, 100] for predictions near either end.
	return Interval{
		Point: point,
		Low:   clamp(point-q, 0, 100),
		High:  clamp(point+q, 0, 100),
		Alpha: c.Alpha(),
	}
}
