// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package pyjson

import (
	"bufio"
	"encoding/hex"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

// TestFormatFloatMatchesPythonCorpus replays reference vectors captured from
// CPython's json.dumps() and asserts the spelling matches for every one.
//
// The corpus mixes the interesting boundaries (zero, the fixed/exponential
// switch at 1e15 / 1e16 and 1e-4 / 1e-5, the extremes of the double range,
// the smallest subnormal), a few thousand ordinary magnitudes, a few thousand
// values in [-1, 1], and a few thousand uniformly random bit patterns — the
// last of which is what actually exercises the shortest-round-trip digit
// generation. testdata/float_repr.txt holds one "<bits>\t<repr>" pair per
// line, big-endian hex.
func TestFormatFloatMatchesPythonCorpus(t *testing.T) {
	t.Parallel()

	path := filepath.Join("testdata", "float_repr.txt")
	fh, err := os.Open(path)
	if err != nil {
		t.Fatalf("open reference corpus: %v", err)
	}
	defer func() {
		if closeErr := fh.Close(); closeErr != nil {
			t.Errorf("close reference corpus: %v", closeErr)
		}
	}()

	rows, mismatches := 0, 0
	scanner := bufio.NewScanner(fh)
	for lineNo := 1; scanner.Scan(); lineNo++ {
		line := strings.TrimRight(scanner.Text(), "\r\n")
		if line == "" {
			continue
		}
		bitsHex, want, ok := strings.Cut(line, "\t")
		if !ok {
			t.Fatalf("line %d: expected a tab-separated pair", lineNo)
		}
		raw, err := hex.DecodeString(bitsHex)
		if err != nil || len(raw) != 8 {
			t.Fatalf("line %d: bad hex float %q: %v", lineNo, bitsHex, err)
		}
		var bits uint64
		for _, b := range raw {
			bits = bits<<8 | uint64(b)
		}
		x := math.Float64frombits(bits)

		got := FormatFloat(x)
		if got != want {
			mismatches++
			if mismatches <= 10 {
				t.Errorf("line %d: FormatFloat(%016x) = %q, want %q", lineNo, bits, got, want)
			}
		}
		rows++
	}
	if err := scanner.Err(); err != nil {
		t.Fatalf("read reference corpus: %v", err)
	}
	if rows == 0 {
		t.Fatal("reference corpus is empty")
	}
	if mismatches > 10 {
		t.Errorf("... and %d further mismatches", mismatches-10)
	}
	t.Logf("verified %d reference vectors", rows)
}

// TestFormatFloatRoundTrips is the property-side companion to the corpus
// test: whatever spelling we emit must parse back to the identical double, so
// a downstream consumer never silently loses a bit.
func TestFormatFloatRoundTrips(t *testing.T) {
	t.Parallel()

	// A deterministic LCG keeps the case list reproducible without pulling in
	// a seeded rand instance.
	state := uint64(0x2026083000000001)
	next := func() uint64 {
		state = state*6364136223846793005 + 1442695040888963407
		return state
	}
	for i := 0; i < 20000; i++ {
		x := math.Float64frombits(next())
		if math.IsNaN(x) || math.IsInf(x, 0) {
			continue
		}
		spelled := FormatFloat(x)
		back, err := strconv.ParseFloat(spelled, 64)
		if err != nil {
			t.Fatalf("FormatFloat(%016x) = %q does not parse: %v",
				math.Float64bits(x), spelled, err)
		}
		if math.Float64bits(back) != math.Float64bits(x) {
			t.Fatalf("round trip lost bits: %016x -> %q -> %016x",
				math.Float64bits(x), spelled, math.Float64bits(back))
		}
	}
}
