// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package pymath

import (
	"bufio"
	"encoding/hex"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestExp2MatchesGlibc replays reference vectors captured from
// CPython's `2.0 ** x` (i.e. glibc's correctly-rounded pow) and asserts
// bit-for-bit equality.
//
// The vectors cover every exponent the auto planner can produce — n/6 for the
// full probe-quality-minus-CRF range — plus dyadic, decimal, large, and
// subnormal-adjacent inputs. testdata/exp2_glibc.txt holds one
// "<x-bits> <2**x-bits>" pair per line, big-endian hex.
func TestExp2MatchesGlibc(t *testing.T) {
	t.Parallel()

	path := filepath.Join("testdata", "exp2_glibc.txt")
	fh, err := os.Open(path)
	if err != nil {
		t.Fatalf("open reference vectors: %v", err)
	}
	defer func() {
		if closeErr := fh.Close(); closeErr != nil {
			t.Errorf("close reference vectors: %v", closeErr)
		}
	}()

	rows := 0
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

		got := Exp2(x)
		if math.Float64bits(got) != math.Float64bits(want) {
			t.Errorf("line %d: exp2(%v) = %v (%016x), want %v (%016x)",
				lineNo, x, got, math.Float64bits(got), want, math.Float64bits(want))
		}
		rows++
	}
	if err := scanner.Err(); err != nil {
		t.Fatalf("read reference vectors: %v", err)
	}
	if rows == 0 {
		t.Fatal("reference vector file is empty")
	}
	t.Logf("verified %d reference vectors", rows)
}

// TestExp2EdgeCases pins the fast paths and the non-finite
// tails that defer to the stdlib.
func TestExp2EdgeCases(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name string
		x    float64
		want float64
	}{
		{"zero", 0, 1},
		{"one", 1, 2},
		{"negative integer", -3, 0.125},
		{"large positive integer", 1000, math.Ldexp(1, 1000)},
		{"overflow", 2000, math.Inf(1)},
		{"underflow", -2000, 0},
		{"positive infinity", math.Inf(1), math.Inf(1)},
		{"negative infinity", math.Inf(-1), 0},
		{"half", 0.5, math.Sqrt2},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			if got := Exp2(tc.x); got != tc.want {
				t.Errorf("exp2(%v) = %v, want %v", tc.x, got, tc.want)
			}
		})
	}
	if !math.IsNaN(Exp2(math.NaN())) {
		t.Error("exp2(NaN) should be NaN")
	}
}

// TestExp2DivergesFromGlibcOutsideThePlannerDomain records the one honest gap
// in the parity story, so a future reader finds the analysis instead of
// rediscovering it.
//
// exp2CorrectlyRounded is correctly rounded; glibc's pow is not, everywhere.
// On the five inputs below the two disagree by 1 ULP, and this implementation
// follows the true value while CPython follows glibc. None of these is
// reachable from the planner — every exponent it evaluates is n/6 for a small
// integer n, and TestExp2MatchesGlibc pins exact agreement
// across that entire family — but anyone reusing this helper for a different
// exponent should know the divergence exists.
//
// The wantExact column is the correctly-rounded value, verified against
// 120-digit decimal arithmetic; glibc is what CPython's `2.0 ** x` returns.
func TestExp2DivergesFromGlibcOutsideThePlannerDomain(t *testing.T) {
	t.Parallel()

	tests := []struct {
		name      string
		x         float64
		wantExact uint64
		glibc     uint64
	}{
		{"large positive exponent", 625.4640624952774, 0x6706121b69df7f89, 0x6706121b69df7f88},
		{"large negative exponent", -269.33811925631153, 0x2f1950760809d62a, 0x2f1950760809d62b},
		{"mid positive exponent", 359.9392741222757, 0x566eae56be1c85fa, 0x566eae56be1c85f9},
		{"just under one", -0.9789253768008641, 0x3fe03c45d41d5a24, 0x3fe03c45d41d5a23},
		{"fractional exponent", 0.8391095593128173, 0x3ffc9f8579eb50e9, 0x3ffc9f8579eb50ea},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			got := math.Float64bits(Exp2(tc.x))
			if got != tc.wantExact {
				t.Errorf("exp2(%v) = %016x, want the correctly-rounded %016x",
					tc.x, got, tc.wantExact)
			}
			if tc.wantExact == tc.glibc {
				t.Errorf("fixture %q no longer diverges; drop it from this test", tc.name)
			}
		})
	}
}

func float64FromHex(t *testing.T, s string) float64 {
	t.Helper()
	raw, err := hex.DecodeString(s)
	if err != nil || len(raw) != 8 {
		t.Fatalf("bad hex float %q: %v", s, err)
	}
	var bits uint64
	for _, b := range raw {
		bits = bits<<8 | uint64(b)
	}
	return math.Float64frombits(bits)
}
