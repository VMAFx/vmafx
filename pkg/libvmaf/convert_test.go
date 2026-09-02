// Copyright 2026 Lusoris. All rights reserved.
// Use of this source code is governed by the BSD-3-Clause-Plus-Patent
// license that can be found in the LICENSE file.

package libvmaf

import (
	"math"
	"testing"
)

func TestSafeUint32(t *testing.T) {
	cases := []struct {
		name string
		in   int
		want uint32
	}{
		{"zero", 0, 0},
		{"small", 7, 7},
		{"negative clamps to 0", -1, 0},
		{"large negative clamps to 0", math.MinInt32, 0},
		{"max uint32 passes", math.MaxUint32, math.MaxUint32},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := SafeUint32(tc.in); got != tc.want {
				t.Fatalf("SafeUint32(%d) = %d, want %d", tc.in, got, tc.want)
			}
		})
	}
}

// TestSafeUint32OverMax exercises the upper clamp only on 64-bit platforms, where an
// int can exceed math.MaxUint32. On 32-bit platforms int cannot hold such a value, so
// the branch is unreachable and the case is skipped rather than overflowing the literal.
func TestSafeUint32OverMax(t *testing.T) {
	if math.MaxInt < math.MaxUint32 {
		t.Skip("int is 32-bit on this platform; over-max case is unreachable")
	}
	over := int(math.MaxUint32) + 1
	if got := SafeUint32(over); got != math.MaxUint32 {
		t.Fatalf("SafeUint32(%d) = %d, want %d (saturated)", over, got, uint32(math.MaxUint32))
	}
}

func TestSafeUint64(t *testing.T) {
	cases := []struct {
		name string
		in   int64
		want uint64
	}{
		{"zero", 0, 0},
		{"small", 42, 42},
		{"negative clamps to 0", -1, 0},
		{"min int64 clamps to 0", math.MinInt64, 0},
		{"max int64 passes", math.MaxInt64, math.MaxInt64},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := SafeUint64(tc.in); got != tc.want {
				t.Fatalf("SafeUint64(%d) = %d, want %d", tc.in, got, tc.want)
			}
		})
	}
}
