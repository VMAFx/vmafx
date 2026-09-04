// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/codecadapter"
	"github.com/VMAFx/vmafx/pkg/ffencode"
	"github.com/VMAFx/vmafx/pkg/pyjson"
	"github.com/VMAFx/vmafx/pkg/saliency"
)

// recommendSaliencyFlags mirrors the Python `vmaf-tune recommend-saliency`
// flag surface.
type recommendSaliencyFlags struct {
	src            string
	width          int
	height         int
	pixFmt         string
	framerate      float64
	encoder        string
	preset         string
	crf            int
	durationFrames int

	saliencyAware      bool
	saliencyOffset     int
	saliencyModel      string
	saliencyAggregator string
	saliencyEMAAlpha   float64
	fallbackPlain      bool

	ffmpegBin string
	output    string
}

// newRecommendSaliencyCmd builds the "recommend-saliency" cobra subcommand.
func newRecommendSaliencyCmd() *cobra.Command {
	flags := &recommendSaliencyFlags{}

	cmd := clikit.Command("recommend-saliency",
		"Saliency-aware ROI encode: bias bits toward salient regions",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runRecommendSaliency(ctx, d, flags)
		})),
	)
	cmd.Long = `Run one encode biased toward the salient regions of the source.

The saliency_student_v1 ONNX model (ADR-0286) is scored over sampled frames,
reduced to a per-block QP-offset map, and handed to the encoder through its
native ROI channel:

  libx264      ASCII --qpfile at 16x16 macroblock granularity
  libaom-av1   the patched FFmpeg -qpfile bridge, same granularity
  libx265      --zones QP delta (per-clip spatial mean)
  libsvtav1    space-separated QP-offset map at 64x64 super-blocks
  libvvenc     comma-separated ROI CSV at 64x64 CTUs

Any other encoder exits 2 unless --saliency-fallback-plain is set (or
VMAFTUNE_SALIENCY_FALLBACK_OK=1), in which case a plain encode runs and an
error is logged (ADR-0546).

INFERENCE AVAILABILITY. This Go port ships the full numeric pipeline — YUV to
ImageNet tensor, temporal aggregation, QP mapping, per-block reduce and every
sidecar format — but has no in-process ONNX Runtime. The subprocess bridge the
fork uses elsewhere (vmafx-ort-runner, ADR-0713) passes tensors as an argv
JSON array, which cannot carry a full-resolution 3xHxW frame. Until an
in-process ORT binding or a streaming runner protocol lands, --saliency-aware
degrades to a plain encode with a warning — the same degradation the Python
takes when onnxruntime is not installed.

Example:
  vmafx-tune-go recommend-saliency \
    --src ref.yuv --width 1920 --height 1080 --duration-frames 240 \
    --encoder libx264 --saliency-aware --output out.mp4`

	cmd.Flags().StringVar(&flags.src, "src", "", "Raw YUV reference (required)")
	cmd.Flags().IntVar(&flags.width, "width", 0, "Reference width (required)")
	cmd.Flags().IntVar(&flags.height, "height", 0, "Reference height (required)")
	cmd.Flags().StringVar(&flags.pixFmt, "pix-fmt", "yuv420p", "ffmpeg pix_fmt")
	cmd.Flags().Float64Var(&flags.framerate, "framerate", 24.0, "Reference framerate")
	cmd.Flags().StringVar(&flags.encoder, "encoder", "libx264",
		"Codec adapter; saliency ROI supports "+
			strings.Join(saliency.SupportedEncoders(), ", "))
	cmd.Flags().StringVar(&flags.preset, "preset", "medium", "Encoder preset")
	cmd.Flags().IntVar(&flags.crf, "crf", -1,
		"Explicit CRF; defaults to the codec adapter's quality default")
	cmd.Flags().IntVar(&flags.durationFrames, "duration-frames", 0,
		"Frame count to score saliency over, typically the full clip (required)")
	cmd.Flags().BoolVar(&flags.saliencyAware, "saliency-aware", false,
		"Enable saliency biasing; without it this is a plain encode")
	cmd.Flags().IntVar(&flags.saliencyOffset, "saliency-offset", -4,
		"QP delta applied at peak saliency (clamped to +/-12)")
	cmd.Flags().StringVar(&flags.saliencyModel, "saliency-model", "",
		"Path to saliency_student_v1.onnx (default: the shipped fork model)")
	cmd.Flags().StringVar(&flags.saliencyAggregator, "saliency-aggregator", "mean",
		"Temporal reducer for the sampled masks: mean, ema, max or motion-weighted")
	cmd.Flags().Float64Var(&flags.saliencyEMAAlpha, "saliency-ema-alpha", 0.6,
		"Current-frame weight for --saliency-aggregator=ema")
	cmd.Flags().BoolVar(&flags.fallbackPlain, "saliency-fallback-plain", false,
		"Accept a plain encode when the encoder has no ROI dispatch (ADR-0546)")
	cmd.Flags().StringVar(&flags.ffmpegBin, "ffmpeg-bin", "ffmpeg", "ffmpeg binary")
	cmd.Flags().StringVar(&flags.output, "output", "",
		"Encode destination (mp4 / mkv / ...); a .json path writes the report "+
			"there and encodes to a sibling _encoded.mp4 (required)")

	_ = cmd.MarkFlagRequired("src")
	_ = cmd.MarkFlagRequired("width")
	_ = cmd.MarkFlagRequired("height")
	_ = cmd.MarkFlagRequired("duration-frames")
	_ = cmd.MarkFlagRequired("output")

	return cmd
}

// saliencyPayload is the emitted JSON report. Keys and ordering reproduce the
// Python handler's dict.
type saliencyPayload struct {
	CRF                int     `json:"crf"`
	EncodeSizeBytes    int64   `json:"encode_size_bytes"`
	EncodeTimeMS       float64 `json:"encode_time_ms"`
	Encoder            string  `json:"encoder"`
	EncoderVersion     string  `json:"encoder_version"`
	ExitStatus         int     `json:"exit_status"`
	FFmpegVersion      string  `json:"ffmpeg_version"`
	Output             string  `json:"output"`
	Preset             string  `json:"preset"`
	SaliencyAggregator string  `json:"saliency_aggregator"`
	SaliencyAware      bool    `json:"saliency_aware"`
}

// runRecommendSaliency drives one saliency-aware encode end to end.
func runRecommendSaliency(ctx context.Context, d deps, flags *recommendSaliencyFlags) error {
	if flags.src == "" || flags.output == "" {
		return errors.New("--src and --output are required")
	}
	if flags.width <= 0 || flags.height <= 0 {
		return errors.New("--width and --height must be positive")
	}
	if flags.durationFrames <= 0 {
		return errors.New("--duration-frames must be positive")
	}
	adapter, adapterErr := codecadapter.Get(flags.encoder)
	if adapterErr != nil {
		return adapterErr
	}
	crf := flags.crf
	if crf < 0 {
		crf = adapter.QualityDefault
	}

	cfg := saliency.DefaultConfig()
	cfg.ForegroundOffset = flags.saliencyOffset
	cfg.TemporalAggregator = saliency.Aggregator(flags.saliencyAggregator)
	cfg.EMAAlpha = flags.saliencyEMAAlpha
	cfg.AllowUnsupportedEncoderFallback = flags.fallbackPlain
	if err := cfg.Validate(); err != nil {
		return err
	}

	// A .json --output names a report destination, not a container: encode to
	// a sibling _encoded.mp4 so ffmpeg gets a valid muxer.
	outputPath := flags.output
	jsonReportPath := ""
	if strings.EqualFold(filepath.Ext(outputPath), ".json") {
		jsonReportPath = outputPath
		stem := strings.TrimSuffix(outputPath, filepath.Ext(outputPath))
		outputPath = stem + "_encoded.mp4"
	}

	req := ffencode.Request{
		Source: flags.src, Width: flags.width, Height: flags.height,
		PixFmt: flags.pixFmt, Framerate: flags.framerate,
		Encoder: flags.encoder, Preset: flags.preset, CRF: crf,
		Output: outputPath,
	}

	// saliencyApplied records whether an ROI sidecar was actually attached, as
	// opposed to merely requested. applySaliencyROI has three paths that fall
	// back to a plain encode without an error (no inference session, inference
	// unavailable, encoder without ROI support), and reporting the flag instead
	// of the outcome made the JSON claim "saliency_aware": true for an encode
	// that had no ROI map at all. The log warned; the machine-readable payload,
	// which is what downstream tooling reads, did not.
	saliencyApplied := false
	if flags.saliencyAware {
		augmented, cleanup, applied, err := applySaliencyROI(ctx, d, flags, cfg, req)
		if err != nil {
			return err
		}
		if cleanup != nil {
			defer cleanup()
		}
		req = augmented
		saliencyApplied = applied
	}

	res, encErr := ffencode.Run(ctx, req, flags.ffmpegBin, nil)
	if encErr != nil {
		return encErr
	}

	payload := saliencyPayload{
		CRF:                res.Request.CRF,
		EncodeSizeBytes:    res.EncodeSizeBytes,
		EncodeTimeMS:       res.EncodeTimeMS,
		Encoder:            res.Request.Encoder,
		EncoderVersion:     res.EncoderVersion,
		ExitStatus:         res.ExitStatus,
		FFmpegVersion:      res.FFmpegVersion,
		Output:             res.Request.Output,
		Preset:             res.Request.Preset,
		SaliencyAggregator: flags.saliencyAggregator,
		SaliencyAware:      saliencyApplied,
	}
	// json.dumps(payload, indent=2, sort_keys=True).
	rendered, marshalErr := pyjson.MarshalIndent(payload, true)
	if marshalErr != nil {
		return fmt.Errorf("render saliency report: %w", marshalErr)
	}

	if jsonReportPath != "" {
		if err := writeOutput(jsonReportPath, string(rendered)+"\n"); err != nil {
			return err
		}
		if _, printErr := fmt.Println(jsonReportPath); printErr != nil {
			return printErr
		}
	} else if _, printErr := fmt.Print(string(rendered) + "\n"); printErr != nil {
		return printErr
	}

	if res.ExitStatus != 0 {
		return &exitCodeError{
			code: res.ExitStatus,
			err:  fmt.Errorf("encode failed with exit status %d", res.ExitStatus),
		}
	}
	return nil
}

// applySaliencyROI computes the saliency map, builds the encoder's ROI
// sidecar, and returns the augmented encode request plus a cleanup for any
// ephemeral sidecar.
//
// When inference is unavailable it logs a warning and returns the request
// unchanged, which demotes the call to a plain encode. An encoder without ROI
// dispatch is an exit-2 error unless the fallback is opted into.
func applySaliencyROI(
	ctx context.Context,
	d deps,
	flags *recommendSaliencyFlags,
	cfg saliency.Config,
	req ffencode.Request,
) (ffencode.Request, func(), bool, error) {
	session, sessionErr := saliencySessionFactory(flags.saliencyModel)
	if sessionErr != nil {
		d.Log.WarnContext(ctx,
			"saliency inference unavailable; falling back to a plain encode",
			"reason", sessionErr.Error())
		return req, nil, false, nil
	}

	mask, mapErr := saliency.ComputeMap(flags.src, flags.width, flags.height, session,
		saliency.MapOptions{
			FrameSamples:       cfg.FrameSamples,
			TemporalAggregator: cfg.TemporalAggregator,
			EMAAlpha:           cfg.EMAAlpha,
		})
	if mapErr != nil {
		if errors.Is(mapErr, saliency.ErrUnavailable) {
			d.Log.WarnContext(ctx,
				"saliency inference failed; falling back to a plain encode",
				"error", mapErr)
			return req, nil, false, nil
		}
		return req, nil, false, mapErr
	}

	qpMap := saliency.ToQPMap(mask, cfg.ForegroundOffset)
	augment, buildErr := saliency.BuildAugment(
		flags.encoder, qpMap, flags.width, flags.height, flags.durationFrames)
	if buildErr != nil {
		var unsupported *saliency.UnsupportedEncoderError
		if errors.As(buildErr, &unsupported) {
			if !saliency.FallbackAllowed(cfg) {
				return req, nil, false, &exitCodeError{code: 2, err: buildErr}
			}
			d.Log.ErrorContext(ctx,
				"saliency ROI is not implemented for this encoder; "+
					"falling back to a plain encode",
				"encoder", flags.encoder,
				"supported", saliency.SupportedEncoders())
			return req, nil, false, nil
		}
		return req, nil, false, buildErr
	}

	// Argv-only encoders (libx265 zones) need no sidecar file.
	if augment.SidecarBody == "" {
		req.ExtraParams = append(append([]string(nil), req.ExtraParams...),
			augment.ExtraParams...)
		return req, nil, false, nil
	}

	var sidecarPath string
	var cleanup func()
	if cfg.PersistSidecar {
		sidecarPath = saliency.PersistedSidecarPath(req.Output, augment.SidecarSuffix)
	} else {
		tmp, err := os.CreateTemp("", "vmafx-tune-roi-*"+augment.SidecarSuffix)
		if err != nil {
			return req, nil, false, fmt.Errorf("create ROI sidecar: %w", err)
		}
		sidecarPath = tmp.Name()
		if closeErr := tmp.Close(); closeErr != nil {
			return req, nil, false, fmt.Errorf("close ROI sidecar: %w", closeErr)
		}
		cleanup = func() {
			if rmErr := os.Remove(sidecarPath); rmErr != nil && !os.IsNotExist(rmErr) {
				d.Log.WarnContext(ctx, "remove ROI sidecar",
					"path", sidecarPath, "error", rmErr)
			}
		}
	}
	if err := saliency.WriteSidecar(augment, sidecarPath); err != nil {
		if cleanup != nil {
			cleanup()
		}
		return req, nil, false, err
	}
	req.ExtraParams = append(append([]string(nil), req.ExtraParams...),
		saliency.ExtraParamsFor(flags.encoder, sidecarPath)...)
	d.Log.InfoContext(ctx, "wrote saliency ROI sidecar",
		"path", sidecarPath, "encoder", flags.encoder)
	return req, cleanup, true, nil
}
