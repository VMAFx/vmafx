// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT

package cmd

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"

	"github.com/spf13/cobra"

	"github.com/VMAFx/vmafx/pkg/report"
)

// reportFlags holds flags parsed by the report subcommand.
type reportFlags struct {
	inputs []string
	output string
	format string
}

// newReportCmd builds and returns the "report" cobra subcommand.
//
// Stage-4 scope (ADR-0770):
//   - Read one or more JSON files produced by "compare" or "ladder" runs.
//   - Render Markdown (default) or HTML.
//   - HTML is a self-contained page (no external CDN dependencies).
func newReportCmd() *cobra.Command {
	flags := &reportFlags{}

	cmd := &cobra.Command{
		Use:   "report <input.json> [input2.json ...]",
		Short: "Render Markdown or HTML report from prior compare / ladder run outputs",
		Long: `Generate a human-readable report from one or more vmafx-tune JSON output files.

The JSON files are produced by the "compare" or "ladder" subcommands.
Multiple inputs are merged into a single report, sorted by tool_version and src.

Supported output formats:
  markdown  (default) — GitHub-flavoured Markdown table
  html      — self-contained HTML page (no external dependencies)

Example — Markdown to stdout:
  vmafx-tune-go report results.json

Example — HTML to file:
  vmafx-tune-go report results.json ladder.json \
    --format html \
    --output report.html

Stage-4 scope (ADR-0770): Markdown and HTML. PDF and JSON-diff modes are
Stage-5 scope.`,
		Args: func(cmd *cobra.Command, args []string) error {
			if len(args) == 0 {
				return errors.New("at least one input JSON file is required")
			}
			return nil
		},
		RunE: func(cmd *cobra.Command, args []string) error {
			flags.inputs = args
			return runReport(cmd.Context(), flags)
		},
	}

	cmd.Flags().StringVarP(&flags.output, "output", "o", "",
		"Output file path (default: stdout)")
	cmd.Flags().StringVar(&flags.format, "format", "markdown",
		"Output format: markdown or html")

	return cmd
}

// runReport is the implementation of the report subcommand.
func runReport(_ context.Context, flags *reportFlags) error {
	format := strings.ToLower(strings.TrimSpace(flags.format))
	if format != "markdown" && format != "html" {
		return fmt.Errorf("unknown --format %q; supported: markdown, html", flags.format)
	}

	// Load all input files.
	var loaded []report.MultiReport
	for _, path := range flags.inputs {
		mr, err := loadReportFile(path)
		if err != nil {
			return fmt.Errorf("load %q: %w", path, err)
		}
		loaded = append(loaded, mr)
	}

	// Render.
	var output string
	switch format {
	case "markdown":
		output = report.RenderMarkdownMulti(loaded)
	case "html":
		output = report.RenderHTMLMulti(loaded)
	}

	return writeOutput(flags.output, output)
}

// loadReportFile reads a JSON file and unmarshals it into a MultiReport.
// It accepts both "compare" (single-Report) and "ladder" (LadderResult) schemas
// by probing the top-level JSON keys.
func loadReportFile(path string) (report.MultiReport, error) {
	data, err := os.ReadFile(path) // #nosec G304 -- path comes from a CLI flag validated by cobra, not user-controlled HTTP input
	if err != nil {
		return report.MultiReport{}, fmt.Errorf("read: %w", err)
	}

	// Probe schema: ladder payloads have "renditions", compare payloads have "rows".
	var probe struct {
		Renditions json.RawMessage `json:"renditions"`
		Rows       json.RawMessage `json:"rows"`
	}
	if err := json.Unmarshal(data, &probe); err != nil {
		return report.MultiReport{}, fmt.Errorf("parse JSON: %w", err)
	}

	if probe.Renditions != nil {
		// Ladder schema.
		var lr report.LadderWirePayload
		if err := json.Unmarshal(data, &lr); err != nil {
			return report.MultiReport{}, fmt.Errorf("parse ladder JSON: %w", err)
		}
		return report.MultiReportFromLadder(lr, path), nil
	}

	// Compare schema (v1 single-target or v2 sweep).
	var cr report.CompareWirePayload
	if err := json.Unmarshal(data, &cr); err != nil {
		return report.MultiReport{}, fmt.Errorf("parse compare JSON: %w", err)
	}
	return report.MultiReportFromCompare(cr, path), nil
}
