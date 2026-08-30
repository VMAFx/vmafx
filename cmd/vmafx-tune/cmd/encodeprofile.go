// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"
	"github.com/spf13/pflag"

	pyjson "github.com/VMAFx/vmafx/internal/pyjsonstrict"
	"github.com/VMAFx/vmafx/pkg/encodeprofile"
)

// encodeProfileFlags holds flags parsed by the encode-profile subcommand. The
// names mirror `vmaf-tune encode-profile` one-for-one.
//
// The pointer-typed fields correspond to Python flags whose default is None
// rather than a value: cobra always materialises a zero, so runEncodeProfile
// consults Flags().Changed to tell "user passed 0" from "user passed nothing".
type encodeProfileFlags struct {
	profile string
	output  string
	src     string
	codec   string

	targetVMAF          float64
	recommendationIndex int
	preset              string
	pixFmt              string
	framerate           float64
	width               int
	height              int
	duration            float64

	sourceKind        string
	sampleClipSeconds float64
	sampleClipStartS  float64
	extraFFmpegArgs   []string
	ffmpegBin         string
	dryRun            bool

	// flagSet is the command's own FlagSet, captured so the *Opt helpers can
	// ask whether a flag was actually passed.
	flagSet *pflag.FlagSet
}

// newEncodeProfileCmd builds and returns the "encode-profile" cobra subcommand.
func newEncodeProfileCmd() *cobra.Command {
	flags := &encodeProfileFlags{}

	cmd := clikit.Command("encode-profile",
		"Read a vmaf-tune report/profile and encode one selected recommendation with FFmpeg",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			// Validation failures exit 2, matching `vmaf-tune encode-profile`;
			// a failed encode keeps FFmpeg's own status instead.
			return asUsageError(runEncodeProfile(ctx, d, flags))
		})),
	)
	cmd.Long = `Reproduce one recommendation from a vmaf-tune report.

Every report embeds a machine-readable "encoder_profile" payload. This
subcommand reads that payload — from the report JSON, the report HTML (payload
in a <pre> block) or the report Markdown (payload in a ` + "```json" + ` fence) —
selects one recommendation, and runs the matching FFmpeg encode.

Selection defaults to the first Pareto-selected row with the lowest bitrate.
--codec and --target-vmaf narrow the candidate set; --recommendation-index then
picks the Nth survivor (zero-based).

--dry-run prints the selected recommendation and the exact FFmpeg argv without
encoding anything. On a real run the process exit status is FFmpeg's own.

Hardware-encoder note: the emitted argv contains no -init_hw_device chain, so
a QSV profile row needs FFmpeg's VA-API device flags supplied separately (see
ADR-0601). This matches the Python implementation exactly — vmaf-tune injects
that chain in its 'compare' sweep, never in 'encode-profile'. Hardware encoders
also reject sources below roughly 320x240.

Example — inspect without encoding:
  vmafx-tune-go encode-profile \
    --profile report.json \
    --output /dev/null \
    --dry-run

Example — reproduce the best x265 row at target 95:
  vmafx-tune-go encode-profile \
    --profile report.html \
    --codec libx265 \
    --target-vmaf 95 \
    --output encoded.mkv`

	f := cmd.Flags()
	flags.flagSet = f
	f.StringVar(&flags.profile, "profile", "",
		"Report JSON/HTML/Markdown containing encoder_profile (required)")
	f.StringVarP(&flags.output, "output", "o", "", "Encoded output path (required)")
	f.StringVar(&flags.src, "src", "", "Override the source path stored in the profile")
	f.StringVar(&flags.codec, "codec", "", "Restrict selection to one codec")
	f.Float64Var(&flags.targetVMAF, "target-vmaf", 0,
		"Restrict selection to one target VMAF")
	f.IntVar(&flags.recommendationIndex, "recommendation-index", 0,
		"Zero-based index after --codec/--target-vmaf filtering")
	f.StringVar(&flags.preset, "preset", "", "Override the stored/default preset")
	f.StringVar(&flags.pixFmt, "pix-fmt", "", "Override raw-source pixel format")
	f.Float64Var(&flags.framerate, "framerate", 0, "Override raw-source framerate")
	f.IntVar(&flags.width, "width", 0, "Override raw-source width")
	f.IntVar(&flags.height, "height", 0, "Override raw-source height")
	f.Float64Var(&flags.duration, "duration", 0, "Override the encode duration in seconds")
	f.StringVar(&flags.sourceKind, "source-kind", "auto",
		"Input interpretation: auto, container or raw (.yuv/.raw/.rgb/.gray are raw)")
	f.Float64Var(&flags.sampleClipSeconds, "sample-clip-seconds", 0,
		"Optional input-side clip length forwarded to FFmpeg")
	f.Float64Var(&flags.sampleClipStartS, "sample-clip-start-s", 0,
		"Optional input-side clip offset forwarded to FFmpeg")
	f.StringArrayVar(&flags.extraFFmpegArgs, "extra-ffmpeg-arg", nil,
		"Append one raw FFmpeg argv token after the codec args; repeat as needed "+
			"(use --extra-ffmpeg-arg=-movflags for tokens beginning with '-')")
	f.StringVar(&flags.ffmpegBin, "ffmpeg-bin", "",
		"Override the profile's ffmpeg_bin (default: profile value, then ffmpeg)")
	f.BoolVar(&flags.dryRun, "dry-run", false,
		"Print the selected recommendation and ffmpeg argv without encoding")

	// Required-flag enforcement lives in runEncodeProfile, not
	// MarkFlagRequired, so a missing flag exits 2 like the Python CLI.
	useUsageExitCode(cmd)

	// The command prints its own JSON result and then exits with FFmpeg's
	// status; cobra must not append an "Error: ..." line after that payload.
	cmd.SilenceErrors = true
	cmd.SilenceUsage = true

	return cmd
}

// runEncodeProfile is the implementation of the encode-profile subcommand.
func runEncodeProfile(ctx context.Context, d deps, flags *encodeProfileFlags) error {
	if flags.profile == "" {
		return errors.New("--profile is required")
	}
	if flags.output == "" {
		return errors.New("--output is required")
	}

	profile, err := encodeprofile.LoadProfilePayload(flags.profile)
	if err != nil {
		return err
	}

	rec, err := encodeprofile.SelectRecommendation(profile, encodeprofile.SelectOptions{
		Codec:      flags.codec,
		TargetVMAF: flags.targetVMAFOpt(),
		Index:      flags.recommendationIndexOpt(),
	})
	if err != nil {
		return err
	}

	req, err := encodeprofile.BuildEncodeRequest(profile, rec, encodeprofile.BuildOptions{
		Output:            flags.output,
		SourceOverride:    flags.src,
		PresetOverride:    flags.preset,
		PixFmtOverride:    flags.pixFmt,
		FramerateOverride: flags.framerateOpt(),
		WidthOverride:     flags.widthOpt(),
		HeightOverride:    flags.heightOpt(),
		DurationOverride:  flags.durationOpt(),
		SourceKind:        flags.sourceKind,
		SampleClipSeconds: flags.sampleClipSeconds,
		SampleClipStartS:  flags.sampleClipStartS,
		ExtraParams:       flags.extraFFmpegArgs,
	})
	if err != nil {
		return err
	}

	ffmpegBin := flags.ffmpegBin
	if ffmpegBin == "" {
		if runMeta, ok := profile["run"].(map[string]any); ok {
			if v, ok := runMeta["ffmpeg_bin"].(string); ok {
				ffmpegBin = v
			}
		}
	}
	if ffmpegBin == "" {
		ffmpegBin = "ffmpeg"
	}

	argv, err := encodeprofile.BuildFFmpegCommand(req, ffmpegBin)
	if err != nil {
		return err
	}

	if flags.dryRun {
		d.Log.InfoContext(ctx, "encode-profile dry run",
			"profile", flags.profile, "codec", req.Encoder, "crf", req.CRF)
		return emitEncodeProfileJSON(map[string]any{
			"ok":          true,
			"dry_run":     true,
			"profile":     encodeprofile.NormalisePath(flags.profile),
			"selected":    map[string]any(rec),
			"ffmpeg_argv": toAnySlice(argv),
			"output":      req.Output,
		})
	}

	// G301: 0o750 keeps the directory accessible to the owner group only.
	if dir := filepath.Dir(req.Output); dir != "" && dir != "." {
		if err := os.MkdirAll(dir, 0o750); err != nil {
			return fmt.Errorf("create output dir: %w", err)
		}
	}

	d.Log.InfoContext(ctx, "encoding profile recommendation",
		"profile", flags.profile, "codec", req.Encoder, "preset", req.Preset,
		"crf", req.CRF, "output", req.Output)

	result, err := encodeprofile.RunEncode(req, ffmpegBin, nil)
	if err != nil {
		return err
	}

	if err := emitEncodeProfileJSON(map[string]any{
		"ok":                result.ExitStatus == 0,
		"profile":           encodeprofile.NormalisePath(flags.profile),
		"selected":          map[string]any(rec),
		"ffmpeg_argv":       toAnySlice(argv),
		"output":            req.Output,
		"exit_status":       result.ExitStatus,
		"encode_size_bytes": result.EncodeSizeBytes,
		"encode_time_ms":    result.EncodeTimeMS,
		"encoder_version":   result.EncoderVersion,
		"ffmpeg_version":    result.FFmpegVersion,
		"stderr_tail":       result.StderrTail,
	}); err != nil {
		return err
	}

	if result.ExitStatus != 0 {
		// Propagate FFmpeg's own status, the way the Python CLI returns
		// int(result.exit_status) from main().
		return exitCodeError{
			code: result.ExitStatus,
			err:  fmt.Errorf("ffmpeg exited with status %d", result.ExitStatus),
		}
	}
	return nil
}

// resultWriter is where the subcommand's JSON result goes. It is a variable
// so tests can capture the payload without redirecting the process stdout;
// production always leaves it at os.Stdout.
var resultWriter io.Writer = os.Stdout

// emitEncodeProfileJSON writes the result payload as CPython's
// json.dumps(payload, indent=2, sort_keys=True) would, with the trailing
// newline the Python CLI appends.
func emitEncodeProfileJSON(payload map[string]any) error {
	// NaNAsToken mirrors json.dumps' default allow_nan=True: the Python CLI
	// calls json.dumps directly here, not jsonio.dumps_strict, so a non-finite
	// value in a recommendation row would surface as a bare NaN token rather
	// than null.
	s, err := pyjson.Marshal(payload, pyjson.NaNAsToken)
	if err != nil {
		return fmt.Errorf("render result JSON: %w", err)
	}
	_, err = io.WriteString(resultWriter, s+"\n")
	return err
}

// toAnySlice widens a []string for the pyjson encoder.
func toAnySlice(items []string) []any {
	out := make([]any, len(items))
	for i, v := range items {
		out[i] = v
	}
	return out
}

// targetVMAFOpt returns the --target-vmaf filter, or nil when the flag was not
// passed. Python defaults it to None, and 0 is a legal (if useless) value, so
// the zero cannot stand in for "absent".
func (f *encodeProfileFlags) targetVMAFOpt() *float64 {
	if !f.changed("target-vmaf") {
		return nil
	}
	return &f.targetVMAF
}

// recommendationIndexOpt returns the --recommendation-index selector, or nil
// when the flag was not passed. Index 0 is the common case, so distinguishing
// it from "absent" matters: with the flag absent, an out-of-range profile is
// not an error.
func (f *encodeProfileFlags) recommendationIndexOpt() *int {
	if !f.changed("recommendation-index") {
		return nil
	}
	return &f.recommendationIndex
}

func (f *encodeProfileFlags) framerateOpt() *float64 {
	if !f.changed("framerate") {
		return nil
	}
	return &f.framerate
}

func (f *encodeProfileFlags) widthOpt() *int {
	if !f.changed("width") {
		return nil
	}
	return &f.width
}

func (f *encodeProfileFlags) heightOpt() *int {
	if !f.changed("height") {
		return nil
	}
	return &f.height
}

// durationOpt returns the --duration override, or nil when the flag was not
// passed. Zero is meaningful here — it suppresses the profile's own duration —
// so it must not be conflated with absence.
func (f *encodeProfileFlags) durationOpt() *float64 {
	if !f.changed("duration") {
		return nil
	}
	return &f.duration
}

// changed reports whether the named flag was passed on the command line. It is
// set by newEncodeProfileCmd before RunE fires.
func (f *encodeProfileFlags) changed(name string) bool {
	if f.flagSet == nil {
		return false
	}
	return f.flagSet.Changed(name)
}
