// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package predictor is the Go port of tools/vmaf-tune/src/vmaftune/
// predictor.py — the predict half of the predict-then-verify loop.
//
// A movie split into shots can be encoded at the right CRF per shot WITHOUT
// running VMAF on every shot, by estimating VMAF from cheap signals: the
// probe encode's bitrate and frame-size statistics, optional saliency
// moments, FFmpeg signalstats, and the shot's structural metadata.
//
// Two prediction paths, exactly as in the Python original:
//
//   - Analytical fallback — a per-codec closed-form curve
//     vmaf ~= a - b*d - c*d^2 + e*log10(bitrate_kbps), d = crf - crf_ref.
//     Monotone decreasing in CRF by construction, which is what makes the
//     binary-search inversion in PickCRF sound. This is pure arithmetic in
//     both languages, so the Go port is numerically identical.
//   - Learned ONNX model — an inference seam (the Session interface). The
//     Python original loads the model with onnxruntime and silently falls
//     back to the analytical curve when onnxruntime is unavailable; the Go
//     port keeps that exact posture, with the seam supplied by the caller.
//     See the package's AGENTS.md note on the ORT runner dependency.
package predictor

import (
	"errors"
	"fmt"
	"log/slog"
	"math"
	"sort"
	"sync"

	"github.com/VMAFx/vmafx/pkg/pymath"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/pershot"
)

// ShotFeatures is the cheap-to-compute signal vector fed to the predictor.
//
// Every entry is either trivially extractable from the probe encode's stderr
// or one forward pass on a single centre frame. Nothing here requires running
// VMAF or the final encoder at the target CRF.
type ShotFeatures struct {
	// Probe-encode-derived complexity barometer.
	ProbeBitrateKbps    float64
	ProbeIFrameAvgBytes float64
	ProbePFrameAvgBytes float64
	ProbeBFrameAvgBytes float64

	// Optional learned signals (0 when unavailable).
	SaliencyMean float64
	SaliencyVar  float64

	// Temporal / spatial signals from FFmpeg signalstats.
	FrameDiffMean float64
	YAvg          float64
	YVar          float64

	// Structural metadata.
	ShotLengthFrames int
	FPS              float64
	Width            int
	Height           int
}

// resolutionClass maps a height onto the ladder.py-aligned class name. The
// bands are inclusive upper bounds.
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

// ResolutionClass returns the resolution-class name for height. Anything
// beyond 4320 lines lumps into the highest class.
func ResolutionClass(height int) string {
	for _, band := range resolutionClasses {
		if height <= band.limit {
			return band.name
		}
	}
	return "uhd8k"
}

// Coefficients are the analytical curve's per-codec constants:
// vmaf ~= A - B*d - C*d^2 + D*log10(bitrate_kbps), where d = crf - CRFRef.
//
// These are seed values, not authoritative fits: they produce a monotone,
// broadly-correct shape so the harness runs without an ONNX model. A trained
// model replaces them at the inference call site.
type Coefficients struct {
	A      float64
	B      float64
	C      float64
	D      float64
	CRFRef float64
}

// defaultCoeffs mirrors vmaftune.predictor._DEFAULT_COEFFS verbatim.
var defaultCoeffs = map[string]Coefficients{
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

// DefaultCoefficients returns a copy of the shipped per-codec coefficient
// table.
func DefaultCoefficients() map[string]Coefficients {
	out := make(map[string]Coefficients, len(defaultCoeffs))
	for k, v := range defaultCoeffs {
		out[k] = v
	}
	return out
}

// Session is the ONNX-inference seam.
//
// Infer receives the fourteen-element feature vector in the layout the model
// card pins (crf first, then the ShotFeatures fields in declaration order)
// and returns the model's scalar output.
//
// The Python original constructs this from onnxruntime.InferenceSession with
// the CPU provider pinned (the model is tiny, ~16x64x1, so a GPU provider
// buys nothing). In Go there is no in-process ORT without a cgo binding, so
// the caller supplies the implementation — see the CLI wiring, which routes
// it through pkg/ai's ORT runner when that runner is on PATH.
type Session interface {
	Infer(inputs []float64) (float64, error)
}

// Predictor estimates VMAF from ShotFeatures.
//
// The zero value is the analytical fallback, which is what the Python
// Predictor() constructor with no model_path produces.
type Predictor struct {
	Coefficients map[string]Coefficients
	// Session, when non-nil, replaces the analytical curve.
	Session Session
	// Log, when non-nil, receives a single warning if the session is
	// configured but inference never succeeds.
	Log *slog.Logger
	// SessionErr holds the first inference error, so a caller can report that
	// a --model run degraded to the analytical curve.
	SessionErr error

	sessionFailOnce sync.Once
}

// New returns a Predictor using the shipped coefficients and no ONNX session.
func New() *Predictor {
	return &Predictor{Coefficients: DefaultCoefficients()}
}

// WithSession returns a Predictor backed by an ONNX inference session.
func WithSession(s Session) *Predictor {
	return &Predictor{Coefficients: DefaultCoefficients(), Session: s}
}

// SessionFailed reports the first inference error, if the ONNX session was
// configured but never usable. Callers use it to tell the operator that a
// --model run silently degraded to the analytical curve.
func (p *Predictor) SessionFailed() error { return p.SessionErr }

// coeffsFor resolves the codec's curve, falling back to libx264's.
func (p *Predictor) coeffsFor(codec string) Coefficients {
	table := p.Coefficients
	if table == nil {
		table = defaultCoeffs
	}
	if c, ok := table[codec]; ok {
		return c
	}
	return defaultCoeffs["libx264"]
}

// PredictVMAF estimates the VMAF score for codec at crf, clamped to [0, 100].
//
// An ONNX session, when present, takes precedence. If inference fails the
// analytical curve is used — a model that errors mid-sweep must degrade the
// prediction, not abort the run, matching the Python fallback posture.
func (p *Predictor) PredictVMAF(features ShotFeatures, crf int, codec string) float64 {
	if p.Session != nil {
		v, err := p.Session.Infer(FeatureVector(features, crf))
		if err == nil {
			return clamp(v, 0.0, 100.0)
		}
		// Record why the learned path did not run. Discarding this error made
		// `predict --model` log "using the learned ONNX predictor" and then
		// return analytical values for the whole sweep with no indication --
		// the operator could not tell a model-backed run from a fallback one.
		// Reported once per Predictor: a sweep calls this per shot per CRF, so
		// logging every failure would bury the run in identical lines.
		p.sessionFailOnce.Do(func() {
			p.SessionErr = err
			if p.Log != nil {
				p.Log.Warn("ONNX predictor unavailable; using the analytical curve",
					"error", err)
			}
		})
	}
	return p.PredictAnalytical(features, crf, codec)
}

// PredictAnalytical evaluates the per-codec closed-form curve.
//
// The bitrate term is the per-shot complexity correction: a high-motion shot
// spends more bits at the reference CRF, which raises predicted quality at
// the same target CRF. The bitrate floor of 1.0 kbps guards log10(0).
func (p *Predictor) PredictAnalytical(features ShotFeatures, crf int, codec string) float64 {
	c := p.coeffsFor(codec)
	delta := float64(crf) - c.CRFRef
	bitrate := math.Max(features.ProbeBitrateKbps, 1.0)
	// pymath.Log10, not math.Log10 (pkg/pymath is the libm parity layer,
	// ADR-1137). Go implements Log10 as Log2*Ln2/Ln10 and lands a ULP off
	// CPython on roughly 27% of realistic probe bitrates (measured over 200k
	// samples in 1..50000 kbps), and this value reaches predicted_vmaf, a
	// user-discoverable JSON field that has to match the Python emitter.
	vmaf := c.A - c.B*delta - c.C*delta*delta + c.D*pymath.Log10(bitrate)
	return clamp(vmaf, 0.0, 100.0)
}

// FeatureVector flattens (crf, features) into the fourteen-element input
// tensor the per-codec predictor ONNX consumes. The layout is pinned by
// predictor_train.py and documented in the model card; reordering it silently
// corrupts every prediction, so it lives in one place.
func FeatureVector(f ShotFeatures, crf int) []float64 {
	return []float64{
		float64(crf),
		f.ProbeBitrateKbps,
		f.ProbeIFrameAvgBytes,
		f.ProbePFrameAvgBytes,
		f.ProbeBFrameAvgBytes,
		f.SaliencyMean,
		f.SaliencyVar,
		f.FrameDiffMean,
		f.YAvg,
		f.YVar,
		float64(f.ShotLengthFrames),
		f.FPS,
		float64(f.Width),
		float64(f.Height),
	}
}

// PickCRF inverts the predictor: binary-search the codec's quality range for
// the LARGEST CRF whose predicted VMAF still clears targetVMAF — the
// most-compressed encode that still meets quality.
//
// The predictor is non-strictly monotone decreasing in CRF for every shipped
// codec (TestPredictAnalytical_isMonotone pins that), so the search converges
// in ceil(log2(range)) predictor calls.
func (p *Predictor) PickCRF(features ShotFeatures, targetVMAF float64, codec string) (int, error) {
	adapter, err := codecadapter.Get(codec)
	if err != nil {
		return 0, err
	}
	lo, hi := adapter.QualityRange[0], adapter.QualityRange[1]
	best := adapter.QualityDefault
	for lo <= hi {
		mid := (lo + hi) / 2
		if p.PredictVMAF(features, mid, codec) >= targetVMAF {
			best = mid
			lo = mid + 1 // try more compression
		} else {
			hi = mid - 1
		}
	}
	return best, nil
}

// PickKeyint picks (keyint, minKeyint) for a shot from the probe bitrate and
// shot length.
//
//   - Long, low-motion shots (below the low-motion band, at least 4 s) get an
//     extended keyint of 4*fps: cheap long temporal references.
//   - High-motion shots (above the high-motion band) get keyint = fps so the
//     encoder can drop stale references.
//   - Everything else gets the canonical keyint = 2*fps, minKeyint = fps/2.
//
// The band thresholds are constants rather than learned values because an
// operator needs to tune them per corpus; the learned model picks the CRF on
// top of these bands.
func PickKeyint(features ShotFeatures, fps float64) (keyint, minKeyint int) {
	fpsInt := int(math.Round(fps))
	if fpsInt < 1 {
		fpsInt = 1
	}
	// Empirical bands in kbps for 1080p natural content at the codec's probe
	// quality; valid within about one octave of that resolution.
	const lowMotionThresholdKbps = 1500.0
	const highMotionThresholdKbps = 8000.0

	isLong := features.ShotLengthFrames >= int(math.Round(4.0*float64(fpsInt)))
	half := fpsInt / 2
	if half < 1 {
		half = 1
	}
	switch {
	case isLong && features.ProbeBitrateKbps < lowMotionThresholdKbps:
		return 4 * fpsInt, fpsInt
	case features.ProbeBitrateKbps > highMotionThresholdKbps:
		return fpsInt, half
	default:
		return 2 * fpsInt, half
	}
}

// PredictMOS estimates subjective MOS in [1, 5].
//
// The Python original prefers an optional konvid_mos_head_v1 ONNX and falls
// back to a documented linear rescale of the VMAF prediction,
// MOS = (VMAF - 30) / 14, mapping VMAF 30 (visibly distorted) to the bottom
// of the scale and VMAF 100 (transparent) to the top. Per ADR-0325 §Phase 3
// the fallback is explicitly approximate, not authoritative.
//
// Only the fallback is ported: the MOS head is not shipped in the tree, and
// wiring a second ONNX seam for a path that is documented as approximate
// would add surface without adding fidelity. Callers that need the head
// should use the Python subcommand until the production-flip gate fires.
func (p *Predictor) PredictMOS(features ShotFeatures, codec string, targetQuality *int) (float64, error) {
	crf := 0
	if targetQuality != nil {
		crf = *targetQuality
	} else {
		adapter, err := codecadapter.Get(codec)
		if err != nil {
			return 0, err
		}
		crf = adapter.QualityDefault
	}
	predicted := p.PredictVMAF(features, crf, codec)
	return clamp((predicted-30.0)/14.0, 1.0, 5.0), nil
}

// Verdict is the outcome of a predictor validation run.
type Verdict string

const (
	// Gospel — every residual is within the threshold. Trust the predictor
	// on the remaining shots.
	Gospel Verdict = "gospel"
	// Recalibrate — residuals are biased but tight. Apply the one-parameter
	// linear shift and redo the picks; no retraining needed.
	Recalibrate Verdict = "recalibrate"
	// FallBack — residuals too wide. Degrade to the full encode-and-score
	// loop on the remaining shots.
	FallBack Verdict = "fall_back"
)

// ShotResidual is one validation point: the predictor said X, real VMAF was Y.
type ShotResidual struct {
	Shot          pershot.Shot
	CRFPicked     int
	PredictedVMAF float64
	MeasuredVMAF  float64
}

// Residual is the signed residual measured - predicted.
//
// The sign matters: a biased-low residual (the predictor over-estimates
// quality) is worse for downstream gating than a biased-high one.
func (r ShotResidual) Residual() float64 { return r.MeasuredVMAF - r.PredictedVMAF }

// ValidationReport summarises a validation run.
type ValidationReport struct {
	Verdict    Verdict
	Residuals  []ShotResidual
	TargetVMAF float64
	// ThresholdVMAF is the max |residual| tolerated before falling back.
	ThresholdVMAF float64
	// BiasCorrection is the signed VMAF offset to add to predictions on the
	// remaining shots. Only set when Verdict is Recalibrate.
	BiasCorrection float64
}

// MaxAbsResidual returns the largest absolute residual, 0 for an empty run.
func (r ValidationReport) MaxAbsResidual() float64 {
	m := 0.0
	for _, res := range r.Residuals {
		if a := math.Abs(res.Residual()); a > m {
			m = a
		}
	}
	return m
}

// MeanResidual returns the mean signed residual, 0 for an empty run.
func (r ValidationReport) MeanResidual() float64 {
	if len(r.Residuals) == 0 {
		return 0.0
	}
	sum := 0.0
	for _, res := range r.Residuals {
		sum += res.Residual()
	}
	return sum / float64(len(r.Residuals))
}

// SelectionStrategy chooses which shots get validated.
type SelectionStrategy string

const (
	// Stratified ranks shots by probe bitrate and draws k/4 from each
	// quartile, so validation samples the predictor's full operating range
	// rather than only the easy shots. The remainder lands in the highest
	// quartile, where the predictor is most likely to be wrong.
	Stratified SelectionStrategy = "stratified"
	// Head takes the first k shots. Useful only where deterministic order
	// matters more than coverage.
	Head SelectionStrategy = "head"
)

// SelectValidationShots picks k shots from shots to validate.
//
// featuresByShot supplies the probe bitrate the stratified strategy ranks on;
// a shot missing from the map ranks at bitrate 0 and clusters into the lowest
// quartile, which is the right place for content the probe could not measure.
func SelectValidationShots(
	shots []pershot.Shot,
	featuresByShot map[pershot.Shot]ShotFeatures,
	k int,
	strategy SelectionStrategy,
) ([]pershot.Shot, error) {
	if k <= 0 || len(shots) == 0 {
		return nil, nil
	}
	if len(shots) <= k {
		out := make([]pershot.Shot, len(shots))
		copy(out, shots)
		return out, nil
	}
	switch strategy {
	case Head:
		out := make([]pershot.Shot, k)
		copy(out, shots[:k])
		return out, nil
	case Stratified, "":
	default:
		return nil, fmt.Errorf(
			"unknown strategy %q; expected %q or %q", strategy, Stratified, Head)
	}

	ranked := make([]pershot.Shot, len(shots))
	copy(ranked, shots)
	sort.SliceStable(ranked, func(i, j int) bool {
		return featuresByShot[ranked[i]].ProbeBitrateKbps <
			featuresByShot[ranked[j]].ProbeBitrateKbps
	})

	quartileSize := len(ranked) / 4
	perQuartile := k / 4
	extra := k - perQuartile*4
	selected := make([]pershot.Shot, 0, k)
	for i := 0; i < 4; i++ {
		lo := i * quartileSize
		hi := (i + 1) * quartileSize
		if i == 3 {
			hi = len(ranked)
		}
		if lo >= hi {
			continue
		}
		quartile := ranked[lo:hi]
		nPick := perQuartile
		if i == 3 {
			nPick += extra
		}
		if nPick > len(quartile) {
			nPick = len(quartile)
		}
		if nPick == 0 {
			continue
		}
		// Evenly spaced within the quartile so adjacent shots are not
		// double-sampled.
		step := len(quartile) / nPick
		if step < 1 {
			step = 1
		}
		for j := 0; j < nPick; j++ {
			idx := j * step
			if idx > len(quartile)-1 {
				idx = len(quartile) - 1
			}
			selected = append(selected, quartile[idx])
		}
	}
	return selected, nil
}

// FeatureExtractor computes the feature vector for one shot.
type FeatureExtractor func(shot pershot.Shot) (ShotFeatures, error)

// RealEncodeAndScore runs the real encode at the predictor-picked CRF and
// scores it with libvmaf, returning (encodedPath, measuredVMAF).
type RealEncodeAndScore func(shot pershot.Shot, crf int, codec string) (string, float64, error)

// ValidateOptions configures Validate.
type ValidateOptions struct {
	TargetVMAF float64
	Codec      string
	// K is the number of shots to verify. K=8 on a 1800-shot movie is 0.4 %
	// of the run — the probe encodes themselves cost another 2-3 %.
	K int
	// ResidualThresholdVMAF is the max |residual| tolerated before the
	// verdict leaves GOSPEL. 1.5 VMAF by default.
	ResidualThresholdVMAF float64
	SelectionStrategy     SelectionStrategy
}

// Validate verifies the predictor against real libvmaf scores on K shots.
//
// Workflow: extract features for every shot (one cheap probe encode each),
// pick K shots, then for each ask the predictor for (crf, vmaf), run the real
// encode plus score, and compute the residual.
func Validate(
	p *Predictor,
	shots []pershot.Shot,
	extract FeatureExtractor,
	encodeAndScore RealEncodeAndScore,
	opts ValidateOptions,
) (ValidationReport, error) {
	if p == nil {
		return ValidationReport{}, errors.New("predictor: Validate needs a predictor")
	}
	featuresByShot := make(map[pershot.Shot]ShotFeatures, len(shots))
	for _, s := range shots {
		f, err := extract(s)
		if err != nil {
			return ValidationReport{}, fmt.Errorf(
				"extract features for shot [%d, %d): %w", s.StartFrame, s.EndFrame, err)
		}
		featuresByShot[s] = f
	}

	selected, err := SelectValidationShots(shots, featuresByShot, opts.K, opts.SelectionStrategy)
	if err != nil {
		return ValidationReport{}, err
	}

	residuals := make([]ShotResidual, 0, len(selected))
	for _, shot := range selected {
		feats := featuresByShot[shot]
		crf, pickErr := p.PickCRF(feats, opts.TargetVMAF, opts.Codec)
		if pickErr != nil {
			return ValidationReport{}, pickErr
		}
		predicted := p.PredictVMAF(feats, crf, opts.Codec)
		_, measured, scoreErr := encodeAndScore(shot, crf, opts.Codec)
		if scoreErr != nil {
			return ValidationReport{}, fmt.Errorf(
				"validate shot [%d, %d): %w", shot.StartFrame, shot.EndFrame, scoreErr)
		}
		residuals = append(residuals, ShotResidual{
			Shot:          shot,
			CRFPicked:     crf,
			PredictedVMAF: predicted,
			MeasuredVMAF:  measured,
		})
	}
	return DecideVerdict(residuals, opts.TargetVMAF, opts.ResidualThresholdVMAF), nil
}

// DecideVerdict maps a residual list onto a ValidationReport.
//
// An empty selection is pessimistic (FALL_BACK) so the caller runs the full
// encode-and-score loop rather than trusting an unvalidated predictor.
// A tight spread with a biased mean is RECALIBRATE, and the bias correction
// is that signed mean. A wide spread is FALL_BACK regardless of the mean.
func DecideVerdict(residuals []ShotResidual, targetVMAF, threshold float64) ValidationReport {
	report := ValidationReport{
		Residuals:     residuals,
		TargetVMAF:    targetVMAF,
		ThresholdVMAF: threshold,
	}
	if len(residuals) == 0 {
		report.Verdict = FallBack
		return report
	}

	maxAbs := 0.0
	sum := 0.0
	minSigned := math.Inf(1)
	maxSigned := math.Inf(-1)
	for _, r := range residuals {
		signed := r.Residual()
		sum += signed
		if a := math.Abs(signed); a > maxAbs {
			maxAbs = a
		}
		if signed < minSigned {
			minSigned = signed
		}
		if signed > maxSigned {
			maxSigned = signed
		}
	}
	meanSigned := sum / float64(len(residuals))
	spread := maxSigned - minSigned

	switch {
	case maxAbs <= threshold:
		report.Verdict = Gospel
	case spread <= 2.0*threshold && math.Abs(meanSigned) > threshold:
		report.Verdict = Recalibrate
		report.BiasCorrection = meanSigned
	default:
		report.Verdict = FallBack
	}
	return report
}

// clamp bounds v to [lo, hi].
func clamp(v, lo, hi float64) float64 {
	return math.Max(lo, math.Min(hi, v))
}

// Clamp constrains v to the inclusive [lo, hi] window.
//
// Carried over from the group-3 predictor when the two implementations were
// merged onto this one: pkg/sidecar clamps predicted VMAF into the valid
// 0..100 range before reporting it.
func Clamp(v, lo, hi float64) float64 {
	return math.Max(lo, math.Min(hi, v))
}

// planner probe
