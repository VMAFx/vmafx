// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/bisect"
	"github.com/VMAFx/vmafx/pkg/encoder"
	"github.com/VMAFx/vmafx/pkg/report"
)

// compareFlags holds flags parsed by the compare subcommand.
type compareFlags struct {
	reference string
	codecs    []string
	targets   []float64
	output    string
	format    string
	ffmpegBin string
	vmafBin   string
	workDir   string
	crfLo     int
	crfHi     int
	maxIter   int
}

// pairKey identifies one (codec, target) bisect task.
type pairKey struct {
	codec  string
	target float64
}

// pairResult holds the outcome of one (codec, target) bisect.
type pairResult struct {
	key   pairKey
	row   report.Row
	order int
}

// newCompareCmd builds and returns the "compare" cobra subcommand.
func newCompareCmd() *cobra.Command {
	flags := &compareFlags{}

	cmd := clikit.Command("compare",
		"Rate-quality sweep: compare codecs at one or more VMAF targets",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runCompare(ctx, d, flags)
		})),
	)
	cmd.Long = `Compare software encoders (libx264, libx265) at one or more VMAF
targets. For each (codec, target) pair the bisect finds the highest CRF whose
measured VMAF still meets the target, then reports bitrate and score.

Example:
  vmafx-tune-go compare \
    --reference src.mp4 \
    --codecs libx264,libx265 \
    --targets 85,90,95 \
    --output results.json \
    --format json`

	cmd.Flags().StringVarP(&flags.reference, "reference", "r", "",
		"Path to the reference video (required)")
	cmd.Flags().StringSliceVarP(&flags.codecs, "codecs", "c",
		encoder.KnownEncoders(), "Comma-separated encoder names (libx264, libx265)")
	cmd.Flags().Float64SliceVarP(&flags.targets, "targets", "t",
		[]float64{85.0}, "Comma-separated VMAF targets (e.g. 85,90,95)")
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
		"Maximum bisect iterations per (codec, target) pair")

	_ = cmd.MarkFlagRequired("reference")

	return cmd
}

// runCompare is the implementation of the compare subcommand. The injected
// golusoris dependencies carry the structured logger used for run diagnostics.
func runCompare(ctx context.Context, d deps, flags *compareFlags) error {
	if flags.reference == "" {
		return errors.New("--reference is required")
	}
	if _, err := os.Stat(flags.reference); err != nil {
		return fmt.Errorf("reference file %q: %w", flags.reference, err)
	}
	if len(flags.codecs) == 0 {
		return errors.New("--codecs must specify at least one encoder")
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

	// Build encoders, failing fast on unknown codecs.
	encoders := make([]encoder.Encoder, 0, len(flags.codecs))
	for _, name := range flags.codecs {
		enc, err := encoder.New(strings.TrimSpace(name))
		if err != nil {
			return fmt.Errorf("encoder %q: %w", name, err)
		}
		encoders = append(encoders, enc)
	}

	scoreFunc := bisect.VMAFScoreFunc(flags.vmafBin)

	// Collect all (encoder, target) pairs in parallel for wall time.
	// Stage-1 does not cap concurrency; Stage-2 adds --workers / semaphores.
	pairs := make([]pairKey, 0, len(encoders)*len(flags.targets))
	for _, enc := range encoders {
		for _, target := range flags.targets {
			pairs = append(pairs, pairKey{codec: enc.Name(), target: target})
		}
	}

	d.Log.InfoContext(ctx, "starting rate-quality sweep",
		"reference", flags.reference,
		"codecs", flags.codecs,
		"targets", flags.targets,
		"pairs", len(pairs))

	results := make([]pairResult, len(pairs))
	var wg sync.WaitGroup
	wg.Add(len(pairs))

	t0 := time.Now()

	for i, pair := range pairs {
		go func(idx int, pk pairKey) {
			defer wg.Done()

			enc, err := encoder.New(pk.codec)
			if err != nil {
				results[idx] = pairResult{
					key:   pk,
					order: idx,
					row:   failRow(pk.codec, pk.target, flags.ffmpegBin, err.Error()),
				}
				return
			}

			params := bisect.Params{
				TargetVMAF: pk.target,
				MaxIter:    flags.maxIter,
				FFmpegBin:  flags.ffmpegBin,
				WorkDir:    flags.workDir,
			}
			if flags.crfLo > 0 || flags.crfHi > 0 {
				params.CRFLo = flags.crfLo
				params.CRFHi = flags.crfHi
			}

			bisectResult, bisectErr := bisect.Run(flags.reference, enc, scoreFunc, params)
			if bisectErr != nil {
				results[idx] = pairResult{
					key:   pk,
					order: idx,
					row:   failRow(pk.codec, pk.target, flags.ffmpegBin, bisectErr.Error()),
				}
				return
			}

			row := rowFromBisect(pk.codec, pk.target, flags.ffmpegBin, bisectResult)
			results[idx] = pairResult{key: pk, order: idx, row: row}
		}(i, pair)
	}

	wg.Wait()
	wallTimeMS := float64(time.Since(t0).Milliseconds())
	d.Log.InfoContext(ctx, "rate-quality sweep complete", "wall_time_ms", wallTimeMS)

	// If there is only one target, emit a single-target (schema-v1) report
	// with rows sorted by ascending bitrate (ok rows first, fails trailing).
	// Multiple targets emit a schema-v2 sweep JSON.
	var output string
	var emitErr error

	if len(flags.targets) == 1 {
		rows := make([]report.Row, len(results))
		for _, r := range results {
			rows[r.order] = r.row
		}
		sortRows(rows)
		rep := report.Report{
			Src:         flags.reference,
			TargetVMAF:  flags.targets[0],
			ToolVersion: report.ToolVersion,
			WallTimeMS:  wallTimeMS,
			Rows:        rows,
		}
		if format == "json" {
			output, emitErr = report.EmitJSON(rep)
		} else {
			output = report.EmitMarkdown(rep)
		}
	} else {
		// Multi-target sweep: emit schema-v2 JSON (or Markdown sweep table).
		if format == "json" {
			output, emitErr = emitSweepJSON(results, flags, wallTimeMS)
		} else {
			output = emitSweepMarkdown(results, flags, wallTimeMS)
		}
	}

	if emitErr != nil {
		return fmt.Errorf("render report: %w", emitErr)
	}

	return writeOutput(flags.output, output)
}

// failRow builds a failed Row for an encoder that could not be run.
func failRow(codec string, target float64, ffmpegBin, reason string) report.Row {
	return report.Row{
		Codec:        codec,
		FFmpegBin:    ffmpegBin,
		BestCRF:      -1,
		BitratekBps:  math.NaN(),
		EncodeTimeMS: math.NaN(),
		VMAFScore:    math.NaN(),
		TargetVMAF:   target,
		OK:           false,
		Error:        reason,
	}
}

// rowFromBisect converts a bisect.Result to a report.Row.
func rowFromBisect(codec string, target float64, ffmpegBin string, res bisect.Result) report.Row {
	if res.BestCRF < 0 {
		return report.Row{
			Codec:        codec,
			FFmpegBin:    ffmpegBin,
			BestCRF:      -1,
			BitratekBps:  math.NaN(),
			EncodeTimeMS: math.NaN(),
			VMAFScore:    math.NaN(),
			TargetVMAF:   target,
			OK:           false,
			Error:        fmt.Sprintf("no CRF in search window achieves VMAF %.1f", target),
		}
	}
	return report.Row{
		Codec:          codec,
		FFmpegBin:      ffmpegBin,
		EncoderVersion: res.EncoderVersion,
		BestCRF:        res.BestCRF,
		BitratekBps:    res.BestBitratekBps,
		EncodeTimeMS:   res.BestEncodeTimeMS,
		VMAFScore:      res.BestVMAFScore,
		TargetVMAF:     target,
		OK:             true,
		BisectSamples:  res.Samples,
	}
}

// sortRows sorts rows in place: ok rows ascending by bitrate, then by codec;
// failed rows trailing sorted by codec. Mirrors Python compare._rank().
func sortRows(rows []report.Row) {
	sort.SliceStable(rows, func(i, j int) bool {
		ri, rj := rows[i], rows[j]
		if ri.OK != rj.OK {
			return ri.OK // ok rows sort before fail rows
		}
		if ri.OK {
			if ri.BitratekBps != rj.BitratekBps {
				return ri.BitratekBps < rj.BitratekBps
			}
			return ri.Codec < rj.Codec
		}
		return ri.Codec < rj.Codec
	})
}

// emitSweepJSON emits a schema-v2 sweep JSON payload. NaN / Inf float
// values (including those nested inside bisect_samples) are coerced to
// JSON null via report.SanitizeBisectSamples; a single corrupt vmaf-XML
// mean value would otherwise crash json.MarshalIndent and break the
// Python ↔ Go parser-parity invariant (AGENTS.md #2).
func emitSweepJSON(
	results []pairResult,
	flags *compareFlags,
	wallTimeMS float64,
) (string, error) {
	// Build a flat payload matching Python SweepReport schema-v2.
	type wireRow struct {
		Codec          string  `json:"codec"`
		Adapter        string  `json:"adapter"`
		RuntimeVariant string  `json:"runtime_variant"`
		FFmpegBin      string  `json:"ffmpeg_bin"`
		EncoderVersion string  `json:"encoder_version"`
		BestCRF        int     `json:"best_crf"`
		BitratekBps    any     `json:"bitrate_kbps"`
		EncodeTimeMS   any     `json:"encode_time_ms"`
		VMAFScore      any     `json:"vmaf_score"`
		TargetVMAF     float64 `json:"target_vmaf"`
		OK             bool    `json:"ok"`
		Error          string  `json:"error"`
		BisectSamples  []any   `json:"bisect_samples,omitempty"`
	}
	nan2null := func(v float64) any {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return nil
		}
		return v
	}
	wireRows := make([]wireRow, len(results))
	for i, r := range results {
		wireRows[i] = wireRow{
			Codec:          r.row.Codec,
			Adapter:        r.row.Adapter,
			RuntimeVariant: r.row.RuntimeVariant,
			FFmpegBin:      r.row.FFmpegBin,
			EncoderVersion: r.row.EncoderVersion,
			BestCRF:        r.row.BestCRF,
			BitratekBps:    nan2null(r.row.BitratekBps),
			EncodeTimeMS:   nan2null(r.row.EncodeTimeMS),
			VMAFScore:      nan2null(r.row.VMAFScore),
			TargetVMAF:     r.row.TargetVMAF,
			OK:             r.row.OK,
			Error:          r.row.Error,
			BisectSamples:  report.SanitizeBisectSamples(r.row.BisectSamples),
		}
	}
	payload := struct {
		SchemaVersion int       `json:"schema_version"`
		Src           string    `json:"src"`
		TargetVMAFs   []float64 `json:"target_vmafs"`
		ToolVersion   string    `json:"tool_version"`
		WallTimeMS    float64   `json:"wall_time_ms"`
		Rows          []wireRow `json:"rows"`
	}{
		SchemaVersion: 2,
		Src:           flags.reference,
		TargetVMAFs:   flags.targets,
		ToolVersion:   report.ToolVersion,
		WallTimeMS:    wallTimeMS,
		Rows:          wireRows,
	}
	b, err := json.MarshalIndent(payload, "", "  ")
	if err != nil {
		return "", fmt.Errorf("marshal sweep: %w", err)
	}
	return string(b) + "\n", nil
}

// emitSweepMarkdown emits a Markdown table for a multi-target sweep.
func emitSweepMarkdown(
	results []pairResult,
	flags *compareFlags,
	wallTimeMS float64,
) string {
	var sb strings.Builder
	fmt.Fprintf(&sb, "# Codec rate-quality sweep\n\n")
	fmt.Fprintf(&sb, "- Source: `%s`\n", flags.reference)
	fmt.Fprintf(&sb, "- Tool: `vmafx-tune %s` (schema v2)\n", report.ToolVersion)
	targetStrs := make([]string, len(flags.targets))
	for i, t := range flags.targets {
		targetStrs[i] = fmt.Sprintf("%g", t)
	}
	fmt.Fprintf(&sb, "- Targets: %s\n", strings.Join(targetStrs, ", "))
	fmt.Fprintf(&sb, "- Wall time: %.1f ms\n\n", wallTimeMS)
	sb.WriteString("| Codec | Encoder | Target VMAF | Best CRF | Bitrate (kbps) | " +
		"Encode time (ms) | VMAF achieved | Status |\n")
	sb.WriteString("|---|---|---:|---:|---:|---:|---:|---|\n")
	for _, r := range results {
		row := r.row
		var status string
		if row.OK {
			status = "ok"
		} else {
			status = "fail: " + row.Error
		}
		crfStr := "—"
		if row.BestCRF >= 0 {
			crfStr = fmt.Sprintf("%d", row.BestCRF)
		}
		enc := row.EncoderVersion
		if enc == "" {
			enc = "—"
		}
		bitrateStr := "—"
		if !math.IsNaN(row.BitratekBps) && row.BitratekBps > 0 {
			bitrateStr = fmt.Sprintf("%.1f", row.BitratekBps)
		}
		encTimeStr := "—"
		if !math.IsNaN(row.EncodeTimeMS) && row.EncodeTimeMS >= 0 {
			encTimeStr = fmt.Sprintf("%.1f", row.EncodeTimeMS)
		}
		vmafStr := "—"
		if !math.IsNaN(row.VMAFScore) && row.VMAFScore > 0 {
			vmafStr = fmt.Sprintf("%.2f", row.VMAFScore)
		}
		fmt.Fprintf(&sb, "| %s | %s | %g | %s | %s | %s | %s | %s |\n",
			row.Codec, enc, row.TargetVMAF, crfStr, bitrateStr, encTimeStr, vmafStr, status)
	}
	return sb.String()
}

// writeOutput writes content to path (creates/truncates) or stdout when
// path is empty.
func writeOutput(path, content string) error {
	if path == "" {
		_, err := fmt.Print(content)
		return err
	}
	// G301: 0o750 keeps directories accessible to the owner group only.
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return fmt.Errorf("create output dir: %w", err)
	}
	// G306: 0o600 — comparison reports may include path strings that leak
	// dataset identifiers; restrict to the owner.
	return os.WriteFile(path, []byte(content), 0o600)
}
