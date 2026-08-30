// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package cmd wires together the vmafx-tune-go subcommands via the golusoris
// clikit (cobra + fx) framework (ADR-1119 Phase-1).
//
// The root is built with clikit.New, and each subcommand with clikit.Command.
// Ported subcommands (compare, ladder, report, auto, sidecar) carry their
// domain RunE; the not-yet-ported stubs redirect users to the Python vmaf-tune
// binary. Stubs run inside a golusoris fx graph so their redirect notice is
// emitted through the injected structured *slog.Logger rather than a bare
// fmt.Fprintf.
package cmd

import (
	"context"
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
		"vmafx-tune-go — Go port of vmaf-tune (compare/ladder/report/auto/sidecar)")
	root.Cobra().Long = `vmafx-tune-go is the Go port of the vmaf-tune rate-quality tuning CLI.
It runs alongside the Python vmaf-tune binary during the migration.

Fully ported subcommands:
  compare     Rate-quality sweep: compare codecs at VMAF targets
  ladder      Per-title ABR bitrate-ladder generation
  report      Render Markdown / HTML from prior compare or ladder runs
  auto        Phase F adaptive recipe-aware planner (plan + optional execute)
  sidecar     Local on-host predictor sidecar (status/predict/record/batch)

Not yet ported (use 'vmaf-tune <subcommand>' for these):
  tune-per-shot, fast, corpus, benchmark, encode-profile`
	root.Cobra().Version = version

	// Ported subcommands.
	root.AddCommand(newCompareCmd())
	root.AddCommand(newLadderCmd())
	root.AddCommand(newReportCmd())
	root.AddCommand(newAutoCmd())
	root.AddCommand(newSidecarCmd())

	// Not-yet-ported stubs: log a redirect rather than silently failing.
	for _, stub := range []struct{ name, desc string }{
		{"tune-per-shot", "Per-shot VMAF tuning"},
		{"fast", "Fast NR-proxy accelerated tune"},
		{"corpus", "Corpus management"},
		{"benchmark", "Encoder benchmark"},
		{"encode-profile", "Encoder profile inspection"},
	} {
		root.AddCommand(stubSubcommand(stub.name, stub.desc))
	}

	return root
}

// Execute builds the clikit root, wires all subcommands, and runs the CLI.
// It calls os.Exit(1) on error (cobra has already printed the message).
func Execute(version string) {
	report.ToolVersion = version

	if err := newRoot(version).Execute(); err != nil {
		os.Exit(1)
	}
}
