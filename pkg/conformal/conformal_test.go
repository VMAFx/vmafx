// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/conformal/conformal_test.go — table-driven tests for the Go port of
// tools/vmaf-tune/src/vmaftune/conformal.py.
//
// Every "want" value in the quantile / sidecar tables was produced by running
// the Python original, e.g.:
//
//	PYTHONPATH=tools/vmaf-tune/src python3 -c "
//	from vmaftune.conformal import SplitConformalCalibration
//	c = SplitConformalCalibration(residuals=(0.5, 1.25, 2.0, 0.125), alpha=0.1)
//	print(c.to_json()); print(repr(c.quantile()))"
//
// so the two implementations are pinned to the same numbers and the same
// sidecar bytes for the duration of the migration (ADR-0703 / ADR-0705).

package conformal

import (
	"encoding/json"
	"errors"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestEmpiricalQuantile pins the type-7 (Hyndman-Fan) quantile against
// numpy's np.quantile(..., method="linear").
func TestEmpiricalQuantile(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		values  []float64
		q       float64
		want    float64
		wantErr bool
	}{
		{name: "single element ignores q", values: []float64{3.0}, q: 0.5, want: 3.0},
		{name: "median of four interpolates", values: []float64{0.5, 1.25, 2.0, 0.125}, q: 0.5, want: 0.875},
		{name: "upper quartile interpolates", values: []float64{0.5, 1.25, 2.0, 0.125}, q: 0.75, want: 1.4375},
		{name: "q=1 is the max", values: []float64{0.5, 1.25, 2.0, 0.125}, q: 1.0, want: 2.0},
		{name: "q=0 is the min", values: []float64{0.5, 1.25, 2.0, 0.125}, q: 0.0, want: 0.125},
		{name: "exact index lands on element", values: []float64{0, 1, 2, 3, 4}, q: 0.5, want: 2.0},
		{name: "unsorted input is sorted first", values: []float64{4, 0, 3, 1, 2}, q: 0.25, want: 1.0},
		{name: "empty sample errors", values: nil, q: 0.5, wantErr: true},
		{name: "q above 1 errors", values: []float64{1}, q: 1.5, wantErr: true},
		{name: "q below 0 errors", values: []float64{1}, q: -0.1, wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := EmpiricalQuantile(tc.values, tc.q)
			if tc.wantErr {
				if err == nil {
					t.Fatalf("EmpiricalQuantile(%v, %v): want error, got %v", tc.values, tc.q, got)
				}
				return
			}
			if err != nil {
				t.Fatalf("EmpiricalQuantile(%v, %v): unexpected error %v", tc.values, tc.q, err)
			}
			if math.Abs(got-tc.want) > 1e-12 {
				t.Errorf("EmpiricalQuantile(%v, %v) = %v, want %v", tc.values, tc.q, got, tc.want)
			}
		})
	}
}

// TestEmpiricalQuantileDoesNotMutateInput guards the defensive copy: callers
// hand us the calibration's own residual slice, and sorting it in place would
// silently reorder the stored calibration.
func TestEmpiricalQuantileDoesNotMutateInput(t *testing.T) {
	t.Parallel()

	values := []float64{4, 0, 3, 1, 2}
	if _, err := EmpiricalQuantile(values, 0.5); err != nil {
		t.Fatalf("EmpiricalQuantile: %v", err)
	}
	want := []float64{4, 0, 3, 1, 2}
	for i := range want {
		if values[i] != want[i] {
			t.Fatalf("input was mutated: got %v, want %v", values, want)
		}
	}
}

// TestSplitCalibrationQuantile pins the finite-sample-corrected split-conformal
// quantile (Lei 2018 §2.2) against the Python implementation.
func TestSplitCalibrationQuantile(t *testing.T) {
	t.Parallel()

	// A 100-point residual ramp: residual[i] = i/100. The corrected level for
	// n=100, alpha=0.05 is ceil(101*0.95)/100 = 96/100 = 0.96, so the type-7
	// quantile is residual[95.04] = 0.95 + 0.04*(0.96-0.95) = 0.9504.
	ramp := make([]float64, 100)
	for i := range ramp {
		ramp[i] = float64(i) / 100.0
	}

	tests := []struct {
		name      string
		residuals []float64
		alpha     float64
		want      float64
		wantEmpty bool
	}{
		{
			name:      "small n saturates the corrected level at 1.0 -> max residual",
			residuals: []float64{0.5, 1.25, 2.0, 0.125},
			alpha:     0.1,
			want:      2.0,
		},
		{
			name:      "n=1 returns the sole residual",
			residuals: []float64{1.75},
			alpha:     DefaultAlpha,
			want:      1.75,
		},
		{
			name:      "n=100 alpha=0.05 uses level 0.96",
			residuals: ramp,
			alpha:     DefaultAlpha,
			want:      0.9504,
		},
		{
			name:      "empty calibration degrades to width 0",
			residuals: nil,
			alpha:     DefaultAlpha,
			want:      0.0,
			wantEmpty: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			cal, err := NewSplitCalibration(tc.residuals, tc.alpha)
			if err != nil {
				t.Fatalf("NewSplitCalibration: %v", err)
			}
			got, qErr := cal.Quantile()
			if tc.wantEmpty {
				if !errors.Is(qErr, ErrEmptyCalibration) {
					t.Fatalf("Quantile on empty calibration: want ErrEmptyCalibration, got %v", qErr)
				}
			} else if qErr != nil {
				t.Fatalf("Quantile: %v", qErr)
			}
			if math.Abs(got-tc.want) > 1e-12 {
				t.Errorf("Quantile() = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestNewSplitCalibrationValidation covers the constructor's contract checks.
func TestNewSplitCalibrationValidation(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		residuals []float64
		alpha     float64
		wantErr   bool
		wantAlpha float64
	}{
		{name: "alpha 0 selects the default", alpha: 0, wantAlpha: DefaultAlpha},
		{name: "alpha in range is kept", alpha: 0.2, wantAlpha: 0.2},
		{name: "alpha 1 rejected", alpha: 1.0, wantErr: true},
		{name: "alpha negative rejected", alpha: -0.1, wantErr: true},
		{name: "negative residual rejected", residuals: []float64{-1}, alpha: 0.05, wantErr: true},
		{name: "NaN residual rejected", residuals: []float64{math.NaN()}, alpha: 0.05, wantErr: true},
		{name: "Inf residual rejected", residuals: []float64{math.Inf(1)}, alpha: 0.05, wantErr: true},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			cal, err := NewSplitCalibration(tc.residuals, tc.alpha)
			if tc.wantErr {
				if err == nil {
					t.Fatal("want error, got nil")
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if cal.Alpha() != tc.wantAlpha {
				t.Errorf("Alpha() = %v, want %v", cal.Alpha(), tc.wantAlpha)
			}
		})
	}
}

// TestSplitCalibrationDefensiveCopy verifies the constructor copies the caller's
// slice so later caller-side mutation cannot corrupt a stored calibration.
func TestSplitCalibrationDefensiveCopy(t *testing.T) {
	t.Parallel()

	src := []float64{1.0, 2.0}
	cal, err := NewSplitCalibration(src, DefaultAlpha)
	if err != nil {
		t.Fatalf("NewSplitCalibration: %v", err)
	}
	src[0] = 99.0
	if got := cal.Residuals(); got[0] != 1.0 {
		t.Errorf("calibration aliased the caller slice: residuals[0] = %v, want 1", got[0])
	}
	// The accessor must also hand back a copy.
	out := cal.Residuals()
	out[1] = 42.0
	if again := cal.Residuals(); again[1] != 2.0 {
		t.Errorf("Residuals() aliased internal state: residuals[1] = %v, want 2", again[1])
	}
}

// TestCalibrateSplit checks the (predictions, targets) factory.
func TestCalibrateSplit(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name        string
		predictions []float64
		targets     []float64
		wantErr     bool
		want        []float64
	}{
		{
			name:        "absolute residuals",
			predictions: []float64{90, 80, 70},
			targets:     []float64{92, 78, 70},
			want:        []float64{2, 2, 0},
		},
		{
			name:        "length mismatch rejected",
			predictions: []float64{1, 2},
			targets:     []float64{1},
			wantErr:     true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			cal, err := CalibrateSplit(tc.predictions, tc.targets, DefaultAlpha)
			if tc.wantErr {
				if err == nil {
					t.Fatal("want error, got nil")
				}
				return
			}
			if err != nil {
				t.Fatalf("CalibrateSplit: %v", err)
			}
			got := cal.Residuals()
			if len(got) != len(tc.want) {
				t.Fatalf("residual count = %d, want %d", len(got), len(tc.want))
			}
			for i := range tc.want {
				if math.Abs(got[i]-tc.want[i]) > 1e-12 {
					t.Errorf("residual[%d] = %v, want %v", i, got[i], tc.want[i])
				}
			}
		})
	}
}

// TestSplitCalibrationSidecarBytes pins the JSON sidecar byte-for-byte against
// Python's json.dumps(..., sort_keys=True) output. Both implementations read
// the same calibration.json during the migration, so the key order and the
// float formatting have to agree.
func TestSplitCalibrationSidecarBytes(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		residuals []float64
		alpha     float64
		want      string
	}{
		{
			name:      "populated calibration",
			residuals: []float64{0.5, 1.25, 2.0, 0.125},
			alpha:     0.1,
			want:      `{"alpha":0.1,"method":"split-conformal","n":4,"residuals":[0.5,1.25,2,0.125]}`,
		},
		{
			name:      "empty calibration keeps an empty array, not null",
			residuals: nil,
			alpha:     DefaultAlpha,
			want:      `{"alpha":0.05,"method":"split-conformal","n":0,"residuals":[]}`,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			cal, err := NewSplitCalibration(tc.residuals, tc.alpha)
			if err != nil {
				t.Fatalf("NewSplitCalibration: %v", err)
			}
			got, err := json.Marshal(cal)
			if err != nil {
				t.Fatalf("Marshal: %v", err)
			}
			if string(got) != tc.want {
				t.Errorf("sidecar bytes:\n got %s\nwant %s", got, tc.want)
			}
		})
	}
}

// TestSplitCalibrationUnmarshal covers reading a sidecar, including the
// method-discriminator guard.
func TestSplitCalibrationUnmarshal(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		payload string
		wantErr bool
		wantN   int
		wantA   float64
	}{
		{
			name:    "python-shaped sidecar round-trips",
			payload: `{"alpha": 0.1, "method": "split-conformal", "n": 4, "residuals": [0.5, 1.25, 2.0, 0.125]}`,
			wantN:   4,
			wantA:   0.1,
		},
		{
			name:    "wrong method rejected",
			payload: `{"alpha": 0.1, "method": "cv-plus", "n": 0, "residuals": []}`,
			wantErr: true,
		},
		{
			name:    "malformed JSON rejected",
			payload: `{`,
			wantErr: true,
		},
		{
			name:    "negative residual rejected",
			payload: `{"alpha": 0.1, "method": "split-conformal", "n": 1, "residuals": [-1]}`,
			wantErr: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			var cal SplitCalibration
			err := json.Unmarshal([]byte(tc.payload), &cal)
			if tc.wantErr {
				if err == nil {
					t.Fatal("want error, got nil")
				}
				return
			}
			if err != nil {
				t.Fatalf("Unmarshal: %v", err)
			}
			if cal.N() != tc.wantN {
				t.Errorf("N() = %d, want %d", cal.N(), tc.wantN)
			}
			if cal.Alpha() != tc.wantA {
				t.Errorf("Alpha() = %v, want %v", cal.Alpha(), tc.wantA)
			}
		})
	}
}

// TestSplitCalibrationFileRoundTrip drives Save -> Load through the filesystem
// and checks the trailing newline the Python writer emits.
func TestSplitCalibrationFileRoundTrip(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	path := filepath.Join(dir, "nested", "calibration.json")

	cal, err := NewSplitCalibration([]float64{0.25, 0.75}, 0.2)
	if err != nil {
		t.Fatalf("NewSplitCalibration: %v", err)
	}
	if err := SaveSplitCalibration(cal, path); err != nil {
		t.Fatalf("SaveSplitCalibration: %v", err)
	}

	raw, err := os.ReadFile(path) //nolint:gosec // test-local temp path
	if err != nil {
		t.Fatalf("read sidecar: %v", err)
	}
	if n := len(raw); n == 0 || raw[n-1] != '\n' {
		t.Errorf("sidecar must end with a newline (Python parity); got %q", raw)
	}

	loaded, err := LoadSplitCalibration(path)
	if err != nil {
		t.Fatalf("LoadSplitCalibration: %v", err)
	}
	if loaded.N() != 2 || loaded.Alpha() != 0.2 {
		t.Errorf("round-trip lost state: n=%d alpha=%v", loaded.N(), loaded.Alpha())
	}
	q1, _ := cal.Quantile()
	q2, _ := loaded.Quantile()
	if q1 != q2 {
		t.Errorf("round-trip changed quantile: %v -> %v", q1, q2)
	}
}

// TestLoadSplitCalibrationMissingFile checks the read-error path.
func TestLoadSplitCalibrationMissingFile(t *testing.T) {
	t.Parallel()

	if _, err := LoadSplitCalibration(filepath.Join(t.TempDir(), "nope.json")); err == nil {
		t.Fatal("want error for a missing sidecar, got nil")
	}
}

// TestCVPlusCalibration covers the jackknife+ estimator.
func TestCVPlusCalibration(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name          string
		foldPreds     [][]float64
		foldTargets   [][]float64
		alpha         float64
		wantErr       bool
		wantN         int
		wantResiduals []float64
		wantQuantile  float64
		wantEmpty     bool
	}{
		{
			name:          "two folds flatten in fold order",
			foldPreds:     [][]float64{{90, 80}, {70}},
			foldTargets:   [][]float64{{92, 78}, {73}},
			alpha:         0.1,
			wantN:         3,
			wantResiduals: []float64{2, 2, 3},
			// n=3, alpha=0.1 -> level = min(1, ceil(4*0.9)/3) = min(1, 4/3) = 1
			// -> max residual.
			wantQuantile: 3,
		},
		{
			name:         "empty folds degrade to width 0",
			foldPreds:    [][]float64{},
			foldTargets:  [][]float64{},
			alpha:        0.05,
			wantN:        0,
			wantEmpty:    true,
			wantQuantile: 0,
		},
		{
			name:        "K mismatch rejected",
			foldPreds:   [][]float64{{1}},
			foldTargets: [][]float64{},
			alpha:       0.05,
			wantErr:     true,
		},
		{
			name:        "per-fold length mismatch rejected",
			foldPreds:   [][]float64{{1, 2}},
			foldTargets: [][]float64{{1}},
			alpha:       0.05,
			wantErr:     true,
		},
		{
			name:        "alpha out of range rejected",
			foldPreds:   [][]float64{{1}},
			foldTargets: [][]float64{{1}},
			alpha:       1.5,
			wantErr:     true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			cal, err := CalibrateCVPlus(tc.foldPreds, tc.foldTargets, tc.alpha)
			if tc.wantErr {
				if err == nil {
					t.Fatal("want error, got nil")
				}
				return
			}
			if err != nil {
				t.Fatalf("CalibrateCVPlus: %v", err)
			}
			if cal.N() != tc.wantN {
				t.Errorf("N() = %d, want %d", cal.N(), tc.wantN)
			}
			if cal.IsEmpty() != tc.wantEmpty {
				t.Errorf("IsEmpty() = %v, want %v", cal.IsEmpty(), tc.wantEmpty)
			}
			got := cal.PerPointResiduals()
			if len(got) != len(tc.wantResiduals) {
				t.Fatalf("residual count = %d, want %d", len(got), len(tc.wantResiduals))
			}
			for i := range tc.wantResiduals {
				if math.Abs(got[i]-tc.wantResiduals[i]) > 1e-12 {
					t.Errorf("residual[%d] = %v, want %v", i, got[i], tc.wantResiduals[i])
				}
			}
			q, qErr := cal.Quantile()
			if tc.wantEmpty && !errors.Is(qErr, ErrEmptyCalibration) {
				t.Errorf("Quantile on empty CV+: want ErrEmptyCalibration, got %v", qErr)
			}
			if math.Abs(q-tc.wantQuantile) > 1e-12 {
				t.Errorf("Quantile() = %v, want %v", q, tc.wantQuantile)
			}
		})
	}
}

// fakePredictor is a PointPredictor stub returning a fixed value (or error).
type fakePredictor struct {
	value float64
	err   error
}

func (f fakePredictor) PredictVMAF(_ []float64, _ int, _ string) (float64, error) {
	return f.value, f.err
}

// TestPredictorPredict covers the interval construction, the clamp, and the
// uncalibrated degraded path.
func TestPredictorPredict(t *testing.T) {
	t.Parallel()

	cal, err := NewSplitCalibration([]float64{2.0, 2.0, 2.0}, 0.1)
	if err != nil {
		t.Fatalf("NewSplitCalibration: %v", err)
	}
	emptyCal, err := NewSplitCalibration(nil, 0.1)
	if err != nil {
		t.Fatalf("NewSplitCalibration: %v", err)
	}

	tests := []struct {
		name      string
		predictor Predictor
		wantPoint float64
		wantLow   float64
		wantHigh  float64
		wantNaNA  bool
		wantErr   bool
		wantEmpty bool
	}{
		{
			name:      "calibrated interval is symmetric around the point",
			predictor: Predictor{Base: fakePredictor{value: 88.0}, Calibration: cal},
			wantPoint: 88.0,
			wantLow:   86.0,
			wantHigh:  90.0,
		},
		{
			name:      "interval clamps to the VMAF ceiling",
			predictor: Predictor{Base: fakePredictor{value: 99.5}, Calibration: cal},
			wantPoint: 99.5,
			wantLow:   97.5,
			wantHigh:  100.0,
		},
		{
			name:      "interval clamps to the VMAF floor",
			predictor: Predictor{Base: fakePredictor{value: 1.0}, Calibration: cal},
			wantPoint: 1.0,
			wantLow:   0.0,
			wantHigh:  3.0,
		},
		{
			name:      "nil calibration collapses the interval and flags alpha NaN",
			predictor: Predictor{Base: fakePredictor{value: 75.0}},
			wantPoint: 75.0,
			wantLow:   75.0,
			wantHigh:  75.0,
			wantNaNA:  true,
			wantEmpty: true,
		},
		{
			name:      "empty calibration behaves like nil",
			predictor: Predictor{Base: fakePredictor{value: 75.0}, Calibration: emptyCal},
			wantPoint: 75.0,
			wantLow:   75.0,
			wantHigh:  75.0,
			wantNaNA:  true,
			wantEmpty: true,
		},
		{
			name:      "base predictor error propagates",
			predictor: Predictor{Base: fakePredictor{err: errors.New("boom")}, Calibration: cal},
			wantErr:   true,
		},
		{
			name:      "nil base is rejected",
			predictor: Predictor{Calibration: cal},
			wantErr:   true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			p := tc.predictor
			iv, err := p.Predict(nil, 23, "libx264")
			switch {
			case tc.wantErr:
				if err == nil {
					t.Fatal("want error, got nil")
				}
				return
			case tc.wantEmpty:
				if !errors.Is(err, ErrEmptyCalibration) {
					t.Fatalf("want ErrEmptyCalibration, got %v", err)
				}
			case err != nil:
				t.Fatalf("unexpected error: %v", err)
			}
			if iv.Point != tc.wantPoint {
				t.Errorf("Point = %v, want %v", iv.Point, tc.wantPoint)
			}
			if iv.Low != tc.wantLow {
				t.Errorf("Low = %v, want %v", iv.Low, tc.wantLow)
			}
			if iv.High != tc.wantHigh {
				t.Errorf("High = %v, want %v", iv.High, tc.wantHigh)
			}
			if tc.wantNaNA && !math.IsNaN(iv.Alpha) {
				t.Errorf("Alpha = %v, want NaN for an uncalibrated wrapper", iv.Alpha)
			}
			if !tc.wantNaNA && iv.Alpha != 0.1 {
				t.Errorf("Alpha = %v, want 0.1", iv.Alpha)
			}
			if got := iv.Width(); math.Abs(got-(tc.wantHigh-tc.wantLow)) > 1e-12 {
				t.Errorf("Width() = %v, want %v", got, tc.wantHigh-tc.wantLow)
			}
		})
	}
}

// TestIntervalMarshalJSON pins the CLI wire schema.
func TestIntervalMarshalJSON(t *testing.T) {
	t.Parallel()

	iv := Interval{Point: 88.5, Low: 86.5, High: 90.5, Alpha: 0.05}
	got, err := json.Marshal(iv)
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	want := `{"point":88.5,"interval":{"low":86.5,"high":90.5,"alpha":0.05}}`
	if string(got) != want {
		t.Errorf("interval JSON:\n got %s\nwant %s", got, want)
	}
}

// TestPredictorCoverageProbe covers the stale-calibration diagnostic.
func TestPredictorCoverageProbe(t *testing.T) {
	t.Parallel()

	// alpha = 0.1 -> nominal coverage 0.90. Residuals are all 2.0, and the
	// corrected level saturates at 1.0, so q = 2.0.
	cal, err := NewSplitCalibration([]float64{2.0, 2.0, 2.0}, 0.1)
	if err != nil {
		t.Fatalf("NewSplitCalibration: %v", err)
	}

	tests := []struct {
		name        string
		calibration Calibration
		predictions []float64
		targets     []float64
		wantCover   float64
		wantNaN     bool
		wantStale   bool
		wantErr     bool
	}{
		{
			name:        "full coverage is not stale",
			calibration: cal,
			predictions: []float64{80, 80, 80, 80},
			targets:     []float64{81, 79, 82, 78},
			wantCover:   1.0,
		},
		{
			name:        "coverage 0.75 is 15pp below nominal -> stale",
			calibration: cal,
			predictions: []float64{80, 80, 80, 80},
			targets:     []float64{81, 79, 82, 90},
			wantCover:   0.75,
			wantStale:   true,
		},
		{
			name:        "coverage 0.875 is 2.5pp below nominal -> not stale",
			calibration: cal,
			predictions: []float64{80, 80, 80, 80, 80, 80, 80, 80},
			targets:     []float64{81, 79, 82, 78, 81, 79, 82, 90},
			wantCover:   0.875,
		},
		{
			name:        "uncalibrated probe returns NaN",
			calibration: nil,
			predictions: []float64{80},
			targets:     []float64{80},
			wantNaN:     true,
		},
		{
			name:        "empty probe returns NaN without error",
			calibration: cal,
			predictions: nil,
			targets:     nil,
			wantNaN:     true,
		},
		{
			name:        "length mismatch is an error",
			calibration: cal,
			predictions: []float64{80, 80},
			targets:     []float64{80},
			wantNaN:     true,
			wantErr:     true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			p := Predictor{Base: fakePredictor{value: 80}, Calibration: tc.calibration}
			cover, err := p.CoverageProbe(tc.predictions, tc.targets)

			var stale *StaleCalibrationError
			isStale := errors.As(err, &stale)
			if isStale != tc.wantStale {
				t.Errorf("stale = %v (err %v), want %v", isStale, err, tc.wantStale)
			}
			if tc.wantStale && stale.GapPP <= p.staleThreshold() {
				t.Errorf("StaleCalibrationError.GapPP = %v, want > %v", stale.GapPP, p.staleThreshold())
			}
			if tc.wantErr && err == nil {
				t.Error("want an error, got nil")
			}
			if tc.wantNaN {
				if !math.IsNaN(cover) {
					t.Errorf("coverage = %v, want NaN", cover)
				}
				return
			}
			if math.Abs(cover-tc.wantCover) > 1e-12 {
				t.Errorf("coverage = %v, want %v", cover, tc.wantCover)
			}
		})
	}
}

// TestStaleCalibrationErrorMessage checks the diagnostic string carries the
// numbers an operator needs to decide whether to re-calibrate.
func TestStaleCalibrationErrorMessage(t *testing.T) {
	t.Parallel()

	err := &StaleCalibrationError{Coverage: 0.75, Nominal: 0.9, GapPP: 15.0, Alpha: 0.1}
	got := err.Error()
	for _, want := range []string{"0.750", "15.0 pp", "0.900", "alpha=0.1"} {
		if !strings.Contains(got, want) {
			t.Errorf("error message %q missing %q", got, want)
		}
	}
}

// TestPredictorDefaults verifies the zero-value clamp / threshold defaults.
func TestPredictorDefaults(t *testing.T) {
	t.Parallel()

	p := Predictor{Base: fakePredictor{value: 50}}
	if p.floor() != 0.0 {
		t.Errorf("default floor = %v, want 0", p.floor())
	}
	if p.ceiling() != 100.0 {
		t.Errorf("default ceiling = %v, want 100", p.ceiling())
	}
	if p.staleThreshold() != DefaultStaleThresholdPP {
		t.Errorf("default stale threshold = %v, want %v", p.staleThreshold(), DefaultStaleThresholdPP)
	}

	// Explicit overrides win.
	q := Predictor{VMAFCeiling: 5, StaleThresholdPP: 1}
	if q.ceiling() != 5 || q.staleThreshold() != 1 {
		t.Errorf("overrides ignored: ceiling=%v threshold=%v", q.ceiling(), q.staleThreshold())
	}
}
