// Copyright 2026 Lusoris
// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent

package pymath

import (
	"math"
	"math/big"
)

// exp2Precision is the working precision, in bits, of the product accumulator
// below. 320 bits leaves ~267 bits of head-room over float64's 53, so the
// accumulated rounding error of a few hundred square roots and multiplications
// stays far below the float64 rounding boundary.
const exp2Precision = 320

// exp2FracPrecision is the precision of the fraction register. x − floor(x) is
// exact in real arithmetic but not in float64: for x = −1e-8 the true fraction
// is 1 − 1e-8, whose binary expansion runs from 2^-1 down to x's own last bit
// near 2^-79. A float64 register silently truncates that, which is exactly the
// bug the first version of this function shipped. The worst case over all
// finite doubles is a fraction spanning 2^-1 down to 2^-1074, so the register
// is sized to hold it whole.
const exp2FracPrecision = 1100

// exp2MaxTerms bounds the digit loop. Beyond k ≈ exp2Precision the factor
// 2^(2^-k) = 1 + 2^-k·ln2 rounds to exactly 1 at the accumulator's precision,
// and the whole discarded tail contributes under 2^-(exp2Precision-2)
// relatively — some 2^260 below the float64 rounding boundary. Capping here
// keeps a pathological input (a fraction with bits near 2^-1074) from running
// a thousand needless square roots.
const exp2MaxTerms = exp2Precision + 8

// Exp2 returns the float64 nearest to 2**x — the same value
// CPython's `2.0 ** x` produces.
//
// # Why this is not math.Pow(2, x)
//
// The auto planner's estimated_bitrate_kbps is a user-discoverable JSON field
// whose bytes must match the Python emitter (ADR-0705 schema-forward
// invariant). Both implementations feed the *same* double into a 2**x kernel,
// but the kernels disagree: CPython delegates to the platform libm, whose pow
// is correctly rounded over the whole range this planner uses, while Go's
// math.Pow and math.Exp2 are documented only as "close" and land 1 ULP off on
// some of exactly those inputs — and in opposite
// directions, so neither is a drop-in. Two of the exponents this planner
// actually produces — (28−30)/6 and (28−23)/6, i.e. probe-quality-minus-CRF
// over six — are exactly such inputs, so the naive port emitted
// 1828.6860118673662 where Python emitted 1828.686011867366. One digit in the
// last mantissa position is enough to fail a byte-for-byte parity diff.
//
// # How
//
// Split x into its integer and fractional parts, x = i + f with f in [0, 1).
// A float64's fraction terminates, so f has a finite binary expansion
// f = Σ b_k·2^-k, and therefore
//
//	2**f = Π_{k : b_k = 1} 2**(2^-k)
//
// where each factor is the k-th repeated square root of 2 — and big.Float has
// a Sqrt that is exact to the receiver's precision. Accumulating that product
// at 320 bits and then scaling by 2**i gives a value whose float64 rounding is
// correct for every input that is not an exact rounding tie, and 2**x is
// irrational (hence never a tie) whenever f != 0. f == 0 is handled exactly by
// Ldexp.
//
// This costs a few microseconds per call and runs once per plan cell, which is
// irrelevant next to the ffprobe and encoder work the planner drives.
//
// # Where this still diverges from CPython
//
// The claim above is "correctly rounded", not "bit-identical to glibc". Those
// coincide only where glibc's pow is itself correctly rounded, which it is not
// everywhere: over 7,800 random doubles, glibc and the exact value disagreed by
// 1 ULP on five (0.06%) — and there this function follows the exact value, not
// glibc. That divergence is unreachable from the planner. Every exponent it
// evaluates is (probe_quality − crf) / 6 for two small integers, and glibc is
// correctly rounded across that entire n/6 family: 12,606 vectors spanning
// n ∈ [−6600, 6600] agree exactly (see TestExp2CorrectlyRoundedMatchesGlibc,
// whose fixtures cover that whole domain, and the divergence recorded in
// TestExp2DivergesFromGlibcOutsideThePlannerDomain). Reusing this helper for an
// exponent outside that family would reintroduce a ~0.06% chance of a
// last-digit difference against the Python emitter.
func Exp2(x float64) float64 {
	// Non-finite, and the over/underflow tails, defer to the stdlib: outside
	// the normal range the answer is ±Inf, 0, or NaN, on which every
	// implementation already agrees.
	if math.IsNaN(x) || math.IsInf(x, 0) || x >= 1024.0 || x <= -1075.0 {
		return math.Exp2(x)
	}

	intPart := math.Floor(x)
	if x == intPart {
		return math.Ldexp(1.0, int(intPart))
	}

	// frac = x − floor(x), computed at a precision that holds the exact
	// difference. Every operation below (multiply by two, subtract one from a
	// value in [1,2)) is an exponent shift or a Sterbenz-exact subtraction, so
	// the register stays exact for the whole walk.
	frac := new(big.Float).SetPrec(exp2FracPrecision).SetFloat64(x)
	frac.Sub(frac, new(big.Float).SetPrec(exp2FracPrecision).SetFloat64(intPart))

	one := new(big.Float).SetPrec(exp2FracPrecision).SetInt64(1)

	// product accumulates 2**frac; root walks 2**(2^-k) by repeated Sqrt.
	product := new(big.Float).SetPrec(exp2Precision).SetInt64(1)
	root := new(big.Float).SetPrec(exp2Precision).SetInt64(2)

	for k := 0; k < exp2MaxTerms && frac.Sign() != 0; k++ {
		root.Sqrt(root) // root = 2**(2^-(k+1))
		frac.SetMantExp(frac, 1)
		if frac.Cmp(one) >= 0 {
			frac.Sub(frac, one)
			product.Mul(product, root)
		}
	}

	// Scale by 2**intPart. SetMantExp shifts the binary exponent directly, so
	// this step is exact.
	scaled := new(big.Float).SetPrec(exp2Precision)
	scaled.SetMantExp(product, int(intPart))

	out, _ := scaled.Float64() // round-to-nearest-even, matching the FPU
	return out
}
