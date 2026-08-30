// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

// Package predictor is the Go port of vmaftune.predictor — the predict half
// of the predict-then-verify loop.
//
// A Predictor estimates the VMAF a (features, CRF, codec) triple would score
// without running the encoder or libvmaf. Two backends:
//
//   - The per-codec analytical curve (the default). Pure arithmetic, no
//     dependencies, monotone-decreasing in CRF. This is what the Python
//     Predictor falls back to whenever onnxruntime is absent or no model
//     path was given, and it is the path the unit tests exercise on both
//     sides.
//   - A trained per-codec MLP shipped as predictor_<codec>.onnx, run through
//     pkg/ai's ORT bridge. Optional: when the ORT runner is not on PATH the
//     constructor degrades to the analytical curve with a logged note, which
//     mirrors the Python ImportError fallback rather than failing the run.
//
// PickCRF inverts the predictor by binary search over the codec adapter's
// quality window, returning the most-compressed CRF that still meets target.
package predictor

import (
	"context"
	"errors"
	"log/slog"
	"math"

	"github.com/VMAFx/vmafx/pkg/ai"
	"github.com/VMAFx/vmafx/pkg/tune/codec"
	"github.com/VMAFx/vmafx/pkg/tune/pymath"
)

// ShotFeatures are the cheap-to-compute per-shot signals the predictor
// consumes. Field order is load-bearing: it pins the ONNX input tensor layout
// (see Predictor.onnxInputs) and the sidecar's ridge-weight column indices.
//
// Every field is a non-negative float so the model's input normalisation is a
// single per-feature (x - mean) / std shift.
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

// resolutionClass buckets mirror the ladder.py / corpus.py vocabulary so the
// predictor's input shape is stable across the harness.
var resolutionClasses = []struct {
	limit int
	name  string
}{
	{480, "sd"},
	{720, "hd_ready"},
	{1080, "hd"},
	{2160, "uhd"},
	{4320, "uhd8k"},
}

// ResolutionClass returns the ladder-aligned resolution-class name for height.
func ResolutionClass(height int) string {
	for _, rc := range resolutionClasses {
		if height <= rc.limit {
			return rc.name
		}
	}
	return "uhd8k"
}

// Coefficients are the per-codec analytical-curve parameters:
//
//	vmaf ≈ A − B·Δ − C·Δ² + D·log10(bitrate_kbps),  Δ = crf − CRFRef
//
// They are seed values that produce a monotone, broadly-correct shape, not
// authoritative fits; a trained ONNX model replaces them at the call site.
type Coefficients struct {
	A      float64
	B      float64
	C      float64
	D      float64
	CRFRef float64
}

// DefaultCoefficients mirrors vmaftune.predictor._DEFAULT_COEFFS verbatim.
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

// Predictor estimates VMAF from ShotFeatures. The zero value is a usable
// analytical-fallback predictor; use New to attach an ONNX model.
type Predictor struct {
	// Coefficients overrides DefaultCoefficients when non-nil.
	Coefficients map[string]Coefficients

	// modelName is the ONNX model to run through the registry; empty means
	// analytical-only.
	modelName string
	registry  *ai.Registry
	log       *slog.Logger
}

// New builds a Predictor. modelPath is optional: pass "" for the analytical
// fallback, or a predictor_<codec>.onnx path / registry name to enable the
// learned predictor.
//
// A missing ORT runner is not an error — the Python constructor also swallows
// the onnxruntime ImportError and falls back to the analytical curve so the
// harness degrades gracefully on hosts without the ML stack. A model path
// that does not resolve to a file *is* an error, matching the Python
// FileNotFoundError.
func New(modelPath string, log *slog.Logger) (*Predictor, error) {
	if log == nil {
		log = slog.Default()
	}
	p := &Predictor{log: log}
	if modelPath == "" {
		return p, nil
	}
	registry := ai.NewRegistry("")
	if _, err := registry.ModelPath(modelPath); err != nil {
		return nil, err
	}
	p.registry = registry
	p.modelName = modelPath
	return p, nil
}

// coefficientsFor resolves the per-codec curve, falling back to libx264 for
// unregistered codecs exactly as the Python dict .get(codec, _DEFAULT["libx264"])
// does.
func (p *Predictor) coefficientsFor(codecName string) Coefficients {
	if p.Coefficients != nil {
		if c, ok := p.Coefficients[codecName]; ok {
			return c
		}
	}
	if c, ok := DefaultCoefficients[codecName]; ok {
		return c
	}
	return DefaultCoefficients["libx264"]
}

// PredictVMAF returns the predicted VMAF for codecName at targetQuality,
// clamped to [0, 100]. Uses the ONNX model when one is loaded and reachable;
// otherwise the analytical curve.
func (p *Predictor) PredictVMAF(features ShotFeatures, targetQuality int, codecName string) float64 {
	if p.registry != nil {
		if score, err := p.predictONNX(features, targetQuality); err == nil {
			return score
		} else if !errors.Is(err, ai.ErrORTRunnerNotFound) {
			p.log.Warn("predictor: ONNX inference failed; using analytical curve",
				"model", p.modelName, "error", err)
		}
	}
	return p.predictAnalytical(features, targetQuality, codecName)
}

// predictAnalytical is the per-codec closed-form fallback.
//
// The bitrate term is the per-shot complexity correction: a high-motion shot's
// probe encode spends more bits at the reference CRF, raising predicted
// quality at the same target CRF.
func (p *Predictor) predictAnalytical(features ShotFeatures, crf int, codecName string) float64 {
	c := p.coefficientsFor(codecName)
	delta := float64(crf) - c.CRFRef
	// log10(0) guard — pin the bitrate floor at 1 kbps.
	bitrate := math.Max(features.ProbeBitrateKbps, 1.0)
	// pymath.Log10, not math.Log10: this value reaches the auto planner's
	// estimated_vmaf, a user-discoverable JSON field that must match the
	// Python emitter, and Go's Log10 (implemented as Log2·Ln2/Ln10) lands a
	// ULP off on ordinary probe bitrates. See pkg/tune/pymath.
	vmaf := c.A - c.B*delta - c.C*delta*delta + c.D*pymath.Log10(bitrate)
	return clamp(vmaf, 0.0, 100.0)
}

// onnxInputs materialises the input tensor. Layout is pinned by
// predictor_train.py and documented in the model card; it must stay in
// lockstep with the Python _predict_onnx tensor.
func onnxInputs(features ShotFeatures, crf int) []float64 {
	return []float64{
		float64(crf),
		features.ProbeBitrateKbps,
		features.ProbeIFrameAvgBytes,
		features.ProbePFrameAvgBytes,
		features.ProbeBFrameAvgBytes,
		features.SaliencyMean,
		features.SaliencyVar,
		features.FrameDiffMean,
		features.YAvg,
		features.YVar,
		float64(features.ShotLengthFrames),
		features.FPS,
		float64(features.Width),
		float64(features.Height),
	}
}

func (p *Predictor) predictONNX(features ShotFeatures, crf int) (float64, error) {
	out, err := p.registry.Infer(context.Background(), p.modelName, onnxInputs(features, crf))
	if err != nil {
		return 0, err
	}
	if len(out) == 0 {
		return 0, errors.New("predictor: ONNX model returned an empty tensor")
	}
	return clamp(out[0], 0.0, 100.0), nil
}

// PickCRF finds the CRF in the codec's quality window that hits targetVMAF.
//
// Binary search for the *largest* CRF whose predicted VMAF is still at or
// above target — the most-compressed encode that still meets quality. The
// predictor is non-strictly monotone-decreasing in CRF for every shipped
// codec, so this converges in ceil(log2(range)) predictor calls.
//
// An unregistered codec returns the error from the adapter registry; the
// caller decides whether to skip the cell or fail the run.
func PickCRF(p *Predictor, features ShotFeatures, targetVMAF float64, codecName string) (int, error) {
	adapter, err := codec.Get(codecName)
	if err != nil {
		return 0, err
	}
	lo, hi := adapter.QualityLo, adapter.QualityHi
	best := adapter.QualityDefault
	for lo <= hi {
		mid := (lo + hi) / 2
		if p.PredictVMAF(features, mid, codecName) >= targetVMAF {
			best = mid
			lo = mid + 1 // try a higher CRF (more compression)
		} else {
			hi = mid - 1
		}
	}
	return best, nil
}

func clamp(v, lo, hi float64) float64 {
	return math.Min(math.Max(v, lo), hi)
}
