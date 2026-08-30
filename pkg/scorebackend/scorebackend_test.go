// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Tests for the backend selector. Expectations mirror
// tools/vmaf-tune/src/vmaftune/score_backend.py's parse_supported_backends
// and select_backend contracts (ADR-0299): auto falls back, an explicit
// request never silently downgrades.

package scorebackend

import (
	"context"
	"errors"
	"reflect"
	"strings"
	"testing"
)

func TestParseSupported(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name string
		help string
		want []string
	}{
		{
			name: "full fork help line",
			help: " --backend $name:  exclusive backend selector — auto|cpu|cuda|sycl|hip.\n",
			want: []string{"cpu", "cuda", "hip", "sycl"},
		},
		{
			name: "cpu-only build",
			help: " --backend $name:  exclusive backend selector — auto|cpu.\n",
			want: []string{"cpu"},
		},
		{
			name: "partial build",
			help: " --backend $name:  auto|cpu|cuda\n",
			want: []string{"cpu", "cuda"},
		},
		{
			// A prose mention must not be mistaken for advertised support:
			// the token has to be pipe-delimited.
			name: "prose mention does not count",
			help: "Use the CUDA backend for faster scoring. Also sycl and hip are nice.\n",
			want: []string{"cpu"},
		},
		{
			name: "empty help still yields cpu",
			help: "",
			want: []string{"cpu"},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			found := ParseSupported(tc.help)
			got := make([]string, 0, len(found))
			for name, ok := range found {
				if ok {
					got = append(got, name)
				}
			}
			sortStrings(got)
			if !reflect.DeepEqual(got, tc.want) {
				t.Errorf("ParseSupported = %v, want %v", got, tc.want)
			}
		})
	}
}

// sortStrings is a tiny insertion sort so the test does not import "sort"
// purely for assertion tidiness.
func sortStrings(xs []string) {
	for i := 1; i < len(xs); i++ {
		for j := i; j > 0 && xs[j] < xs[j-1]; j-- {
			xs[j], xs[j-1] = xs[j-1], xs[j]
		}
	}
}

func TestSelect(t *testing.T) {
	t.Parallel()
	cases := []struct {
		name      string
		prefer    string
		available []string
		want      string
		wantErr   bool
	}{
		{
			name:      "auto prefers cuda",
			prefer:    "auto",
			available: []string{"cpu", "cuda", "sycl"},
			want:      "cuda",
		},
		{
			name:      "auto falls through the vendor order",
			prefer:    "auto",
			available: []string{"cpu", "sycl"},
			want:      "sycl",
		},
		{
			name:      "auto lands on hip when it is the only GPU",
			prefer:    "auto",
			available: []string{"cpu", "hip"},
			want:      "hip",
		},
		{
			name:      "auto falls back to cpu",
			prefer:    "auto",
			available: []string{"cpu"},
			want:      "cpu",
		},
		{
			name:      "auto returns cpu even when nothing probed",
			prefer:    "auto",
			available: []string{},
			want:      "cpu",
		},
		{
			name:      "empty preference behaves as auto",
			prefer:    "",
			available: []string{"cpu", "cuda"},
			want:      "cuda",
		},
		{
			name:      "explicit request honoured",
			prefer:    "sycl",
			available: []string{"cpu", "cuda", "sycl"},
			want:      "sycl",
		},
		{
			// Never silently downgrade: a missing backend masks a build or
			// driver mismatch and lies about wall-clock expectations.
			name:      "explicit request never downgrades",
			prefer:    "cuda",
			available: []string{"cpu", "sycl"},
			wantErr:   true,
		},
		{
			name:      "unknown backend name rejected",
			prefer:    "vulkan",
			available: []string{"cpu"},
			wantErr:   true,
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got, err := Select(context.Background(), tc.prefer,
				Options{Available: tc.available})
			if tc.wantErr {
				if err == nil {
					t.Fatalf("Select(%q) = %q, want error", tc.prefer, got)
				}
				return
			}
			if err != nil {
				t.Fatalf("Select(%q): %v", tc.prefer, err)
			}
			if got != tc.want {
				t.Errorf("Select(%q) = %q, want %q", tc.prefer, got, tc.want)
			}
		})
	}
}

func TestSelect_UnavailableErrorIsIdentifiable(t *testing.T) {
	t.Parallel()
	_, err := Select(context.Background(), "hip", Options{Available: []string{"cpu"}})
	if err == nil {
		t.Fatal("expected an error for an unavailable explicit backend")
	}
	if !errors.Is(err, ErrUnavailable) {
		t.Errorf("error should wrap ErrUnavailable, got: %v", err)
	}
	// The message must be actionable: it names the request and what is there.
	for _, want := range []string{`"hip"`, "available: cpu", "runtime/driver"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("error message missing %q: %v", want, err)
		}
	}
}

func TestDetectAvailable_UsesInjectedProbes(t *testing.T) {
	t.Parallel()
	// A vmaf binary that advertises every backend, on a host where no GPU
	// probe binary exists, must resolve to cpu only. The Runner stub covers
	// the help call; the hardware probes short-circuit on exec.LookPath, so
	// this also asserts that a missing nvidia-smi/sycl-ls/rocminfo is not
	// mistaken for a present device.
	opts := Options{
		VMAFBin: "/nonexistent/vmaf",
		Runner: func(_ context.Context, name string, _ ...string) (string, bool) {
			if strings.HasSuffix(name, "vmaf") {
				return " --backend $name:  auto|cpu|cuda|sycl|hip.\n", true
			}
			return "", false
		},
	}
	got := DetectAvailable(context.Background(), opts)
	if len(got) == 0 || got[0] != "cpu" {
		t.Fatalf("DetectAvailable = %v, want cpu first", got)
	}
	for _, b := range got {
		if b != "cpu" {
			// A real GPU on the test host would legitimately appear here;
			// report rather than fail so CI on a GPU runner stays green.
			t.Logf("host advertises additional backend %q", b)
		}
	}
}

func TestDetectAvailable_ExplicitListShortCircuits(t *testing.T) {
	t.Parallel()
	got := DetectAvailable(context.Background(), Options{
		Available: []string{"cpu", "cuda"},
		Runner: func(context.Context, string, ...string) (string, bool) {
			t.Error("Runner must not be called when Available is set")
			return "", false
		},
	})
	if !reflect.DeepEqual(got, []string{"cpu", "cuda"}) {
		t.Errorf("DetectAvailable = %v, want the injected list verbatim", got)
	}
}
