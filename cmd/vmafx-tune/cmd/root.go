// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

// Package cmd wires together the vmafx-tune-go subcommands via cobra.
//
// Stage-1 ships one fully-functional subcommand ("compare") and stubs
// for the remaining Python vmaf-tune subcommands so the binary is
// discoverable as an upgrade path. The stubs print a redirect message
// pointing users at the Python binary until Stage-2 ports them.
package cmd

import (
	"fmt"
	"os"

	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/report"
)

// stubSubcommand returns a cobra.Command that prints a "not yet ported"
// message and exits 1 when invoked.
func stubSubcommand(name, shortDesc string) *cobra.Command {
	return &cobra.Command{
		Use:   name,
		Short: shortDesc + " [not yet ported — use vmaf-tune " + name + "]",
		Long: fmt.Sprintf(`%s is not yet ported to vmafx-tune-go.

Use the Python vmaf-tune binary for this subcommand:
  vmaf-tune %s [flags...]

It will be ported in a future Stage-2 release.`, shortDesc, name),
		RunE: func(cmd *cobra.Command, args []string) error {
			fmt.Fprintf(os.Stderr,
				"vmafx-tune-go: %q is not yet ported. Use 'vmaf-tune %s' instead.\n",
				name, name)
			return fmt.Errorf("subcommand %q not yet ported", name)
		},
		// SilenceUsage prevents cobra from printing the usage string on error,
		// since the error is intentional ("not ported").
		SilenceUsage: true,
	}
}

// Execute builds the root command, adds all subcommands, and runs cobra.
// It calls os.Exit on error.
func Execute(version string) {
	report.ToolVersion = version

	root := &cobra.Command{
		Use:   "vmafx-tune-go",
		Short: "vmafx-tune-go — Go port of vmaf-tune (Stage 4: compare/ladder/report subcommands)",
		Long: `vmafx-tune-go is the Go port of the vmaf-tune rate-quality tuning CLI.
It runs alongside the Python vmaf-tune binary during the migration.

Fully ported subcommands:
  compare     Rate-quality sweep: compare codecs at VMAF targets (Stage 1)
  ladder      Per-title ABR bitrate-ladder generation (Stage 2)
  report      Render Markdown / HTML from prior compare or ladder runs (Stage 4)

Not yet ported (use 'vmaf-tune <subcommand>' for these):
  tune-per-shot, fast, corpus, benchmark, auto, sidecar`,
		Version: version,
		// Do not print usage on subcommand errors.
		SilenceUsage: true,
	}

	// Ported subcommands (Stages 1, 2, 4).
	root.AddCommand(newCompareCmd())
	root.AddCommand(newLadderCmd())
	root.AddCommand(newReportCmd())

	// Stage-5+ stubs: print redirect rather than silently failing.
	for _, stub := range []struct{ name, desc string }{
		{"tune-per-shot", "Per-shot VMAF tuning"},
		{"fast", "Fast NR-proxy accelerated tune"},
		{"corpus", "Corpus management"},
		{"benchmark", "Encoder benchmark"},
		{"auto", "Automatic subcommand selection"},
		{"sidecar", "Sidecar state management"},
		{"encode-profile", "Encoder profile inspection"},
	} {
		root.AddCommand(stubSubcommand(stub.name, stub.desc))
	}

	if err := root.Execute(); err != nil {
		os.Exit(1)
	}
}
