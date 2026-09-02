// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package uncertainty_test

import (
	"io"
	"log/slog"
	"math"
	"os"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/pkg/uncertainty"
)

// quietLogger discards the warnings LoadThresholds emits on every fallback so
// the test output stays readable.
func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

// TestClassify sweeps the band edges. Both boundaries are inclusive on the
// side that names them: width == tight is Tight, width == wide is Wide.
func TestClassify(t *testing.T) {
	t.Parallel()

	thresholds := uncertainty.Thresholds{
		TightIntervalMaxWidth: 2.0,
		WideIntervalMinWidth:  5.0,
	}
	tests := []struct {
		name    string
		width   float64
		want    uncertainty.Decision
		wantErr bool
	}{
		{"zero width is tight", 0.0, uncertainty.Tight, false},
		{"just below the tight edge", 1.999, uncertainty.Tight, false},
		{"exactly at the tight edge", 2.0, uncertainty.Tight, false},
		{"just above the tight edge", 2.001, uncertainty.Middle, false},
		{"mid band", 3.5, uncertainty.Middle, false},
		{"just below the wide edge", 4.999, uncertainty.Middle, false},
		{"exactly at the wide edge", 5.0, uncertainty.Wide, false},
		{"far above the wide edge", 50.0, uncertainty.Wide, false},
		{"NaN defers to the native recipe", math.NaN(), uncertainty.Middle, false},
		{"negative width is a caller bug", -0.1, "", true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := uncertainty.Classify(tc.width, thresholds)
			if (err != nil) != tc.wantErr {
				t.Fatalf("Classify error = %v, wantErr %v", err, tc.wantErr)
			}
			if !tc.wantErr && got != tc.want {
				t.Errorf("Classify(%v) = %q, want %q", tc.width, got, tc.want)
			}
		})
	}
}

// TestClassify_degenerateThresholds pins the tight==wide case: a width equal
// to both boundaries resolves Tight, because the tight test runs first.
func TestClassify_degenerateThresholds(t *testing.T) {
	t.Parallel()

	thresholds := uncertainty.Thresholds{
		TightIntervalMaxWidth: 3.0, WideIntervalMinWidth: 3.0,
	}
	got, err := uncertainty.Classify(3.0, thresholds)
	if err != nil {
		t.Fatalf("Classify: %v", err)
	}
	if got != uncertainty.Tight {
		t.Errorf("Classify(3.0) = %q, want %q", got, uncertainty.Tight)
	}
}

// TestValidate enforces the 0 < tight <= wide invariant.
func TestValidate(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		tight   float64
		wide    float64
		wantErr bool
	}{
		{"documented defaults", 2.0, 5.0, false},
		{"tight equal to wide", 3.0, 3.0, false},
		{"tight above wide", 6.0, 5.0, true},
		{"zero tight", 0.0, 5.0, true},
		{"zero wide", 2.0, 0.0, true},
		{"negative tight", -1.0, 5.0, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			th := uncertainty.Thresholds{
				TightIntervalMaxWidth: tc.tight, WideIntervalMinWidth: tc.wide,
			}
			if err := th.Validate(); (err != nil) != tc.wantErr {
				t.Errorf("Validate error = %v, wantErr %v", err, tc.wantErr)
			}
		})
	}
}

// TestLoadThresholds covers every degradation path. None of them may fail the
// run: a missing or broken sidecar must yield the documented floor.
func TestLoadThresholds(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name        string
		payload     string
		write       bool
		emptyPath   bool
		wantTight   float64
		wantWide    float64
		wantDefault bool
	}{
		{
			name: "valid sidecar", write: true,
			payload:   `{"tight_interval_max_width":1.6,"wide_interval_min_width":4.2}`,
			wantTight: 1.6, wantWide: 4.2,
		},
		{
			name: "extra keys are ignored", write: true,
			payload: `{"tight_interval_max_width":1.0,"wide_interval_min_width":3.0,` +
				`"future_key":"whatever"}`,
			wantTight: 1.0, wantWide: 3.0,
		},
		{
			name: "no path supplied", emptyPath: true, wantDefault: true,
		},
		{
			name: "file does not exist", write: false, wantDefault: true,
		},
		{
			name: "malformed JSON", write: true, payload: `{"tight`,
			wantDefault: true,
		},
		{
			name: "missing keys", write: true, payload: `{"something_else":1}`,
			wantDefault: true,
		},
		{
			name: "violates tight<=wide", write: true,
			payload:     `{"tight_interval_max_width":9.0,"wide_interval_min_width":4.0}`,
			wantDefault: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			path := ""
			if !tc.emptyPath {
				path = filepath.Join(t.TempDir(), "thresholds.json")
				if tc.write {
					if err := os.WriteFile(path, []byte(tc.payload), 0o600); err != nil {
						t.Fatalf("write sidecar: %v", err)
					}
				}
			}
			got := uncertainty.LoadThresholds(path, quietLogger())
			wantTight, wantWide := tc.wantTight, tc.wantWide
			if tc.wantDefault {
				wantTight = uncertainty.DefaultTightIntervalMaxWidth
				wantWide = uncertainty.DefaultWideIntervalMinWidth
				if got.Source != "default" {
					t.Errorf("Source = %q, want %q", got.Source, "default")
				}
			}
			if got.TightIntervalMaxWidth != wantTight || got.WideIntervalMinWidth != wantWide {
				t.Errorf("thresholds = (%v, %v), want (%v, %v)",
					got.TightIntervalMaxWidth, got.WideIntervalMinWidth, wantTight, wantWide)
			}
		})
	}
}

// TestExcludesTarget covers the short-circuit predicate and its slack margin.
func TestExcludesTarget(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name   string
		low    float64
		high   float64
		target float64
		slack  float64
		want   bool
	}{
		{"interval straddles the target", 88.0, 95.0, 90.0, 0.0, false},
		{"interval wholly below", 80.0, 85.0, 90.0, 0.0, true},
		{"interval wholly above", 95.0, 99.0, 90.0, 0.0, true},
		{"touching from below is not exclusion", 80.0, 90.0, 90.0, 0.0, false},
		{"touching from above is not exclusion", 90.0, 99.0, 90.0, 0.0, false},
		{"slack widens the miss requirement", 80.0, 89.0, 90.0, 2.0, false},
		{"slack still admits a clear miss", 80.0, 87.0, 90.0, 2.0, true},
		{"NaN low is never conclusive", math.NaN(), 85.0, 90.0, 0.0, false},
		{"NaN high is never conclusive", 80.0, math.NaN(), 90.0, 0.0, false},
		{"infinite bound is never conclusive", math.Inf(-1), 85.0, 90.0, 0.0, false},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := uncertainty.ExcludesTarget(tc.low, tc.high, tc.target, tc.slack)
			if got != tc.want {
				t.Errorf("ExcludesTarget(%v, %v, %v, %v) = %v, want %v",
					tc.low, tc.high, tc.target, tc.slack, got, tc.want)
			}
		})
	}
}
