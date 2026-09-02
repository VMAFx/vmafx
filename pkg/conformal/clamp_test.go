// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent

package conformal

import "testing"

// TestIntervalForClampsToVMAFRange pins the [0, 100] clamp that conformal.py
// applies via _clamp(point, vmaf_floor, vmaf_ceiling). The merge-written
// IntervalFor bridge originally dropped it, so `predict --with-uncertainty`
// reported interval bounds outside the VMAF range near either end of the scale.
func TestIntervalForClampsToVMAFRange(t *testing.T) {
	t.Parallel()

	cal, err := NewSplitCalibration([]float64{5, 10, 20}, 0.05)
	if err != nil {
		t.Fatalf("NewSplitCalibration: %v", err)
	}
	for _, tc := range []struct {
		name  string
		point float64
	}{
		{"near the ceiling", 99.5},
		{"at the ceiling", 100},
		{"near the floor", 0.5},
		{"at the floor", 0},
	} {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			iv := cal.IntervalFor(tc.point)
			if iv.Low < 0 || iv.Low > 100 {
				t.Errorf("Low = %v, outside [0, 100]", iv.Low)
			}
			if iv.High < 0 || iv.High > 100 {
				t.Errorf("High = %v, outside [0, 100]", iv.High)
			}
		})
	}
}
