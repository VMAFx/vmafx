// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
//
// pkg/corpus/pysum.go — CPython-compatible float summation and statistics.
//
// CPython 3.12 switched builtins.sum() to Neumaier (improved Kahan-Babuska)
// compensated summation for float inputs, and statistics.pstdev() computes its
// variance exactly over Fractions before taking a correctly-rounded square
// root. A naive Go loop therefore lands one or two ULP away from the Python
// corpus writer on the encoder-stats aggregates and the shot-duration spread.
//
// The corpus JSONL is a cross-implementation contract during the ADR-0703 /
// ADR-0704 migration — a trainer re-reading a Go-written corpus must see the
// same bytes a Python-written one would carry — so both algorithms are ported
// here rather than approximated.

package corpus

import (
	"math"
	"math/big"
)

// pySum returns the sum of values using the Neumaier compensated algorithm
// CPython's builtins.sum() applies to float iterables.
//
// The compensation term is dropped once the running total goes non-finite: an
// infinite total plus a finite compensation would otherwise turn an honest
// +Inf into a NaN, which is not what CPython reports.
func pySum(values []float64) float64 {
	total := 0.0
	c := 0.0
	for _, x := range values {
		t := total + x
		if math.Abs(total) >= math.Abs(x) {
			c += (total - t) + x
		} else {
			c += (x - t) + total
		}
		total = t
		if math.IsInf(total, 0) || math.IsNaN(total) {
			c = 0.0
		}
	}
	if math.IsInf(total, 0) || math.IsNaN(total) {
		return total
	}
	return total + c
}

// exactRat converts a finite float64 to the rational it represents exactly.
// ok is false for NaN / Inf, which have no rational value.
func exactRat(v float64) (*big.Rat, bool) {
	r := new(big.Rat).SetFloat64(v)
	if r == nil {
		return nil, false
	}
	return r, true
}

// pyPopulationStdev returns the population standard deviation of values the
// way statistics.pstdev() does: an exact rational variance followed by a
// correctly-rounded square root.
//
// It returns 0.0 for an empty slice and NaN when any value is non-finite
// (statistics.pstdev raises ValueError there; the corpus callers treat a
// non-finite spread as "unavailable").
func pyPopulationStdev(values []float64) float64 {
	n := len(values)
	if n == 0 {
		return 0.0
	}
	// _ss: sx = sum(x), sxx = sum(x*x), both exact over Fractions, then
	// ssd = (count*sxx - sx*sx) / count.
	sx := new(big.Rat)
	sxx := new(big.Rat)
	for _, v := range values {
		r, ok := exactRat(v)
		if !ok {
			return math.NaN()
		}
		sx.Add(sx, r)
		sxx.Add(sxx, new(big.Rat).Mul(r, r))
	}
	count := new(big.Rat).SetInt64(int64(n))
	ssd := new(big.Rat).Sub(new(big.Rat).Mul(count, sxx), new(big.Rat).Mul(sx, sx))
	ssd.Quo(ssd, count)
	// pstdev: mss = ssd / n, then sqrt(mss) correctly rounded.
	mss := new(big.Rat).Quo(ssd, count)
	return floatSqrtOfFrac(mss.Num(), mss.Denom())
}

// sqrtBitWidth is 2 * float64 mantissa digits + 3, the working precision
// statistics._float_sqrt_of_frac uses for 53-bit floats.
const sqrtBitWidth = 2*53 + 3

// integerSqrtOfFracRTO returns the square root of n/m rounded to the nearest
// integer using round-to-odd, matching statistics._integer_sqrt_of_frac_rto.
func integerSqrtOfFracRTO(n, m *big.Int) *big.Int {
	q := new(big.Int).Quo(n, m)
	a := new(big.Int).Sqrt(q)
	// a |= (a*a*m != n)
	lhs := new(big.Int).Mul(new(big.Int).Mul(a, a), m)
	if lhs.Cmp(n) != 0 {
		a.Or(a, big.NewInt(1))
	}
	return a
}

// floatSqrtOfFrac returns sqrt(n/m) as a correctly-rounded float64, matching
// statistics._float_sqrt_of_frac. n must be non-negative and m positive, which
// holds for a variance.
func floatSqrtOfFrac(n, m *big.Int) float64 {
	if n.Sign() == 0 {
		return 0.0
	}
	// q = (n.bit_length() - m.bit_length() - sqrtBitWidth) // 2, using
	// Python's floor division for negative numerators.
	diff := n.BitLen() - m.BitLen() - sqrtBitWidth
	q := floorDiv2(diff)

	var numerator *big.Int
	denominator := big.NewInt(1)
	if q >= 0 {
		shifted := new(big.Int).Lsh(m, uint(2*q))
		numerator = new(big.Int).Lsh(integerSqrtOfFracRTO(n, shifted), uint(q))
	} else {
		shifted := new(big.Int).Lsh(n, uint(-2*q))
		numerator = integerSqrtOfFracRTO(shifted, m)
		denominator = new(big.Int).Lsh(big.NewInt(1), uint(-q))
	}
	f, _ := new(big.Rat).SetFrac(numerator, denominator).Float64()
	return f
}

// floorDiv2 is Python's x // 2 (floor division), which differs from Go's
// truncating division for negative x.
func floorDiv2(x int) int {
	if x >= 0 {
		return x / 2
	}
	return -((-x + 1) / 2)
}
