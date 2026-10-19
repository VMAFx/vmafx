// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// pkg/scorebackend/scorebackend_test.go — tests for the Go twin of the
// backend-selection half of vmaftune/score_backend.py. The report and
// selection cases come from testdata/score_backend_selection.json, which the
// Python tests replay too (ADR-1874).

package scorebackend

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"testing"
)

// sharedCases is testdata/score_backend_selection.json.
type sharedCases struct {
	Reports []struct {
		Case   string   `json:"case"`
		Stdout string   `json:"stdout"`
		Usable []string `json:"usable"`
	} `json:"reports"`
	Selections []struct {
		Prefer    string   `json:"prefer"`
		Available []string `json:"available"`
		Fallbacks []string `json:"fallbacks"`
		Want      string   `json:"want"`
		Error     string   `json:"error"`
	} `json:"selections"`
}

func loadSharedCases(t *testing.T) sharedCases {
	t.Helper()
	path := filepath.Join("..", "..", "testdata", "score_backend_selection.json")
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	var cases sharedCases
	if err := json.Unmarshal(raw, &cases); err != nil {
		t.Fatalf("parse %s: %v", path, err)
	}
	if len(cases.Reports) == 0 || len(cases.Selections) == 0 {
		t.Fatalf("%s holds no cases", path)
	}
	return cases
}

// reportRunner returns Options whose vmaf prints stdout for --list-backends
// (or fails), and records each argv it was given.
func reportRunner(stdout string, ok bool, calls *[][]string) Options {
	return Options{
		VMAFBin: "/opt/vmaf/bin/vmaf",
		Run: func(_ context.Context, name string, args ...string) (string, string, bool) {
			if calls != nil {
				*calls = append(*calls, append([]string{name}, args...))
			}
			return stdout, "unknown option", ok
		},
		Warnings: &bytes.Buffer{},
	}
}

// TestSharedReportCases replays the report cases of the shared table.
func TestSharedReportCases(t *testing.T) {
	t.Parallel()
	for _, tc := range loadSharedCases(t).Reports {
		report, err := ParseReport(tc.Stdout)
		if tc.Usable == nil {
			if err == nil {
				t.Errorf("%s: ParseReport accepted %q", tc.Case, tc.Stdout)
			}
			continue
		}
		if err != nil {
			t.Errorf("%s: ParseReport: %v", tc.Case, err)
			continue
		}
		if got := UsableBackends(report); !slices.Equal(got, tc.Usable) {
			t.Errorf("%s: usable = %v, want %v", tc.Case, got, tc.Usable)
		}
	}
}

// TestSharedSelectionCases replays the selection cases of the shared table.
func TestSharedSelectionCases(t *testing.T) {
	t.Parallel()
	for _, tc := range loadSharedCases(t).Selections {
		got, err := Select(context.Background(), tc.Prefer, Options{
			Available: tc.Available,
			Fallbacks: tc.Fallbacks,
		})
		label := fmt.Sprintf("Select(%q, available=%v, fallbacks=%v)", tc.Prefer, tc.Available,
			tc.Fallbacks)
		switch tc.Error {
		case "unavailable":
			if !IsUnavailable(err) {
				t.Errorf("%s = %q, %v; want an *UnavailableError", label, got, err)
			}
		case "unknown":
			if err == nil || IsUnavailable(err) {
				t.Errorf("%s = %q, %v; want an unknown-backend error", label, got, err)
			}
		default:
			if err != nil || got != tc.Want {
				t.Errorf("%s = %q, %v; want %q", label, got, err, tc.Want)
			}
		}
	}
}

// TestDetectRunsListBackends checks Detect asks the binary, not the host.
func TestDetectRunsListBackends(t *testing.T) {
	t.Parallel()
	cases := loadSharedCases(t)
	var calls [][]string
	got := Detect(context.Background(), reportRunner(cases.Reports[1].Stdout, true, &calls))
	if !slices.Equal(got, []string{"cpu", "cuda"}) {
		t.Errorf("Detect = %v, want [cpu cuda]", got)
	}
	if len(calls) != 1 || !slices.Equal(calls[0], []string{"/opt/vmaf/bin/vmaf", "--list-backends"}) {
		t.Errorf("runner calls = %v, want one vmaf --list-backends", calls)
	}
}

// TestCPUOnlyBuildIsNotOfferedAGPU is the defect this design replaces: a
// CPU-only vmaf on a host with GPU tools. The help text names every backend
// whatever the build, and the vendor tools answered for the host, so auto
// picked a backend the binary then refused.
func TestCPUOnlyBuildIsNotOfferedAGPU(t *testing.T) {
	t.Parallel()
	cases := loadSharedCases(t)
	opts := reportRunner(cases.Reports[0].Stdout, true, nil)
	got, err := Select(context.Background(), "auto", opts)
	if err != nil || got != "cpu" {
		t.Errorf("Select(auto) on a CPU-only build = %q, %v; want cpu", got, err)
	}
	if _, err := Select(context.Background(), "cuda", opts); !IsUnavailable(err) {
		t.Errorf("Select(cuda) on a CPU-only build: err = %v, want *UnavailableError", err)
	}
}

// TestOldBinaryYieldsCPUAndAWarning covers a vmaf without --list-backends.
func TestOldBinaryYieldsCPUAndAWarning(t *testing.T) {
	t.Parallel()
	opts := reportRunner("", false, nil)
	warnings := &bytes.Buffer{}
	opts.Warnings = warnings
	if got := Detect(context.Background(), opts); !slices.Equal(got, []string{"cpu"}) {
		t.Errorf("Detect = %v, want [cpu]", got)
	}
	if !strings.Contains(warnings.String(), "--list-backends failed") {
		t.Errorf("warning %q does not name the failed report", warnings.String())
	}
}

// TestReportSkipsMissingBinary verifies a bare binary name that is not on
// PATH is reported, not executed.
func TestReportSkipsMissingBinary(t *testing.T) {
	t.Parallel()
	var ran bool
	opts := Options{
		LookPath: func(string) (string, error) { return "", exec.ErrNotFound },
		Run: func(context.Context, string, ...string) (string, string, bool) {
			ran = true
			return "", "", true
		},
	}
	if _, err := Report(context.Background(), opts); err == nil || !strings.Contains(err.Error(), "not on PATH") {
		t.Errorf("Report error = %v, want a not-on-PATH error", err)
	}
	if ran {
		t.Error("the runner must not be invoked for a binary absent from PATH")
	}
}

// TestExplicitCPUNeedsNoReport checks cpu is answered without running vmaf.
func TestExplicitCPUNeedsNoReport(t *testing.T) {
	t.Parallel()
	var calls [][]string
	got, err := Select(context.Background(), "cpu", reportRunner("", false, &calls))
	if err != nil || got != "cpu" || len(calls) != 0 {
		t.Errorf("Select(cpu) = %q, %v with %d runs; want cpu, nil, 0", got, err, len(calls))
	}
}

// TestDeprecatedHelpParserNamesEveryBackend pins why the help text was
// dropped: it names every backend, whatever the build.
func TestDeprecatedHelpParserNamesEveryBackend(t *testing.T) {
	t.Parallel()
	help := " --backend $name:              exclusive backend selector — auto|cpu|cuda|sycl|hip|metal.\n"
	got := ParseSupportedBackends(help)
	for _, name := range AllBackends() {
		if !got[name] {
			t.Errorf("ParseSupportedBackends missed %q", name)
		}
	}
	if got := ParseSupportedBackends(""); len(got) != 1 || !got["cpu"] {
		t.Errorf("ParseSupportedBackends(\"\") = %v, want cpu only", got)
	}
}

// TestUnavailableErrorMessage checks the diagnostic names both the request
// and what the host can actually do.
func TestUnavailableErrorMessage(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		err       *UnavailableError
		wantParts []string
	}{
		{
			name: "with an available list",
			err:  &UnavailableError{Requested: "hip", Available: []string{"cpu", "cuda"}},
			wantParts: []string{
				`backend "hip" requested`, "available: cpu, cuda", "--list-backends",
			},
		},
		{
			name:      "an empty available list still reads as cpu",
			err:       &UnavailableError{Requested: "sycl"},
			wantParts: []string{`backend "sycl" requested`, "available: cpu"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			msg := tc.err.Error()
			for _, part := range tc.wantParts {
				if !strings.Contains(msg, part) {
					t.Errorf("message %q missing %q", msg, part)
				}
			}
			if !IsUnavailable(fmt.Errorf("wrapped: %w", tc.err)) {
				t.Error("IsUnavailable must see through a wrap")
			}
		})
	}

	if IsUnavailable(errors.New("unrelated")) {
		t.Error("IsUnavailable must not match an unrelated error")
	}
}

// TestAllBackendsAndFallbacksAreCopies guards the accessors against callers
// mutating the package's canonical ordering.
func TestAllBackendsAndFallbacksAreCopies(t *testing.T) {
	t.Parallel()

	all := AllBackends()
	all[0] = "mutated"
	if AllBackends()[0] != "cpu" {
		t.Error("AllBackends must return a fresh slice")
	}

	fb := DefaultFallbacks()
	fb[0] = "mutated"
	if DefaultFallbacks()[0] != "cuda" {
		t.Error("DefaultFallbacks must return a fresh slice")
	}
}

// TestOptionDefaults covers the zero-value resolution.
func TestOptionDefaults(t *testing.T) {
	t.Parallel()

	var o Options
	if o.vmafBin() != "vmaf" {
		t.Errorf("default vmafBin = %q, want vmaf", o.vmafBin())
	}
	if strings.Join(o.fallbacks(), ",") != strings.Join(DefaultFallbacks(), ",") {
		t.Errorf("default fallbacks = %v", o.fallbacks())
	}
	if o.runner() == nil || o.lookPath() == nil {
		t.Error("default runner and lookPath must not be nil")
	}
	if o.warnings() != os.Stderr {
		t.Error("default warnings must go to stderr")
	}

	explicit := Options{VMAFBin: "/opt/vmaf", Fallbacks: []string{"cpu"}}
	if explicit.vmafBin() != "/opt/vmaf" {
		t.Errorf("explicit vmafBin = %q", explicit.vmafBin())
	}
	if strings.Join(explicit.fallbacks(), ",") != "cpu" {
		t.Errorf("explicit fallbacks = %v", explicit.fallbacks())
	}
}

// TestExecRunnerReportsFailure exercises the production runner against real
// subprocesses so the exit-status plumbing is covered.
func TestExecRunnerReportsFailure(t *testing.T) {
	t.Parallel()

	if _, err := exec.LookPath("sh"); err != nil {
		t.Skip("sh not available")
	}

	stdout, _, ok := execRunner(context.Background(), "sh", "-c", "echo hello")
	if !ok {
		t.Error("a successful command must report ok")
	}
	if !strings.Contains(stdout, "hello") {
		t.Errorf("stdout = %q, want it to contain hello", stdout)
	}

	if _, _, ok := execRunner(context.Background(), "sh", "-c", "exit 3"); ok {
		t.Error("a failing command must report not-ok")
	}
	if _, _, ok := execRunner(context.Background(), "definitely-not-a-real-binary-xyz"); ok {
		t.Error("a missing binary must report not-ok")
	}
}
