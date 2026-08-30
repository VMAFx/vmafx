// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package conformal_test

import (
	"math"
	"os"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/pkg/conformal"
)

// TestEmpiricalQuantile pins the type-7 (Hyndman-Fan) interpolation against
// values produced by numpy's np.quantile(..., method="linear") for the same
// samples — the contract the Python module documents.
func TestEmpiricalQuantile(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		values  []float64
		q       float64
		want    float64
		wantErr bool
	}{
		{"single point ignores q", []float64{4.2}, 0.95, 4.2, false},
		{"median of an odd sample", []float64{1, 2, 3}, 0.5, 2, false},
		{"median of an even sample interpolates", []float64{1, 2, 3, 4}, 0.5, 2.5, false},
		{"minimum", []float64{5, 1, 3}, 0.0, 1, false},
		{"maximum", []float64{5, 1, 3}, 1.0, 5, false},
		{"interpolated 0.9", []float64{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 0.9, 8.1, false},
		{"unsorted input is sorted first", []float64{9, 0, 5}, 0.5, 5, false},
		{"empty sample is undefined", nil, 0.5, 0, true},
		{"q below range", []float64{1, 2}, -0.1, 0, true},
		{"q above range", []float64{1, 2}, 1.1, 0, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := conformal.EmpiricalQuantile(tc.values, tc.q)
			if (err != nil) != tc.wantErr {
				t.Fatalf("EmpiricalQuantile error = %v, wantErr %v", err, tc.wantErr)
			}
			if !tc.wantErr && math.Abs(got-tc.want) > 1e-12 {
				t.Errorf("EmpiricalQuantile = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestQuantile_finiteSampleCorrection pins the ceil((n+1)(1-alpha))/n level
// that keeps marginal coverage >= 1-alpha at small n. At n=3 and alpha=0.05
// the corrected level is min(1, ceil(3.8)/3) = 1.0, i.e. the max residual.
func TestQuantile_finiteSampleCorrection(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		residuals []float64
		alpha     float64
		want      float64
		wantOK    bool
	}{
		{
			name:      "small n saturates to the max residual",
			residuals: []float64{0.5, 1.0, 2.0}, alpha: 0.05,
			want: 2.0, wantOK: true,
		},
		{
			name: "large n interpolates below the max",
			residuals: []float64{
				0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0,
				1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 1.9, 2.0,
			},
			alpha: 0.5,
			// level = ceil(21*0.5)/20 = 11/20 = 0.55; type-7 on 20 points:
			// h = 19*0.55 = 10.45 -> 1.1 + 0.45*(1.2-1.1) = 1.145
			want: 1.145, wantOK: true,
		},
		{
			name:      "empty calibration has no quantile",
			residuals: nil, alpha: 0.05,
			want: 0.0, wantOK: false,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			cal := conformal.SplitCalibration{Residuals: tc.residuals, Alpha: tc.alpha}
			got, ok := cal.Quantile()
			if ok != tc.wantOK {
				t.Fatalf("Quantile ok = %v, want %v", ok, tc.wantOK)
			}
			if math.Abs(got-tc.want) > 1e-9 {
				t.Errorf("Quantile = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestIntervalFor covers the clamping to the VMAF scale and the degraded
// zero-width path an empty calibration must produce.
func TestIntervalFor(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		residuals []float64
		point     float64
		wantLow   float64
		wantHigh  float64
	}{
		{
			name: "symmetric interval", residuals: []float64{1.0, 2.0, 3.0},
			point: 90.0, wantLow: 87.0, wantHigh: 93.0,
		},
		{
			name: "clamped at the top of the scale", residuals: []float64{1.0, 2.0, 5.0},
			point: 98.0, wantLow: 93.0, wantHigh: 100.0,
		},
		{
			name: "clamped at the bottom of the scale", residuals: []float64{1.0, 2.0, 5.0},
			point: 3.0, wantLow: 0.0, wantHigh: 8.0,
		},
		{
			name: "empty calibration degrades to a point", residuals: nil,
			point: 90.0, wantLow: 90.0, wantHigh: 90.0,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			cal := conformal.SplitCalibration{
				Residuals: tc.residuals, Alpha: conformal.DefaultAlpha,
			}
			iv := cal.IntervalFor(tc.point)
			if iv.Point != tc.point {
				t.Errorf("Point = %v, want %v", iv.Point, tc.point)
			}
			if math.Abs(iv.Low-tc.wantLow) > 1e-9 || math.Abs(iv.High-tc.wantHigh) > 1e-9 {
				t.Errorf("interval = [%v, %v], want [%v, %v]",
					iv.Low, iv.High, tc.wantLow, tc.wantHigh)
			}
		})
	}
}

// TestValidate rejects the malformed calibrations the loader must not accept.
func TestValidate(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		residuals []float64
		alpha     float64
		wantErr   bool
	}{
		{"well formed", []float64{0.5, 1.0}, 0.05, false},
		{"alpha at zero", []float64{0.5}, 0.0, true},
		{"alpha at one", []float64{0.5}, 1.0, true},
		{"negative residual", []float64{-0.5}, 0.05, true},
		{"NaN residual", []float64{math.NaN()}, 0.05, true},
		{"infinite residual", []float64{math.Inf(1)}, 0.05, true},
		{"empty residuals are valid but uncalibrated", nil, 0.05, false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			cal := conformal.SplitCalibration{Residuals: tc.residuals, Alpha: tc.alpha}
			if err := cal.Validate(); (err != nil) != tc.wantErr {
				t.Errorf("Validate error = %v, wantErr %v", err, tc.wantErr)
			}
		})
	}
}

// TestCalibrateSplit builds a calibration from prediction/target pairs.
func TestCalibrateSplit(t *testing.T) {
	t.Parallel()

	cal, err := conformal.CalibrateSplit(
		[]float64{90.0, 85.0, 95.0},
		[]float64{91.0, 83.0, 95.5},
		0.05,
	)
	if err != nil {
		t.Fatalf("CalibrateSplit: %v", err)
	}
	want := []float64{1.0, 2.0, 0.5}
	if len(cal.Residuals) != len(want) {
		t.Fatalf("residual count = %d, want %d", len(cal.Residuals), len(want))
	}
	for i := range want {
		if math.Abs(cal.Residuals[i]-want[i]) > 1e-9 {
			t.Errorf("residual[%d] = %v, want %v", i, cal.Residuals[i], want[i])
		}
	}

	if _, mismatchErr := conformal.CalibrateSplit(
		[]float64{1, 2}, []float64{1}, 0.05); mismatchErr == nil {
		t.Error("expected a length-mismatch error")
	}
}

// TestLoadSplitCalibration round-trips the on-disk sidecar schema and covers
// each rejection path.
func TestLoadSplitCalibration(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		payload string
		write   bool
		wantErr bool
		wantN   int
	}{
		{
			name:  "valid sidecar",
			write: true,
			payload: `{"method":"split-conformal","alpha":0.05,"n":3,` +
				`"residuals":[0.5,1.0,2.0]}`,
			wantN: 3,
		},
		{
			name: "missing file", write: false, wantErr: true,
		},
		{
			name: "malformed JSON", write: true, payload: `{"alpha":`, wantErr: true,
		},
		{
			name: "alpha out of range", write: true,
			payload: `{"alpha":1.5,"residuals":[0.5]}`, wantErr: true,
		},
		{
			name: "negative residual", write: true,
			payload: `{"alpha":0.05,"residuals":[-1.0]}`, wantErr: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			path := filepath.Join(t.TempDir(), "calibration.json")
			if tc.write {
				if err := os.WriteFile(path, []byte(tc.payload), 0o600); err != nil {
					t.Fatalf("write sidecar: %v", err)
				}
			}
			cal, err := conformal.LoadSplitCalibration(path)
			if (err != nil) != tc.wantErr {
				t.Fatalf("LoadSplitCalibration error = %v, wantErr %v", err, tc.wantErr)
			}
			if !tc.wantErr && cal.N() != tc.wantN {
				t.Errorf("N = %d, want %d", cal.N(), tc.wantN)
			}
		})
	}
}

// TestWithAlpha covers the --alpha CLI override, which must not mutate the
// loaded calibration.
func TestWithAlpha(t *testing.T) {
	t.Parallel()

	base := conformal.SplitCalibration{Residuals: []float64{1, 2, 3}, Alpha: 0.05}
	overridden := base.WithAlpha(0.5)

	if base.Alpha != 0.05 {
		t.Errorf("WithAlpha mutated the receiver: alpha = %v", base.Alpha)
	}
	if overridden.Alpha != 0.5 {
		t.Errorf("overridden alpha = %v, want 0.5", overridden.Alpha)
	}
}
