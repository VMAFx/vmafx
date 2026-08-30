// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package cmd wires together the vmafx-tune-go subcommands via the golusoris
// clikit (cobra + fx) framework (ADR-1119 Phase-1).
//
// The root is built with clikit.New, and each subcommand with clikit.Command.
// Ported subcommands carry their domain RunE; the not-yet-ported stubs
// redirect users to the Python vmaf-tune binary. Stubs run inside a golusoris
// fx graph so their redirect notice is emitted through the injected structured
// *slog.Logger rather than a bare fmt.Fprintf.
package cmd

import (
	"context"
	"errors"
	"fmt"
	"os"

	"github.com/golusoris/golusoris/clikit"
	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/report"
)

// stubSubcommand returns a clikit-built command that logs a "not yet ported"
// notice through the injected golusoris logger and exits non-zero when invoked.
func stubSubcommand(name, shortDesc string) *cobra.Command {
	cmd := clikit.Command(name, shortDesc+" [not yet ported — use vmaf-tune "+name+"]",
		clikit.WithRunE(withGolusoris(func(ctx context.Context, d deps, _ []string) error {
			d.Log.WarnContext(ctx,
				"subcommand not yet ported to vmafx-tune-go; use the Python vmaf-tune binary",
				"subcommand", name,
				"redirect", "vmaf-tune "+name)
			return fmt.Errorf("subcommand %q not yet ported", name)
		})),
	)
	cmd.Long = fmt.Sprintf(`%s is not yet ported to vmafx-tune-go.

Use the Python vmaf-tune binary for this subcommand:
  vmaf-tune %s [flags...]

It will be ported in a future Stage-2 release.`, shortDesc, name)
	return cmd
}

// newRoot builds the clikit root command with all subcommands attached. It is
// factored out of Execute so tests can assemble the command tree without
// running it.
func newRoot(version string) *clikit.Root {
	root := clikit.New("vmafx-tune-go",
		"vmafx-tune-go — Go port of the vmaf-tune rate-quality tuning CLI")
	root.Cobra().Long = `vmafx-tune-go is the Go port of the vmaf-tune rate-quality tuning CLI.
It runs alongside the Python vmaf-tune binary during the migration.

Fully ported subcommands:
  compare              Rate-quality sweep: compare codecs at VMAF targets
  ladder               Per-title ABR bitrate-ladder generation
  report               Render Markdown / HTML from prior compare or ladder runs
  recommend            Pick the CRF meeting a VMAF or bitrate target
  predict              Predict per-shot VMAF, then verify on K real encodes
  recommend-saliency   Saliency-aware ROI encode
  prefilter            Joint TPE autotune over deband strengths + CRF
  tune-per-shot        Per-shot CRF tuning: shot detection + bisect + plan

Not yet ported (use 'vmaf-tune <subcommand>' for these):
  fast, corpus, benchmark, auto, sidecar, encode-profile`
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

	// Not-yet-ported stubs: log a redirect rather than silently failing.
	for _, stub := range []struct{ name, desc string }{
		{"fast", "Fast NR-proxy accelerated tune"},
		{"corpus", "Corpus management"},
		{"benchmark", "Encoder benchmark"},
		{"auto", "Automatic subcommand selection"},
		{"sidecar", "Sidecar state management"},
		{"encode-profile", "Encoder profile inspection"},
	} {
		root.AddCommand(stubSubcommand(stub.name, stub.desc))
	}

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
// It exits non-zero on error (cobra has already printed the message),
// honouring a subcommand's requested exit status when it supplies one.
func Execute(version string) {
	report.ToolVersion = version

	if err := newRoot(version).Execute(); err != nil {
		var coder exitCoder
		if errors.As(err, &coder) {
			os.Exit(coder.ExitCode())
		}
		if errors.Is(err, errFallBackVerdict) {
			os.Exit(2)
		}
		os.Exit(1)
	}
}
