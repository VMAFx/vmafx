// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/predictor/predictor.go — Go port of vmaftune.predictor.
//
// The shipped predictor estimates VMAF from cheap per-shot signals so a
// per-title / per-shot tune can pick a CRF without running libvmaf on every
// candidate. The Python module ships two inference paths:
//
//   - a per-codec analytical curve (the default, used whenever no ONNX model
//     is loaded), and
//   - an ONNX MLP loaded through onnxruntime when the caller passes
//     --model predictor_<codec>.onnx.
//
// This port implements the analytical curve, which is the path the sidecar
// operator surface exercises by default (vmaf-tune sidecar's --model flag
// defaults to None). See the package README note in sidecar.go and the
// migration report for the ONNX gap.
package predictor

import (
	"math"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
)

// ShotFeatures holds the cheap-to-compute signals fed to the VMAF predictor.
//
// Every entry is either trivially extractable from a probe encode's stderr or
// one ONNX forward pass on a single centre frame; no entry requires running
// VMAF or the final encoder at the target CRF.
type ShotFeatures struct {
	// Probe-encode-derived complexity barometer.
	ProbeBitrateKbps    float64 `json:"probe_bitrate_kbps"`
	ProbeIFrameAvgBytes float64 `json:"probe_i_frame_avg_bytes"`
	ProbePFrameAvgBytes float64 `json:"probe_p_frame_avg_bytes"`
	ProbeBFrameAvgBytes float64 `json:"probe_b_frame_avg_bytes"`

	// Optional learned signals (0.0 when unavailable).
	SaliencyMean float64 `json:"saliency_mean"`
	SaliencyVar  float64 `json:"saliency_var"`

	// Temporal / spatial signals from FFmpeg signalstats.
	FrameDiffMean float64 `json:"frame_diff_mean"`
	YAvg          float64 `json:"y_avg"`
	YVar          float64 `json:"y_var"`

	// Structural metadata.
	ShotLengthFrames int     `json:"shot_length_frames"`
	FPS              float64 `json:"fps"`
	Width            int     `json:"width"`
	Height           int     `json:"height"`
}

// resolutionClassBand pairs a max height with its ladder-aligned class name.
type resolutionClassBand struct {
	limit int
	name  string
}

// resolutionClasses mirrors the ladder.py / corpus.py vocabulary.
var resolutionClasses = []resolutionClassBand{
	{480, "sd"},       // <= 480 lines
	{720, "hd_ready"}, // 480 < h <= 720
	{1080, "hd"},      // 720 < h <= 1080
	{2160, "uhd"},     // 1080 < h <= 2160
	{4320, "uhd8k"},   // 2160 < h <= 4320
}

// ResolutionClass returns the ladder-aligned resolution-class name for height.
func ResolutionClass(height int) string {
	for _, band := range resolutionClasses {
		if height <= band.limit {
			return band.name
		}
	}
	return "uhd8k" // everything beyond 4320 lumps into the highest class
}

// Coefficients are the per-codec analytical-curve parameters. The curve is
//
//	vmaf ~= a - b*delta - c*delta^2 + d*log10(bitrate_kbps)
//
// where delta = crf - crfRef. These are seed values that produce a monotone,
// broadly-correct shape; the trained ONNX model replaces them at the inference
// call site when one is loaded.
type Coefficients struct {
	A      float64
	B      float64
	C      float64
	D      float64
	CRFRef float64
}

// DefaultCoefficients mirrors predictor._DEFAULT_COEFFS verbatim.
var DefaultCoefficients = map[string]Coefficients{
	"libx264":    {95.0, 1.20, 0.012, 1.5, 23.0},
	"libx265":    {96.0, 1.10, 0.010, 1.6, 28.0},
	"libsvtav1":  {94.0, 0.95, 0.009, 1.4, 35.0},
	"libaom-av1": {94.0, 0.90, 0.009, 1.4, 35.0},
	"libvvenc":   {95.0, 0.85, 0.008, 1.5, 32.0},
	"h264_nvenc": {93.0, 1.30, 0.014, 1.4, 23.0},
	"hevc_nvenc": {94.0, 1.20, 0.012, 1.5, 23.0},
	"av1_nvenc":  {93.0, 1.10, 0.011, 1.5, 23.0},
	"h264_amf":   {92.0, 1.40, 0.015, 1.3, 23.0},
	"hevc_amf":   {93.0, 1.30, 0.013, 1.4, 23.0},
	"av1_amf":    {92.0, 1.20, 0.012, 1.4, 23.0},
	"h264_qsv":   {93.0, 1.30, 0.013, 1.4, 23.0},
	"hevc_qsv":   {94.0, 1.20, 0.012, 1.5, 23.0},
	"av1_qsv":    {93.0, 1.10, 0.011, 1.5, 23.0},
}

// Predictor evaluates the per-codec analytical VMAF curve.
//
// Construct with New() to get the shipped coefficient table. The zero value is
// usable but carries no coefficients and therefore falls back to the libx264
// row for every codec, matching the Python .get(codec, _DEFAULT_COEFFS["libx264"])
// lookup.
type Predictor struct {
	// Coefficients is the per-codec curve table. Nil means "use the
	// shipped defaults".
	Coefficients map[string]Coefficients
}

// New returns a Predictor backed by the shipped coefficient table.
func New() *Predictor {
	coeffs := make(map[string]Coefficients, len(DefaultCoefficients))
	for k, v := range DefaultCoefficients {
		coeffs[k] = v
	}
	return &Predictor{Coefficients: coeffs}
}

// coefficientsFor resolves the curve for codec, falling back to libx264.
func (p *Predictor) coefficientsFor(codec string) Coefficients {
	table := p.Coefficients
	if table == nil {
		table = DefaultCoefficients
	}
	if c, ok := table[codec]; ok {
		return c
	}
	return DefaultCoefficients["libx264"]
}

// PredictVMAF returns the predicted VMAF for codec at targetQuality, clamped
// to [0, 100].
func (p *Predictor) PredictVMAF(f ShotFeatures, targetQuality int, codec string) float64 {
	c := p.coefficientsFor(codec)
	delta := float64(targetQuality) - c.CRFRef
	// log(0) guard — pin the bitrate floor to a kbps that maps to roughly
	// the lowest probe encode any test fixture produces.
	bitrate := math.Max(f.ProbeBitrateKbps, 1.0)
	vmaf := c.A - c.B*delta - c.C*delta*delta + c.D*math.Log10(bitrate)
	return Clamp(vmaf, 0.0, 100.0)
}

// PickCRF inverts the predictor: it binary-searches the codec adapter's
// quality range for the highest CRF whose predicted VMAF still meets
// targetVMAF (the most-compressed encode that still hits quality).
func (p *Predictor) PickCRF(f ShotFeatures, targetVMAF float64, codec string) (int, error) {
	adapter, err := codecadapter.Get(codec)
	if err != nil {
		return 0, err
	}
	lo, hi := adapter.QualityRange()
	best := adapter.QualityDefault()
	for lo <= hi {
		mid := (lo + hi) / 2
		if p.PredictVMAF(f, mid, codec) >= targetVMAF {
			best = mid
			lo = mid + 1 // try a higher CRF (more compression)
		} else {
			hi = mid - 1
		}
	}
	return best, nil
}

// PickKeyint is the heuristic GOP / min-GOP picker driven by probe bitrate and
// shot length. It returns (keyint, minKeyint).
//
//   - Long, low-motion shots get an extended keyint of 4*fps.
//   - High-motion shots get a tight keyint = fps.
//   - Everything else gets keyint = 2*fps, minKeyint = fps/2.
func PickKeyint(f ShotFeatures, fps float64) (int, int) {
	fpsInt := int(math.Round(fps))
	if fpsInt < 1 {
		fpsInt = 1
	}
	// Empirical bands in kbps for 1080p natural content at the codec's
	// probe_quality.
	const lowMotionThresholdKbps = 1500.0
	const highMotionThresholdKbps = 8000.0

	isLong := f.ShotLengthFrames >= int(math.Round(4.0*float64(fpsInt)))
	bitrate := f.ProbeBitrateKbps
	if isLong && bitrate < lowMotionThresholdKbps {
		return 4 * fpsInt, fpsInt
	}
	half := fpsInt / 2
	if half < 1 {
		half = 1
	}
	if bitrate > highMotionThresholdKbps {
		return fpsInt, half
	}
	return 2 * fpsInt, half
}

// Clamp returns v constrained to [lo, hi].
func Clamp(v, lo, hi float64) float64 {
	return math.Max(lo, math.Min(hi, v))
}
