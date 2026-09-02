// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/predictor/python_curve_parity_test.go — bit-exact parity of the
// analytical curve and its CRF inversion against vmaftune.predictor.
//
// testdata/python_predictor.json was dumped from the Python module: ~1700
// (features, codec, CRF) triples with the predicted VMAF as raw IEEE-754
// bits, the PickCRF inversion across every registered codec, and the
// resolution-class table. It came across from the pkg/tune/predictor port
// when that duplicate was folded into this package (ADR-1137).

package predictor

import (
	"encoding/hex"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"testing"

	"github.com/VMAFx/vmafx/pkg/pymath"
)

// pythonFixture mirrors testdata/python_predictor.json.
type pythonFixture struct {
	FeatureSets []map[string]float64 `json:"feature_sets"`
	Predict     []struct {
		FeaturesIndex int    `json:"features_index"`
		Codec         string `json:"codec"`
		CRF           int    `json:"crf"`
		VMAFBits      string `json:"vmaf_bits"`
	} `json:"predict"`
	PickCRF []struct {
		FeaturesIndex int     `json:"features_index"`
		Codec         string  `json:"codec"`
		TargetVMAF    float64 `json:"target_vmaf"`
		CRF           int     `json:"crf"`
	} `json:"pick_crf"`
	ResolutionClass []struct {
		Height int    `json:"height"`
		Class  string `json:"class"`
	} `json:"resolution_class"`
}

func loadPythonFixture(t *testing.T) pythonFixture {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join("testdata", "python_predictor.json"))
	if err != nil {
		t.Fatalf("read predictor fixture: %v", err)
	}
	var fixture pythonFixture
	if err := json.Unmarshal(raw, &fixture); err != nil {
		t.Fatalf("parse predictor fixture: %v", err)
	}
	if len(fixture.Predict) == 0 || len(fixture.PickCRF) == 0 {
		t.Fatal("predictor fixture is empty")
	}
	return fixture
}

// featuresFromFixture rebuilds a ShotFeatures from the fixture's keyword
// dict, leaving unset fields at the Python dataclass defaults (all zero).
func featuresFromFixture(m map[string]float64) ShotFeatures {
	return ShotFeatures{
		ProbeBitrateKbps:    m["probe_bitrate_kbps"],
		ProbeIFrameAvgBytes: m["probe_i_frame_avg_bytes"],
		ProbePFrameAvgBytes: m["probe_p_frame_avg_bytes"],
		ProbeBFrameAvgBytes: m["probe_b_frame_avg_bytes"],
		SaliencyMean:        m["saliency_mean"],
		SaliencyVar:         m["saliency_var"],
		FrameDiffMean:       m["frame_diff_mean"],
		YAvg:                m["y_avg"],
		YVar:                m["y_var"],
		ShotLengthFrames:    int(m["shot_length_frames"]),
		FPS:                 m["fps"],
		Width:               int(m["width"]),
		Height:              int(m["height"]),
	}
}

func float64FromHex(t *testing.T, s string) float64 {
	t.Helper()
	raw, err := hex.DecodeString(s)
	if err != nil || len(raw) != 8 {
		t.Fatalf("bad hex float %q: %v", s, err)
	}
	var bits uint64
	for _, b := range raw {
		bits = bits<<8 | uint64(b)
	}
	return math.Float64frombits(bits)
}

// TestPredictVMAFMatchesPythonFixture demands bit-identical output for every
// fixture triple. The analytical fallback is the path both implementations
// take whenever no ONNX model is loaded — the default, and the only path the
// auto planner exercises — so bit-identity here is what makes the planner's
// estimated_vmaf field byte-comparable with the Python emitter.
func TestPredictVMAFMatchesPythonFixture(t *testing.T) {
	t.Parallel()

	fixture := loadPythonFixture(t)
	p := New()

	mismatches := 0
	for _, row := range fixture.Predict {
		features := featuresFromFixture(fixture.FeatureSets[row.FeaturesIndex])
		want := float64FromHex(t, row.VMAFBits)
		got := p.PredictVMAF(features, row.CRF, row.Codec)
		if math.Float64bits(got) != math.Float64bits(want) {
			mismatches++
			if mismatches <= 10 {
				t.Errorf("PredictVMAF(set %d, %s, crf=%d) = %v (%016x), want %v (%016x)",
					row.FeaturesIndex, row.Codec, row.CRF,
					got, math.Float64bits(got), want, math.Float64bits(want))
			}
		}
	}
	if mismatches > 10 {
		t.Errorf("... and %d further mismatches", mismatches-10)
	}
	t.Logf("verified %d predict vectors", len(fixture.Predict))
}

// TestPickCRFMatchesPythonFixture replays the binary-search inversion. A
// single off-by-one in the search bounds would move every planned CRF, so
// the fixture sweeps every registered codec across round and random targets.
func TestPickCRFMatchesPythonFixture(t *testing.T) {
	t.Parallel()

	fixture := loadPythonFixture(t)
	p := New()

	for _, row := range fixture.PickCRF {
		features := featuresFromFixture(fixture.FeatureSets[row.FeaturesIndex])
		got, err := p.PickCRF(features, row.TargetVMAF, row.Codec)
		if err != nil {
			t.Fatalf("PickCRF(%s): %v", row.Codec, err)
		}
		if got != row.CRF {
			t.Errorf("PickCRF(set %d, %s, target=%v) = %d, want %d",
				row.FeaturesIndex, row.Codec, row.TargetVMAF, got, row.CRF)
		}
	}
	t.Logf("verified %d pick_crf vectors", len(fixture.PickCRF))
}

func TestResolutionClassMatchesPythonFixture(t *testing.T) {
	t.Parallel()

	for _, row := range loadPythonFixture(t).ResolutionClass {
		if got := ResolutionClass(row.Height); got != row.Class {
			t.Errorf("ResolutionClass(%d) = %q, want %q", row.Height, got, row.Class)
		}
	}
}

// TestAnalyticalCurvePinnedCPythonValues pins a handful of hand-checked
// points against the values CPython's math.log10 produces on this platform's
// libm, as raw bits — the regression guard for the failure ADR-1137 records:
// pkg/predictor once used Go's math.Log10, which lands one ULP off CPython on
// roughly 27% of realistic probe bitrates, and the two Go predictors then
// disagreed on the same input. Every expected value below was produced by
//
//	python3 -c 'import math; print(repr(a - b*d - c*d*d + e*math.log10(max(kbps, 1.0))))'
//
// with the codec's coefficients, then clamped to [0, 100].
func TestAnalyticalCurvePinnedCPythonValues(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		kbps      float64
		crf       int
		codec     string
		wantVMAF  float64
		wantLog10 float64
	}{
		{"libx264 at the reference CRF", 905.82, 23, "libx264", 99.43556285814883, 2.9570419054325505},
		{"libx264 five points above reference", 905.82, 28, "libx264", 93.13556285814883, 2.9570419054325505},
		{"libx264 saturates at 100", 4200.5, 23, "libx264", 100.0, 3.623300989044697},
		{"libx264 4K probe bitrate", 48000.0, 30, "libx264", 93.03386185606338, 4.681241237375588},
		{"libx264 bitrate floor", 1.0, 23, "libx264", 95.0, 0.0},
		{"libx264 below the bitrate floor", 0.5, 23, "libx264", 95.0, 0.0},
		{"libx265", 12000.0, 35, "libx265", 94.3366899936762, 4.079181246047625},
		{"h264_nvenc", 2500.0, 26, "h264_nvenc", 93.73111601214084, 3.3979400086720375},
		{"libsvtav1", 777.7, 40, "libsvtav1", 93.07213693853718, 2.8908120989551245},
	}
	p := New()
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := pymath.Log10(math.Max(tc.kbps, 1.0)); math.Float64bits(got) != math.Float64bits(tc.wantLog10) {
				t.Errorf("pymath.Log10(%v) = %v (%016x), want CPython's %v (%016x)",
					tc.kbps, got, math.Float64bits(got), tc.wantLog10, math.Float64bits(tc.wantLog10))
			}
			got := p.PredictVMAF(ShotFeatures{ProbeBitrateKbps: tc.kbps}, tc.crf, tc.codec)
			if math.Float64bits(got) != math.Float64bits(tc.wantVMAF) {
				t.Errorf("PredictVMAF(%v kbps, crf %d, %s) = %v (%016x), want CPython's %v (%016x)",
					tc.kbps, tc.crf, tc.codec, got, math.Float64bits(got),
					tc.wantVMAF, math.Float64bits(tc.wantVMAF))
			}
		})
	}
}

// TestPredictVMAFClampsToVMAFRange guards the [0, 100] contract at both ends.
func TestPredictVMAFClampsToVMAFRange(t *testing.T) {
	t.Parallel()

	p := New()
	tests := []struct {
		name     string
		features ShotFeatures
		crf      int
		want     float64
	}{
		{"absurdly high bitrate saturates at 100", ShotFeatures{ProbeBitrateKbps: 1e300}, 0, 100},
		{"absurdly high CRF floors at 0", ShotFeatures{ProbeBitrateKbps: 1000}, 5000, 0},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := p.PredictVMAF(tc.features, tc.crf, "libx264"); got != tc.want {
				t.Errorf("PredictVMAF = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestPredictVMAFBitrateFloor pins the log10(0) guard: a zero or negative
// probe bitrate is treated as 1 kbps rather than producing -Inf.
func TestPredictVMAFBitrateFloor(t *testing.T) {
	t.Parallel()

	p := New()
	atOne := p.PredictVMAF(ShotFeatures{ProbeBitrateKbps: 1.0}, 23, "libx264")
	for _, bitrate := range []float64{0.0, 0.5, -100.0} {
		got := p.PredictVMAF(ShotFeatures{ProbeBitrateKbps: bitrate}, 23, "libx264")
		if got != atOne {
			t.Errorf("probe bitrate %v predicted %v, want the 1 kbps floor value %v",
				bitrate, got, atOne)
		}
	}
}

// TestCustomCoefficientsOverrideDefaults documents the Coefficients hook and
// its fallback, which is Python's `coefficients.get(codec,
// _DEFAULT_COEFFS["libx264"])`: a codec absent from a caller-supplied table
// lands on the shipped libx264 curve, not on that codec's own default. The
// pkg/tune/predictor implementation deleted in ADR-1137 had invented a
// per-codec fallback here; this pins the Python one.
func TestCustomCoefficientsOverrideDefaults(t *testing.T) {
	t.Parallel()

	custom := &Predictor{
		Coefficients: map[string]Coefficients{
			"libx265": {A: 50, B: 0, C: 0, D: 0, CRFRef: 0},
		},
	}
	if got := custom.PredictVMAF(ShotFeatures{ProbeBitrateKbps: 1000}, 30, "libx265"); got != 50 {
		t.Errorf("custom curve = %v, want 50", got)
	}
	fallback := custom.PredictVMAF(ShotFeatures{ProbeBitrateKbps: 1000}, 30, "libaom-av1")
	baseline := New().PredictVMAF(ShotFeatures{ProbeBitrateKbps: 1000}, 30, "libx264")
	if fallback != baseline {
		t.Errorf("uncovered codec = %v, want the shipped libx264 curve %v", fallback, baseline)
	}
}
