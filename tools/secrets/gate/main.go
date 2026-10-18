//go:build secretsgate

// SPDX-FileCopyrightText: 2026 lusoris <lusoris@pm.me>
//
// SPDX-License-Identifier: EUPL-1.2

// Command gate is the emitted standalone secrets history scan gate.
// It executes pinned gitleaks with full history and merge commits (-m), reads
// HISS-12 exceptions from SECRETS_EXCEPTIONS, and evaluates them with standard-library rules.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

const (
	scanTimeout   = 5 * time.Minute
	defaultConfig = ".gitleaks.toml"
	logOpts       = "--full-history --all -m"

	exitPass       = 0
	exitIncomplete = 2
	exitFinding    = 3
)

func main() {
	exitCode := runGate(os.Args[1:], os.Stdout, os.Stderr)
	os.Exit(exitCode)
}

func runGate(args []string, stdout, stderr io.Writer) int {
	fs := flag.NewFlagSet("secrets-gate", flag.ContinueOnError)
	fs.SetOutput(stderr)
	repoPath := fs.String("path", ".", "Path to repository root")
	configPath := fs.String("config", "", "Path to .gitleaks.toml")
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return exitPass
		}
		return exitIncomplete
	}

	entries, err := loadEnvironmentExceptions(time.Now())
	if err != nil {
		fmt.Fprintf(stderr, "secrets gate: invalid exceptions: %v\n", err)
		return exitIncomplete
	}

	cfg := *configPath
	if cfg == "" {
		cfg = filepath.Join(*repoPath, defaultConfig)
	}

	ctx, cancel := context.WithTimeout(context.Background(), scanTimeout)
	defer cancel()

	findings, err := executeGitleaks(ctx, *repoPath, cfg)
	if err != nil {
		fmt.Fprintf(stderr, "secrets gate: %v\n", err)
		return exitIncomplete
	}

	today := Day(time.Now())
	result := Judge(findings, entries, *repoPath, today)
	return reportGateResult(result, stdout, stderr)
}

func loadEnvironmentExceptions(today time.Time) ([]Entry, error) {
	raw := os.Getenv("SECRETS_EXCEPTIONS")
	if strings.TrimSpace(raw) == "" {
		return nil, nil
	}
	decoder := json.NewDecoder(strings.NewReader(raw))
	decoder.DisallowUnknownFields()
	var entries []Entry
	if err := decoder.Decode(&entries); err != nil {
		return nil, fmt.Errorf("parse SECRETS_EXCEPTIONS JSON: %w", err)
	}
	if len(entries) > MaxJudgedEntries {
		return nil, fmt.Errorf("exceptions count %d exceeds maximum %d", len(entries), MaxJudgedEntries)
	}
	for i := 0; i < len(entries) && i < MaxJudgedEntries; i++ {
		e := entries[i]
		if prob := ValidateHISS12Target(e.Path, e.RuleID, e.Fingerprint, ""); prob != "" {
			return nil, fmt.Errorf("entry[%d]: %s", i, prob)
		}
		if prob := CategoryExpiryProblem(e.Category, e.Entered, e.Expires, today); prob != "" {
			return nil, fmt.Errorf("entry[%d]: %s", i, prob)
		}
	}
	return entries, nil
}

func executeGitleaks(ctx context.Context, dir, cfg string) (findings []Finding, err error) {
	if _, err := exec.LookPath("gitleaks"); err != nil {
		return nil, fmt.Errorf("gitleaks not found on PATH: %w", err)
	}
	reportFile, err := os.CreateTemp("", "gitleaks-gate-*.json")
	if err != nil {
		return nil, fmt.Errorf("create temp report: %w", err)
	}
	reportPath := reportFile.Name()
	if closeErr := reportFile.Close(); closeErr != nil {
		return nil, errors.Join(fmt.Errorf("close temp report: %w", closeErr), os.Remove(reportPath))
	}
	defer func() { err = errors.Join(err, os.Remove(reportPath)) }()

	cmdArgs := []string{
		"detect", "--no-banner", "--redact", "--config", cfg,
		"--log-opts=" + logOpts, "--report-format", "json", "--report-path", reportPath,
	}
	cmd := exec.CommandContext(ctx, "gitleaks", cmdArgs...)
	cmd.Dir = dir
	out, runErr := cmd.CombinedOutput()

	data, readErr := os.ReadFile(reportPath)
	if readErr != nil {
		if runErr != nil {
			return nil, fmt.Errorf("gitleaks execution failed: %w: %s", runErr, out)
		}
		return nil, fmt.Errorf("read gitleaks report: %w", readErr)
	}
	return parseGateReport(data, runErr, out)
}

func parseGateReport(data []byte, runErr error, out []byte) ([]Finding, error) {
	trimmed := strings.TrimSpace(string(data))
	if len(trimmed) == 0 {
		if runErr != nil {
			return nil, fmt.Errorf("gitleaks did not complete: %w: %s", runErr, out)
		}
		return nil, nil
	}
	var findings []Finding
	if err := json.Unmarshal(data, &findings); err != nil {
		return nil, fmt.Errorf("unmarshal report JSON: %w", err)
	}
	if len(findings) == 0 && runErr != nil {
		return nil, fmt.Errorf("gitleaks did not complete: %w: %s", runErr, out)
	}
	return findings, nil
}

func reportGateResult(res Result, stdout, stderr io.Writer) int {
	for i := 0; i < len(res.Allowed) && i < MaxJudgedFindings; i++ {
		f := res.Allowed[i]
		fmt.Fprintf(stdout, "secrets: allowed: commit %s: %s:%d [%s] (%s)\n", f.Commit, f.File, f.StartLine, f.RuleID, f.Fingerprint)
	}
	if !res.Failed() {
		summary := formatGatePassSummary(len(res.Allowed))
		fmt.Fprintf(stdout, "secrets: PASS: %s\n", summary)
		return exitPass
	}
	for i := 0; i < len(res.Blocking) && i < MaxJudgedFindings; i++ {
		f := res.Blocking[i]
		fmt.Fprintf(stderr, "secrets: finding: commit %s: %s:%d [%s] (%s)\n", f.Commit, f.File, f.StartLine, f.RuleID, f.Fingerprint)
	}
	for i := 0; i < len(res.Stale) && i < MaxJudgedEntries; i++ {
		s := res.Stale[i]
		fmt.Fprintf(stderr, "secrets: exceptions entry (path %s, rule %s): excuses no finding: remove it\n", s.Path, s.RuleID)
	}
	for i := 0; i < len(res.Expired) && i < MaxJudgedEntries; i++ {
		e := res.Expired[i]
		fmt.Fprintf(stderr, "secrets: exceptions entry (path %s, rule %s): expired on %s: remove or update it\n", e.Path, e.RuleID, e.Expires)
	}
	fmt.Fprintf(stderr, "secrets: FAIL: %s\n", formatGateFailSummary(res))
	return exitFinding
}

func formatGatePassSummary(allowedCount int) string {
	if allowedCount == 0 {
		return "zero secret findings in history"
	}
	return fmt.Sprintf("zero blocking secret findings (%d allowed by unexpired exceptions)", allowedCount)
}

func formatGateFailSummary(res Result) string {
	var parts []string
	if len(res.Blocking) > 0 {
		parts = append(parts, fmt.Sprintf("%d blocking secret finding(s)", len(res.Blocking)))
	}
	if len(res.Stale) > 0 {
		parts = append(parts, fmt.Sprintf("%d unused exception(s)", len(res.Stale)))
	}
	if len(res.Expired) > 0 {
		parts = append(parts, fmt.Sprintf("%d expired exception(s)", len(res.Expired)))
	}
	return strings.Join(parts, ", ")
}
