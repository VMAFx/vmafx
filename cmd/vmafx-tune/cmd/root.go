// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package cmd wires together the vmafx-tune-go subcommands via the golusoris
// clikit (cobra + fx) framework (ADR-1119).
//
// The root is built with clikit.New, and each subcommand with clikit.Command.
package cmd

import (
	"errors"
	"os"

	"github.com/golusoris/golusoris/clikit"

	"github.com/VMAFx/vmafx/pkg/report"
)

// newRoot builds the clikit root command with all subcommands attached. It is
// factored out of Execute so tests can assemble the command tree without
// running it.
func newRoot(version string) *clikit.Root {
	root := clikit.New("vmafx-tune-go",
		"vmafx-tune-go — Go port of the vmaf-tune rate-quality tuning CLI")
	root.Cobra().Long = `vmafx-tune-go is the Go port of the vmaf-tune rate-quality tuning CLI.

Subcommands:
  compare              Rate-quality sweep: compare codecs at VMAF targets
  ladder               Per-title ABR bitrate-ladder generation
  report               Render Markdown / HTML from prior compare or ladder runs
  recommend            Pick the CRF meeting a VMAF or bitrate target
  predict              Predict per-shot VMAF, then verify on K real encodes
  recommend-saliency   Saliency-aware ROI encode
  prefilter            Joint TPE autotune over deband strengths + CRF
  tune-per-shot        Per-shot CRF tuning: shot detection + bisect + plan
  fast                 NR-proxy accelerated tune with conformal intervals
  corpus               Phase A (preset, crf) grid sweep -> JSONL corpus
  sidecar              Local on-host predictor sidecar (status / predict /
                       record / batch-record)
  benchmark            Encoder benchmark sweep
  encode-profile       Encoder profile inspection
  auto                 Pick and run the right tuning subcommand`
	root.Cobra().Version = version

	// Ported subcommands.
	root.AddCommand(newCompareCmd())
	root.AddCommand(newLadderCmd())
	root.AddCommand(newReportCmd())
	root.AddCommand(newRecommendCmd())
	root.AddCommand(newPredictCmd())
	root.AddCommand(newRecommendSaliencyCmd())
	root.AddCommand(newPrefilterCmd())
	root.AddCommand(newPerShotCmd())
	root.AddCommand(newFastCmd())
	root.AddCommand(newCorpusCmd())
	root.AddCommand(newSidecarCmd())
	root.AddCommand(newBenchmarkCmd())
	root.AddCommand(newEncodeProfileCmd())
	root.AddCommand(newAutoCmd())

	return root
}

// exitCoder lets a subcommand choose a specific process exit status.
//
// The Python CLI distinguishes exit 2 (a requested-but-unavailable feature:
// no Pelorus filter, no ROI dispatch for the encoder, a bad CRF range) from
// exit 1 (a plain failure), and `predict` uses exit 2 for a FALL_BACK
// verdict. Scripts branch on those codes, so the Go port preserves them.
type exitCoder interface {
	ExitCode() int
}

// Execute builds the clikit root, wires all subcommands, and runs the CLI.
//
// Cobra maps any RunE error to exit 1. Subcommands that carry their own exit
// contract — `fast` distinguishes usage errors (2) from an out-of-distribution
// recommendation (3), matching `vmaf-tune fast` — attach the intended status
// to the error, and Execute honours it. Everything else keeps exit 1.
func Execute(version string) {
	report.ToolVersion = version

	// Most subcommands exit 1 on failure; encode-profile propagates FFmpeg's
	// own status instead (see exitcode.go).
	if err := newRoot(version).Execute(); err != nil {
		// Match our OWN error type, not the exitCoder interface. *exec.ExitError
		// also satisfies exitCoder (via the embedded *os.ProcessState), so a bare
		// interface match reports a failed ffmpeg/vmaf child's exit status as
		// vmafx-tune's own -- e.g. ffmpeg exiting 42 made the CLI exit 42, which
		// collides with the documented usage/verdict codes. Both construction
		// styles are in the tree, so check pointer and value forms.
		var codePtr *exitCodeError
		if errors.As(err, &codePtr) {
			os.Exit(codePtr.ExitCode())
		}
		var codeVal exitCodeError
		if errors.As(err, &codeVal) {
			os.Exit(codeVal.ExitCode())
		}
		if code, ok := fastExitCode(err); ok {
			os.Exit(code)
		}
		if errors.Is(err, errFallBackVerdict) {
			os.Exit(2)
		}
		// exitCodeOf recognises group 4's exitCodeError and otherwise
		// returns 1, so it composes with the checks above.
		os.Exit(exitCodeOf(err))
	}
}
