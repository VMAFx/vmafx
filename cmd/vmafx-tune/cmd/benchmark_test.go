// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// cmd/vmafx-tune/cmd/benchmark_test.go — unit tests for the "benchmark"
// subcommand.
//
// Tests cover:
//   - Flag surface matches `vmaf-tune benchmark` name-for-name.
//   - Happy path: each --format renders through the clikit-wired root.
//   - --output writes the report to a file and creates the parent directory.
//   - Sad path: unknown --format, missing corpus file, empty corpus, absent
//     baseline encoder.
//
// ADR-0770: vmafx-tune Stage 4 — benchmark subcommand.

package cmd

import (
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// corpusJSONL is a two-encoder Phase-A corpus: libx264 clears a 92 target at
// 3100.5 kbps, libx265 clears it more cheaply at 2500, and libsvtav1 never
// clears.
const corpusJSONL = `{"encoder": "libx264", "src": "a.mkv", "preset": "medium", "crf": 26, "vmaf_score": 92.1, "bitrate_kbps": 3100.5, "exit_status": 0, "duration_s": 10.0, "encode_time_ms": 4800.0, "score_time_ms": 2400.0, "framerate": 30.0}
{"encoder": "libx265", "src": "a.mkv", "preset": "medium", "crf": 28, "vmaf_score": 92.0, "bitrate_kbps": 2500.0, "exit_status": 0, "duration_s": 10.0, "encode_time_ms": 15000.0, "score_time_ms": 2500.0, "framerate": 30.0}
{"encoder": "libsvtav1", "src": "a.mkv", "preset": "medium", "crf": 35, "vmaf_score": 88.4, "bitrate_kbps": 1500.0, "exit_status": 0, "duration_s": 10.0, "encode_time_ms": 7000.0, "score_time_ms": 2500.0, "framerate": 30.0}
`

// writeCorpus writes a corpus fixture into a temp dir and returns its path.
// writeCorpusRaw writes corpus content verbatim. Distinct from writeCorpus in
// recommend_cli_test.go, which joins variadic lines and appends a trailing
// newline; this one must not touch the bytes.
func writeCorpusRaw(t *testing.T, content string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "corpus.jsonl")
	if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
		t.Fatalf("write corpus: %v", err)
	}
	return path
}

// TestBenchmarkFlagSurface pins the flag names against the Python CLI. A
// renamed flag silently breaks every script and CI job that calls the command.
func TestBenchmarkFlagSurface(t *testing.T) {
	t.Parallel()

	cmd := newBenchmarkCmd()

	tests := []struct {
		flag    string
		wantDef string
	}{
		{"from-corpus", ""},
		{"target-vmaf", "92"},
		{"baseline-encoder", ""},
		{"format", "markdown"},
		{"output", ""},
	}

	for _, tt := range tests {
		t.Run(tt.flag, func(t *testing.T) {
			t.Parallel()
			f := cmd.Flags().Lookup(tt.flag)
			if f == nil {
				t.Fatalf("--%s is missing", tt.flag)
			}
			if f.DefValue != tt.wantDef {
				t.Errorf("--%s default = %q, want %q", tt.flag, f.DefValue, tt.wantDef)
			}
		})
	}
}

// TestBenchmarkRendersEachFormat drives the full clikit root (fx graph, logger
// injection, domain RunE) once per output format.
func TestBenchmarkRendersEachFormat(t *testing.T) {
	corpus := writeCorpusRaw(t, corpusJSONL)

	tests := []struct {
		name     string
		format   string
		wantSubs []string
	}{
		{
			name:   "markdown",
			format: "markdown",
			wantSubs: []string{
				"| Encoder | Status | VMAF |",
				"| libx265 | ok |",
				"| libsvtav1 | unmet |",
			},
		},
		{
			name:   "json",
			format: "json",
			wantSubs: []string{
				`"encoder": "libx265"`,
				`"status": "ok"`,
				// CPython's repr keeps the trailing ".0" that Go's default
				// float formatting would drop.
				`"target_vmaf": 92.0`,
			},
		},
		{
			name:   "csv",
			format: "csv",
			wantSubs: []string{
				"encoder,status,target_vmaf",
				"libx265,ok,92.000",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			out := filepath.Join(t.TempDir(), "report.out")
			root := newRoot("dev")
			root.Cobra().SetArgs([]string{
				"benchmark", "--from-corpus", corpus,
				"--format", tt.format, "--output", out,
			})
			if err := root.Execute(); err != nil {
				t.Fatalf("execute benchmark: %v", err)
			}

			data, err := os.ReadFile(out)
			if err != nil {
				t.Fatalf("read report: %v", err)
			}
			for _, want := range tt.wantSubs {
				if !strings.Contains(string(data), want) {
					t.Errorf("report missing %q; got:\n%s", want, data)
				}
			}
		})
	}
}

// TestBenchmarkCreatesOutputDirectory verifies the report lands even when the
// requested parent directory does not exist yet.
func TestBenchmarkCreatesOutputDirectory(t *testing.T) {
	corpus := writeCorpusRaw(t, corpusJSONL)
	out := filepath.Join(t.TempDir(), "nested", "deeper", "report.md")

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{"benchmark", "--from-corpus", corpus, "--output", out})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute benchmark: %v", err)
	}
	if _, err := os.Stat(out); err != nil {
		t.Fatalf("report was not written: %v", err)
	}
}

// TestBenchmarkBaselineEncoder verifies that pinning the baseline reassigns
// which encoder carries the zero delta.
func TestBenchmarkBaselineEncoder(t *testing.T) {
	corpus := writeCorpusRaw(t, corpusJSONL)
	out := filepath.Join(t.TempDir(), "report.csv")

	root := newRoot("dev")
	root.Cobra().SetArgs([]string{
		"benchmark", "--from-corpus", corpus,
		"--baseline-encoder", "libx264", "--format", "csv", "--output", out,
	})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute benchmark: %v", err)
	}

	data, err := os.ReadFile(out)
	if err != nil {
		t.Fatalf("read report: %v", err)
	}
	// libx264 is the baseline, so its delta is 0.000; libx265 is cheaper, so
	// its delta is negative.
	if !strings.Contains(string(data), "libx264,ok,92.000,92.100,0.100,3100.500,0.000,") {
		t.Errorf("libx264 is not carrying the zero baseline delta; got:\n%s", data)
	}
	if !strings.Contains(string(data), ",-19.368,") {
		t.Errorf("libx265 delta against the pinned baseline is missing; got:\n%s", data)
	}
}

func TestBenchmarkErrors(t *testing.T) {
	corpus := writeCorpusRaw(t, corpusJSONL)
	// A corpus whose only row failed: nothing is eligible.
	emptyCorpus := writeCorpus(t,
		`{"encoder": "libx264", "vmaf_score": 90.0, "bitrate_kbps": 100.0, "exit_status": 1}`+"\n")

	tests := []struct {
		name    string
		args    []string
		wantSub string
	}{
		{
			name:    "unknown format",
			args:    []string{"benchmark", "--from-corpus", corpus, "--format", "pdf"},
			wantSub: "unknown --format",
		},
		{
			name:    "missing corpus file",
			args:    []string{"benchmark", "--from-corpus", "/nonexistent/corpus.jsonl"},
			wantSub: "corpus file not found",
		},
		{
			name:    "no eligible rows",
			args:    []string{"benchmark", "--from-corpus", emptyCorpus},
			wantSub: "no successful finite corpus rows",
		},
		{
			name: "baseline encoder absent from corpus",
			args: []string{
				"benchmark", "--from-corpus", corpus, "--baseline-encoder", "libtheora",
			},
			wantSub: "not present in corpus",
		},
		{
			name:    "missing required flag",
			args:    []string{"benchmark"},
			wantSub: "from-corpus",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			root := newRoot("dev")
			root.Cobra().SetArgs(tt.args)
			root.Cobra().SetOut(io.Discard)
			root.Cobra().SetErr(io.Discard)
			err := root.Execute()
			if err == nil {
				t.Fatal("expected an error, got nil")
			}
			if !strings.Contains(err.Error(), tt.wantSub) {
				t.Errorf("error %q does not contain %q", err, tt.wantSub)
			}
		})
	}
}
