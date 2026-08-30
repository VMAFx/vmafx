// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"errors"
	"fmt"
	"os"
	"slices"
	"strings"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/benchmark"
)

// benchmarkFlags holds flags parsed by the benchmark subcommand. The names
// mirror `vmaf-tune benchmark` one-for-one.
type benchmarkFlags struct {
	fromCorpus      string
	targetVMAF      float64
	baselineEncoder string
	format          string
	output          string
}

// newBenchmarkCmd builds and returns the "benchmark" cobra subcommand.
//
// Phase G (ADR-0770): rank encoders from an existing corpus JSONL at a matched
// target VMAF. It launches no ffmpeg and no libvmaf — the corpus stays the
// source of truth — so the whole subcommand is a pure data transformation and
// ports to Go without any runtime dependency.
func newBenchmarkCmd() *cobra.Command {
	flags := &benchmarkFlags{}

	cmd := clikit.Command("benchmark",
		"Rank encoders from an existing corpus JSONL at a matched target VMAF",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			// Validation failures exit 2, matching `vmaf-tune benchmark`.
			return asUsageError(runBenchmark(ctx, d, flags))
		})),
	)
	cmd.Long = `Rank encoders from an existing Phase-A corpus JSONL at a matched target
VMAF, without running new encodes.

For every encoder in the corpus the report picks the lowest-bitrate row whose
measured VMAF still clears --target-vmaf. An encoder that never clears is
reported as "unmet" with its closest miss, so a missing encoder build is not
mistaken for a quality result.

Bitrate deltas are measured against --baseline-encoder, defaulting to the
lowest-bitrate encoder that cleared the target.

Example — Markdown to stdout:
  vmafx-tune-go benchmark --from-corpus corpus.jsonl

Example — CSV at a stricter target, pinned baseline:
  vmafx-tune-go benchmark \
    --from-corpus corpus.jsonl \
    --target-vmaf 95 \
    --baseline-encoder libx264 \
    --format csv \
    --output benchmark.csv`

	cmd.Flags().StringVar(&flags.fromCorpus, "from-corpus", "",
		"Phase-A corpus JSONL to benchmark (required)")
	cmd.Flags().Float64Var(&flags.targetVMAF, "target-vmaf", 92.0,
		"Matched-quality threshold each encoder must clear")
	cmd.Flags().StringVar(&flags.baselineEncoder, "baseline-encoder", "",
		"Encoder used for bitrate-delta percentages "+
			"(default: lowest-bitrate encoder that clears the target)")
	cmd.Flags().StringVar(&flags.format, "format", "markdown",
		"Report format: markdown, json or csv")
	cmd.Flags().StringVarP(&flags.output, "output", "o", "",
		"Report destination (default: stdout)")

	// Required-flag enforcement lives in runBenchmark, not MarkFlagRequired,
	// so a missing flag exits 2 like the Python CLI. See useUsageExitCode.
	useUsageExitCode(cmd)

	return cmd
}

// benchmarkFormats is the accepted --format set, in the order the help text
// lists them.
var benchmarkFormats = []string{"markdown", "json", "csv"}

// runBenchmark is the implementation of the benchmark subcommand.
func runBenchmark(ctx context.Context, d deps, flags *benchmarkFlags) error {
	if flags.fromCorpus == "" {
		return errors.New("--from-corpus is required")
	}
	format := strings.ToLower(strings.TrimSpace(flags.format))
	if !slices.Contains(benchmarkFormats, format) {
		return fmt.Errorf("unknown --format %q; supported: %s",
			flags.format, strings.Join(benchmarkFormats, ", "))
	}
	if _, err := os.Stat(flags.fromCorpus); err != nil {
		return fmt.Errorf("corpus file not found: %s", flags.fromCorpus)
	}

	rows, err := benchmark.LoadCorpusJSONL(flags.fromCorpus)
	if err != nil {
		return fmt.Errorf("read corpus %q: %w", flags.fromCorpus, err)
	}

	d.Log.InfoContext(ctx, "benchmarking corpus",
		"corpus", flags.fromCorpus,
		"rows", len(rows),
		"target_vmaf", flags.targetVMAF,
		"baseline_encoder", flags.baselineEncoder,
		"format", format)

	summaries, err := benchmark.Summarize(rows, flags.targetVMAF, flags.baselineEncoder)
	if err != nil {
		return err
	}
	rendered, err := benchmark.Render(summaries, format)
	if err != nil {
		return err
	}

	d.Log.InfoContext(ctx, "benchmark complete",
		"encoders", len(summaries),
		"output", outputTarget(flags.output))

	return writeOutput(flags.output, rendered)
}
