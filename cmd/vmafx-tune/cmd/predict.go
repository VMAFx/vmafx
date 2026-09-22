// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

package cmd

import (
	"context"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"
	"sync"

	"github.com/golusoris/golusoris/core/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/conformal"
	"github.com/VMAFx/vmafx/pkg/ffencode"
	"github.com/VMAFx/vmafx/pkg/pershot"
	"github.com/VMAFx/vmafx/pkg/predictor"
	"github.com/VMAFx/vmafx/pkg/pyjson"
	"github.com/VMAFx/vmafx/pkg/saliency"
	"github.com/VMAFx/vmafx/pkg/scorecli"
)

// predictFlags mirrors the Python `vmaf-tune predict` flag surface.
type predictFlags struct {
	source            string
	codec             string
	targetVMAF        float64
	validateK         int
	residualThreshold float64

	useSaliency   bool
	saliencyModel string
	model         string

	perShotBin  string
	ffmpegBin   string
	ffprobeBin  string
	vmafBin     string
	bitdepth    int
	totalFrames int
	reportOut   string

	withUncertainty    bool
	calibrationSidecar string
	alpha              float64
}

// newPredictCmd builds the "predict" cobra subcommand.
func newPredictCmd() *cobra.Command {
	flags := &predictFlags{}

	cmd := clikit.Command("predict",
		"Predict per-shot VMAF without running it, then verify on K shots",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runPredict(ctx, d, flags)
		})),
	)
	cmd.Long = `Predict per-shot VMAF, then verify the prediction on a few real encodes.

Pipeline:
  1. Detect shots with vmaf-perShot (TransNet V2). Falls back to a single
     shot spanning the clip when the binary is unavailable.
  2. Extract cheap per-shot features: one fast probe encode per shot for the
     complexity barometer, optionally FFmpeg signalstats and saliency moments.
  3. For K stratified shots, ask the predictor for (crf, vmaf), run the real
     encode at that CRF, score it with libvmaf, and compute the residual.
  4. Emit the verdict:
       gospel       every residual within --residual-threshold; trust the
                    predictor on the remaining shots
       recalibrate  residuals biased but tight; apply the reported
                    bias_correction and redo the picks (no retraining)
       fall_back    residuals too wide; degrade to the full encode-and-score
                    loop (exit code 2)

Without --model the predictor uses its per-codec analytical curve, which is
numerically identical to the Python fallback. With --model it routes inference
through the vmafx-ort-runner subprocess; if that runner is not on PATH the
predictor degrades to the analytical curve and says so in the log, matching
the Python behaviour when onnxruntime is not installed.

Example:
  vmafx-tune-go predict --source movie.mkv --codec libx264 \
    --target-vmaf 93 --validate-k 8 --report-out predict.json`

	registerPredictFlags(cmd, flags)

	markCommandFlagsRequired(cmd, "source")

	return cmd
}

// registerPredictFlags registers the flags of the predict subcommand.
func registerPredictFlags(cmd *cobra.Command, flags *predictFlags) {
	cmd.Flags().StringVar(&flags.source, "source", "",
		"Reference video, any FFmpeg-readable container (required)")
	cmd.Flags().StringVar(&flags.codec, "codec", "libx264",
		"Codec adapter ("+strings.Join(codecadapter.Known(), ", ")+")")
	cmd.Flags().Float64Var(&flags.targetVMAF, "target-vmaf", 93.0,
		"Target pooled-mean VMAF")
	cmd.Flags().IntVar(&flags.validateK, "validate-k", 8,
		"Number of shots to verify against real libvmaf")
	cmd.Flags().Float64Var(&flags.residualThreshold, "residual-threshold", 1.5,
		"Max abs(predicted - measured) VMAF before falling back")
	cmd.Flags().BoolVar(&flags.useSaliency, "use-saliency", false,
		"Include saliency_student mean/variance in the predictor features")
	cmd.Flags().StringVar(&flags.saliencyModel, "saliency-model", "",
		"Path to saliency_student ONNX for --use-saliency")
	cmd.Flags().StringVar(&flags.model, "model", "",
		"Path to predictor_<codec>.onnx (default: the analytical fallback)")
	cmd.Flags().StringVar(&flags.perShotBin, "per-shot-bin", "vmaf-perShot",
		"Path to the vmaf-perShot binary")
	cmd.Flags().StringVar(&flags.ffmpegBin, "ffmpeg-bin", "ffmpeg", "ffmpeg binary")
	cmd.Flags().StringVar(&flags.ffprobeBin, "ffprobe-bin", "ffprobe", "ffprobe binary")
	cmd.Flags().StringVar(&flags.vmafBin, "vmaf-bin", "vmaf", "libvmaf CLI binary")
	cmd.Flags().IntVar(&flags.bitdepth, "bitdepth", 8,
		"Source bit depth, forwarded to vmaf-perShot (8, 10 or 12)")
	cmd.Flags().IntVar(&flags.totalFrames, "total-frames", 0,
		"Frame count for the single-shot fallback when vmaf-perShot is unavailable")
	cmd.Flags().StringVar(&flags.reportOut, "report-out", "",
		"Write the validation report here (default: stdout)")
	cmd.Flags().BoolVar(&flags.withUncertainty, "with-uncertainty", false,
		"Emit conformal prediction intervals alongside each predicted VMAF (ADR-0279)")
	cmd.Flags().StringVar(&flags.calibrationSidecar, "calibration-sidecar", "",
		"Split-conformal calibration JSON; without one the intervals are degenerate")
	cmd.Flags().Float64Var(&flags.alpha, "alpha", math.NaN(),
		"Override the sidecar's miscoverage level (0.05 = 95% coverage)")
}

// predictInterval is the per-residual interval block emitted under
// --with-uncertainty.
type predictInterval struct {
	Low   float64  `json:"low"`
	High  float64  `json:"high"`
	Alpha *float64 `json:"alpha"`
}

// predictResidual is one row of the report's residuals array.
type predictResidual struct {
	ShotStart     int              `json:"shot_start"`
	ShotEnd       int              `json:"shot_end"`
	CRF           int              `json:"crf"`
	PredictedVMAF float64          `json:"predicted_vmaf"`
	MeasuredVMAF  float64          `json:"measured_vmaf"`
	Residual      float64          `json:"residual"`
	Interval      *predictInterval `json:"interval,omitempty"`
}

// predictUncertainty is the report's uncertainty metadata block.
type predictUncertainty struct {
	Enabled    bool     `json:"enabled"`
	Calibrated bool     `json:"calibrated"`
	Alpha      *float64 `json:"alpha"`
}

// predictReport is the emitted JSON payload. The field order and key names
// reproduce the Python handler's dict so downstream consumers parse it
// unchanged.
type predictReport struct {
	Verdict           string             `json:"verdict"`
	TargetVMAF        float64            `json:"target_vmaf"`
	ResidualThreshold float64            `json:"residual_threshold"`
	MaxAbsResidual    float64            `json:"max_abs_residual"`
	MeanResidual      float64            `json:"mean_residual"`
	BiasCorrection    float64            `json:"bias_correction"`
	KValidated        int                `json:"k_validated"`
	Uncertainty       predictUncertainty `json:"uncertainty"`
	Residuals         []predictResidual  `json:"residuals"`
}

// errFallBackVerdict signals the FALL_BACK exit status without printing a
// second error line — the report has already been emitted.
var errFallBackVerdict = errors.New("predictor validation verdict: fall_back")

// runPredict is the implementation of the predict subcommand.
func runPredict(ctx context.Context, d deps, flags *predictFlags) error {
	if err := validatePredictFlags(flags); err != nil {
		return err
	}

	extractorCfg := predictor.ExtractorConfig{
		FFmpegBin:            flags.ffmpegBin,
		FFprobeBin:           flags.ffprobeBin,
		UseSignalstats:       true,
		UseSaliency:          flags.useSaliency,
		SaliencyModel:        flags.saliencyModel,
		SaliencyFrameSamples: 8,
		ProbeMaxFrames:       240,
	}

	geometry, geomErr := probePredictGeometry(ctx, d, flags, extractorCfg)
	if geomErr != nil {
		return geomErr
	}

	shots, shotErr := detectPredictShots(ctx, d, flags, geometry)
	if shotErr != nil {
		return shotErr
	}

	pred := newPredictPredictor(ctx, d, flags)

	// The validation work area lives for the whole run so the score step's
	// lazy decode still finds the encoded file on disk.
	workdir, err := os.MkdirTemp("", "vmafx-tune-predict-")
	if err != nil {
		return fmt.Errorf("create predict workdir: %w", err)
	}
	defer func() {
		if rmErr := os.RemoveAll(workdir); rmErr != nil {
			d.Log.WarnContext(ctx, "remove predict workdir",
				"path", workdir, "error", rmErr)
		}
	}()

	extract, encodeAndScore := newPredictCallbacks(ctx, d, flags, geometry, extractorCfg, workdir)

	report, validateErr := predictor.Validate(pred, shots, extract, encodeAndScore,
		predictPlanOptions(flags))
	if validateErr != nil {
		return validateErr
	}

	if err := emitPredictReport(report, flags); err != nil {
		return err
	}

	if report.Verdict == predictor.FallBack {
		return errFallBackVerdict
	}
	return nil
}

// newPredictCallbacks builds the two seams predictor.Validate calls back into: feature
// extraction for a shot, and the real encode-then-score that grounds a prediction.
//
// Both close over the run's geometry, config and work area, which is what keeps Validate
// free of any knowledge of ffmpeg, the CLI flags, or where the scratch files live.
func newPredictCallbacks(
	ctx context.Context,
	d deps,
	flags *predictFlags,
	geometry predictor.Geometry,
	extractorCfg predictor.ExtractorConfig,
	workdir string,
) (predictor.FeatureExtractor, predictor.RealEncodeAndScore) {
	var salFunc predictor.SaliencyFunc
	if flags.useSaliency {
		salFunc = newPredictSaliencyFunc(ctx, d)
	}
	extract := func(shot pershot.Shot) (predictor.ShotFeatures, error) {
		return predictor.ExtractFeatures(ctx, shot, flags.source, flags.codec,
			geometry, extractorCfg, runCommand, salFunc)
	}
	encodeAndScore := func(shot pershot.Shot, crf int, codec string) (string, float64, error) {
		return realEncodeAndScore(ctx, d, flags, geometry, workdir, shot, crf, codec)
	}
	return extract, encodeAndScore
}

// predictPlanOptions maps the CLI flags onto the validation options.
func predictPlanOptions(flags *predictFlags) predictor.ValidateOptions {
	return predictor.ValidateOptions{
		TargetVMAF:            flags.targetVMAF,
		Codec:                 flags.codec,
		K:                     flags.validateK,
		ResidualThresholdVMAF: flags.residualThreshold,
		SelectionStrategy:     predictor.Stratified,
	}
}

// probePredictGeometry reads the source geometry once.
//
// Shot detection needs it and so does the encode/score loop; probing per shot would cost
// an ffprobe per shot on a feature-length source. A geometry ffprobe could not read is
// fatal: continuing would silently mis-parse every encode.
func probePredictGeometry(
	ctx context.Context,
	d deps,
	flags *predictFlags,
	extractorCfg predictor.ExtractorConfig,
) (predictor.Geometry, error) {
	geometry := predictor.ProbeGeometry(ctx, flags.source, extractorCfg, runCommand)
	if geometry.Width <= 0 || geometry.Height <= 0 {
		return geometry, errors.New(
			"ffprobe could not read the source geometry (width/height); " +
				"continuing would silently mis-parse every encode")
	}
	d.Log.InfoContext(ctx, "probed source geometry",
		"width", geometry.Width, "height", geometry.Height, "fps", geometry.FPS)
	return geometry, nil
}

// emitPredictReport renders the validation report and writes it to --report-out.
func emitPredictReport(report predictor.ValidationReport, flags *predictFlags) error {
	payload, buildErr := buildPredictReport(report, flags)
	if buildErr != nil {
		return buildErr
	}
	// json.dumps(payload, indent=2) — no sort_keys, so the Python handler's
	// dict insertion order stands, which is the struct field order here.
	rendered, marshalErr := pyjson.MarshalIndent(payload, false)
	if marshalErr != nil {
		return fmt.Errorf("render predict report: %w", marshalErr)
	}
	return writeOutput(flags.reportOut, string(rendered)+"\n")
}

// validatePredictFlags rejects a request the predictor cannot serve, before any probe or
// encode has cost anything.
func validatePredictFlags(flags *predictFlags) error {
	if flags.saliencyModel != "" {
		if _, err := os.Stat(flags.saliencyModel); err != nil {
			return &exitCodeError{code: usageExitCode, err: fmt.Errorf("saliency model %q: %w", flags.saliencyModel, err)}
		}
	}

	if flags.source == "" {
		return errors.New("--source is required")
	}
	if _, err := os.Stat(flags.source); err != nil {
		return fmt.Errorf("source %q: %w", flags.source, err)
	}
	if _, err := codecadapter.Get(flags.codec); err != nil {
		return err
	}
	switch flags.bitdepth {
	case 8, 10, 12:
	default:
		return fmt.Errorf("--bitdepth must be 8, 10 or 12; got %d", flags.bitdepth)
	}
	return nil
}

// detectPredictShots splits the source into the shots the validation runs over.
//
// pkg/pershot is the group-1 implementation: DetectShotsStatus is the status-returning
// variant of DetectShots, and the subprocess runner is a DetectOptions field rather than a
// trailing argument. A failed detector still yields one whole-source shot, which is a
// usable run and is logged as the degraded path it is; zero shots is not.
func detectPredictShots(
	ctx context.Context,
	d deps,
	flags *predictFlags,
	geometry predictor.Geometry,
) ([]pershot.Shot, error) {
	shots, detected := pershot.DetectShotsStatus(ctx, flags.source, pershot.DetectOptions{
		Width: geometry.Width, Height: geometry.Height,
		PixFmt: "yuv420p", Bitdepth: flags.bitdepth,
		TotalFrames: flags.totalFrames, Bin: flags.perShotBin,
	})
	if len(shots) == 0 {
		return nil, errors.New("no shots detected; nothing to do")
	}
	if !detected {
		d.Log.WarnContext(ctx,
			"vmaf-perShot unavailable or failed; falling back to a single shot",
			"per_shot_bin", flags.perShotBin, "shots", len(shots))
	} else {
		d.Log.InfoContext(ctx, "detected shots", "count", len(shots))
	}
	return shots, nil
}

// newPredictPredictor builds the predictor, preferring the learned ONNX session when one
// resolves and falling back to the analytical model otherwise.
func newPredictPredictor(ctx context.Context, d deps, flags *predictFlags) *predictor.Predictor {
	pred := predictor.New()
	session := predictor.NewORTSession(ctx, flags.model)
	if session == nil {
		return pred
	}
	pred = predictor.WithSession(session)
	pred.Log = d.Log
	// Deliberately phrased as a request, not an accomplishment: the runner
	// is a subprocess resolved at first inference, so at this point we do
	// not yet know whether the learned path will work. Claiming "using the
	// learned ONNX predictor" here is what made a silent fallback look
	// like a model-backed run.
	d.Log.InfoContext(ctx, "ONNX predictor requested", "model", flags.model)
	return pred
}

// buildPredictReport assembles the JSON payload, resolving the conformal
// calibration once and reusing it for every per-shot interval.
func buildPredictReport(report predictor.ValidationReport, flags *predictFlags) (predictReport, error) {
	calibration, uncalibrated, err := resolvePredictCalibration(flags)
	if err != nil {
		return predictReport{}, err
	}

	var reportedAlpha *float64
	if calibration != nil {
		alpha := calibration.Alpha()
		reportedAlpha = &alpha
	}

	residuals := buildPredictResiduals(report, flags, calibration)

	return predictReport{
		Verdict:           string(report.Verdict),
		TargetVMAF:        report.TargetVMAF,
		ResidualThreshold: report.ThresholdVMAF,
		MaxAbsResidual:    report.MaxAbsResidual(),
		MeanResidual:      report.MeanResidual(),
		BiasCorrection:    report.BiasCorrection,
		KValidated:        len(report.Residuals),
		Uncertainty: predictUncertainty{
			Enabled:    flags.withUncertainty,
			Calibrated: flags.withUncertainty && !uncalibrated,
			Alpha:      reportedAlpha,
		},
		Residuals: residuals,
	}, nil
}

// resolvePredictCalibration loads the conformal calibration the intervals are drawn from,
// reporting whether the run is uncalibrated.
//
// --with-uncertainty without a sidecar is the documented degraded path: intervals are
// still emitted, but flagged uncalibrated so nobody reads a coverage guarantee into them.
func resolvePredictCalibration(flags *predictFlags) (*conformal.SplitCalibration, bool, error) {
	if !flags.withUncertainty {
		return nil, false, nil
	}
	if flags.calibrationSidecar == "" {
		return nil, true, nil
	}
	cal, err := conformal.LoadSplitCalibration(flags.calibrationSidecar)
	if err != nil {
		return nil, false, err
	}
	if !math.IsNaN(flags.alpha) {
		// LoadSplitCalibration already returns a pointer, and WithAlpha
		// re-quantiles into a new one rather than mutating in place.
		if cal, err = cal.WithAlpha(flags.alpha); err != nil {
			return nil, false, err
		}
	}
	return cal, false, nil
}

// buildPredictResiduals projects the validation residuals onto the report rows, attaching
// a prediction interval to each when uncertainty was requested.
func buildPredictResiduals(
	report predictor.ValidationReport,
	flags *predictFlags,
	calibration *conformal.SplitCalibration,
) []predictResidual {
	residuals := make([]predictResidual, 0, len(report.Residuals))
	for _, r := range report.Residuals {
		row := predictResidual{
			ShotStart:     r.Shot.StartFrame,
			ShotEnd:       r.Shot.EndFrame,
			CRF:           r.CRFPicked,
			PredictedVMAF: r.PredictedVMAF,
			MeasuredVMAF:  r.MeasuredVMAF,
			Residual:      r.Residual(),
		}
		if flags.withUncertainty {
			if calibration == nil {
				// Degraded path: low == high == point, and the report is
				// flagged uncalibrated so nobody reads a coverage guarantee
				// into a zero-width interval.
				row.Interval = &predictInterval{
					Low: r.PredictedVMAF, High: r.PredictedVMAF, Alpha: nil,
				}
			} else {
				iv := calibration.IntervalFor(r.PredictedVMAF)
				alpha := iv.Alpha
				row.Interval = &predictInterval{Low: iv.Low, High: iv.High, Alpha: &alpha}
			}
		}
		residuals = append(residuals, row)
	}
	return residuals
}

// realEncodeAndScore extracts one shot to raw YUV, encodes it at crf, decodes
// the result back to raw YUV for the vmaf CLI, and scores it.
//
// A failure at any stage yields a NaN score rather than an error: one
// unscorable shot must not abort a validation run, and a NaN residual is
// visible in the report.
func realEncodeAndScore(
	ctx context.Context,
	d deps,
	flags *predictFlags,
	geometry predictor.Geometry,
	workdir string,
	shot pershot.Shot,
	crf int,
	codec string,
) (string, float64, error) {
	const pixFmt = "yuv420p" // canonical reference format, matching the corpus loop

	refYUV := filepath.Join(workdir,
		fmt.Sprintf("ref_%d_%d.yuv", shot.StartFrame, shot.EndFrame))
	distPath := filepath.Join(workdir,
		fmt.Sprintf("dist_%d_%d.mp4", shot.StartFrame, shot.EndFrame))

	if !extractShotReference(ctx, d, flags, geometry, shot, pixFmt, refYUV) {
		return distPath, math.NaN(), nil
	}

	framerate := geometry.FPS
	if framerate <= 0.0 {
		framerate = 24.0
	}
	encRes, encErr := ffencode.Run(ctx, ffencode.Request{
		Source: refYUV, Width: geometry.Width, Height: geometry.Height,
		PixFmt: pixFmt, Framerate: framerate,
		Encoder: codec, Preset: "medium", CRF: crf, Output: distPath,
	}, flags.ffmpegBin, nil)
	if encErr != nil {
		return distPath, math.NaN(), encErr
	}
	if encRes.ExitStatus != 0 {
		d.Log.WarnContext(ctx, "validation encode failed; scoring the shot NaN",
			"shot_start", shot.StartFrame, "crf", crf,
			"exit_status", encRes.ExitStatus, "stderr_tail", encRes.StderrTail)
		return distPath, math.NaN(), nil
	}

	distForScore := decodeDistortedForScore(ctx, d, flags, workdir, shot, pixFmt, distPath)

	scoreRes, scoreErr := scorecli.Run(ctx, scorecli.Request{
		Reference: refYUV, Distorted: distForScore,
		Width: geometry.Width, Height: geometry.Height, PixFmt: pixFmt,
	}, flags.vmafBin, "", nil)
	if scoreErr != nil {
		return distPath, math.NaN(), scoreErr
	}
	return distPath, scoreRes.VMAFScore, nil
}

// extractShotReference cuts one shot out of the source as raw YUV, reporting whether the
// extraction produced a file the scorer can use.
//
// A failure is logged and reported as false rather than returned as an error: the caller
// turns that into a NaN score, because one unscorable shot must not abort the run.
func extractShotReference(
	ctx context.Context,
	d deps,
	flags *predictFlags,
	geometry predictor.Geometry,
	shot pershot.Shot,
	pixFmt, refYUV string,
) bool {
	extractArgv := []string{
		flags.ffmpegBin, "-y", "-hide_banner", "-loglevel", "error",
		"-ss", predictor.ShotStartArg(shot, geometry.FPS),
		"-i", flags.source,
		"-frames:v", fmt.Sprintf("%d", shot.Length()),
		"-pix_fmt", pixFmt,
		"-f", "rawvideo",
		refYUV,
	}
	if _, _, exitStatus, err := runCommand(ctx, extractArgv); err != nil || exitStatus != 0 {
		d.Log.WarnContext(ctx, "reference extraction failed; scoring the shot NaN",
			"shot_start", shot.StartFrame, "exit_status", exitStatus, "error", err)
		return false
	}
	return true
}

// decodeDistortedForScore decodes the encoded container back to raw YUV for the scorer.
//
// The vmaf CLI only accepts raw YUV once geometry is pinned, so the container has to be
// decoded first. A failed decode falls back to scoring the container directly: that is
// worse input for the scorer but still an answer, which beats losing the shot.
func decodeDistortedForScore(
	ctx context.Context,
	d deps,
	flags *predictFlags,
	workdir string,
	shot pershot.Shot,
	pixFmt, distPath string,
) string {
	distYUV := filepath.Join(workdir,
		fmt.Sprintf("dist_%d_%d.decoded.yuv", shot.StartFrame, shot.EndFrame))
	decodeArgv := scorecli.DecodeCommand(distPath, distYUV, pixFmt, flags.ffmpegBin, 0)
	if _, _, exitStatus, err := runCommand(ctx, decodeArgv); err != nil || exitStatus != 0 {
		d.Log.WarnContext(ctx, "distorted decode failed; scoring against the container",
			"shot_start", shot.StartFrame, "exit_status", exitStatus)
		return distPath
	}
	return distYUV
}

// computeSaliencyMoments computes the population mean and variance of mask.
// An empty slice returns (0.0, 0.0).
func computeSaliencyMoments(mask []float64) (mean, variance float64) {
	if len(mask) == 0 {
		return 0.0, 0.0
	}
	var sum float64
	for _, v := range mask {
		sum += v
	}
	mean = sum / float64(len(mask))
	var sumSq float64
	for _, v := range mask {
		diff := v - mean
		sumSq += diff * diff
	}
	variance = sumSq / float64(len(mask))
	return mean, variance
}

// newPredictSaliencyFunc constructs a SaliencyFunc for the predictor feature
// extraction pipeline. If inference is unavailable or fails, it logs a warning
// once and degrades to zero moments, matching the Python behavior.
func newPredictSaliencyFunc(ctx context.Context, d deps) predictor.SaliencyFunc {
	var warnOnce sync.Once
	return func(rawYUVPath string, width, height, frameSamples int, modelPath string) (float64, float64, error) {
		session, sessionErr := saliencySessionFactory(modelPath)
		if sessionErr != nil {
			warnOnce.Do(func() {
				d.Log.WarnContext(ctx,
					"saliency inference unavailable; degrading saliency moments to 0.0",
					"reason", sessionErr.Error())
			})
			return 0.0, 0.0, nil
		}
		mask, mapErr := saliency.ComputeMap(rawYUVPath, width, height, session, saliency.MapOptions{
			FrameSamples:       frameSamples,
			TemporalAggregator: saliency.DefaultAggregator,
			EMAAlpha:           saliency.DefaultEMAAlpha,
		})
		if mapErr != nil {
			if errors.Is(mapErr, saliency.ErrUnavailable) {
				warnOnce.Do(func() {
					d.Log.WarnContext(ctx,
						"saliency inference failed; degrading saliency moments to 0.0",
						"error", mapErr)
				})
				return 0.0, 0.0, nil
			}
			return 0.0, 0.0, mapErr
		}
		mean, variance := computeSaliencyMoments(mask)
		return mean, variance, nil
	}
}
