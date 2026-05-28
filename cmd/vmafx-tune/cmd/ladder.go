// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/bisect"
	"github.com/VMAFx/vmafx/pkg/encoder"
	"github.com/VMAFx/vmafx/pkg/ladder"
	"github.com/VMAFx/vmafx/pkg/report"
)

// ladderFlags holds flags parsed by the ladder subcommand.
type ladderFlags struct {
	reference         string
	codec             string
	resolutions       []string // raw "WxH" strings; parsed in runLadder
	targets           []float64
	output            string
	format            string
	ffmpegBin         string
	vmafBin           string
	workDir           string
	crfLo             int
	crfHi             int
	maxIter           int
	maxRungs          int
	minBitrateGapKbps float64
	workers           int // Stage-3: concurrent grid sampling semaphore size
}

// defaultResolutions is the canonical multi-resolution ladder grid used by
// the Python ladder.py default sampler (ADR-0307, Research-0079).
// Callers can override via --resolutions.
var defaultResolutions = []string{
	"320x240", "480x360", "640x480", "768x432", "1280x720", "1920x1080",
}

// newLadderCmd builds and returns the "ladder" cobra subcommand.
func newLadderCmd() *cobra.Command {
	flags := &ladderFlags{}

	cmd := &cobra.Command{
		Use:   "ladder",
		Short: "Per-title ABR bitrate-ladder generation from a VMAF-target sweep",
		Long: `Build an ABR bitrate ladder for a single source clip.

For each (resolution, VMAF target) cell in the sampling grid, a CRF bisect
finds the highest CRF whose measured VMAF meets the target.  The resulting
(bitrate, vmaf) cloud is reduced to its upper convex hull (the Pareto
frontier), and a small set of "knee" renditions is selected from the hull.

The JSON output is a superset of the Python vmaf-tune ladder schema and is
compatible with the existing HLS/DASH manifest renderer.

Stage-3 additions (ADR-0734):
  - Resolution-aware downscaling: the source is scaled to each rendition
    resolution before encoding using a Lanczos filter, so VMAF is measured
    at the correct playback resolution.
  - Concurrent grid sampling: --workers N runs up to N encoder/scorer pairs
    in parallel (default: NumCPU/2, clamped to [1,8]).

Hardware encoders (h264_nvenc, h264_qsv, h264_amf, etc.) are supported
when available; pass the codec name via --codec.

Example:
  vmafx-tune-go ladder \
    --reference src.mp4 \
    --codec libx264 \
    --targets 75,85,95 \
    --workers 4 \
    --output ladder.json`,
		RunE: func(cmd *cobra.Command, args []string) error {
			return runLadder(flags)
		},
	}

	cmd.Flags().StringVarP(&flags.reference, "reference", "r", "",
		"Path to the reference video (required)")
	cmd.Flags().StringVarP(&flags.codec, "codec", "c", "libx264",
		"Encoder codec name (e.g. libx264, libx265, h264_nvenc)")
	cmd.Flags().StringSliceVar(&flags.resolutions, "resolutions", defaultResolutions,
		"Comma-separated WxH resolution grid (e.g. 640x480,1280x720,1920x1080)")
	cmd.Flags().Float64SliceVarP(&flags.targets, "targets", "t",
		[]float64{75.0, 85.0, 95.0}, "Comma-separated VMAF targets for the sampling grid")
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
	cmd.Flags().IntVar(&flags.crfLo, "crf-lo", 0,
		"Lower bound of CRF search window (best quality)")
	cmd.Flags().IntVar(&flags.crfHi, "crf-hi", 0,
		"Upper bound of CRF search window (0 = encoder default)")
	cmd.Flags().IntVar(&flags.maxIter, "max-iter", bisect.DefaultMaxIter,
		"Maximum bisect iterations per (resolution, target) cell")
	cmd.Flags().IntVar(&flags.maxRungs, "max-rungs", ladder.DefaultMaxRungs,
		"Maximum renditions to select from the convex hull")
	cmd.Flags().Float64Var(&flags.minBitrateGapKbps, "min-bitrate-gap", ladder.DefaultMinBitrateGapKbps,
		"Minimum bitrate gap between adjacent renditions (kbps)")
	cmd.Flags().IntVar(&flags.workers, "workers", 0,
		"Concurrent grid-sampling goroutines (0 = NumCPU/2 clamped to [1,8])")

	_ = cmd.MarkFlagRequired("reference")

	return cmd
}

// parseResolution parses a "WxH" string into (width, height).
func parseResolution(s string) (int, int, error) {
	s = strings.TrimSpace(s)
	idx := strings.IndexByte(s, 'x')
	if idx < 0 {
		idx = strings.IndexByte(s, 'X')
	}
	if idx < 0 {
		return 0, 0, fmt.Errorf("resolution %q must be in WxH format (e.g. 1280x720)", s)
	}
	w, wErr := strconv.Atoi(s[:idx])
	h, hErr := strconv.Atoi(s[idx+1:])
	if wErr != nil || hErr != nil {
		return 0, 0, fmt.Errorf("resolution %q: non-integer dimension", s)
	}
	if w <= 0 || h <= 0 {
		return 0, 0, fmt.Errorf("resolution %q: dimensions must be positive", s)
	}
	return w, h, nil
}

// runLadder is the implementation of the ladder subcommand.
func runLadder(flags *ladderFlags) error {
	if flags.reference == "" {
		return errors.New("--reference is required")
	}
	if _, err := os.Stat(flags.reference); err != nil {
		return fmt.Errorf("reference file %q: %w", flags.reference, err)
	}
	if len(flags.targets) == 0 {
		return errors.New("--targets must specify at least one VMAF target")
	}
	for _, t := range flags.targets {
		if t <= 0 || t > 100 {
			return fmt.Errorf("target VMAF %g is out of range (0, 100]", t)
		}
	}
	format := strings.ToLower(flags.format)
	if format != "json" && format != "markdown" {
		return fmt.Errorf("unknown --format %q; supported: json, markdown", flags.format)
	}

	// Parse resolutions.
	if len(flags.resolutions) == 0 {
		return errors.New("--resolutions must specify at least one WxH resolution")
	}
	resolutions := make([][2]int, 0, len(flags.resolutions))
	for _, raw := range flags.resolutions {
		w, h, err := parseResolution(raw)
		if err != nil {
			return fmt.Errorf("--resolutions: %w", err)
		}
		resolutions = append(resolutions, [2]int{w, h})
	}

	// Validate and construct encoder (supports Stage-2 hardware encoders).
	enc, encErr := encoder.NewExtended(strings.TrimSpace(flags.codec))
	if encErr != nil {
		return fmt.Errorf("encoder %q: %w", flags.codec, encErr)
	}

	scoreFunc := bisect.VMAFScoreFunc(flags.vmafBin)

	// Build the sampler: wires bisect.Run for each (resolution, target) cell.
	// The closure captures enc, scoreFunc, and bisect params.
	//
	// Stage-3 (ADR-0734): the sampler injects ScaleWidth/ScaleHeight into
	// EncodeParams so the source is downscaled to each rendition resolution
	// before encoding.  VMAF is therefore measured at the actual playback
	// resolution rather than at the native source resolution (Stage-2
	// behaviour).  The scale filter uses Lanczos (flags=lanczos) via
	// pkg/encoder.EncodeParams.ScaleWidth/ScaleHeight.
	sampler := func(src, codecName string, width, height int, targetVMAF float64) (ladder.Point, error) {
		bisectParams := bisect.Params{
			TargetVMAF:  targetVMAF,
			MaxIter:     flags.maxIter,
			FFmpegBin:   flags.ffmpegBin,
			WorkDir:     flags.workDir,
			ScaleWidth:  width,
			ScaleHeight: height,
		}
		if flags.crfLo > 0 || flags.crfHi > 0 {
			bisectParams.CRFLo = flags.crfLo
			bisectParams.CRFHi = flags.crfHi
		}

		bisectResult, bisectErr := bisect.Run(src, enc, scoreFunc, bisectParams)
		if bisectErr != nil {
			return ladder.Point{}, bisectErr
		}
		if bisectResult.BestCRF < 0 {
			return ladder.Point{}, fmt.Errorf(
				"no CRF in search window achieves VMAF %.1f for %dx%d", targetVMAF, width, height)
		}

		return ladder.Point{
			Width:       width,
			Height:      height,
			BitratekBps: bisectResult.BestBitratekBps,
			VMAF:        bisectResult.BestVMAFScore,
			CRF:         bisectResult.BestCRF,
			TargetVMAF:  targetVMAF,
			OK:          true,
		}, nil
	}

	t0 := time.Now()
	result, buildErr := ladder.Build(flags.reference, enc.Name(), ladder.Params{
		Resolutions:       resolutions,
		TargetVMAFs:       flags.targets,
		Sampler:           sampler,
		MaxRungs:          flags.maxRungs,
		MinBitrateGapKbps: flags.minBitrateGapKbps,
		Workers:           flags.workers,
	})
	wallTimeMS := float64(time.Since(t0).Milliseconds())
	if buildErr != nil {
		return fmt.Errorf("ladder build: %w", buildErr)
	}

	var output string
	var emitErr error
	if format == "json" {
		output, emitErr = emitLadderJSON(result, flags, wallTimeMS)
	} else {
		output = emitLadderMarkdown(result, flags, wallTimeMS)
	}
	if emitErr != nil {
		return fmt.Errorf("render ladder: %w", emitErr)
	}

	return writeOutput(flags.output, output)
}

// ladderWirePayload is the JSON wire format for the ladder subcommand.
// Fields mirror the Python ladder.py Ladder + rendition output for schema
// compatibility (ADR-0705 schema-forward invariant).
type ladderWirePayload struct {
	SchemaVersion int                `json:"schema_version"`
	Src           string             `json:"src"`
	Encoder       string             `json:"encoder"`
	TargetVMAFs   []float64          `json:"target_vmafs"`
	Resolutions   []string           `json:"resolutions"`
	ToolVersion   string             `json:"tool_version"`
	WallTimeMS    float64            `json:"wall_time_ms"`
	Cloud         []ladder.Point     `json:"cloud"`
	Hull          []ladder.Point     `json:"hull"`
	Renditions    []ladder.Rendition `json:"renditions"`
}

// emitLadderJSON serialises the ladder result as pretty-printed JSON.
// NaN / Inf bitrate fields are coerced to null (RFC 8259 compliance).
func emitLadderJSON(
	result ladder.LadderResult,
	flags *ladderFlags,
	wallTimeMS float64,
) (string, error) {
	// Sanitise cloud points: NaN bitrates → 0 (ladder JSON does not use the
	// report.Row NaN convention since failed points carry OK=false).
	cloud := make([]ladder.Point, len(result.Cloud))
	for i, p := range result.Cloud {
		if math.IsNaN(p.BitratekBps) || math.IsInf(p.BitratekBps, 0) {
			p.BitratekBps = 0
		}
		cloud[i] = p
	}

	resStrs := make([]string, len(flags.resolutions))
	copy(resStrs, flags.resolutions)

	payload := ladderWirePayload{
		SchemaVersion: 1,
		Src:           result.Src,
		Encoder:       result.Encoder,
		TargetVMAFs:   flags.targets,
		Resolutions:   resStrs,
		ToolVersion:   report.ToolVersion,
		WallTimeMS:    wallTimeMS,
		Cloud:         cloud,
		Hull:          result.Hull,
		Renditions:    result.Renditions,
	}
	b, err := json.MarshalIndent(payload, "", "  ")
	if err != nil {
		return "", fmt.Errorf("marshal ladder: %w", err)
	}
	return string(b) + "\n", nil
}

// emitLadderMarkdown renders the ladder result as a Markdown document.
func emitLadderMarkdown(
	result ladder.LadderResult,
	flags *ladderFlags,
	wallTimeMS float64,
) string {
	var sb strings.Builder

	fmt.Fprintf(&sb, "# Per-title ABR ladder — `%s`\n\n", result.Src)
	fmt.Fprintf(&sb, "- Encoder: `%s`\n", result.Encoder)
	fmt.Fprintf(&sb, "- Tool: `vmafx-tune %s` (ADR-0730)\n", report.ToolVersion)
	targetStrs := make([]string, len(flags.targets))
	for i, t := range flags.targets {
		targetStrs[i] = fmt.Sprintf("%g", t)
	}
	fmt.Fprintf(&sb, "- VMAF targets: %s\n", strings.Join(targetStrs, ", "))
	fmt.Fprintf(&sb, "- Resolutions: %s\n", strings.Join(flags.resolutions, ", "))
	fmt.Fprintf(&sb, "- Wall time: %.1f ms\n\n", wallTimeMS)

	// Renditions table.
	sb.WriteString("## Selected renditions\n\n")
	if len(result.Renditions) == 0 {
		sb.WriteString("**No renditions selected** (all sampled points failed or hull is empty).\n\n")
	} else {
		sb.WriteString("| Rank | Resolution | Bitrate (kbps) | VMAF | CRF |\n")
		sb.WriteString("|---:|---|---:|---:|---:|\n")
		for i, r := range result.Renditions {
			fmt.Fprintf(&sb, "| %d | %dx%d | %.1f | %.2f | %d |\n",
				i+1, r.Width, r.Height, r.BitratekBps, r.VMAF, r.CRF)
		}
		sb.WriteByte('\n')
	}

	// Hull table.
	sb.WriteString("## Convex hull (Pareto frontier)\n\n")
	if len(result.Hull) == 0 {
		sb.WriteString("Hull is empty — no successful samples.\n\n")
	} else {
		sb.WriteString("| Resolution | Bitrate (kbps) | VMAF | CRF | Target VMAF |\n")
		sb.WriteString("|---|---:|---:|---:|---:|\n")
		for _, p := range result.Hull {
			target := "—"
			if p.TargetVMAF > 0 {
				target = fmt.Sprintf("%g", p.TargetVMAF)
			}
			fmt.Fprintf(&sb, "| %dx%d | %.1f | %.2f | %d | %s |\n",
				p.Width, p.Height, p.BitratekBps, p.VMAF, p.CRF, target)
		}
		sb.WriteByte('\n')
	}

	// Cloud summary.
	okCount := 0
	for _, p := range result.Cloud {
		if p.OK {
			okCount++
		}
	}
	fmt.Fprintf(&sb, "_Sampled %d / %d grid cells successfully._\n", okCount, len(result.Cloud))

	return sb.String()
}
