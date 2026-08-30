// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/root_test.go — in-package tests for the clikit (cobra+fx)
// root wiring introduced in ADR-1119 Phase-1 PR-6.
//
// Tests cover:
//   - newRoot builds the clikit root and lists every expected subcommand.
//   - The root carries the injected --version.
//   - A ported subcommand (report) runs end-to-end through the clikit-wired
//     RunE (which boots a golusoris fx graph, injects *slog.Logger / *config.Config,
//     and propagates the domain error as the command exit status).
//   - The clikit-wired RunE on report propagates a domain error (unknown format).

package cmd

import (
	"context"
	"log/slog"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/spf13/cobra"
	"go.uber.org/fx"

	"github.com/VMAFx/vmafx/internal/app/bootstrap"
)

// TestNewRoot_ListsExpectedSubcommands verifies the clikit root assembles the
// full command tree: the three ported subcommands plus the not-yet-ported
// stubs.
func TestNewRoot_ListsExpectedSubcommands(t *testing.T) {
	t.Parallel()

	root := newRoot("v9.9.9-test")
	if root == nil {
		t.Fatal("newRoot returned nil")
	}

	got := map[string]bool{}
	for _, c := range root.Cobra().Commands() {
		got[c.Name()] = true
	}

	wantPorted := []string{"compare", "ladder", "report", "auto", "sidecar"}
	wantStubs := []string{
		"tune-per-shot", "fast", "corpus", "benchmark", "encode-profile",
	}
	for _, name := range append(append([]string{}, wantPorted...), wantStubs...) {
		if !got[name] {
			t.Errorf("root is missing expected subcommand %q (have: %s)",
				name, strings.Join(sortedKeys(got), ", "))
		}
	}

	// A ported subcommand must not still carry the stub's redirect notice —
	// that is what distinguishes "implemented" from merely "registered".
	byName := map[string]*cobra.Command{}
	for _, c := range root.Cobra().Commands() {
		byName[c.Name()] = c
	}
	for _, name := range wantPorted {
		if strings.Contains(byName[name].Long, "not yet ported") {
			t.Errorf("subcommand %q is listed as ported but still has the stub Long text", name)
		}
	}
	for _, name := range wantStubs {
		if !strings.Contains(byName[name].Long, "not yet ported") {
			t.Errorf("subcommand %q is listed as a stub but has lost its redirect notice", name)
		}
	}
}

// TestNewRoot_CarriesVersion verifies the injected version reaches the cobra
// root's Version field (consumed by the built-in --version flag).
func TestNewRoot_CarriesVersion(t *testing.T) {
	t.Parallel()

	root := newRoot("v1.2.3-test")
	if got := root.Cobra().Version; got != "v1.2.3-test" {
		t.Errorf("root Version: got %q, want %q", got, "v1.2.3-test")
	}
	if root.Cobra().Use != "vmafx-tune-go" {
		t.Errorf("root Use: got %q, want %q", root.Cobra().Use, "vmafx-tune-go")
	}
}

// TestNewRoot_ReportRunsEndToEnd drives the clikit root with the "report"
// subcommand against a real compare-shaped JSON file and asserts the rendered
// Markdown lands in the requested output file. This exercises the full
// clikit -> withGolusoris (fx graph + slog/config injection) -> runReport path.
func TestNewRoot_ReportRunsEndToEnd(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	in := filepath.Join(dir, "compare.json")
	if err := os.WriteFile(in, comparePayloadJSON("clip.yuv", 90.0), 0o600); err != nil {
		t.Fatalf("write input: %v", err)
	}
	out := filepath.Join(dir, "report.md")

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{"report", in, "--format", "markdown", "--output", out})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute report via clikit root: %v", err)
	}

	data, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read rendered report: %v", err)
	}
	if !strings.Contains(string(data), "clip.yuv") {
		t.Errorf("rendered report missing source reference; got:\n%s", data)
	}
}

// TestNewRoot_ReportPropagatesError verifies that a domain error raised inside
// the clikit-wired RunE (here: an unknown --format) is surfaced as a non-nil
// Execute() error rather than swallowed by the fx lifecycle.
func TestNewRoot_ReportPropagatesError(t *testing.T) {
	t.Parallel()

	dir := t.TempDir()
	in := filepath.Join(dir, "compare.json")
	if err := os.WriteFile(in, comparePayloadJSON("clip.yuv", 90.0), 0o600); err != nil {
		t.Fatalf("write input: %v", err)
	}

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{"report", in, "--format", "pdf"})
	err := root.Execute()
	if err == nil {
		t.Fatal("expected Execute to return an error for unknown --format, got nil")
	}
	if !strings.Contains(err.Error(), "pdf") {
		t.Errorf("error should mention the bad format 'pdf', got: %v", err)
	}
}

// TestGolusorisInjection_ConfigDrivesLogLevel verifies that the VMAFX_-prefixed
// config reaches the injected *slog.Logger: with VMAFX_LOG_LEVEL=warn the
// injected logger must suppress Info but keep Warn. This is the regression guard
// for golusoris v0.5.0's native penetration (golusoris#234) — the root
// VMAFX_-prefixed config override reaches the log submodule with no decorator,
// so the graph below matches withGolusoris() exactly (no fx.Decorate).
func TestGolusorisInjection_ConfigDrivesLogLevel(t *testing.T) {
	t.Setenv("VMAFX_LOG_LEVEL", "warn")

	var d deps
	app := fx.New(
		bootstrap.Base,
		fx.Replace(configOptions()),
		fx.NopLogger,
		fx.Populate(&d.Log, &d.Cfg),
	)
	if err := app.Err(); err != nil {
		t.Fatalf("build graph: %v", err)
	}
	ctx := context.Background()
	if err := app.Start(ctx); err != nil {
		t.Fatalf("start graph: %v", err)
	}
	defer func() { _ = app.Stop(ctx) }()

	if d.Cfg.String("log.level") != "warn" {
		t.Errorf("VMAFX_LOG_LEVEL did not reach config: log.level=%q", d.Cfg.String("log.level"))
	}
	if d.Log.Enabled(ctx, slog.LevelInfo) {
		t.Error("injected logger should suppress Info at VMAFX_LOG_LEVEL=warn")
	}
	if !d.Log.Enabled(ctx, slog.LevelWarn) {
		t.Error("injected logger should still emit Warn at VMAFX_LOG_LEVEL=warn")
	}
}

func sortedKeys(m map[string]bool) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}
