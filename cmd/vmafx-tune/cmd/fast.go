// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"github.com/VMAFx/vmafx/pkg/model"
	"os"
	"strings"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/encoder"
	"github.com/VMAFx/vmafx/pkg/fast"
	"github.com/VMAFx/vmafx/pkg/scorebackend"
)

// fastFlags holds flags parsed by the fast subcommand. Every field mirrors a
// `vmaf-tune fast` flag one-for-one so operators can swap binaries without
// re-learning the surface.
type fastFlags struct {
	src                string
	width              int
	height             int
	pixFmt             string
	framerate          float64
	targetVMAF         float64
	encoderName        string
	preset             string
	crfMin             int
	crfMax             int
	nTrials            int
	timeBudgetS        int
	proxyTolerance     float64
	sampleChunkSeconds float64
	smoke              bool
	scoreBackend       string
	ffmpegBin          string
	vmafBin            string
	vmafModel          string
	encodeDir          string
	output             string
}

// Exit codes. The Python `vmaf-tune fast` contract is:
//
//	0 — recommendation emitted, proxy and verify agree within tolerance
//	2 — usage / environment error (bad CRF range, missing --src, unavailable
//	    backend, proxy unavailable)
//	3 — recommendation emitted but the proxy/verify gap exceeds tolerance;
//	    the caller should fall back to the slow Phase A grid (ADR-0276)
//
// cobra maps a RunE error to exit 1, so the two non-zero domain codes are
// carried by fastExitError and applied by the command's own os.Exit.
const (
	exitUsage = 2
	exitOOD   = 3
)

// fastExitError carries a specific process exit status out of runFast.
type fastExitError struct {
	code int
	err  error
}

func (e *fastExitError) Error() string { return e.err.Error() }
func (e *fastExitError) Unwrap() error { return e.err }

// usageError wraps err as an exit-2 usage failure.
func usageError(err error) error { return &fastExitError{code: exitUsage, err: err} }

// newFastCmd builds and returns the "fast" cobra subcommand.
func newFastCmd() *cobra.Command {
	flags := &fastFlags{}

	cmd := clikit.Command("fast",
		"Phase A.5 fast-path — proxy + Bayesian search + GPU-verify recommend",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runFast(ctx, d, flags)
		})),
	)
	cmd.Long = `Recommend a CRF for a VMAF target without running the full Phase A grid.

The fast path replaces the grid sweep with a TPE (Tree-structured Parzen
Estimator) search over the integer CRF axis. Each trial encodes a short probe
slice, extracts the canonical-6 libvmaf features, and predicts VMAF with the
fr_regressor_v2 proxy in microseconds. A single real encode + libvmaf score at
the chosen CRF then verifies the recommendation — the proxy alone never wins
(ADR-0304). When the proxy and the verify pass disagree by more than
--proxy-tolerance the result is flagged out-of-distribution and the command
exits 3 so callers can fall back to the slow grid (ADR-0276).

Smoke mode (--smoke) runs the deterministic synthetic CRF->VMAF curve: no
ffmpeg, no ONNX, no GPU. It exercises the search loop end to end on bare CI
hosts and is the recommended way to sanity-check the wiring.

PORT STATUS: production mode reaches the proxy-inference step and stops there.
fr_regressor_v2 is a two-named-input ONNX graph ("features" + "codec") and the
Go ONNX seam (pkg/ai -> vmafx-ort-runner) drives a single flat input vector
only, so the model cannot be evaluated correctly from Go yet. The TPE search,
the probe-encode + canonical-6 extraction, the verify pass, the JSON schema
and the exit codes are all ported; use 'vmaf-tune fast' for a production run
until the ONNX seam grows named inputs.

Exit codes:
  0  recommendation emitted; proxy and verify agree within tolerance
  2  usage or environment error
  3  recommendation emitted; proxy/verify gap exceeds tolerance (fall back)

Example:
  vmafx-tune-go fast --smoke --target-vmaf 90

  vmafx-tune-go fast \
    --src ref_1920x1080.yuv --width 1920 --height 1080 \
    --target-vmaf 93 --encoder libx264 --preset medium \
    --output fast.json`

	cmd.Flags().StringVar(&flags.src, "src", "",
		"Source video (raw YUV or any ffmpeg-readable container); required unless --smoke")
	cmd.Flags().IntVar(&flags.width, "width", 0,
		"Raw-YUV reference width (required in production mode)")
	cmd.Flags().IntVar(&flags.height, "height", 0,
		"Raw-YUV reference height (required in production mode)")
	cmd.Flags().StringVar(&flags.pixFmt, "pix-fmt", "yuv420p",
		"ffmpeg pix_fmt")
	cmd.Flags().Float64Var(&flags.framerate, "framerate", 24.0,
		"Reference framerate")
	cmd.Flags().Float64Var(&flags.targetVMAF, "target-vmaf", 0,
		"Quality target on the standard VMAF [0, 100] scale (required)")
	cmd.Flags().StringVar(&flags.encoderName, "encoder", "libx264",
		"Codec adapter (must be in the proxy model's encoder vocabulary in production mode)")
	cmd.Flags().StringVar(&flags.preset, "preset", "medium",
		"Encoder preset for the probe + verify encodes")
	cmd.Flags().IntVar(&flags.crfMin, "crf-min", fast.DefaultCRFLo,
		"Minimum CRF in the TPE search range")
	cmd.Flags().IntVar(&flags.crfMax, "crf-max", fast.DefaultCRFHi,
		"Maximum CRF in the TPE search range")
	cmd.Flags().IntVar(&flags.nTrials, "n-trials", 0,
		fmt.Sprintf("TPE trial budget (default %d in production, %d with --smoke)",
			fast.ProdNTrials, fast.SmokeNTrials))
	cmd.Flags().IntVar(&flags.timeBudgetS, "time-budget-s", fast.DefaultTimeBudgetSeconds,
		"Soft wall-clock cap in seconds for the TPE loop (in-flight trials finish)")
	cmd.Flags().Float64Var(&flags.proxyTolerance, "proxy-tolerance", fast.DefaultProxyTolerance,
		"Max absolute proxy/verify VMAF gap before the result is flagged out-of-distribution")
	cmd.Flags().Float64Var(&flags.sampleChunkSeconds, "sample-chunk-seconds", fast.SampleChunkSeconds,
		"Duration in seconds of the probe-encode slice per TPE trial")
	cmd.Flags().BoolVar(&flags.smoke, "smoke", false,
		"Use the deterministic synthetic CRF->VMAF curve; no ffmpeg, no ONNX, no GPU verify")
	cmd.Flags().StringVar(&flags.scoreBackend, "score-backend", "auto",
		"libvmaf scoring backend for the verify pass: auto, "+
			strings.Join(scorebackend.AllBackends(), ", "))
	cmd.Flags().StringVar(&flags.ffmpegBin, "ffmpeg-bin", "ffmpeg",
		"Path to the ffmpeg binary")
	cmd.Flags().StringVar(&flags.vmafBin, "vmaf-bin", "vmaf",
		"Path to the libvmaf CLI binary")
	cmd.Flags().StringVar(&flags.vmafModel, "vmaf-model", model.DefaultVersion,
		"vmaf model version string")
	cmd.Flags().StringVar(&flags.encodeDir, "encode-dir", ".workingdir2/fast",
		"Scratch dir for probe + verify encodes")
	cmd.Flags().StringVarP(&flags.output, "output", "o", "",
		"JSON destination for the recommendation payload (default: stdout)")

	_ = cmd.MarkFlagRequired("target-vmaf")

	return cmd
}

// runFast drives the fast subcommand end to end and emits the JSON payload.
func runFast(ctx context.Context, d deps, flags *fastFlags) error {
	if flags.crfMin < 0 || flags.crfMax < flags.crfMin {
		return usageError(fmt.Errorf("invalid CRF range [%d, %d]", flags.crfMin, flags.crfMax))
	}
	if flags.targetVMAF <= 0 || flags.targetVMAF > 100 {
		return usageError(fmt.Errorf("target VMAF %g is out of range (0, 100]", flags.targetVMAF))
	}
	if flags.timeBudgetS <= 0 {
		return usageError(fmt.Errorf("--time-budget-s must be > 0; got %d", flags.timeBudgetS))
	}

	params := fast.Params{
		Src:               flags.src,
		TargetVMAF:        flags.targetVMAF,
		Encoder:           flags.encoderName,
		CRFLo:             flags.crfMin,
		CRFHi:             flags.crfMax,
		NTrials:           flags.nTrials,
		TimeBudgetSeconds: flags.timeBudgetS,
		Smoke:             flags.smoke,
		ProxyTolerance:    flags.proxyTolerance,
	}

	var selectedBackend string
	if !flags.smoke {
		backend, cfg, err := buildFastPipeline(ctx, d, flags)
		if err != nil {
			return err
		}
		selectedBackend = backend

		predict, predictErr := fast.NewSamplePredictor(ctx, cfg)
		if predictErr != nil {
			return usageError(predictErr)
		}
		verify, verifyErr := fast.NewVerifier(cfg)
		if verifyErr != nil {
			return usageError(verifyErr)
		}
		params.Predict = predict
		params.Verify = verify
	}

	result, err := fast.Recommend(ctx, params)
	if err != nil {
		if errors.Is(err, fast.ErrSrcRequired) || errors.Is(err, fast.ErrProxyPortsUnsupported) {
			return usageError(err)
		}
		return err
	}
	result.ScoreBackend = selectedBackend

	rendered, marshalErr := json.MarshalIndent(result, "", "  ")
	if marshalErr != nil {
		return fmt.Errorf("render fast recommendation: %w", marshalErr)
	}
	payload := string(rendered) + "\n"

	if err := writeOutput(flags.output, payload); err != nil {
		return err
	}
	if flags.output != "" {
		fmt.Fprintf(os.Stderr, "wrote fast recommendation -> %s\n", flags.output)
	}

	d.Log.InfoContext(ctx, "fast recommendation complete",
		"recommended_crf", result.RecommendedCRF,
		"predicted_vmaf", result.PredictedVMAF,
		"n_trials", result.NTrials,
		"smoke", result.Smoke)

	if result.ProxyVerifyGap != nil && *result.ProxyVerifyGap > flags.proxyTolerance {
		// Out-of-distribution signal — the caller should fall back to the
		// slow grid. The payload has already been written.
		return &fastExitError{
			code: exitOOD,
			err: fmt.Errorf("proxy/verify gap %.3f exceeds tolerance %.2f",
				*result.ProxyVerifyGap, flags.proxyTolerance),
		}
	}
	return nil
}

// buildFastPipeline validates production-mode flags, selects the scoring
// backend, and assembles the encode/score pipeline configuration.
func buildFastPipeline(
	ctx context.Context,
	d deps,
	flags *fastFlags,
) (string, fast.PipelineConfig, error) {
	if flags.src == "" {
		return "", fast.PipelineConfig{}, usageError(
			errors.New("--src is required in production mode (use --smoke for the synthetic pipeline)"))
	}
	if _, err := os.Stat(flags.src); err != nil {
		return "", fast.PipelineConfig{}, usageError(fmt.Errorf("source %q: %w", flags.src, err))
	}
	if flags.width <= 0 || flags.height <= 0 {
		return "", fast.PipelineConfig{}, usageError(
			errors.New("--width / --height are required in production mode (raw-YUV geometry)"))
	}
	if _, err := encoder.NewExtended(flags.encoderName); err != nil {
		return "", fast.PipelineConfig{}, usageError(fmt.Errorf("--encoder: %w", err))
	}

	backend, err := scorebackend.Select(ctx, flags.scoreBackend, scorebackend.Options{
		VMAFBin: flags.vmafBin,
	})
	if err != nil {
		return "", fast.PipelineConfig{}, usageError(err)
	}
	fmt.Fprintf(os.Stderr, "vmafx-tune fast: scoring backend = %s\n", backend)
	d.Log.InfoContext(ctx, "fast: scoring backend selected", "backend", backend)

	proxy, proxyErr := fast.NewORTProxy("", fast.DefaultProxyModelID)
	if proxyErr != nil {
		return "", fast.PipelineConfig{}, usageError(fmt.Errorf(
			"fast-path proxy unavailable: %w", proxyErr))
	}

	return backend, fast.PipelineConfig{
		Src:                flags.src,
		Width:              flags.width,
		Height:             flags.height,
		PixFmt:             flags.pixFmt,
		Framerate:          flags.framerate,
		Encoder:            flags.encoderName,
		Preset:             flags.preset,
		CRFLo:              flags.crfMin,
		CRFHi:              flags.crfMax,
		SampleChunkSeconds: flags.sampleChunkSeconds,
		FFmpegBin:          flags.ffmpegBin,
		VMAFBin:            flags.vmafBin,
		VMAFModel:          flags.vmafModel,
		ScoreBackend:       backend,
		EncodeDir:          flags.encodeDir,
		Proxy:              proxy,
	}, nil
}

// fastExitCode reports the process exit status an error from the fast
// subcommand should produce, and whether the error carries one at all.
// Execute consults it so the Python 2 / 3 exit contract survives cobra's
// blanket "any RunE error is exit 1".
func fastExitCode(err error) (int, bool) {
	var target *fastExitError
	if errors.As(err, &target) {
		return target.code, true
	}
	return 0, false
}
