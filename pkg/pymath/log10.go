// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package pymath

import (
	"math"
	"math/big"
	"sync"
)

// logPrecision is the working precision, in bits, of the natural-log kernel.
// 320 bits leaves ~267 bits of head-room over float64's 53, which absorbs the
// ~100 series terms plus the final division with room to spare.
const logPrecision = 320

// Log10 returns the float64 nearest to log10(x).
//
// # Why this is not math.Log10
//
// The predictor's analytical curve ends in `+ d·log10(bitrate_kbps)`, and its
// output lands in the auto planner's estimated_vmaf — a user-discoverable JSON
// field whose bytes must match the Python emitter. Go's math.Log10 is not a
// direct log10 at all: it is implemented as Log2(x) · (Ln2/Ln10), and that
// extra multiply costs it a ULP on ordinary inputs. Both of the probe bitrates
// the planner's own fixtures use are such inputs — log10(4200.5) and
// log10(48000) each came out one ULP below CPython's — which moved 31 of 1684
// predictor reference vectors.
//
// # How
//
// Decompose x = m·2^e with m in [0.5, 1), so log10(x) = (ln m + e·ln 2) / ln 10.
// ln m comes from the atanh series
//
//	ln m = 2·(z + z³/3 + z⁵/5 + …),   z = (m−1)/(m+1)
//
// which for m in [0.5, 1) has |z| <= 1/3 and therefore gains about 3.17 bits
// per term — roughly 100 terms at 320 bits. ln 2 and ln 10 come from the same
// series and are computed once. The quotient is then rounded to float64.
//
// # Where this still diverges from CPython
//
// Correct rounding is not the same as bit-identity with glibc, and glibc's
// log10 is not correctly rounded everywhere: over 5,000 random positive
// doubles it disagreed with the exact value on 31 (0.6%), and on those this
// function follows the exact value. Unlike Exp2, whose inputs the planner
// confines to a family where glibc is exact, the probe bitrate fed to Log10 is
// operator data, so that 0.6% is genuinely reachable — a run whose
// complexity_score happens to land on one of those values will differ from the
// Python emitter in the last mantissa digit of estimated_vmaf. See
// TestLog10DivergesFromGlibc, which pins known examples. Closing that last gap
// would mean reimplementing glibc's exact log10 algorithm, not improving
// accuracy.
func Log10(x float64) float64 {
	// Non-finite, zero, and negative inputs are the domain edges, where every
	// implementation already agrees on ±Inf / NaN.
	if math.IsNaN(x) || math.IsInf(x, 0) || x <= 0 {
		return math.Log10(x)
	}
	if x == 1 {
		return 0
	}

	ln := bigLn(new(big.Float).SetPrec(logPrecision).SetFloat64(x))
	ln.Quo(ln, bigLn10())

	out, _ := ln.Float64() // round-to-nearest-even, matching the FPU
	return out
}

// bigLn returns ln(x) at logPrecision for a finite positive x.
func bigLn(x *big.Float) *big.Float {
	mant := new(big.Float).SetPrec(logPrecision)
	exp := x.MantExp(mant) // x = mant · 2**exp, mant in [0.5, 1)

	result := lnSeries(mant)
	if exp != 0 {
		scaled := new(big.Float).SetPrec(logPrecision).SetInt64(int64(exp))
		scaled.Mul(scaled, bigLn2())
		result.Add(result, scaled)
	}
	return result
}

// lnSeries returns ln(m) via the atanh expansion. Convergence is driven by
// z = (m−1)/(m+1): the callers only ever pass m in [0.5, 1) (from MantExp) or
// the constants 2 and 0.625, all of which keep |z| <= 1/3.
func lnSeries(m *big.Float) *big.Float {
	one := new(big.Float).SetPrec(logPrecision).SetInt64(1)

	numerator := new(big.Float).SetPrec(logPrecision).Sub(m, one)
	if numerator.Sign() == 0 {
		return new(big.Float).SetPrec(logPrecision) // ln(1) = 0
	}
	denominator := new(big.Float).SetPrec(logPrecision).Add(m, one)

	z := new(big.Float).SetPrec(logPrecision).Quo(numerator, denominator)
	zSquared := new(big.Float).SetPrec(logPrecision).Mul(z, z)

	term := new(big.Float).SetPrec(logPrecision).Set(z)
	sum := new(big.Float).SetPrec(logPrecision).Set(z)
	addend := new(big.Float).SetPrec(logPrecision)
	divisor := new(big.Float).SetPrec(logPrecision)

	// |z| <= 1/3 gains ~3.17 bits per term, so the guard trips after ~100
	// iterations; the cap is a safety net, not the exit condition.
	for k := int64(3); k < 4096; k += 2 {
		term.Mul(term, zSquared)
		divisor.SetInt64(k)
		addend.Quo(term, divisor)
		if addend.Sign() == 0 {
			break
		}
		// Stop once the addend sits entirely below the working precision:
		// adding it can no longer change a single retained bit.
		if sum.MantExp(nil)-addend.MantExp(nil) > logPrecision+8 {
			break
		}
		sum.Add(sum, addend)
	}
	return sum.Mul(sum, new(big.Float).SetPrec(logPrecision).SetInt64(2))
}

var (
	ln2Once  sync.Once
	ln2Value *big.Float
	ln10Once sync.Once
	ln10Val  *big.Float
)

// bigLn2 returns ln(2) at logPrecision, computed once. Callers must not
// mutate the result.
func bigLn2() *big.Float {
	ln2Once.Do(func() {
		// ln(2) directly from the series: z = (2−1)/(2+1) = 1/3.
		ln2Value = lnSeries(new(big.Float).SetPrec(logPrecision).SetInt64(2))
	})
	return ln2Value
}

// bigLn10 returns ln(10) at logPrecision, computed once. Callers must not
// mutate the result.
func bigLn10() *big.Float {
	ln10Once.Do(func() {
		// 10 = 0.625 · 2^4, and 0.625 is in the series' fast-convergence band.
		mantissa := new(big.Float).SetPrec(logPrecision).SetFloat64(0.625)
		value := lnSeries(mantissa)
		four := new(big.Float).SetPrec(logPrecision).SetInt64(4)
		four.Mul(four, bigLn2())
		ln10Val = value.Add(value, four)
	})
	return ln10Val
}
