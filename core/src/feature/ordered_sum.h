/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

#ifndef FEATURE_ORDERED_SUM_H_
#define FEATURE_ORDERED_SUM_H_

/*
 * The sum `for (i = 0; i < n; i++) s += x[i];` of non-negative doubles, with
 * the bits a sequential loop gives, from pieces a device computes in
 * parallel (ADR-1433).
 *
 * Why a plain parallel sum is not enough: every `s += x[i]` rounds, and where
 * it rounds depends on `s`, so another order of the same terms ends a few
 * units in the last place elsewhere. A twin that must return the CPU
 * extractor's bits has to round where the CPU's loop rounds.
 *
 * What makes it parallel anyway. While the running sum stays in one binade
 * [2^e, 2^(e+1)) it is a multiple of u = 2^(e-52), and adding a term x >= 0
 * moves it by x rounded to a multiple of u (to nearest; a tie goes to the
 * even multiple of the *result*). So inside a binade the loop is an integer
 * sum of per-term increments, and integer sums can be formed in any
 * grouping. The only order dependence left is the tie rule, which needs the
 * parity of the running integer; a piece therefore carries two increments,
 * one for an even and one for an odd start (VmafOrdsumUnits), and two pieces
 * compose associatively (vmaf_ordsum_then).
 *
 * The pieces:
 *
 *   1. the terms are cut into chunks in loop order;
 *   2. a plan names the binade each chunk is expected to start and end in
 *      (vmaf_ordsum_plan, from any approximation of the prefix sums);
 *   3. per chunk, the increments of its terms in that binade are composed
 *      (vmaf_ordsum_planned_term, vmaf_ordsum_then);
 *   4. one walk over the chunks adds each chunk's increment to the exact sum
 *      (vmaf_ordsum_add_chunk). Where the plan does not hold at the exact
 *      sum, the walk is told so and adds that chunk's terms one by one.
 *
 * The plan is advice. Step 4 checks at the exact sum that the chunk starts
 * in the planned binade and does not leave it, and falls back to the terms
 * otherwise, so a wrong or even arbitrary plan costs time and never changes
 * the result. A sum crosses a binade a few dozen times at most, so only a
 * few chunks take the fallback.
 *
 * Preconditions: every term is >= 0, or NaN. Negative terms are not
 * supported (the running sum must not decrease); one that slips in sends its
 * chunk to the fallback when the chunk has a plan, and is wrong when its
 * chunk's approximate sum cancels to zero.
 *
 * The functions are `static inline` on the host. A caller that runs them in
 * device code defines VMAF_ORDSUM_FUNC, VMAF_ORDSUM_BITS and
 * VMAF_ORDSUM_FROM_BITS before including the header.
 *
 * Every function that takes or returns a `double` has a `_bits` form on the
 * fp64 bit pattern, and is that form plus the conversion. The `_bits` forms
 * use integers only, for a device without an fp64 type (a SYCL kernel,
 * ADR-0220). Such a caller defines VMAF_ORDSUM_NO_FP64 before including the
 * header and gets the `_bits` forms alone; vmaf_ordsum_plan(), whose prefix
 * is an fp64 sum, has no such form, and that caller plans from its own
 * approximation.
 */

#include <stdint.h>

#ifndef VMAF_ORDSUM_FUNC
#define VMAF_ORDSUM_FUNC static inline
#endif

#if !defined(VMAF_ORDSUM_BITS) && !defined(VMAF_ORDSUM_NO_FP64)
#include <string.h>
static inline uint64_t vmaf_ordsum_host_bits(double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}
static inline double vmaf_ordsum_host_from_bits(uint64_t bits)
{
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}
#define VMAF_ORDSUM_BITS(v) vmaf_ordsum_host_bits(v)
#define VMAF_ORDSUM_FROM_BITS(bits) vmaf_ordsum_host_from_bits(bits)
#endif

/* Plan of one chunk: a binade exponent in [VMAF_ORDSUM_MIN_EXP,
 * VMAF_ORDSUM_MAX_EXP], or one of the two codes. */
#define VMAF_ORDSUM_MIN_EXP (-900)
#define VMAF_ORDSUM_MAX_EXP 900
/* Every term of the chunk is expected to be zero. */
#define VMAF_ORDSUM_PLAN_ZERO 32767
/* No binade is planned: the walk adds the chunk's terms one by one. */
#define VMAF_ORDSUM_PLAN_TERMS (-32768)

/* 2^52 and 2^53: the integer range of a binade's multiples of u. */
#define VMAF_ORDSUM_HIDDEN_BIT ((int64_t)((uint64_t)1 << 52u))
#define VMAF_ORDSUM_BINADE_END ((int64_t)((uint64_t)1 << 53u))
/* A chunk increment at or above this is "does not fit the plan". It is
 * larger than any increment a chunk that stays in its binade can have (2^52)
 * and small enough that adding it to a 53-bit integer cannot overflow. */
#define VMAF_ORDSUM_UNFIT ((int64_t)((uint64_t)1 << 54u))

#define VMAF_ORDSUM_EXP_MASK 0x7ffu
#define VMAF_ORDSUM_EXP_BIAS 1023
#define VMAF_ORDSUM_FRACTION_MASK (((uint64_t)1 << 52u) - 1u)

/* What a run of terms adds to a running sum that is `m * u`: `even` when m is
 * even, `odd` when it is odd. The two differ only by the ties in the run. */
struct VmafOrdsumUnits {
    int64_t even;
    int64_t odd;
};
#ifndef __cplusplus
typedef struct VmafOrdsumUnits VmafOrdsumUnits;
#endif

/* Biased exponent field of a double: 0 for zero and subnormals, 2047 for
 * infinities and NaN. */
VMAF_ORDSUM_FUNC unsigned vmaf_ordsum_exponent_field_bits(uint64_t bits)
{
    return (unsigned)((bits >> 52u) & VMAF_ORDSUM_EXP_MASK);
}

/* 1 for a NaN. */
VMAF_ORDSUM_FUNC int vmaf_ordsum_is_nan_bits(uint64_t bits)
{
    return vmaf_ordsum_exponent_field_bits(bits) == VMAF_ORDSUM_EXP_MASK &&
           (bits & VMAF_ORDSUM_FRACTION_MASK) != 0u;
}

/* Binade of a positive normal double, VMAF_ORDSUM_PLAN_TERMS for every
 * other value and for a binade outside the planned range. Zero, a negative
 * value and a NaN are not above zero; a subnormal, an infinity and a NaN lie
 * outside the planned range. */
VMAF_ORDSUM_FUNC int vmaf_ordsum_binade_bits(uint64_t bits)
{
    const unsigned field = vmaf_ordsum_exponent_field_bits(bits);
    if ((bits >> 63u) != 0u || field == 0u)
        return VMAF_ORDSUM_PLAN_TERMS;
    const int e = (int)field - VMAF_ORDSUM_EXP_BIAS;
    if (e < VMAF_ORDSUM_MIN_EXP || e > VMAF_ORDSUM_MAX_EXP)
        return VMAF_ORDSUM_PLAN_TERMS;
    return e;
}

#ifndef VMAF_ORDSUM_NO_FP64
VMAF_ORDSUM_FUNC unsigned vmaf_ordsum_exponent_field(double v)
{
    return vmaf_ordsum_exponent_field_bits(VMAF_ORDSUM_BITS(v));
}

VMAF_ORDSUM_FUNC int vmaf_ordsum_binade(double v)
{
    return vmaf_ordsum_binade_bits(VMAF_ORDSUM_BITS(v));
}
#endif

VMAF_ORDSUM_FUNC int vmaf_ordsum_plan_is_binade(int plan)
{
    return plan >= VMAF_ORDSUM_MIN_EXP && plan <= VMAF_ORDSUM_MAX_EXP;
}

VMAF_ORDSUM_FUNC VmafOrdsumUnits vmaf_ordsum_units(int64_t even, int64_t odd)
{
    VmafOrdsumUnits r;
    r.even = even;
    r.odd = odd;
    return r;
}

/* `mantissa >> shift` rounded to nearest, 1 <= shift <= 53; a tie goes to the
 * value that makes the running integer even: m becomes m + whole or
 * m + whole + 1, whichever is even. */
VMAF_ORDSUM_FUNC VmafOrdsumUnits vmaf_ordsum_round_shifted(uint64_t mantissa, int shift)
{
    const int64_t whole = (int64_t)(mantissa >> (unsigned)shift);
    const uint64_t rest = mantissa & (((uint64_t)1 << (unsigned)shift) - 1u);
    const uint64_t half = (uint64_t)1 << (unsigned)(shift - 1);
    if (rest != half) {
        const int64_t rounded = whole + (rest > half ? 1 : 0);
        return vmaf_ordsum_units(rounded, rounded);
    }
    const int64_t odd_whole = (int64_t)((uint64_t)whole & 1u);
    return vmaf_ordsum_units(whole + odd_whole, whole + 1 - odd_whole);
}

/* 1 for a term the integer form cannot take: negative (a negative zero is a
 * zero), infinite or NaN. */
VMAF_ORDSUM_FUNC int vmaf_ordsum_term_is_unfit(uint64_t bits)
{
    const int negative = (bits >> 63u) != 0u && (bits << 1u) != 0u;
    return negative || ((bits >> 52u) & VMAF_ORDSUM_EXP_MASK) == VMAF_ORDSUM_EXP_MASK;
}

/* Increment of one term `x` for a running sum in binade `e`: x / 2^(e-52)
 * rounded to an integer, to nearest, a tie to the value that makes the
 * running integer even.
 *
 * In integers, from the term's own bits: x = m * 2^(ex-52) with m in
 * [2^52, 2^53), so x / u = m >> (e - ex). A term in a higher binade than the
 * sum, a negative, infinite or NaN term does not fit (the sum would leave the
 * binade, or is not a sum of this kind) and yields VMAF_ORDSUM_UNFIT. */
VMAF_ORDSUM_FUNC VmafOrdsumUnits vmaf_ordsum_term_bits(uint64_t bits, int e)
{
    const unsigned field = (unsigned)((bits >> 52u) & VMAF_ORDSUM_EXP_MASK);
    if (vmaf_ordsum_term_is_unfit(bits))
        return vmaf_ordsum_units(VMAF_ORDSUM_UNFIT, VMAF_ORDSUM_UNFIT);
    if (field == 0u) /* zero or subnormal: below half a unit of every planned binade */
        return vmaf_ordsum_units(0, 0);
    const int shift = e - ((int)field - VMAF_ORDSUM_EXP_BIAS);
    if (shift < 0)
        return vmaf_ordsum_units(VMAF_ORDSUM_UNFIT, VMAF_ORDSUM_UNFIT);
    if (shift > 53) /* x < u / 2 */
        return vmaf_ordsum_units(0, 0);
    const uint64_t mantissa = (bits & VMAF_ORDSUM_FRACTION_MASK) | (uint64_t)VMAF_ORDSUM_HIDDEN_BIT;
    if (shift == 0)
        return vmaf_ordsum_units((int64_t)mantissa, (int64_t)mantissa);
    return vmaf_ordsum_round_shifted(mantissa, shift);
}

/* Increment of one term under a chunk's plan. Under VMAF_ORDSUM_PLAN_ZERO a
 * term is either a zero (of either sign) or does not fit; under
 * VMAF_ORDSUM_PLAN_TERMS nothing is read and the result is zero. */
VMAF_ORDSUM_FUNC VmafOrdsumUnits vmaf_ordsum_planned_term_bits(uint64_t bits, int plan)
{
    if (vmaf_ordsum_plan_is_binade(plan))
        return vmaf_ordsum_term_bits(bits, plan);
    if (plan == VMAF_ORDSUM_PLAN_ZERO && (bits << 1u) != 0u)
        return vmaf_ordsum_units(VMAF_ORDSUM_UNFIT, VMAF_ORDSUM_UNFIT);
    return vmaf_ordsum_units(0, 0);
}

#ifndef VMAF_ORDSUM_NO_FP64
VMAF_ORDSUM_FUNC VmafOrdsumUnits vmaf_ordsum_term(double x, int e)
{
    return vmaf_ordsum_term_bits(VMAF_ORDSUM_BITS(x), e);
}

VMAF_ORDSUM_FUNC VmafOrdsumUnits vmaf_ordsum_planned_term(double x, int plan)
{
    return vmaf_ordsum_planned_term_bits(VMAF_ORDSUM_BITS(x), plan);
}
#endif

/* 1 for an odd running integer (the integers here are never negative). */
VMAF_ORDSUM_FUNC int vmaf_ordsum_is_odd(int64_t v)
{
    return ((uint64_t)v & 1u) != 0u;
}

VMAF_ORDSUM_FUNC int64_t vmaf_ordsum_cap(int64_t v)
{
    return v > VMAF_ORDSUM_UNFIT ? VMAF_ORDSUM_UNFIT : v;
}

/* Increment of run `a` followed by run `b`. Associative, not commutative:
 * `b` starts at the parity `a` leaves. A result past VMAF_ORDSUM_UNFIT stays
 * there, so long runs cannot overflow. */
VMAF_ORDSUM_FUNC VmafOrdsumUnits vmaf_ordsum_then(VmafOrdsumUnits a, VmafOrdsumUnits b)
{
    const int64_t even = a.even + (vmaf_ordsum_is_odd(a.even) ? b.odd : b.even);
    const int64_t odd = a.odd + (vmaf_ordsum_is_odd(a.odd) ? b.even : b.odd);
    return vmaf_ordsum_units(vmaf_ordsum_cap(even), vmaf_ordsum_cap(odd));
}

/* Plan of the next chunk. `prefix` approximates the sum of everything before
 * the chunk and is advanced by `chunk_sum`, an approximation of the chunk's
 * own sum (the same terms added in any order will do).
 *
 * Once the prefix is infinite or NaN the exact sum no longer moves unless a
 * NaN term arrives, and a finite chunk gets the largest binade as its plan:
 * its increment then only says whether every term is finite. */
#ifndef VMAF_ORDSUM_NO_FP64
VMAF_ORDSUM_FUNC int vmaf_ordsum_plan(double *prefix, double chunk_sum)
{
    const double before = *prefix;
    const double after = before + chunk_sum;
    *prefix = after;
    if (chunk_sum == 0.0)
        return VMAF_ORDSUM_PLAN_ZERO;
    const int chunk_finite = vmaf_ordsum_exponent_field(chunk_sum) != VMAF_ORDSUM_EXP_MASK;
    if (vmaf_ordsum_exponent_field(before) == VMAF_ORDSUM_EXP_MASK)
        return chunk_finite ? VMAF_ORDSUM_MAX_EXP : VMAF_ORDSUM_PLAN_TERMS;
    const int e = vmaf_ordsum_binade(before);
    if (e != vmaf_ordsum_binade(after))
        return VMAF_ORDSUM_PLAN_TERMS;
    return e;
}
#endif

/* The bits of `total * 2^(e-52)` for 2^52 <= total <= 2^53; 2^53 is the first
 * value of the next binade. */
VMAF_ORDSUM_FUNC uint64_t vmaf_ordsum_from_units_bits(int64_t total, int e)
{
    if (total == VMAF_ORDSUM_BINADE_END)
        return (uint64_t)(e + 1 + VMAF_ORDSUM_EXP_BIAS) << 52u;
    return ((uint64_t)(e + VMAF_ORDSUM_EXP_BIAS) << 52u) |
           ((uint64_t)total & VMAF_ORDSUM_FRACTION_MASK);
}

/* Adds one chunk to the exact running sum `*sum_bits` from its plan and the
 * increment computed for that plan. Returns 1 when the chunk is added, 0
 * when the caller must add the chunk's terms to the sum one by one instead
 * (`*sum_bits` is unchanged then).
 *
 * `units` must be the chunk's terms in order through
 * vmaf_ordsum_planned_term_bits(term, plan) and vmaf_ordsum_then. */
VMAF_ORDSUM_FUNC int vmaf_ordsum_add_chunk_bits(uint64_t *sum_bits, int plan, VmafOrdsumUnits units)
{
    const uint64_t s = *sum_bits;
    if (vmaf_ordsum_is_nan_bits(s)) /* NaN absorbs every term */
        return 1;
    if (plan == VMAF_ORDSUM_PLAN_TERMS)
        return 0;
    if (plan == VMAF_ORDSUM_PLAN_ZERO)
        return units.even == 0;
    if (vmaf_ordsum_exponent_field_bits(s) ==
        VMAF_ORDSUM_EXP_MASK) /* +inf stays unless a NaN follows */
        return units.even < VMAF_ORDSUM_UNFIT;
    if (vmaf_ordsum_binade_bits(s) != plan)
        return 0;
    const int64_t m = (int64_t)((s & VMAF_ORDSUM_FRACTION_MASK) | (uint64_t)VMAF_ORDSUM_HIDDEN_BIT);
    const int64_t total = m + (vmaf_ordsum_is_odd(m) ? units.odd : units.even);
    if (total > VMAF_ORDSUM_BINADE_END)
        return 0;
    *sum_bits = vmaf_ordsum_from_units_bits(total, plan);
    return 1;
}

#ifndef VMAF_ORDSUM_NO_FP64
VMAF_ORDSUM_FUNC double vmaf_ordsum_from_units(int64_t total, int e)
{
    return VMAF_ORDSUM_FROM_BITS(vmaf_ordsum_from_units_bits(total, e));
}

VMAF_ORDSUM_FUNC int vmaf_ordsum_add_chunk(double *sum, int plan, VmafOrdsumUnits units)
{
    uint64_t bits = VMAF_ORDSUM_BITS(*sum);
    const int added = vmaf_ordsum_add_chunk_bits(&bits, plan, units);
    if (added)
        *sum = VMAF_ORDSUM_FROM_BITS(bits);
    return added;
}
#endif

#endif /* FEATURE_ORDERED_SUM_H_ */
