// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/backend_test.go — scoring-backend selection tests.
//
// The alternation-parse cases mirror the shapes vmaftune.score_backend's
// parse_supported_backends accepts, and the selection cases pin the strict
// "never silently downgrade" contract from ADR-0299 / ADR-0314.

package corpus

import (
	"context"
	"errors"
	"reflect"
	"sort"
	"testing"
)

func TestParseSupportedBackends(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		help string
		want []string
	}{
		{
			name: "the fork's help line alternation",
			help: "  --backend $name:  exclusive backend selector — auto|cpu|cuda|sycl|hip.\n",
			want: []string{"cpu", "cuda", "hip", "sycl"},
		},
		{
			name: "a CPU-only build",
			help: "  --backend $name:  exclusive backend selector — auto|cpu.\n",
			want: []string{"cpu"},
		},
		{
			name: "cpu is added even when the help line is missing",
			help: "vmaf 3.0.0\n",
			want: []string{"cpu"},
		},
		{
			// The token match requires a leading pipe, so prose mentioning
			// a backend name does not create a false positive.
			name: "prose mentioning cuda does not advertise support",
			help: "This build has no CUDA support; see docs for cuda builds.\n",
			want: []string{"cpu"},
		},
		{
			name: "a trailing-space alternation still matches",
			help: "--backend $name: auto|cpu|cuda more text\n",
			want: []string{"cpu", "cuda"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			set := ParseSupportedBackends(tc.help)
			got := make([]string, 0, len(set))
			for k, ok := range set {
				if ok {
					got = append(got, k)
				}
			}
			sort.Strings(got)
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("ParseSupportedBackends() = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestSelectBackend(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		prefer    string
		available []string
		want      string
		wantErr   bool
	}{
		{
			name: "auto prefers cuda", prefer: "auto",
			available: []string{"cpu", "cuda", "sycl"}, want: "cuda",
		},
		{
			name: "auto falls to sycl without cuda", prefer: "auto",
			available: []string{"cpu", "sycl", "hip"}, want: "sycl",
		},
		{
			name: "auto falls to hip without cuda or sycl", prefer: "auto",
			available: []string{"cpu", "hip"}, want: "hip",
		},
		{
			name: "auto ends at cpu", prefer: "auto",
			available: []string{"cpu"}, want: "cpu",
		},
		{
			// cpu is universally available even if every probe failed.
			name: "auto with nothing available still answers cpu", prefer: "auto",
			available: []string{}, want: "cpu",
		},
		{
			name: "an available explicit backend is honoured", prefer: "sycl",
			available: []string{"cpu", "sycl"}, want: "sycl",
		},
		{
			// Silently downgrading would mask a hardware / build mismatch
			// and lie about wall-clock expectations.
			name: "an unavailable explicit backend is refused", prefer: "cuda",
			available: []string{"cpu"}, wantErr: true,
		},
		{
			name: "an unknown backend name is refused", prefer: "vulkan",
			available: []string{"cpu"}, wantErr: true,
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := SelectBackend(context.Background(), tc.prefer, nil,
				tc.available, "vmaf", nil)
			if (err != nil) != tc.wantErr {
				t.Fatalf("SelectBackend error = %v, wantErr = %v", err, tc.wantErr)
			}
			if tc.wantErr {
				return
			}
			if got != tc.want {
				t.Errorf("SelectBackend(%q) = %q, want %q", tc.prefer, got, tc.want)
			}
		})
	}
}

func TestSelectBackendUnavailableErrorIsTyped(t *testing.T) {
	t.Parallel()

	_, err := SelectBackend(context.Background(), "hip", nil, []string{"cpu"}, "vmaf", nil)
	var unavailable *BackendUnavailableError
	if !errors.As(err, &unavailable) {
		t.Fatalf("SelectBackend error = %v, want a *BackendUnavailableError", err)
	}
	if unavailable.Requested != "hip" {
		t.Errorf("Requested = %q, want hip", unavailable.Requested)
	}
	if msg := unavailable.Error(); msg == "" {
		t.Error("BackendUnavailableError has an empty message")
	}
}

func TestSelectBackendHonoursACustomFallbackChain(t *testing.T) {
	t.Parallel()

	got, err := SelectBackend(context.Background(), "auto",
		[]string{"hip", "cuda", "cpu"}, []string{"cpu", "cuda", "hip"}, "vmaf", nil)
	if err != nil {
		t.Fatalf("SelectBackend: %v", err)
	}
	if got != "hip" {
		t.Errorf("SelectBackend with a hip-first chain = %q, want hip", got)
	}
}

func TestDetectAvailableBackends(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		// stub answers keyed by the first argv token.
		answers map[string]RunResult
		want    []string
	}{
		{
			name: "a CPU-only vmaf build probes nothing else",
			answers: map[string]RunResult{
				"vmaf": {Stdout: "--backend $name: auto|cpu.\n"},
			},
			want: []string{"cpu"},
		},
		{
			name: "a CUDA build with a reachable GPU",
			answers: map[string]RunResult{
				"vmaf":       {Stdout: "--backend $name: auto|cpu|cuda|sycl|hip.\n"},
				"nvidia-smi": {Stdout: "GPU 0: NVIDIA GeForce RTX 4090 (UUID: GPU-x)\n"},
				"sycl-ls":    {ReturnCode: 1},
				"rocminfo":   {ReturnCode: 1},
				"rocm-smi":   {ReturnCode: 1},
			},
			want: []string{"cpu", "cuda"},
		},
		{
			name: "a build advertising cuda with no reachable GPU",
			answers: map[string]RunResult{
				"vmaf":       {Stdout: "--backend $name: auto|cpu|cuda.\n"},
				"nvidia-smi": {ReturnCode: 1},
			},
			want: []string{"cpu"},
		},
		{
			name: "a SYCL build with a level-zero GPU",
			answers: map[string]RunResult{
				"vmaf":       {Stdout: "--backend $name: auto|cpu|sycl.\n"},
				"sycl-ls":    {Stdout: "[ext_oneapi_level_zero:gpu][0] Intel Arc A770\n"},
				"nvidia-smi": {ReturnCode: 1},
			},
			want: []string{"cpu", "sycl"},
		},
		{
			name: "a HIP build with rocminfo reporting a gfx target",
			answers: map[string]RunResult{
				"vmaf":     {Stdout: "--backend $name: auto|cpu|hip.\n"},
				"rocminfo": {Stdout: "  Name:  gfx1100\n"},
			},
			want: []string{"cpu", "hip"},
		},
		{
			name: "a HIP build falling back to rocm-smi",
			answers: map[string]RunResult{
				"vmaf":     {Stdout: "--backend $name: auto|cpu|hip.\n"},
				"rocminfo": {ReturnCode: 1},
				"rocm-smi": {Stdout: "GPU[0] : Card series: Radeon RX 7900 XTX\n"},
			},
			want: []string{"cpu", "hip"},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			stub := func(_ context.Context, argv []string) RunResult {
				if res, ok := tc.answers[argv[0]]; ok {
					return res
				}
				return RunResult{ReturnCode: 1}
			}
			got := DetectAvailableBackends(context.Background(), "vmaf", stub)
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("DetectAvailableBackends() = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestAllBackendsAndFallbackOrder(t *testing.T) {
	t.Parallel()

	// ADR-0726 dropped the Vulkan backend; the vocabulary and the auto
	// preference order are part of the CLI's documented contract.
	if !reflect.DeepEqual(AllBackends, []string{"cpu", "cuda", "sycl", "hip"}) {
		t.Errorf("AllBackends = %v, want [cpu cuda sycl hip]", AllBackends)
	}
	if !reflect.DeepEqual(DefaultFallbacks, []string{"cuda", "sycl", "hip", "cpu"}) {
		t.Errorf("DefaultFallbacks = %v, want [cuda sycl hip cpu]", DefaultFallbacks)
	}
}
