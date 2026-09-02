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

	"github.com/VMAFx/vmafx/pkg/predictor"
	"github.com/VMAFx/vmafx/pkg/tune/auto"
	"github.com/VMAFx/vmafx/pkg/tune/executor"
)

// autoFlags holds flags parsed by the auto subcommand. Names and defaults
// mirror the Python `vmaf-tune auto` argparse surface exactly.
type autoFlags struct {
	src               string
	targetVMAF        float64
	maxBudgetBitrate  float64
	allowCodecs       string
	pinnedCodec       string
	sampleClipSeconds float64
	smoke             bool
	output            string
	execute           bool
	runsDir           string
	executeAll        bool
	// model is a fork-local addition over the Python surface: it selects the
	// optional predictor ONNX. The Python auto driver constructs Predictor()
	// with no model path, which is the analytical fallback — the same thing
	// this flag defaults to.
	model string
}

// newAutoCmd builds and returns the "auto" cobra subcommand.
func newAutoCmd() *cobra.Command {
	flags := &autoFlags{}

	cmd := clikit.Command("auto",
		"Phase F — adaptive recipe-aware tuning entry point (ADR-0364)",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			return runAuto(ctx, d, flags)
		})),
	)
	cmd.Long = `Compose the per-phase subcommands into one deterministic decision tree.

The tree walks ten short-circuit predicates over probed source metadata,
applies the F.4 per-content-type recipe override, estimates a CRF / VMAF /
bitrate triple per (rung, codec) cell through the predictor, and picks a
Pareto winner against the VMAF target and the bitrate budget.

Non-smoke runs probe source geometry, duration, and HDR signalling via
ffprobe. --smoke exercises the same planner deterministically with synthetic
metadata — no ffmpeg, no ffprobe, no ONNX.

The JSON plan is byte-compatible with the Python vmaf-tune auto output: the
same keys, the same sort order, and the same numeric spelling, including the
bare NaN token an uncalibrated conformal interval width produces.

Short-circuits recorded under metadata.short_circuits:
  ladder-single-rung, codec-pinned, predictor-gospel, skip-saliency,
  sdr-skip, sample-clip-propagate, skip-per-shot, low-complexity,
  baseline-meets-target, no-two-pass

Example — plan only, to stdout:
  vmafx-tune-go auto --src src.mp4 --target-vmaf 93 --allow-codecs libx264,libx265

Example — plan and realise the winner:
  vmafx-tune-go auto \
    --src src.mp4 \
    --target-vmaf 95 \
    --max-budget-bitrate 6000 \
    --execute --runs-dir runs/`

	cmd.Flags().StringVar(&flags.src, "src", "",
		"reference video (raw YUV or any FFmpeg-readable container)")
	cmd.Flags().Float64Var(&flags.targetVMAF, "target-vmaf", 93.0,
		"target pooled-mean VMAF (default 93)")
	cmd.Flags().Float64Var(&flags.maxBudgetBitrate, "max-budget-bitrate", 8000.0,
		"upper bound on the picked rendition's bitrate in kbps (default 8000)")
	cmd.Flags().StringVar(&flags.allowCodecs, "allow-codecs", "libx264",
		"comma-separated list of codecs the tree may pick from (default libx264); "+
			"a single-entry list short-circuits the compare-shortlist stage")
	cmd.Flags().StringVar(&flags.pinnedCodec, "codec", "",
		"pin the codec choice (overrides --allow-codecs ranking); "+
			"short-circuits the compare-shortlist stage")
	cmd.Flags().Float64Var(&flags.sampleClipSeconds, "sample-clip-seconds", 0.0,
		"propagate this clip length to internal sweeps rather than re-deciding "+
			"per stage (ADR-0301). 0 = full source")
	cmd.Flags().BoolVar(&flags.smoke, "smoke", false,
		"exercise the composition end-to-end with mocked sub-phases "+
			"(no ffmpeg, no ONNX); non-smoke probes source metadata")
	cmd.Flags().StringVar(&flags.output, "output", "",
		"emit the JSON plan to this path (default: stdout)")
	cmd.Flags().BoolVar(&flags.execute, "execute", false,
		"Phase F execute mode (ADR-0454): after planning, run real FFmpeg encodes "+
			"and libvmaf scores for the selected cell(s). Results are written to "+
			"--runs-dir/tune_results.jsonl. Default: plan-only")
	cmd.Flags().StringVar(&flags.runsDir, "runs-dir", "runs",
		"output directory for encoded files and tune_results.jsonl "+
			"(used with --execute; default: runs/)")
	cmd.Flags().BoolVar(&flags.executeAll, "execute-all", false,
		"with --execute: run every plan cell, not just the selected winner "+
			"(useful for post-hoc A/B comparison)")
	cmd.Flags().StringVar(&flags.model, "model", "",
		"optional predictor_<codec>.onnx path; default uses the analytical fallback")

	_ = cmd.MarkFlagRequired("src")

	return cmd
}

// parseAllowCodecs splits and trims the --allow-codecs list, dropping empty
// tokens the way the Python comprehension does.
func parseAllowCodecs(raw string) []string {
	out := make([]string, 0, 4)
	for _, token := range strings.Split(raw, ",") {
		if trimmed := strings.TrimSpace(token); trimmed != "" {
			out = append(out, trimmed)
		}
	}
	return out
}

// runAuto is the implementation of the auto subcommand.
func runAuto(ctx context.Context, d deps, flags *autoFlags) error {
	if flags.src == "" {
		return errors.New("--src is required")
	}
	allow := parseAllowCodecs(flags.allowCodecs)
	if len(allow) == 0 {
		return errors.New("--allow-codecs is empty")
	}

	pred, err := predictor.NewWithModel(ctx, flags.model, d.Log)
	if err != nil {
		return fmt.Errorf("predictor: %w", err)
	}

	d.Log.InfoContext(ctx, "planning Phase F auto run",
		"src", flags.src,
		"target_vmaf", flags.targetVMAF,
		"max_budget_kbps", flags.maxBudgetBitrate,
		"allow_codecs", allow,
		"smoke", flags.smoke)

	plan, err := auto.RunAuto(ctx, auto.Options{
		Src:               flags.src,
		TargetVMAF:        flags.targetVMAF,
		MaxBudgetKbps:     flags.maxBudgetBitrate,
		AllowCodecs:       allow,
		UserPinnedCodec:   flags.pinnedCodec,
		SampleClipSeconds: flags.sampleClipSeconds,
		Smoke:             flags.smoke,
		Predictor:         pred,
		Log:               d.Log,
	})
	if err != nil {
		return fmt.Errorf("auto: %w", err)
	}

	rendered, err := auto.EmitPlanJSON(plan)
	if err != nil {
		return fmt.Errorf("render auto plan: %w", err)
	}
	if err := writePlan(flags.output, rendered); err != nil {
		return err
	}
	if flags.output != "" {
		fmt.Fprintf(os.Stderr, "wrote auto plan -> %s\n", flags.output)
	}

	if !flags.execute {
		return nil
	}
	return executePlan(ctx, d, flags, plan)
}

// writePlan mirrors the Python emitter: the file form gets the rendered JSON
// verbatim (no trailing newline), the stdout form gets a newline appended.
func writePlan(path, rendered string) error {
	if path == "" {
		if _, err := fmt.Print(rendered); err != nil {
			return err
		}
		if !strings.HasSuffix(rendered, "\n") {
			if _, err := fmt.Print("\n"); err != nil {
				return err
			}
		}
		return nil
	}
	// G301: 0o750 keeps the output directory owner-and-group readable only.
	if err := os.MkdirAll(filepath.Dir(path), 0o750); err != nil {
		return fmt.Errorf("create output dir: %w", err)
	}
	// G306: 0o600 — the plan embeds source path strings.
	return os.WriteFile(path, []byte(rendered), 0o600)
}

// executePlan realises the plan's selected cell(s) through the executor.
//
// Exit semantics mirror the Python: a run where every executed cell failed to
// score is an error; a run with no cells to execute is a success.
func executePlan(ctx context.Context, d deps, flags *autoFlags, plan auto.Plan) error {
	params := executor.DefaultParams(flags.runsDir)
	params.ExecuteAll = flags.executeAll
	params.Log = d.Log

	d.Log.InfoContext(ctx, "auto execute mode", "runs_dir", flags.runsDir)

	results, err := executor.RunPlan(ctx, plan, flags.src, params)
	if err != nil {
		return fmt.Errorf("execute plan: %w", err)
	}
	scored := 0
	for _, r := range results {
		if r.Score != nil && r.Score.ExitStatus == 0 {
			scored++
		}
	}
	d.Log.InfoContext(ctx, "auto execute complete",
		"cells_executed", len(results),
		"cells_scored", scored,
		"results", filepath.Join(flags.runsDir, executor.ResultsFilename))

	if scored == 0 && len(results) > 0 {
		return fmt.Errorf("auto: executed %d cell(s), none scored successfully", len(results))
	}
	return nil
}
