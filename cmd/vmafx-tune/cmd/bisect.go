// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/bisect"
	"github.com/VMAFx/vmafx/pkg/bitratesearch"
	"github.com/VMAFx/vmafx/pkg/encoder"
	"github.com/VMAFx/vmafx/pkg/report"
)

// bisectFlags holds flags parsed by the bisect subcommand.
type bisectFlags struct {
	reference      string
	codec          string
	targetVMAF     float64
	bitrateMin     float64
	bitrateMax     float64
	tolerance      float64
	maxIter        int
	scaleWidth     int
	scaleHeight    int
	output         string
	format         string
	ffmpegBin      string
	vmafBin        string
	workDir        string
}

// newBisectCmd builds and returns the "bisect" cobra subcommand.
func newBisectCmd() *cobra.Command {
	flags := &bisectFlags{}

	cmd := &cobra.Command{
		Use:   "bisect",
		Short: "Binary-search a bitrate range to hit a target VMAF score",
		Long: `Find the minimum bitrate that achieves a target VMAF score for a
fixed encoder, using binary search in the bitrate domain.

Unlike the CRF-domain bisect (used by compare/ladder), this subcommand
encodes in VBR mode (-b:v) at each probe bitrate and measures the resulting
VMAF.  The binary search converges to the lowest bitrate that meets the target
within the --tolerance window.

This is the Go port of 'vmaf-tune bisect' (ADR-0734).

Algorithm:
  1. Probe --bitrate-min: if VMAF ≥ target, return immediately.
  2. Probe --bitrate-max: if VMAF < target, no bitrate in the window works.
  3. Otherwise bisect: keep the lower half when VMAF ≥ target, upper half
     when below.  Stop when hi − lo ≤ --tolerance or --max-iter is reached.

Output schema (schema_version 1) mirrors Python vmaf-tune bisect JSON.

Example:
  vmafx-tune-go bisect \
    --reference src.mp4 \
    --encoder libx264 \
    --target-vmaf 90 \
    --bitrate-min 500 \
    --bitrate-max 10000 \
    --tolerance 50 \
    --output result.json`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runBisect(flags)
		},
	}

	cmd.Flags().StringVarP(&flags.reference, "reference", "r", "",
		"Path to the reference video (required)")
	cmd.Flags().StringVarP(&flags.codec, "encoder", "e", "libx264",
		"Encoder codec name (e.g. libx264, libx265, libsvtav1)")
	cmd.Flags().Float64VarP(&flags.targetVMAF, "target-vmaf", "t", 90.0,
		"Target VMAF score to achieve")
	cmd.Flags().Float64Var(&flags.bitrateMin, "bitrate-min", 500,
		"Lower bound of bitrate search window (kbps)")
	cmd.Flags().Float64Var(&flags.bitrateMax, "bitrate-max", 10000,
		"Upper bound of bitrate search window (kbps)")
	cmd.Flags().Float64Var(&flags.tolerance, "tolerance", bitratesearch.DefaultTolerance,
		"Convergence tolerance (kbps); bisect stops when hi−lo ≤ tolerance")
	cmd.Flags().IntVar(&flags.maxIter, "max-iter", bitratesearch.DefaultMaxIter,
		"Maximum bisect iterations")
	cmd.Flags().IntVar(&flags.scaleWidth, "scale-width", 0,
		"Downscale width before encoding (0 = no scaling)")
	cmd.Flags().IntVar(&flags.scaleHeight, "scale-height", 0,
		"Downscale height before encoding (0 = no scaling)")
	cmd.Flags().StringVarP(&flags.output, "output", "o", "",
		"Output file path (default: stdout)")
	cmd.Flags().StringVar(&flags.format, "format", "json",
		"Output format: json or markdown")
	cmd.Flags().StringVar(&flags.ffmpegBin, "ffmpeg", "ffmpeg",
		"Path to ffmpeg binary")
	cmd.Flags().StringVar(&flags.vmafBin, "vmaf", "vmaf",
		"Path to vmaf binary for scoring")
	cmd.Flags().StringVar(&flags.workDir, "work-dir", "",
		"Directory for temporary encode outputs (defaults to OS temp dir)")

	_ = cmd.MarkFlagRequired("reference")

	return cmd
}

// runBisect is the implementation of the bisect subcommand.
func runBisect(flags *bisectFlags) error {
	if flags.reference == "" {
		return errors.New("--reference is required")
	}
	if _, err := os.Stat(flags.reference); err != nil {
		return fmt.Errorf("reference file %q: %w", flags.reference, err)
	}
	if flags.targetVMAF <= 0 || flags.targetVMAF > 100 {
		return fmt.Errorf("--target-vmaf %g out of range (0, 100]", flags.targetVMAF)
	}
	if flags.bitrateMin <= 0 {
		return errors.New("--bitrate-min must be > 0")
	}
	if flags.bitrateMax <= flags.bitrateMin {
		return fmt.Errorf("--bitrate-max (%g) must be > --bitrate-min (%g)",
			flags.bitrateMax, flags.bitrateMin)
	}
	format := strings.ToLower(flags.format)
	if format != "json" && format != "markdown" {
		return fmt.Errorf("unknown --format %q; supported: json, markdown", flags.format)
	}

	enc, encErr := encoder.NewExtended(strings.TrimSpace(flags.codec))
	if encErr != nil {
		return fmt.Errorf("encoder %q: %w", flags.codec, encErr)
	}

	scoreFunc := bisect.VMAFScoreFunc(flags.vmafBin)

	params := bitratesearch.Params{
		TargetVMAF:     flags.targetVMAF,
		BitrateMinKbps: flags.bitrateMin,
		BitrateMaxKbps: flags.bitrateMax,
		Tolerance:      flags.tolerance,
		MaxIter:        flags.maxIter,
		FFmpegBin:      flags.ffmpegBin,
		WorkDir:        flags.workDir,
		ScaleWidth:     flags.scaleWidth,
		ScaleHeight:    flags.scaleHeight,
	}

	t0 := time.Now()
	result, runErr := bitratesearch.Run(flags.reference, enc, scoreFunc, params)
	wallTimeMS := float64(time.Since(t0).Milliseconds())
	if runErr != nil {
		return fmt.Errorf("bisect: %w", runErr)
	}

	var output string
	var emitErr error
	if format == "json" {
		output, emitErr = emitBisectJSON(result, flags, wallTimeMS)
	} else {
		output = emitBisectMarkdown(result, flags, wallTimeMS)
	}
	if emitErr != nil {
		return fmt.Errorf("render bisect: %w", emitErr)
	}

	return writeOutput(flags.output, output)
}

// bisectWirePayload is the JSON wire format for the bisect subcommand.
// schema_version 1 mirrors the Python vmaf-tune bisect JSON schema.
type bisectWirePayload struct {
	SchemaVersion    int                    `json:"schema_version"`
	Src              string                 `json:"src"`
	Encoder          string                 `json:"encoder"`
	TargetVMAF       float64                `json:"target_vmaf"`
	BitrateMinKbps   float64                `json:"bitrate_min_kbps"`
	BitrateMaxKbps   float64                `json:"bitrate_max_kbps"`
	ToleranceKbps    float64                `json:"tolerance_kbps"`
	ToolVersion      string                 `json:"tool_version"`
	WallTimeMS       float64                `json:"wall_time_ms"`
	BestBitrateKbps  float64                `json:"best_bitrate_kbps"`
	BestVMAFScore    float64                `json:"best_vmaf_score"`
	BestEncodeTimeMS float64                `json:"best_encode_time_ms"`
	Converged        bool                   `json:"converged"`
	Iterations       int                    `json:"iterations"`
	Samples          []bitratesearch.Sample `json:"samples"`
}

func emitBisectJSON(
	result bitratesearch.Result,
	flags *bisectFlags,
	wallTimeMS float64,
) (string, error) {
	payload := bisectWirePayload{
		SchemaVersion:    1,
		Src:              flags.reference,
		Encoder:          flags.codec,
		TargetVMAF:       flags.targetVMAF,
		BitrateMinKbps:   flags.bitrateMin,
		BitrateMaxKbps:   flags.bitrateMax,
		ToleranceKbps:    flags.tolerance,
		ToolVersion:      report.ToolVersion,
		WallTimeMS:       wallTimeMS,
		BestBitrateKbps:  result.BestBitrateKbps,
		BestVMAFScore:    result.BestVMAFScore,
		BestEncodeTimeMS: result.BestEncodeTimeMS,
		Converged:        result.Converged,
		Iterations:       result.Iterations,
		Samples:          result.Samples,
	}
	b, err := json.MarshalIndent(payload, "", "  ")
	if err != nil {
		return "", fmt.Errorf("marshal bisect: %w", err)
	}
	return string(b) + "\n", nil
}

func emitBisectMarkdown(
	result bitratesearch.Result,
	flags *bisectFlags,
	wallTimeMS float64,
) string {
	var sb strings.Builder

	fmt.Fprintf(&sb, "# Bitrate bisect — `%s`\n\n", flags.reference)
	fmt.Fprintf(&sb, "- Encoder: `%s`\n", flags.codec)
	fmt.Fprintf(&sb, "- Tool: `vmafx-tune %s` (ADR-0734)\n", report.ToolVersion)
	fmt.Fprintf(&sb, "- Target VMAF: %g\n", flags.targetVMAF)
	fmt.Fprintf(&sb, "- Bitrate window: %.0f – %.0f kbps\n", flags.bitrateMin, flags.bitrateMax)
	fmt.Fprintf(&sb, "- Tolerance: %.0f kbps\n", flags.tolerance)
	fmt.Fprintf(&sb, "- Wall time: %.1f ms\n\n", wallTimeMS)

	if result.BestBitrateKbps < 0 {
		sb.WriteString("**No bitrate in the search window achieves the target VMAF.**\n\n")
	} else {
		fmt.Fprintf(&sb, "## Result\n\n")
		fmt.Fprintf(&sb, "| Metric | Value |\n|---|---|\n")
		fmt.Fprintf(&sb, "| Best bitrate | %.0f kbps |\n", result.BestBitrateKbps)
		fmt.Fprintf(&sb, "| VMAF at best bitrate | %.2f |\n", result.BestVMAFScore)
		fmt.Fprintf(&sb, "| Converged | %v |\n", result.Converged)
		fmt.Fprintf(&sb, "| Iterations | %d |\n\n", result.Iterations)
	}

	if len(result.Samples) > 0 {
		sb.WriteString("## Probe history\n\n")
		sb.WriteString("| # | Bitrate (kbps) | VMAF | Encode time (ms) |\n")
		sb.WriteString("|---:|---:|---:|---:|\n")
		for i, s := range result.Samples {
			fmt.Fprintf(&sb, "| %d | %.0f | %.2f | %.0f |\n",
				i+1, s.BitrateKbps, s.VMAFScore, s.EncodeTimeMS)
		}
		sb.WriteByte('\n')
	}

	return sb.String()
}
