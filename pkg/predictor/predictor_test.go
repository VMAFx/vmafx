// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package predictor_test

import (
	"errors"
	"math"
	"slices"
	"testing"

	"github.com/VMAFx/vmafx/pkg/pershot"
	"github.com/VMAFx/vmafx/pkg/predictor"
)

// TestResolutionClass pins the ladder-aligned band edges.
func TestResolutionClass(t *testing.T) {
	t.Parallel()

	tests := []struct {
		height int
		want   string
	}{
		{240, "sd"},
		{480, "sd"},
		{481, "hd_ready"},
		{720, "hd_ready"},
		{721, "hd"},
		{1080, "hd"},
		{1081, "uhd"},
		{2160, "uhd"},
		{2161, "uhd8k"},
		{4320, "uhd8k"},
		{8640, "uhd8k"},
	}
	for _, tc := range tests {
		t.Run(tc.want, func(t *testing.T) {
			t.Parallel()

			if got := predictor.ResolutionClass(tc.height); got != tc.want {
				t.Errorf("ResolutionClass(%d) = %q, want %q", tc.height, got, tc.want)
			}
		})
	}
}

// TestPredictAnalytical pins the closed-form curve against values computed
// directly from the documented formula
// vmaf = A - B*d - C*d^2 + D*log10(max(bitrate, 1)), d = crf - CRFRef.
func TestPredictAnalytical(t *testing.T) {
	t.Parallel()

	p := predictor.New()

	tests := []struct {
		name    string
		codec   string
		crf     int
		bitrate float64
		want    float64
	}{
		{
			// libx264: A=95, B=1.2, C=0.012, D=1.5, ref=23. d=0.
			// 95 + 1.5*log10(5000) = 95 + 1.5*3.6989700043 = 100.548... -> clamped
			name:  "at the reference CRF the curve clamps at 100",
			codec: "libx264", crf: 23, bitrate: 5000, want: 100.0,
		},
		{
			// d = 12; 95 - 14.4 - 0.012*144 + 1.5*log10(1000)
			//      = 95 - 14.4 - 1.728 + 4.5 = 83.372
			name:  "well above the reference CRF",
			codec: "libx264", crf: 35, bitrate: 1000, want: 83.372,
		},
		{
			// libx265: A=96, B=1.1, C=0.010, D=1.6, ref=28. d=10.
			// 96 - 11 - 1.0 + 1.6*log10(2000) = 84 + 1.6*3.3010299957 = 89.2816479
			name:  "libx265 uses its own coefficients",
			codec: "libx265", crf: 38, bitrate: 2000, want: 89.28164799,
		},
		{
			// Unknown codec falls back to the libx264 curve. d = 12 again.
			name:  "unknown codec falls back to libx264",
			codec: "libtheora", crf: 35, bitrate: 1000, want: 83.372,
		},
		{
			// Zero bitrate must be floored at 1 kbps, not log10(0) = -Inf.
			// d = 28; 95 - 33.6 - 0.012*784 + 1.5*0 = 95 - 33.6 - 9.408 = 51.992
			name:  "zero bitrate is floored, not -Inf",
			codec: "libx264", crf: 51, bitrate: 0, want: 51.992,
		},
		{
			// Far past the reference the quadratic drags the curve below 0.
			// libsvtav1: A=94, B=0.95, C=0.009, ref=35, crf=0 -> d=-35.
			// 94 + 33.25 - 11.025 + 1.4*log10(1) = 116.225 -> clamped to 100.
			name:  "below the reference clamps at 100",
			codec: "libsvtav1", crf: 0, bitrate: 1, want: 100.0,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := p.PredictAnalytical(
				predictor.ShotFeatures{ProbeBitrateKbps: tc.bitrate}, tc.crf, tc.codec)
			if math.Abs(got-tc.want) > 1e-6 {
				t.Errorf("PredictAnalytical = %v, want %v", got, tc.want)
			}
			if got < 0.0 || got > 100.0 {
				t.Errorf("PredictAnalytical returned %v, outside [0, 100]", got)
			}
		})
	}
}

// TestPredictAnalytical_isMonotone is the load-bearing invariant behind
// PickCRF: the curve must be non-increasing in CRF across every codec's whole
// quality range, or the binary-search inversion is unsound.
func TestPredictAnalytical_isMonotone(t *testing.T) {
	t.Parallel()

	p := predictor.New()
	codecs := []string{
		"libx264", "libx265", "libsvtav1", "libaom-av1", "libvvenc",
		"h264_nvenc", "hevc_nvenc", "av1_nvenc",
		"h264_amf", "hevc_amf", "av1_amf",
		"h264_qsv", "hevc_qsv", "av1_qsv",
	}
	features := predictor.ShotFeatures{ProbeBitrateKbps: 4000}

	for _, codec := range codecs {
		t.Run(codec, func(t *testing.T) {
			t.Parallel()

			prev := math.Inf(1)
			for crf := 0; crf <= 63; crf++ {
				got := p.PredictAnalytical(features, crf, codec)
				if got > prev+1e-9 {
					t.Fatalf("curve rose at crf=%d: %v > %v", crf, got, prev)
				}
				prev = got
			}
		})
	}
}

// TestPickCRF asserts the inversion returns the largest CRF that still clears
// the target, and that it stays inside the adapter's quality window.
func TestPickCRF(t *testing.T) {
	t.Parallel()

	p := predictor.New()
	features := predictor.ShotFeatures{ProbeBitrateKbps: 4000}

	tests := []struct {
		name    string
		codec   string
		target  float64
		wantErr bool
	}{
		{name: "libx264 mid target", codec: "libx264", target: 90},
		{name: "libx264 high target", codec: "libx264", target: 97},
		{name: "libx265 narrow window", codec: "libx265", target: 90},
		{name: "unknown codec is an error", codec: "libtheora", target: 90, wantErr: true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			crf, err := p.PickCRF(features, tc.target, tc.codec)
			if (err != nil) != tc.wantErr {
				t.Fatalf("PickCRF error = %v, wantErr %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			// The pick must clear the target...
			if got := p.PredictVMAF(features, crf, tc.codec); got < tc.target {
				t.Errorf("picked crf=%d predicts %v, below target %v", crf, got, tc.target)
			}
			// ...and crf+1 must not, or the search stopped early.
			if next := p.PredictVMAF(features, crf+1, tc.codec); next >= tc.target {
				t.Errorf("crf=%d also clears target %v; the search stopped early at %d",
					crf+1, tc.target, crf)
			}
		})
	}
}

// TestPickCRF_matchesPythonGolden pins the exact CRF the Python pick_crf
// returns for the same inputs, so a drift in either the curve or the search
// bounds is caught rather than only the weaker "clears the target" property.
func TestPickCRF_matchesPythonGolden(t *testing.T) {
	t.Parallel()

	p := predictor.New()
	features := predictor.ShotFeatures{ProbeBitrateKbps: 4000}

	tests := []struct {
		codec  string
		target float64
		want   int
	}{
		{"libx264", 90, 31},
		{"libx264", 97, 25},
		{"libx265", 90, 37},
		{"libx265", 97, 32},
	}
	for _, tc := range tests {
		t.Run(tc.codec, func(t *testing.T) {
			t.Parallel()

			got, err := p.PickCRF(features, tc.target, tc.codec)
			if err != nil {
				t.Fatalf("PickCRF: %v", err)
			}
			if got != tc.want {
				t.Errorf("PickCRF(%s, target=%v) = %d, want %d",
					tc.codec, tc.target, got, tc.want)
			}
		})
	}
}

// TestPickCRF_unreachableTargetReturnsDefault pins the fallback: when nothing
// in the window clears the bar, the adapter's quality_default is returned
// rather than an out-of-range CRF.
func TestPickCRF_unreachableTargetReturnsDefault(t *testing.T) {
	t.Parallel()

	p := predictor.New()
	// The curve clamps at 100, so any target above the scale is unreachable
	// at every CRF in the window. (Verified against the Python pick_crf,
	// which returns 23 for the same inputs.)
	crf, err := p.PickCRF(
		predictor.ShotFeatures{ProbeBitrateKbps: 1.0}, 100.1, "libx264")
	if err != nil {
		t.Fatalf("PickCRF: %v", err)
	}
	if crf != 23 {
		t.Errorf("crf = %d, want the libx264 quality_default 23", crf)
	}
}

// TestPickKeyint covers the three bands.
func TestPickKeyint(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name         string
		lengthFrames int
		bitrate      float64
		fps          float64
		wantKeyint   int
		wantMin      int
	}{
		{
			name:         "long low-motion shot gets an extended GOP",
			lengthFrames: 240, bitrate: 900, fps: 24,
			wantKeyint: 96, wantMin: 24,
		},
		{
			name:         "high-motion shot gets a tight GOP",
			lengthFrames: 240, bitrate: 9000, fps: 24,
			wantKeyint: 24, wantMin: 12,
		},
		{
			name:         "everything else gets the canonical GOP",
			lengthFrames: 240, bitrate: 4000, fps: 24,
			wantKeyint: 48, wantMin: 12,
		},
		{
			name:         "short low-motion shot is not long enough for the extension",
			lengthFrames: 40, bitrate: 900, fps: 24,
			wantKeyint: 48, wantMin: 12,
		},
		{
			name:         "sub-1 fps floors at 1",
			lengthFrames: 10, bitrate: 4000, fps: 0.2,
			wantKeyint: 2, wantMin: 1,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			keyint, minKeyint := predictor.PickKeyint(predictor.ShotFeatures{
				ShotLengthFrames: tc.lengthFrames, ProbeBitrateKbps: tc.bitrate,
			}, tc.fps)
			if keyint != tc.wantKeyint || minKeyint != tc.wantMin {
				t.Errorf("PickKeyint = (%d, %d), want (%d, %d)",
					keyint, minKeyint, tc.wantKeyint, tc.wantMin)
			}
		})
	}
}

// TestFeatureVector pins the fourteen-element ONNX input layout. Reordering
// it silently corrupts every learned prediction, so the order is asserted
// element by element.
func TestFeatureVector(t *testing.T) {
	t.Parallel()

	f := predictor.ShotFeatures{
		ProbeBitrateKbps: 1, ProbeIFrameAvgBytes: 2,
		ProbePFrameAvgBytes: 3, ProbeBFrameAvgBytes: 4,
		SaliencyMean: 5, SaliencyVar: 6,
		FrameDiffMean: 7, YAvg: 8, YVar: 9,
		ShotLengthFrames: 10, FPS: 11, Width: 12, Height: 13,
	}
	want := []float64{99, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}
	if got := predictor.FeatureVector(f, 99); !slices.Equal(got, want) {
		t.Errorf("FeatureVector = %v, want %v", got, want)
	}
}

// stubSession is a Session that returns a fixed value, or an error.
type stubSession struct {
	value float64
	err   error
	calls int
}

func (s *stubSession) Infer(inputs []float64) (float64, error) {
	s.calls++
	if s.err != nil {
		return 0, s.err
	}
	if len(inputs) != 14 {
		return 0, errors.New("stub: unexpected input arity")
	}
	return s.value, nil
}

// TestPredictVMAF_sessionPrecedence asserts an ONNX session wins over the
// analytical curve, that its output is clamped, and that an inference failure
// degrades to the curve instead of aborting the sweep.
func TestPredictVMAF_sessionPrecedence(t *testing.T) {
	t.Parallel()

	features := predictor.ShotFeatures{ProbeBitrateKbps: 1000}

	t.Run("session output wins", func(t *testing.T) {
		t.Parallel()

		session := &stubSession{value: 42.0}
		p := predictor.WithSession(session)
		if got := p.PredictVMAF(features, 35, "libx264"); got != 42.0 {
			t.Errorf("PredictVMAF = %v, want the session's 42.0", got)
		}
		if session.calls != 1 {
			t.Errorf("session calls = %d, want 1", session.calls)
		}
	})

	t.Run("session output is clamped", func(t *testing.T) {
		t.Parallel()

		p := predictor.WithSession(&stubSession{value: 250.0})
		if got := p.PredictVMAF(features, 35, "libx264"); got != 100.0 {
			t.Errorf("PredictVMAF = %v, want it clamped to 100", got)
		}
	})

	t.Run("inference failure falls back to the curve", func(t *testing.T) {
		t.Parallel()

		p := predictor.WithSession(&stubSession{err: errors.New("ort exploded")})
		got := p.PredictVMAF(features, 35, "libx264")
		want := predictor.New().PredictAnalytical(features, 35, "libx264")
		if math.Abs(got-want) > 1e-9 {
			t.Errorf("PredictVMAF = %v, want the analytical %v", got, want)
		}
	})
}

// TestPredictMOS covers the documented linear rescale and its clamping.
func TestPredictMOS(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		session *stubSession
		want    float64
	}{
		// VMAF 100 -> (100-30)/14 = 5.0
		{name: "transparent maps to the top of the scale",
			session: &stubSession{value: 100.0}, want: 5.0},
		// VMAF 72 -> (72-30)/14 = 3.0
		{name: "mid quality", session: &stubSession{value: 72.0}, want: 3.0},
		// VMAF 30 -> 0.0, clamped up to 1.0
		{name: "visibly distorted clamps at the bottom",
			session: &stubSession{value: 30.0}, want: 1.0},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			p := predictor.WithSession(tc.session)
			got, err := p.PredictMOS(predictor.ShotFeatures{}, "libx264", nil)
			if err != nil {
				t.Fatalf("PredictMOS: %v", err)
			}
			if math.Abs(got-tc.want) > 1e-9 {
				t.Errorf("PredictMOS = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestSelectValidationShots pins the stratified sampler against a dump of the
// Python select_validation_shots for the same 20-shot timeline, where shot i
// has probe bitrate 100*(20-i) so the ranking is the reverse of row order.
func TestSelectValidationShots(t *testing.T) {
	t.Parallel()

	shots := make([]pershot.Shot, 20)
	features := make(map[pershot.Shot]predictor.ShotFeatures, 20)
	for i := range shots {
		shots[i] = pershot.Shot{StartFrame: i * 10, EndFrame: (i + 1) * 10}
		features[shots[i]] = predictor.ShotFeatures{
			ProbeBitrateKbps: float64(100 * (20 - i)),
		}
	}

	tests := []struct {
		name      string
		k         int
		strategy  predictor.SelectionStrategy
		wantStart []int
		wantErr   bool
	}{
		{
			name: "k=8 draws two per quartile", k: 8, strategy: predictor.Stratified,
			wantStart: []int{190, 170, 140, 120, 90, 70, 40, 20},
		},
		{
			name: "k=5 puts the remainder in the top quartile",
			k:    5, strategy: predictor.Stratified,
			wantStart: []int{190, 140, 90, 40, 20},
		},
		{
			name: "k=3 is fewer than four quartiles",
			k:    3, strategy: predictor.Stratified,
			wantStart: []int{40, 30, 20},
		},
		{
			name: "k=4 draws one per quartile", k: 4, strategy: predictor.Stratified,
			wantStart: []int{190, 140, 90, 40},
		},
		{
			name: "k equal to the shot count returns everything in row order",
			k:    20, strategy: predictor.Stratified,
			wantStart: []int{
				0, 10, 20, 30, 40, 50, 60, 70, 80, 90,
				100, 110, 120, 130, 140, 150, 160, 170, 180, 190,
			},
		},
		{
			name: "k above the shot count returns everything",
			k:    25, strategy: predictor.Stratified,
			wantStart: []int{
				0, 10, 20, 30, 40, 50, 60, 70, 80, 90,
				100, 110, 120, 130, 140, 150, 160, 170, 180, 190,
			},
		},
		{
			name: "head strategy takes the first k in row order",
			k:    3, strategy: predictor.Head,
			wantStart: []int{0, 10, 20},
		},
		{
			name: "k=0 selects nothing", k: 0, strategy: predictor.Stratified,
			wantStart: nil,
		},
		{
			name: "unknown strategy is an error", k: 4, strategy: "random",
			wantErr: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got, err := predictor.SelectValidationShots(shots, features, tc.k, tc.strategy)
			if (err != nil) != tc.wantErr {
				t.Fatalf("SelectValidationShots error = %v, wantErr %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			starts := make([]int, len(got))
			for i, s := range got {
				starts[i] = s.StartFrame
			}
			if !slices.Equal(starts, tc.wantStart) {
				t.Errorf("selected starts = %v, want %v", starts, tc.wantStart)
			}
		})
	}
}

// TestDecideVerdict pins the three-way verdict rule and the bias correction.
func TestDecideVerdict(t *testing.T) {
	t.Parallel()

	mk := func(residuals ...float64) []predictor.ShotResidual {
		out := make([]predictor.ShotResidual, len(residuals))
		for i, r := range residuals {
			out[i] = predictor.ShotResidual{
				Shot:          pershot.Shot{StartFrame: i, EndFrame: i + 1},
				PredictedVMAF: 90.0,
				MeasuredVMAF:  90.0 + r,
			}
		}
		return out
	}

	tests := []struct {
		name        string
		residuals   []predictor.ShotResidual
		threshold   float64
		wantVerdict predictor.Verdict
		wantBias    float64
	}{
		{
			name:      "every residual inside the threshold is gospel",
			residuals: mk(0.5, -1.0, 1.4), threshold: 1.5,
			wantVerdict: predictor.Gospel,
		},
		{
			name:      "exactly at the threshold is still gospel",
			residuals: mk(1.5, -1.5), threshold: 1.5,
			wantVerdict: predictor.Gospel,
		},
		{
			name:      "tight spread with a biased mean recalibrates",
			residuals: mk(2.0, 2.2, 2.4), threshold: 1.5,
			wantVerdict: predictor.Recalibrate, wantBias: 2.2,
		},
		{
			name:      "wide spread falls back regardless of the mean",
			residuals: mk(-5.0, 0.0, 5.0), threshold: 1.5,
			wantVerdict: predictor.FallBack,
		},
		{
			name:      "tight spread with an unbiased mean falls back",
			residuals: mk(-1.6, 1.6), threshold: 1.5,
			wantVerdict: predictor.FallBack,
		},
		{
			name:      "an empty selection is pessimistic",
			residuals: nil, threshold: 1.5,
			wantVerdict: predictor.FallBack,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			got := predictor.DecideVerdict(tc.residuals, 93.0, tc.threshold)
			if got.Verdict != tc.wantVerdict {
				t.Errorf("verdict = %q, want %q", got.Verdict, tc.wantVerdict)
			}
			if math.Abs(got.BiasCorrection-tc.wantBias) > 1e-9 {
				t.Errorf("bias correction = %v, want %v", got.BiasCorrection, tc.wantBias)
			}
		})
	}
}

// TestValidationReport_aggregates covers the max / mean helpers including the
// empty-run guard.
func TestValidationReport_aggregates(t *testing.T) {
	t.Parallel()

	report := predictor.ValidationReport{
		Residuals: []predictor.ShotResidual{
			{PredictedVMAF: 90, MeasuredVMAF: 92}, // +2
			{PredictedVMAF: 90, MeasuredVMAF: 87}, // -3
			{PredictedVMAF: 90, MeasuredVMAF: 91}, // +1
		},
	}
	if got := report.MaxAbsResidual(); math.Abs(got-3.0) > 1e-9 {
		t.Errorf("MaxAbsResidual = %v, want 3", got)
	}
	if got := report.MeanResidual(); math.Abs(got-0.0) > 1e-9 {
		t.Errorf("MeanResidual = %v, want 0", got)
	}

	empty := predictor.ValidationReport{}
	if got := empty.MaxAbsResidual(); got != 0 {
		t.Errorf("empty MaxAbsResidual = %v, want 0", got)
	}
	if got := empty.MeanResidual(); got != 0 {
		t.Errorf("empty MeanResidual = %v, want 0", got)
	}
}

// TestValidate drives the whole harness through injected seams so no ffmpeg
// or vmaf binary is needed.
func TestValidate(t *testing.T) {
	t.Parallel()

	shots := make([]pershot.Shot, 12)
	for i := range shots {
		shots[i] = pershot.Shot{StartFrame: i * 100, EndFrame: (i + 1) * 100}
	}
	extract := func(s pershot.Shot) (predictor.ShotFeatures, error) {
		return predictor.ShotFeatures{
			ProbeBitrateKbps: float64(1000 + s.StartFrame),
			ShotLengthFrames: s.Length(),
		}, nil
	}

	t.Run("gospel when the encoder agrees with the predictor", func(t *testing.T) {
		t.Parallel()

		p := predictor.New()
		encodeAndScore := func(s pershot.Shot, crf int, codec string) (string, float64, error) {
			feats, _ := extract(s)
			// Measure exactly what the predictor said, plus a hair.
			return "/tmp/enc.mp4", p.PredictVMAF(feats, crf, codec) + 0.25, nil
		}
		report, err := predictor.Validate(p, shots, extract, encodeAndScore,
			predictor.ValidateOptions{
				TargetVMAF: 90, Codec: "libx264", K: 4,
				ResidualThresholdVMAF: 1.5,
			})
		if err != nil {
			t.Fatalf("Validate: %v", err)
		}
		if report.Verdict != predictor.Gospel {
			t.Errorf("verdict = %q, want %q", report.Verdict, predictor.Gospel)
		}
		if len(report.Residuals) != 4 {
			t.Errorf("residual count = %d, want 4", len(report.Residuals))
		}
	})

	t.Run("fall back when the encoder disagrees wildly", func(t *testing.T) {
		t.Parallel()

		p := predictor.New()
		encodeAndScore := func(s pershot.Shot, _ int, _ string) (string, float64, error) {
			// Alternate large positive and negative errors -> wide spread.
			if s.StartFrame%200 == 0 {
				return "/tmp/enc.mp4", 60.0, nil
			}
			return "/tmp/enc.mp4", 99.0, nil
		}
		report, err := predictor.Validate(p, shots, extract, encodeAndScore,
			predictor.ValidateOptions{
				TargetVMAF: 90, Codec: "libx264", K: 4,
				ResidualThresholdVMAF: 1.5,
			})
		if err != nil {
			t.Fatalf("Validate: %v", err)
		}
		if report.Verdict != predictor.FallBack {
			t.Errorf("verdict = %q, want %q", report.Verdict, predictor.FallBack)
		}
	})

	t.Run("a feature-extraction failure aborts", func(t *testing.T) {
		t.Parallel()

		failing := func(pershot.Shot) (predictor.ShotFeatures, error) {
			return predictor.ShotFeatures{}, errors.New("ffprobe missing")
		}
		_, err := predictor.Validate(predictor.New(), shots, failing,
			func(pershot.Shot, int, string) (string, float64, error) {
				return "", 0, nil
			},
			predictor.ValidateOptions{TargetVMAF: 90, Codec: "libx264", K: 4})
		if err == nil {
			t.Fatal("expected an error when feature extraction fails")
		}
	})

	t.Run("a scoring failure aborts", func(t *testing.T) {
		t.Parallel()

		_, err := predictor.Validate(predictor.New(), shots, extract,
			func(pershot.Shot, int, string) (string, float64, error) {
				return "", 0, errors.New("vmaf missing")
			},
			predictor.ValidateOptions{TargetVMAF: 90, Codec: "libx264", K: 4})
		if err == nil {
			t.Fatal("expected an error when scoring fails")
		}
	})
}
