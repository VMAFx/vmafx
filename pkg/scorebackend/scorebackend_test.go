// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
//
// pkg/scorebackend/scorebackend_test.go — table-driven tests for the Go port
// of the backend-selection half of vmaftune/score_backend.py.

package scorebackend

import (
	"context"
	"errors"
	"fmt"
	"os/exec"
	"strings"
	"testing"
)

// fakeHost builds a Runner + LookPath pair describing a synthetic machine.
//
//	present  — binaries LookPath resolves
//	stdout   — per-binary stdout
//	failing  — binaries whose exit status is non-zero
func fakeHost(present map[string]bool, stdout map[string]string, failing map[string]bool) Options {
	return Options{
		LookPath: func(name string) (string, error) {
			if present[name] {
				return "/usr/bin/" + name, nil
			}
			return "", exec.ErrNotFound
		},
		Run: func(_ context.Context, name string, _ ...string) (string, string, bool) {
			return stdout[name], "", !failing[name]
		},
	}
}

// forkHelpText is the fork's `vmaf --help` line advertising every backend.
const forkHelpText = `
  --backend $name:              exclusive backend selector — auto|cpu|cuda|sycl|hip.
`

// TestParseSupportedBackends pins the --help alternation parser.
func TestParseSupportedBackends(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		help string
		want []string
	}{
		{
			name: "fork help advertises every backend",
			help: forkHelpText,
			want: []string{"cpu", "cuda", "sycl", "hip"},
		},
		{
			name: "empty help still yields cpu",
			help: "",
			want: []string{"cpu"},
		},
		{
			name: "upstream help without a backend line yields cpu",
			help: "usage: vmaf --reference REF --distorted DIS\n",
			want: []string{"cpu"},
		},
		{
			name: "partial build advertises only what it has",
			help: "  --backend $name: auto|cpu|cuda.\n",
			want: []string{"cpu", "cuda"},
		},
		{
			name: "prose mentioning CUDA must not be a false positive",
			help: "This build was compiled without cuda support; see docs about cuda.\n",
			want: []string{"cpu"},
		},
		{
			name: "trailing-newline delimiter matches",
			help: "auto|cpu|sycl\nmore text",
			want: []string{"cpu", "sycl"},
		},
		{
			name: "trailing-space delimiter matches",
			help: "auto|cpu|hip and so on",
			want: []string{"cpu", "hip"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := ParseSupportedBackends(tc.help)
			for _, want := range tc.want {
				if !got[want] {
					t.Errorf("backend %q missing from %v", want, got)
				}
			}
			if len(got) != len(tc.want) {
				t.Errorf("parsed %v, want exactly %v", got, tc.want)
			}
		})
	}
}

// TestDetect covers the probe matrix: the vmaf binary has to advertise the
// backend AND the hardware probe has to succeed.
func TestDetect(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name    string
		present map[string]bool
		stdout  map[string]string
		failing map[string]bool
		want    []string
	}{
		{
			name:    "no vmaf binary at all yields cpu only",
			present: map[string]bool{},
			want:    []string{"cpu"},
		},
		{
			name:    "fork build with an NVIDIA GPU",
			present: map[string]bool{"vmaf": true, "nvidia-smi": true},
			stdout: map[string]string{
				"vmaf":        forkHelpText,
				"nvidia-smi":  "GPU 0: NVIDIA GeForce RTX 4090 (UUID: GPU-abc)",
				"placeholder": "",
			},
			want: []string{"cpu", "cuda"},
		},
		{
			name:    "nvidia-smi present but reporting no GPU",
			present: map[string]bool{"vmaf": true, "nvidia-smi": true},
			stdout: map[string]string{
				"vmaf":       forkHelpText,
				"nvidia-smi": "No devices found.",
			},
			want: []string{"cpu"},
		},
		{
			name:    "nvidia-smi present but failing",
			present: map[string]bool{"vmaf": true, "nvidia-smi": true},
			stdout: map[string]string{
				"vmaf":       forkHelpText,
				"nvidia-smi": "GPU 0: NVIDIA",
			},
			failing: map[string]bool{"nvidia-smi": true},
			want:    []string{"cpu"},
		},
		{
			name:    "sycl-ls listing a GPU device",
			present: map[string]bool{"vmaf": true, "sycl-ls": true},
			stdout: map[string]string{
				"vmaf":    forkHelpText,
				"sycl-ls": "[ext_oneapi_level_zero:gpu][0] Intel(R) Arc(TM) A770",
			},
			want: []string{"cpu", "sycl"},
		},
		{
			name:    "sycl-ls listing only CPU devices",
			present: map[string]bool{"vmaf": true, "sycl-ls": true},
			stdout: map[string]string{
				"vmaf":    forkHelpText,
				"sycl-ls": "[opencl:cpu][0] Intel(R) OpenCL",
			},
			want: []string{"cpu"},
		},
		{
			name:    "rocminfo naming a gfx target",
			present: map[string]bool{"vmaf": true, "rocminfo": true},
			stdout: map[string]string{
				"vmaf":     forkHelpText,
				"rocminfo": "  Name:                    gfx1100",
			},
			want: []string{"cpu", "hip"},
		},
		{
			name:    "rocm-smi fallback when rocminfo is absent",
			present: map[string]bool{"vmaf": true, "rocm-smi": true},
			stdout: map[string]string{
				"vmaf":     forkHelpText,
				"rocm-smi": "GPU[0] : Card Series: Radeon RX 7900 XTX",
			},
			want: []string{"cpu", "hip"},
		},
		{
			name:    "every backend live, reported in canonical order",
			present: map[string]bool{"vmaf": true, "nvidia-smi": true, "sycl-ls": true, "rocminfo": true},
			stdout: map[string]string{
				"vmaf":       forkHelpText,
				"nvidia-smi": "GPU 0: NVIDIA",
				"sycl-ls":    "[opencl:gpu][0] Intel",
				"rocminfo":   "gfx1100",
			},
			want: []string{"cpu", "cuda", "sycl", "hip"},
		},
		{
			name:    "a GPU present but the vmaf build lacks the backend",
			present: map[string]bool{"vmaf": true, "nvidia-smi": true},
			stdout: map[string]string{
				"vmaf":       "usage: vmaf ...\n",
				"nvidia-smi": "GPU 0: NVIDIA",
			},
			want: []string{"cpu"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			opts := fakeHost(tc.present, tc.stdout, tc.failing)
			got := Detect(context.Background(), opts)
			if strings.Join(got, ",") != strings.Join(tc.want, ",") {
				t.Errorf("Detect = %v, want %v", got, tc.want)
			}
		})
	}
}

// TestSelect covers preference resolution, including the strict-mode failure
// that must never silently downgrade.
func TestSelect(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name       string
		prefer     string
		available  []string
		fallbacks  []string
		want       string
		wantErr    bool
		wantUnavai bool
	}{
		{
			name: "auto prefers cuda", prefer: "auto",
			available: []string{"cpu", "cuda", "sycl"}, want: "cuda",
		},
		{
			name: "auto falls through to sycl", prefer: "auto",
			available: []string{"cpu", "sycl"}, want: "sycl",
		},
		{
			name: "auto falls through to hip", prefer: "auto",
			available: []string{"cpu", "hip"}, want: "hip",
		},
		{
			name: "auto lands on cpu", prefer: "auto",
			available: []string{"cpu"}, want: "cpu",
		},
		{
			name: "auto returns cpu even when nothing is available", prefer: "auto",
			available: []string{}, want: "cpu",
		},
		{
			name: "a custom fallback chain is honoured", prefer: "auto",
			available: []string{"cpu", "cuda", "sycl"},
			fallbacks: []string{"sycl", "cuda", "cpu"}, want: "sycl",
		},
		{
			name: "an explicit available backend is honoured", prefer: "cuda",
			available: []string{"cpu", "cuda"}, want: "cuda",
		},
		{
			name: "explicit cpu is honoured", prefer: "cpu",
			available: []string{"cpu", "cuda"}, want: "cpu",
		},
		{
			name:   "an explicit unavailable backend fails rather than downgrading",
			prefer: "cuda", available: []string{"cpu"},
			wantErr: true, wantUnavai: true,
		},
		{
			name: "an unknown backend name is rejected", prefer: "vulkan",
			available: []string{"cpu"}, wantErr: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := Select(context.Background(), tc.prefer, Options{
				Available: tc.available,
				Fallbacks: tc.fallbacks,
			})
			if tc.wantErr {
				if err == nil {
					t.Fatalf("want an error, got %q", got)
				}
				if IsUnavailable(err) != tc.wantUnavai {
					t.Errorf("IsUnavailable = %v, want %v (err: %v)",
						IsUnavailable(err), tc.wantUnavai, err)
				}
				return
			}
			if err != nil {
				t.Fatalf("Select: %v", err)
			}
			if got != tc.want {
				t.Errorf("Select(%q) = %q, want %q", tc.prefer, got, tc.want)
			}
		})
	}
}

// TestSelectRunsDetectionWhenAvailableIsNil verifies the default path probes
// the host rather than assuming a list.
func TestSelectRunsDetectionWhenAvailableIsNil(t *testing.T) {
	t.Parallel()

	opts := fakeHost(
		map[string]bool{"vmaf": true, "nvidia-smi": true},
		map[string]string{"vmaf": forkHelpText, "nvidia-smi": "GPU 0: NVIDIA"},
		nil,
	)
	got, err := Select(context.Background(), "auto", opts)
	if err != nil {
		t.Fatalf("Select: %v", err)
	}
	if got != "cuda" {
		t.Errorf("Select = %q, want cuda", got)
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
				`backend "hip" requested`, "available: cpu, cuda", "runtime/driver",
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
	if o.runner() == nil {
		t.Error("default runner must not be nil")
	}
	if o.lookPath() == nil {
		t.Error("default lookPath must not be nil")
	}

	explicit := Options{VMAFBin: "/opt/vmaf", Fallbacks: []string{"cpu"}}
	if explicit.vmafBin() != "/opt/vmaf" {
		t.Errorf("explicit vmafBin = %q", explicit.vmafBin())
	}
	if strings.Join(explicit.fallbacks(), ",") != "cpu" {
		t.Errorf("explicit fallbacks = %v", explicit.fallbacks())
	}
}

// TestVMAFHelpSkipsMissingBinary verifies a bare binary name that is not on
// PATH degrades to empty help rather than executing anything.
func TestVMAFHelpSkipsMissingBinary(t *testing.T) {
	t.Parallel()

	var ran bool
	opts := Options{
		LookPath: func(string) (string, error) { return "", exec.ErrNotFound },
		Run: func(context.Context, string, ...string) (string, string, bool) {
			ran = true
			return forkHelpText, "", true
		},
	}
	if got := vmafHelp(context.Background(), opts); got != "" {
		t.Errorf("help = %q, want empty for a missing binary", got)
	}
	if ran {
		t.Error("the runner must not be invoked for a binary absent from PATH")
	}

	// An explicit path (containing a slash) skips the PATH lookup entirely.
	opts.VMAFBin = "/opt/vmaf/bin/vmaf"
	if got := vmafHelp(context.Background(), opts); !strings.Contains(got, "--backend") {
		t.Errorf("an explicit binary path must be executed; got %q", got)
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
