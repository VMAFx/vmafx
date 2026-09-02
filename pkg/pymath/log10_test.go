// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package pymath

import (
	"bufio"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestLog10AgainstGlibcCorpus measures how often Log10 reproduces CPython's
// math.log10 byte for byte, and fails if the agreement rate regresses below
// what the current implementation achieves.
//
// This is a *rate* gate rather than an equality gate, and deliberately so:
// glibc's log10 is not correctly rounded everywhere (about 0.6% of random
// positive doubles), so no correctly-rounded implementation can match it
// everywhere. The threshold below is set just under the measured rate, which
// makes the test catch a real regression — swapping back to math.Log10 drops
// agreement by orders of magnitude — while staying stable across libm
// versions.
func TestLog10AgainstGlibcCorpus(t *testing.T) {
	t.Parallel()

	// minAgreement is the floor for the fraction of corpus vectors that must
	// match glibc exactly. The big.Float kernel measures ~99.4%; Go's stdlib
	// math.Log10 measures far lower, so this cleanly separates them.
	const minAgreement = 0.99

	path := filepath.Join("testdata", "log10_glibc.txt")
	fh, err := os.Open(path)
	if err != nil {
		t.Fatalf("open reference vectors: %v", err)
	}
	defer func() {
		if closeErr := fh.Close(); closeErr != nil {
			t.Errorf("close reference vectors: %v", closeErr)
		}
	}()

	rows, agree, stdlibAgree := 0, 0, 0
	var firstDivergences []string

	scanner := bufio.NewScanner(fh)
	for lineNo := 1; scanner.Scan(); lineNo++ {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) != 2 {
			t.Fatalf("line %d: want 2 fields, got %d", lineNo, len(fields))
		}
		x := float64FromHex(t, fields[0])
		want := float64FromHex(t, fields[1])

		if math.Float64bits(Log10(x)) == math.Float64bits(want) {
			agree++
		} else if len(firstDivergences) < 5 {
			firstDivergences = append(firstDivergences,
				strings.Join([]string{fields[0], fields[1]}, " -> want "))
		}
		if math.Float64bits(math.Log10(x)) == math.Float64bits(want) {
			stdlibAgree++
		}
		rows++
	}
	if err := scanner.Err(); err != nil {
		t.Fatalf("read reference vectors: %v", err)
	}
	if rows == 0 {
		t.Fatal("reference vector file is empty")
	}

	rate := float64(agree) / float64(rows)
	stdlibRate := float64(stdlibAgree) / float64(rows)
	t.Logf("Log10 agrees with glibc on %d/%d (%.4f); math.Log10 on %d/%d (%.4f)",
		agree, rows, rate, stdlibAgree, rows, stdlibRate)
	if len(firstDivergences) > 0 {
		t.Logf("first divergences (hex x -> want): %s", strings.Join(firstDivergences, ", "))
	}

	if rate < minAgreement {
		t.Errorf("agreement rate %.4f fell below the %.2f floor", rate, minAgreement)
	}
	if rate <= stdlibRate {
		t.Errorf("Log10 (%.4f) is no better than math.Log10 (%.4f); the big.Float "+
			"kernel is not earning its keep", rate, stdlibRate)
	}
}

// TestLog10PlannerInputs pins the exact values the predictor's own reference
// vectors depend on. These are the two probe bitrates where Go's math.Log10
// lands a ULP below CPython; the whole reason this kernel exists.
func TestLog10PlannerInputs(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		x    float64
		want uint64
	}{
		{"probe bitrate 4200.5", 4200.5, 0x400cfc853a9c1768},
		{"probe bitrate 48000", 48000.0, 0x4012b9974d8cdbe0},
		{"the log10(0) guard's floor", 1.0, 0x0000000000000000},
		{"one half", 0.5, 0xbfd34413509f79ff},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := math.Float64bits(Log10(tc.x)); got != tc.want {
				t.Errorf("Log10(%v) = %016x, want %016x", tc.x, got, tc.want)
			}
		})
	}
}

// TestLog10ExactPowersOfTen checks the values a reader would most expect to be
// exact.
func TestLog10ExactPowersOfTen(t *testing.T) {
	t.Parallel()

	tests := []struct {
		x    float64
		want float64
	}{
		{1, 0}, {10, 1}, {100, 2}, {1000, 3}, {1e15, 15}, {0.1, -1}, {0.01, -2},
	}
	for _, tc := range tests {
		if got := Log10(tc.x); got != tc.want {
			t.Errorf("Log10(%v) = %v, want %v", tc.x, got, tc.want)
		}
	}
}

// TestLog10DomainEdges pins the non-finite and non-positive tails, which
// defer to the stdlib.
func TestLog10DomainEdges(t *testing.T) {
	t.Parallel()

	if got := Log10(0); !math.IsInf(got, -1) {
		t.Errorf("Log10(0) = %v, want -Inf", got)
	}
	if got := Log10(math.Inf(1)); !math.IsInf(got, 1) {
		t.Errorf("Log10(+Inf) = %v, want +Inf", got)
	}
	if !math.IsNaN(Log10(-1)) {
		t.Error("Log10(-1) should be NaN")
	}
	if !math.IsNaN(Log10(math.NaN())) {
		t.Error("Log10(NaN) should be NaN")
	}
}

// TestLog10DivergesFromGlibc records the honest residual: on these inputs
// glibc's log10 is itself 1 ULP off the true value, and this kernel follows
// the true value instead. Unlike the Exp2 case, the planner *can* reach these
// — the probe bitrate is operator data — so a run landing on one will differ
// from the Python emitter in the last digit of estimated_vmaf.
//
// wantExact is the correctly-rounded value (verified against 60-digit decimal
// arithmetic); glibc is what CPython's math.log10 returns.
func TestLog10DivergesFromGlibc(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		x         float64
		wantExact uint64
		glibc     uint64
	}{
		{"large magnitude a", 347552180.15588444, 0x40211500936026a0, 0x402115009360269f},
		{"large magnitude b", 627767108.5215408, 0x402198790d715733, 0x402198790d715734},
		{"large magnitude c", 739828591.421002, 0x4021bcfec0c3cd9f, 0x4021bcfec0c3cda0},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := math.Float64bits(Log10(tc.x))
			if got != tc.wantExact {
				t.Errorf("Log10(%v) = %016x, want the correctly-rounded %016x",
					tc.x, got, tc.wantExact)
			}
			if tc.wantExact == tc.glibc {
				t.Errorf("fixture %q no longer diverges; drop it from this test", tc.name)
			}
		})
	}
}
