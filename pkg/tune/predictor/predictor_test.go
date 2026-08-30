// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package predictor

import (
	"encoding/hex"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"testing"
)

// pythonFixture mirrors testdata/python_predictor.json, dumped from
// vmaftune.predictor.
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

// featuresFromMap rebuilds a ShotFeatures from the fixture's keyword dict,
// leaving unset fields at the Python dataclass defaults (all zero).
func featuresFromMap(m map[string]float64) ShotFeatures {
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

// TestPredictVMAFMatchesPython replays ~1700 (features, codec, CRF) triples
// captured from the Python analytical curve and demands bit-identical output.
//
// The analytical fallback is the path both implementations take whenever no
// ONNX model is loaded, which is the default and the only path the unit tests
// and the auto planner exercise. Bit-identity here is what makes the planner's
// estimated_vmaf field byte-comparable.
func TestPredictVMAFMatchesPython(t *testing.T) {
	t.Parallel()

	fixture := loadPythonFixture(t)
	p := &Predictor{}

	mismatches := 0
	for _, row := range fixture.Predict {
		features := featuresFromMap(fixture.FeatureSets[row.FeaturesIndex])
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

// TestPickCRFMatchesPython replays the binary-search inversion. A single
// off-by-one in the search bounds would move every planned CRF, so the fixture
// sweeps every registered codec across a mix of round and random targets.
func TestPickCRFMatchesPython(t *testing.T) {
	t.Parallel()

	fixture := loadPythonFixture(t)
	p := &Predictor{}

	for _, row := range fixture.PickCRF {
		features := featuresFromMap(fixture.FeatureSets[row.FeaturesIndex])
		got, err := PickCRF(p, features, row.TargetVMAF, row.Codec)
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

func TestResolutionClassMatchesPython(t *testing.T) {
	t.Parallel()

	for _, row := range loadPythonFixture(t).ResolutionClass {
		if got := ResolutionClass(row.Height); got != row.Class {
			t.Errorf("ResolutionClass(%d) = %q, want %q", row.Height, got, row.Class)
		}
	}
}

// TestPredictVMAFIsMonotoneInCRF pins the property PickCRF's binary search
// depends on: predicted VMAF never rises as CRF rises.
func TestPredictVMAFIsMonotoneInCRF(t *testing.T) {
	t.Parallel()

	features := ShotFeatures{ProbeBitrateKbps: 4200.0, Width: 1920, Height: 1080}
	p := &Predictor{}

	for codecName := range DefaultCoefficients {
		t.Run(codecName, func(t *testing.T) {
			t.Parallel()
			previous := math.Inf(1)
			for crf := 0; crf <= 63; crf++ {
				got := p.PredictVMAF(features, crf, codecName)
				if got > previous {
					t.Fatalf("crf %d predicted %v, above the previous %v — not monotone",
						crf, got, previous)
				}
				previous = got
			}
		})
	}
}

// TestPredictVMAFClampsToVMAFRange guards the [0, 100] contract at both ends.
func TestPredictVMAFClampsToVMAFRange(t *testing.T) {
	t.Parallel()

	p := &Predictor{}
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

	p := &Predictor{}
	atOne := p.PredictVMAF(ShotFeatures{ProbeBitrateKbps: 1.0}, 23, "libx264")
	for _, bitrate := range []float64{0.0, 0.5, -100.0} {
		got := p.PredictVMAF(ShotFeatures{ProbeBitrateKbps: bitrate}, 23, "libx264")
		if got != atOne {
			t.Errorf("probe bitrate %v predicted %v, want the 1 kbps floor value %v",
				bitrate, got, atOne)
		}
	}
}

// TestNewWithMissingModel checks that a model path which resolves to nothing
// is an error (matching the Python FileNotFoundError), while the empty path is
// the analytical fallback.
func TestNewWithMissingModel(t *testing.T) {
	t.Parallel()

	if _, err := New("", nil); err != nil {
		t.Errorf("an empty model path must build the analytical fallback, got %v", err)
	}
	missing := filepath.Join(t.TempDir(), "predictor_nope.onnx")
	if _, err := New(missing, nil); err == nil {
		t.Error("a model path that resolves to nothing must be an error")
	}
}

// TestPickCRFRejectsUnknownCodec — PickCRF needs the adapter's quality window,
// so an unregistered codec cannot silently fall back the way PredictVMAF does.
func TestPickCRFRejectsUnknownCodec(t *testing.T) {
	t.Parallel()

	if _, err := PickCRF(&Predictor{}, ShotFeatures{}, 93.0, "not_a_real_codec"); err == nil {
		t.Error("expected an error for an unregistered codec")
	}
}

// TestCustomCoefficientsOverrideDefaults documents the Coefficients hook.
func TestCustomCoefficientsOverrideDefaults(t *testing.T) {
	t.Parallel()

	custom := &Predictor{
		Coefficients: map[string]Coefficients{
			"libx264": {A: 50, B: 0, C: 0, D: 0, CRFRef: 0},
		},
	}
	if got := custom.PredictVMAF(ShotFeatures{ProbeBitrateKbps: 1000}, 30, "libx264"); got != 50 {
		t.Errorf("custom curve = %v, want 50", got)
	}
	// A codec absent from the override map still falls back to the shipped
	// defaults rather than to the override's single entry.
	fallback := custom.PredictVMAF(ShotFeatures{ProbeBitrateKbps: 1000}, 30, "libx265")
	baseline := (&Predictor{}).PredictVMAF(ShotFeatures{ProbeBitrateKbps: 1000}, 30, "libx265")
	if fallback != baseline {
		t.Errorf("uncovered codec = %v, want the shipped default %v", fallback, baseline)
	}
}
