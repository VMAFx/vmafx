// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// CLI-level tests for the `predict` and `recommend-saliency` subcommands.
// Both need external binaries for their happy path, so these cover the report
// assembly, the flag surface and every rejection that is reachable without
// ffmpeg.

package cmd

import (
	"encoding/json"
	"errors"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/VMAFx/vmafx/pkg/conformal"
	"github.com/VMAFx/vmafx/pkg/pershot"
	"github.com/VMAFx/vmafx/pkg/predictor"
	"github.com/VMAFx/vmafx/pkg/saliency"
)

// mkResiduals builds a validation report with the given signed residuals.
func mkResiduals(residuals ...float64) predictor.ValidationReport {
	rows := make([]predictor.ShotResidual, len(residuals))
	for i, r := range residuals {
		rows[i] = predictor.ShotResidual{
			Shot:          pershot.Shot{StartFrame: i * 100, EndFrame: (i + 1) * 100},
			CRFPicked:     23 + i,
			PredictedVMAF: 93.0,
			MeasuredVMAF:  93.0 + r,
		}
	}
	return predictor.DecideVerdict(rows, 93.0, 1.5)
}

// TestBuildPredictReport_schema pins the emitted JSON key set and the values
// the Python handler produces for the same report.
func TestBuildPredictReport_schema(t *testing.T) {
	t.Parallel()

	report := mkResiduals(0.5, -1.0, 1.4)
	payload, err := buildPredictReport(report, &predictFlags{
		targetVMAF: 93.0, residualThreshold: 1.5, alpha: math.NaN(),
	})
	if err != nil {
		t.Fatalf("buildPredictReport: %v", err)
	}

	blob, marshalErr := json.Marshal(payload)
	if marshalErr != nil {
		t.Fatalf("marshal report: %v", marshalErr)
	}
	var decoded map[string]any
	if unmarshalErr := json.Unmarshal(blob, &decoded); unmarshalErr != nil {
		t.Fatalf("unmarshal report: %v", unmarshalErr)
	}

	wantKeys := []string{
		"verdict", "target_vmaf", "residual_threshold", "max_abs_residual",
		"mean_residual", "bias_correction", "k_validated", "uncertainty",
		"residuals",
	}
	for _, key := range wantKeys {
		if _, ok := decoded[key]; !ok {
			t.Errorf("report is missing the %q key", key)
		}
	}
	if len(decoded) != len(wantKeys) {
		t.Errorf("report has %d keys, want exactly %d", len(decoded), len(wantKeys))
	}
	if decoded["verdict"] != "gospel" {
		t.Errorf("verdict = %v, want gospel", decoded["verdict"])
	}
	if got := decoded["k_validated"].(float64); int(got) != 3 {
		t.Errorf("k_validated = %v, want 3", got)
	}
	if got := decoded["max_abs_residual"].(float64); math.Abs(got-1.4) > 1e-9 {
		t.Errorf("max_abs_residual = %v, want 1.4", got)
	}

	residuals := decoded["residuals"].([]any)
	if len(residuals) != 3 {
		t.Fatalf("residuals = %d entries, want 3", len(residuals))
	}
	first := residuals[0].(map[string]any)
	for _, key := range []string{
		"shot_start", "shot_end", "crf", "predicted_vmaf",
		"measured_vmaf", "residual",
	} {
		if _, ok := first[key]; !ok {
			t.Errorf("residual row is missing the %q key", key)
		}
	}
	// Without --with-uncertainty the interval block must be absent entirely,
	// not present-and-null.
	if _, ok := first["interval"]; ok {
		t.Error("interval should be omitted when --with-uncertainty is off")
	}
	uncertainty := decoded["uncertainty"].(map[string]any)
	if uncertainty["enabled"] != false || uncertainty["calibrated"] != false {
		t.Errorf("uncertainty block = %v, want both flags false", uncertainty)
	}
}

// TestBuildPredictReport_verdicts covers the three-way verdict mapping.
func TestBuildPredictReport_verdicts(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name        string
		residuals   []float64
		wantVerdict string
		wantBias    float64
	}{
		{"tight residuals are gospel", []float64{0.5, -1.0, 1.4}, "gospel", 0},
		{"biased but tight recalibrates", []float64{2.0, 2.2, 2.4}, "recalibrate", 2.2},
		{"wide spread falls back", []float64{-5.0, 0.0, 5.0}, "fall_back", 0},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			payload, err := buildPredictReport(mkResiduals(tc.residuals...),
				&predictFlags{targetVMAF: 93.0, residualThreshold: 1.5, alpha: math.NaN()})
			if err != nil {
				t.Fatalf("buildPredictReport: %v", err)
			}
			if payload.Verdict != tc.wantVerdict {
				t.Errorf("verdict = %q, want %q", payload.Verdict, tc.wantVerdict)
			}
			if math.Abs(payload.BiasCorrection-tc.wantBias) > 1e-9 {
				t.Errorf("bias_correction = %v, want %v",
					payload.BiasCorrection, tc.wantBias)
			}
		})
	}
}

// TestBuildPredictReport_uncertainty covers both interval paths: the
// calibrated one and the degraded zero-width one that must be flagged
// uncalibrated so nobody reads a coverage guarantee into it.
func TestBuildPredictReport_uncertainty(t *testing.T) {
	t.Parallel()

	report := mkResiduals(0.5, -1.0)

	t.Run("uncalibrated intervals are degenerate and flagged", func(t *testing.T) {
		t.Parallel()

		payload, err := buildPredictReport(report, &predictFlags{
			targetVMAF: 93.0, residualThreshold: 1.5,
			withUncertainty: true, alpha: math.NaN(),
		})
		if err != nil {
			t.Fatalf("buildPredictReport: %v", err)
		}
		if !payload.Uncertainty.Enabled || payload.Uncertainty.Calibrated {
			t.Errorf("uncertainty = %+v, want enabled but not calibrated",
				payload.Uncertainty)
		}
		if payload.Uncertainty.Alpha != nil {
			t.Errorf("alpha = %v, want nil without a sidecar", *payload.Uncertainty.Alpha)
		}
		for i, r := range payload.Residuals {
			if r.Interval == nil {
				t.Fatalf("residual %d has no interval", i)
			}
			if r.Interval.Low != r.PredictedVMAF || r.Interval.High != r.PredictedVMAF {
				t.Errorf("residual %d interval = [%v, %v], want a degenerate point",
					i, r.Interval.Low, r.Interval.High)
			}
			if r.Interval.Alpha != nil {
				t.Errorf("residual %d reports alpha %v without a calibration",
					i, *r.Interval.Alpha)
			}
		}
	})

	t.Run("calibrated intervals widen around the point", func(t *testing.T) {
		t.Parallel()

		sidecar := filepath.Join(t.TempDir(), "cal.json")
		body := `{"method":"split-conformal","alpha":0.05,"n":3,` +
			`"residuals":[0.5,1.0,2.0]}`
		if err := os.WriteFile(sidecar, []byte(body), 0o600); err != nil {
			t.Fatalf("write sidecar: %v", err)
		}

		payload, err := buildPredictReport(report, &predictFlags{
			targetVMAF: 93.0, residualThreshold: 1.5,
			withUncertainty: true, calibrationSidecar: sidecar, alpha: math.NaN(),
		})
		if err != nil {
			t.Fatalf("buildPredictReport: %v", err)
		}
		if !payload.Uncertainty.Calibrated {
			t.Error("uncertainty should be flagged calibrated with a sidecar")
		}
		if payload.Uncertainty.Alpha == nil || *payload.Uncertainty.Alpha != 0.05 {
			t.Errorf("alpha = %v, want 0.05", payload.Uncertainty.Alpha)
		}
		// n=3, alpha=0.05 -> corrected level 1.0 -> q = max residual = 2.0.
		for i, r := range payload.Residuals {
			wantLow := r.PredictedVMAF - 2.0
			wantHigh := r.PredictedVMAF + 2.0
			if math.Abs(r.Interval.Low-wantLow) > 1e-9 ||
				math.Abs(r.Interval.High-wantHigh) > 1e-9 {
				t.Errorf("residual %d interval = [%v, %v], want [%v, %v]",
					i, r.Interval.Low, r.Interval.High, wantLow, wantHigh)
			}
		}
	})

	t.Run("alpha override widens the interval", func(t *testing.T) {
		t.Parallel()

		sidecar := filepath.Join(t.TempDir(), "cal.json")
		body := `{"method":"split-conformal","alpha":0.05,"n":20,"residuals":[` +
			`0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1.0,` +
			`1.1,1.2,1.3,1.4,1.5,1.6,1.7,1.8,1.9,2.0]}`
		if err := os.WriteFile(sidecar, []byte(body), 0o600); err != nil {
			t.Fatalf("write sidecar: %v", err)
		}

		tight, err := buildPredictReport(report, &predictFlags{
			targetVMAF: 93.0, residualThreshold: 1.5, withUncertainty: true,
			calibrationSidecar: sidecar, alpha: 0.5,
		})
		if err != nil {
			t.Fatalf("buildPredictReport: %v", err)
		}
		wide, wideErr := buildPredictReport(report, &predictFlags{
			targetVMAF: 93.0, residualThreshold: 1.5, withUncertainty: true,
			calibrationSidecar: sidecar, alpha: math.NaN(),
		})
		if wideErr != nil {
			t.Fatalf("buildPredictReport: %v", wideErr)
		}
		tightWidth := tight.Residuals[0].Interval.High - tight.Residuals[0].Interval.Low
		wideWidth := wide.Residuals[0].Interval.High - wide.Residuals[0].Interval.Low
		if tightWidth >= wideWidth {
			t.Errorf("alpha=0.5 width %v should be below the default-alpha width %v",
				tightWidth, wideWidth)
		}
		if *tight.Uncertainty.Alpha != 0.5 {
			t.Errorf("reported alpha = %v, want the 0.5 override",
				*tight.Uncertainty.Alpha)
		}
	})

	t.Run("a missing sidecar is an error", func(t *testing.T) {
		t.Parallel()

		_, err := buildPredictReport(report, &predictFlags{
			targetVMAF: 93.0, residualThreshold: 1.5, withUncertainty: true,
			calibrationSidecar: "/nonexistent/cal.json", alpha: math.NaN(),
		})
		if err == nil {
			t.Error("expected an error for a missing calibration sidecar")
		}
	})
}

// TestConformalQuantile_isTheFiniteSampleCorrected pins the half-width the
// report depends on, independently of the report assembly.
func TestConformalQuantile_isTheFiniteSampleCorrected(t *testing.T) {
	t.Parallel()

	// The merged pkg/conformal (group 2) keeps the residual set unexported
	// behind NewSplitCalibration, and Quantile reports an error rather than a
	// bool. The pinned value is unchanged: with n=3 and alpha=0.05 the
	// finite-sample correction ceil((n+1)(1-alpha))/n exceeds 1, so the
	// quantile clamps to the largest residual.
	cal, err := conformal.NewSplitCalibration([]float64{0.5, 1.0, 2.0}, 0.05)
	if err != nil {
		t.Fatalf("NewSplitCalibration: %v", err)
	}
	q, err := cal.Quantile()
	if err != nil {
		t.Fatalf("expected a quantile for a non-empty calibration: %v", err)
	}
	if math.Abs(q-2.0) > 1e-9 {
		t.Errorf("quantile = %v, want 2.0", q)
	}
}

// TestPredict_errors covers the rejections reachable without ffmpeg.
func TestPredict_errors(t *testing.T) {
	t.Parallel()

	src := filepath.Join(t.TempDir(), "movie.mkv")
	if err := os.WriteFile(src, []byte("not really a movie"), 0o600); err != nil {
		t.Fatalf("write source: %v", err)
	}

	tests := []struct {
		name string
		args []string
	}{
		{
			name: "missing source file",
			args: []string{"predict", "--source", "/nonexistent/movie.mkv"},
		},
		{
			name: "unknown codec",
			args: []string{"predict", "--source", src, "--codec", "libtheora"},
		},
		{
			name: "invalid bitdepth",
			args: []string{"predict", "--source", src, "--bitdepth", "9"},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			root := newRoot("dev")
			root.Cobra().SetArgs(tc.args)
			root.Cobra().SetOut(&strings.Builder{})
			root.Cobra().SetErr(&strings.Builder{})
			if err := root.Execute(); err == nil {
				t.Error("expected an error")
			}
		})
	}
}

// TestPredict_flagSurface asserts every Python flag name is present.
func TestPredict_flagSurface(t *testing.T) {
	t.Parallel()

	cmd := newPredictCmd()
	want := []string{
		"source", "codec", "target-vmaf", "validate-k", "residual-threshold",
		"use-saliency", "saliency-model", "model", "per-shot-bin",
		"ffmpeg-bin", "ffprobe-bin", "bitdepth", "total-frames", "report-out",
		"with-uncertainty", "calibration-sidecar", "alpha",
	}
	for _, name := range want {
		if cmd.Flags().Lookup(name) == nil {
			t.Errorf("predict is missing the --%s flag", name)
		}
	}
}

// TestRecommendSaliency_errors covers the flag-validation rejections.
func TestRecommendSaliency_errors(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		args []string
	}{
		{
			name: "non-positive geometry",
			args: []string{
				"recommend-saliency", "--src", "/a.yuv", "--width", "0",
				"--height", "1080", "--duration-frames", "10", "--output", "/o.mp4",
			},
		},
		{
			name: "non-positive duration",
			args: []string{
				"recommend-saliency", "--src", "/a.yuv", "--width", "1920",
				"--height", "1080", "--duration-frames", "0", "--output", "/o.mp4",
			},
		},
		{
			name: "unknown encoder",
			args: []string{
				"recommend-saliency", "--src", "/a.yuv", "--width", "1920",
				"--height", "1080", "--duration-frames", "10", "--output", "/o.mp4",
				"--encoder", "libtheora",
			},
		},
		{
			name: "unknown aggregator",
			args: []string{
				"recommend-saliency", "--src", "/a.yuv", "--width", "1920",
				"--height", "1080", "--duration-frames", "10", "--output", "/o.mp4",
				"--saliency-aggregator", "median",
			},
		},
		{
			name: "ema alpha out of range",
			args: []string{
				"recommend-saliency", "--src", "/a.yuv", "--width", "1920",
				"--height", "1080", "--duration-frames", "10", "--output", "/o.mp4",
				"--saliency-ema-alpha", "0",
			},
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			root := newRoot("dev")
			root.Cobra().SetArgs(tc.args)
			root.Cobra().SetOut(&strings.Builder{})
			root.Cobra().SetErr(&strings.Builder{})
			if err := root.Execute(); err == nil {
				t.Error("expected an error")
			}
		})
	}
}

// TestRecommendSaliency_flagSurface asserts every Python flag name is present.
func TestRecommendSaliency_flagSurface(t *testing.T) {
	t.Parallel()

	cmd := newRecommendSaliencyCmd()
	want := []string{
		"src", "width", "height", "pix-fmt", "framerate", "encoder", "preset",
		"crf", "duration-frames", "saliency-aware", "saliency-offset",
		"saliency-model", "saliency-aggregator", "saliency-ema-alpha",
		"saliency-fallback-plain", "ffmpeg-bin", "output",
	}
	for _, name := range want {
		if cmd.Flags().Lookup(name) == nil {
			t.Errorf("recommend-saliency is missing the --%s flag", name)
		}
	}
}

// TestSaliencySession_reportsWhyInferenceIsUnavailable is the honesty gate on
// the one piece of this group that is NOT functionally complete. If an ORT
// path ever lands, this test fails and forces the doc comment, the CLI help
// and the port report to be updated together.
func TestSaliencySession_reportsWhyInferenceIsUnavailable(t *testing.T) {
	t.Parallel()

	// A bad explicit path must surface as a path error, not the generic one,
	// so an operator typo is diagnosable.
	if _, err := newSaliencySession("/nonexistent/model.onnx"); err == nil {
		t.Error("expected an error for a missing explicit model path")
	} else if errors.Is(err, ErrSaliencyInferenceUnavailable) {
		t.Error("a missing model path should report the path, not the ORT gap")
	}

	// With a real file present, the session still reports the ORT gap.
	dir := t.TempDir()
	modelPath := filepath.Join(dir, "saliency.onnx")
	if err := os.WriteFile(modelPath, []byte("onnx"), 0o600); err != nil {
		t.Fatalf("write model stub: %v", err)
	}
	_, err := newSaliencySession(modelPath)
	if err == nil {
		t.Fatal("saliency inference is not implemented; expected an error")
	}
	if !errors.Is(err, ErrSaliencyInferenceUnavailable) {
		t.Errorf("error = %v, want it to wrap ErrSaliencyInferenceUnavailable", err)
	}
}

// TestSaliency_pipelineIsCompleteWithoutInference documents what the port DOES
// deliver: everything around the single forward pass. A caller with any
// Session implementation gets the full ROI path.
func TestSaliency_pipelineIsCompleteWithoutInference(t *testing.T) {
	t.Parallel()

	const w, h = 64, 64
	mask := make([]float64, w*h)
	for i := range mask {
		mask[i] = float64(i%w) / float64(w-1)
	}
	qpMap := saliency.ToQPMap(mask, -4)

	for _, encoder := range saliency.SupportedEncoders() {
		t.Run(encoder, func(t *testing.T) {
			t.Parallel()

			augment, err := saliency.BuildAugment(encoder, qpMap, w, h, 2)
			if err != nil {
				t.Fatalf("BuildAugment(%s): %v", encoder, err)
			}
			hasSidecar := augment.SidecarBody != ""
			hasArgv := len(augment.ExtraParams) > 0
			if !hasSidecar && !hasArgv {
				t.Errorf("%s produced neither a sidecar nor argv", encoder)
			}
			if hasSidecar {
				params := saliency.ExtraParamsFor(encoder, "/tmp/roi")
				if len(params) == 0 {
					t.Errorf("%s writes a sidecar but has no argv to reference it",
						encoder)
				}
			}
		})
	}
}
