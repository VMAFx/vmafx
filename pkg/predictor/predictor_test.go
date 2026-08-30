// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/predictor/predictor_test.go — analytical-curve parity tests.
//
// The expected VMAF values were produced by running the Python
// vmaftune.predictor.Predictor()._predict_analytical() on the same inputs, so
// the table pins cross-implementation agreement rather than restating the Go
// arithmetic.

package predictor

import (
	"math"
	"testing"
)

// sampleFeatures is the fixture the parity values were computed against.
func sampleFeatures() ShotFeatures {
	return ShotFeatures{
		ProbeBitrateKbps:    4200.5,
		ProbeIFrameAvgBytes: 51234.0,
		ProbePFrameAvgBytes: 8123.25,
		ProbeBFrameAvgBytes: 2011.75,
		SaliencyMean:        0.42,
		SaliencyVar:         0.031,
		FrameDiffMean:       7.5,
		YAvg:                112.25,
		YVar:                1830.5,
		ShotLengthFrames:    240,
		FPS:                 24.0,
		Width:               1920,
		Height:              1080,
	}
}

func TestPredictVMAF(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name  string
		codec string
		crf   int
		want  float64
	}{
		{name: "libx264 at its reference CRF", codec: "libx264", crf: 23, want: 100.0},
		{name: "libx264 at crf 26", codec: "libx264", crf: 26, want: 96.72695148356705},
		{name: "libx264 at crf 40", codec: "libx264", crf: 40, want: 76.56695148356704},
		{name: "libx265 at crf 32", codec: "libx265", crf: 32, want: 97.23728158247151},
		{name: "libsvtav1 at crf 40", codec: "libsvtav1", crf: 40, want: 94.09762138466257},
		{
			name:  "an unregistered codec falls back to the libx264 row",
			codec: "definitely-not-a-codec", crf: 26, want: 96.72695148356705,
		},
	}

	p := New()
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := p.PredictVMAF(sampleFeatures(), tc.crf, tc.codec)
			if math.Abs(got-tc.want) > 1e-9 {
				t.Errorf("PredictVMAF(%s, crf=%d) = %v, want %v",
					tc.codec, tc.crf, got, tc.want)
			}
		})
	}
}

func TestPredictVMAFClampsToVMAFRange(t *testing.T) {
	t.Parallel()

	p := New()
	f := sampleFeatures()

	// A very low CRF drives the curve above 100; a very high one below 0.
	if got := p.PredictVMAF(f, 0, "libx264"); got != 100.0 {
		t.Errorf("PredictVMAF at crf 0 = %v, want the 100.0 clamp", got)
	}
	if got := p.PredictVMAF(f, 51, "libx264"); got < 0 || got > 100 {
		t.Errorf("PredictVMAF at crf 51 = %v, want it inside [0, 100]", got)
	}
}

func TestPredictVMAFGuardsZeroBitrate(t *testing.T) {
	t.Parallel()

	// log10(0) is -Inf; the Python curve pins the bitrate floor at 1.0 kbps.
	p := New()
	zero := ShotFeatures{}
	one := ShotFeatures{ProbeBitrateKbps: 1.0}
	if p.PredictVMAF(zero, 30, "libx264") != p.PredictVMAF(one, 30, "libx264") {
		t.Error("a zero probe bitrate should behave like the 1.0 kbps floor")
	}
	if math.IsInf(p.PredictVMAF(zero, 30, "libx264"), 0) {
		t.Error("a zero probe bitrate produced a non-finite prediction")
	}
}

func TestPredictVMAFIsMonotoneInCRF(t *testing.T) {
	t.Parallel()

	// PickCRF binary-searches on the assumption that the curve is
	// non-increasing in CRF across every codec's own quality window.
	p := New()
	f := sampleFeatures()
	for _, codec := range []string{"libx264", "libx265", "libsvtav1", "libaom-av1", "libvvenc"} {
		prev := math.Inf(1)
		for crf := 0; crf <= 51; crf++ {
			got := p.PredictVMAF(f, crf, codec)
			if got > prev {
				t.Errorf("%s: PredictVMAF rose from %v to %v at crf %d", codec, prev, got, crf)
				break
			}
			prev = got
		}
	}
}

func TestPickCRF(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name       string
		codec      string
		targetVMAF float64
		want       int
		wantErr    bool
	}{
		{name: "libx264 at target 90", codec: "libx264", targetVMAF: 90, want: 31},
		{name: "libx264 at target 96", codec: "libx264", targetVMAF: 96, want: 26},
		{
			name:  "an unreachable target falls back to the codec default",
			codec: "libx264", targetVMAF: 101, want: 23,
		},
		{
			name:  "a trivially reachable target picks the top of the window",
			codec: "libx264", targetVMAF: 1, want: 51,
		},
		{name: "an unknown codec errors", codec: "libx999", targetVMAF: 90, wantErr: true},
	}

	p := New()
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := p.PickCRF(sampleFeatures(), tc.targetVMAF, tc.codec)
			if (err != nil) != tc.wantErr {
				t.Fatalf("PickCRF error = %v, wantErr = %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if got != tc.want {
				t.Errorf("PickCRF(%s, %g) = %d, want %d", tc.codec, tc.targetVMAF, got, tc.want)
			}
		})
	}
}

func TestPickCRFRespectsTheTarget(t *testing.T) {
	t.Parallel()

	// The contract is "the highest CRF that still meets the target": the
	// picked CRF must pass, and the next one up must not.
	p := New()
	f := sampleFeatures()
	const target = 90.0
	crf, err := p.PickCRF(f, target, "libx264")
	if err != nil {
		t.Fatalf("PickCRF: %v", err)
	}
	if got := p.PredictVMAF(f, crf, "libx264"); got < target {
		t.Errorf("picked crf %d predicts %v, below the %g target", crf, got, target)
	}
	if got := p.PredictVMAF(f, crf+1, "libx264"); got >= target {
		t.Errorf("crf %d also meets the %g target (predicts %v) — the pick is not maximal",
			crf+1, target, got)
	}
}

func TestPickKeyint(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name         string
		features     ShotFeatures
		fps          float64
		wantKeyint   int
		wantMinKeyin int
	}{
		{
			name:     "long low-motion shots get an extended GOP",
			features: ShotFeatures{ProbeBitrateKbps: 900, ShotLengthFrames: 240},
			fps:      24, wantKeyint: 96, wantMinKeyin: 24,
		},
		{
			name:     "high-motion shots get a tight GOP",
			features: ShotFeatures{ProbeBitrateKbps: 12000, ShotLengthFrames: 240},
			fps:      24, wantKeyint: 24, wantMinKeyin: 12,
		},
		{
			name:     "everything else gets the canonical 2x GOP",
			features: ShotFeatures{ProbeBitrateKbps: 4000, ShotLengthFrames: 240},
			fps:      24, wantKeyint: 48, wantMinKeyin: 12,
		},
		{
			name:     "a short low-motion shot is not long enough for the extended GOP",
			features: ShotFeatures{ProbeBitrateKbps: 900, ShotLengthFrames: 10},
			fps:      24, wantKeyint: 48, wantMinKeyin: 12,
		},
		{
			name:     "a sub-1 fps input clamps to 1",
			features: ShotFeatures{ProbeBitrateKbps: 4000},
			fps:      0.2, wantKeyint: 2, wantMinKeyin: 1,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			keyint, minKeyint := PickKeyint(tc.features, tc.fps)
			if keyint != tc.wantKeyint || minKeyint != tc.wantMinKeyin {
				t.Errorf("PickKeyint() = (%d, %d), want (%d, %d)",
					keyint, minKeyint, tc.wantKeyint, tc.wantMinKeyin)
			}
		})
	}
}

func TestResolutionClass(t *testing.T) {
	t.Parallel()

	tests := []struct {
		height int
		want   string
	}{
		{height: 240, want: "sd"},
		{height: 480, want: "sd"},
		{height: 481, want: "hd_ready"},
		{height: 720, want: "hd_ready"},
		{height: 1080, want: "hd"},
		{height: 2160, want: "uhd"},
		{height: 4320, want: "uhd8k"},
		{height: 8640, want: "uhd8k"},
	}
	for _, tc := range tests {
		if got := ResolutionClass(tc.height); got != tc.want {
			t.Errorf("ResolutionClass(%d) = %q, want %q", tc.height, got, tc.want)
		}
	}
}

func TestClamp(t *testing.T) {
	t.Parallel()

	tests := []struct {
		v, lo, hi, want float64
	}{
		{v: 5, lo: 0, hi: 10, want: 5},
		{v: -1, lo: 0, hi: 10, want: 0},
		{v: 11, lo: 0, hi: 10, want: 10},
		{v: 0, lo: 0, hi: 0, want: 0},
	}
	for _, tc := range tests {
		if got := Clamp(tc.v, tc.lo, tc.hi); got != tc.want {
			t.Errorf("Clamp(%v, %v, %v) = %v, want %v", tc.v, tc.lo, tc.hi, got, tc.want)
		}
	}
}
