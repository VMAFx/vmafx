// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/internal/pyjson"
	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/conformal"
	"github.com/VMAFx/vmafx/pkg/ffencode"
	"github.com/VMAFx/vmafx/pkg/pershot"
	"github.com/VMAFx/vmafx/pkg/predictor"
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

	_ = cmd.MarkFlagRequired("source")

	return cmd
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
	// --use-saliency is not wired in this binary. predictor.ExtractorConfig
	// gates the saliency pass on `cfg.UseSaliency && saliency != nil`, and no
	// production caller ever supplies a predictor.SaliencyFunc -- it is
	// referenced only by features.go itself and its own test. Passing the flag
	// therefore left the saliency mean/variance features at 0.0 for every shot
	// while the run reported success, which silently changes the feature vector
	// the prediction is built from. Fail instead of pretending.
	if flags.useSaliency {
		return &exitCodeError{code: usageExitCode, err: errors.New(
			"--use-saliency is not implemented in vmafx-tune-go: the saliency " +
				"feature pass needs an ONNX forward pass, and no SaliencyFunc is " +
				"wired into the Go feature extractor. Use " +
				"'vmaf-tune predict --use-saliency'")}
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

	extractorCfg := predictor.ExtractorConfig{
		FFmpegBin:            flags.ffmpegBin,
		FFprobeBin:           flags.ffprobeBin,
		UseSignalstats:       true,
		UseSaliency:          flags.useSaliency,
		SaliencyModel:        flags.saliencyModel,
		SaliencyFrameSamples: 8,
		ProbeMaxFrames:       240,
	}

	// Probe geometry once: shot detection needs it, and so does the
	// encode/score loop. Probing per shot would cost an ffprobe per shot on
	// a feature-length source.
	geometry := predictor.ProbeGeometry(ctx, flags.source, extractorCfg, runCommand)
	if geometry.Width <= 0 || geometry.Height <= 0 {
		return errors.New(
			"ffprobe could not read the source geometry (width/height); " +
				"continuing would silently mis-parse every encode")
	}
	d.Log.InfoContext(ctx, "probed source geometry",
		"width", geometry.Width, "height", geometry.Height, "fps", geometry.FPS)

	// pkg/pershot is the group-1 implementation: DetectShotsStatus is the
	// status-returning variant of DetectShots, and the subprocess runner is a
	// DetectOptions field rather than a trailing argument.
	shots, detected := pershot.DetectShotsStatus(ctx, flags.source, pershot.DetectOptions{
		Width: geometry.Width, Height: geometry.Height,
		PixFmt: "yuv420p", Bitdepth: flags.bitdepth,
		TotalFrames: flags.totalFrames, Bin: flags.perShotBin,
	})
	if len(shots) == 0 {
		return errors.New("no shots detected; nothing to do")
	}
	if !detected {
		d.Log.WarnContext(ctx,
			"vmaf-perShot unavailable or failed; falling back to a single shot",
			"per_shot_bin", flags.perShotBin, "shots", len(shots))
	} else {
		d.Log.InfoContext(ctx, "detected shots", "count", len(shots))
	}

	pred := predictor.New()
	if session := newORTPredictorSession(ctx, flags.model); session != nil {
		pred = predictor.WithSession(session)
		pred.Log = d.Log
		// Deliberately phrased as a request, not an accomplishment: the runner
		// is a subprocess resolved at first inference, so at this point we do
		// not yet know whether the learned path will work. Claiming "using the
		// learned ONNX predictor" here is what made a silent fallback look
		// like a model-backed run.
		d.Log.InfoContext(ctx, "ONNX predictor requested", "model", flags.model)
	}

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

	extract := func(shot pershot.Shot) (predictor.ShotFeatures, error) {
		return predictor.ExtractFeatures(ctx, shot, flags.source, flags.codec,
			geometry, extractorCfg, runCommand, nil)
	}
	encodeAndScore := func(shot pershot.Shot, crf int, codec string) (string, float64, error) {
		return realEncodeAndScore(ctx, d, flags, geometry, workdir, shot, crf, codec)
	}

	report, validateErr := predictor.Validate(pred, shots, extract, encodeAndScore,
		predictor.ValidateOptions{
			TargetVMAF:            flags.targetVMAF,
			Codec:                 flags.codec,
			K:                     flags.validateK,
			ResidualThresholdVMAF: flags.residualThreshold,
			SelectionStrategy:     predictor.Stratified,
		})
	if validateErr != nil {
		return validateErr
	}

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
	if err := writeOutput(flags.reportOut, string(rendered)+"\n"); err != nil {
		return err
	}

	if report.Verdict == predictor.FallBack {
		return errFallBackVerdict
	}
	return nil
}

// buildPredictReport assembles the JSON payload, resolving the conformal
// calibration once and reusing it for every per-shot interval.
func buildPredictReport(report predictor.ValidationReport, flags *predictFlags) (predictReport, error) {
	var calibration *conformal.SplitCalibration
	uncalibrated := false
	if flags.withUncertainty {
		if flags.calibrationSidecar != "" {
			cal, err := conformal.LoadSplitCalibration(flags.calibrationSidecar)
			if err != nil {
				return predictReport{}, err
			}
			if !math.IsNaN(flags.alpha) {
				// LoadSplitCalibration already returns a pointer, and WithAlpha
				// re-quantiles into a new one rather than mutating in place.
				if cal, err = cal.WithAlpha(flags.alpha); err != nil {
					return predictReport{}, err
				}
			}
			calibration = cal
		} else {
			uncalibrated = true
		}
	}

	var reportedAlpha *float64
	if calibration != nil {
		alpha := calibration.Alpha()
		reportedAlpha = &alpha
	}

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

	// The vmaf CLI only accepts raw YUV once geometry is pinned, so the
	// encoded container has to be decoded back first.
	distYUV := filepath.Join(workdir,
		fmt.Sprintf("dist_%d_%d.decoded.yuv", shot.StartFrame, shot.EndFrame))
	decodeArgv := scorecli.DecodeCommand(distPath, distYUV, pixFmt, flags.ffmpegBin, 0)
	distForScore := distYUV
	if _, _, exitStatus, err := runCommand(ctx, decodeArgv); err != nil || exitStatus != 0 {
		d.Log.WarnContext(ctx, "distorted decode failed; scoring against the container",
			"shot_start", shot.StartFrame, "exit_status", exitStatus)
		distForScore = distPath
	}

	scoreRes, scoreErr := scorecli.Run(ctx, scorecli.Request{
		Reference: refYUV, Distorted: distForScore,
		Width: geometry.Width, Height: geometry.Height, PixFmt: pixFmt,
	}, flags.vmafBin, "", nil)
	if scoreErr != nil {
		return distPath, math.NaN(), scoreErr
	}
	return distPath, scoreRes.VMAFScore, nil
}
