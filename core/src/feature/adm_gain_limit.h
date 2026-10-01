/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * The integer ADM enhancement gain limit without binary64.
 *
 * The scalar decouple kernels (adm_decouple_band() and
 * adm_decouple_band_s123() in integer_adm_kernels.h) bound the restored sample
 * with
 *
 *     rst = MIN(rst * gain, t)      or      rst = MAX(rst * gain, t)
 *
 * where `rst` and `t` are integers and `gain` is the double
 * `adm_enhn_gain_limit`. The product is a double, rounded to nearest, and the
 * store back into the integer truncates it toward zero. `t` is an integer, so
 * truncating the product first and taking the integer minimum or maximum
 * afterwards gives the same sample: the limit needs trunc(fl(rst * gain)) and
 * nothing else from floating point.
 *
 * adm_gain_limit_product() returns that value from 64-bit integer arithmetic,
 * for a backend whose device code may not hold a double (SYCL, ADR-0220). It
 * is not an approximation: for every limit the option admits and every int32
 * sample it returns what the scalar's double product truncates to.
 *
 * Derivation. Write gain = M * 2^-s with the 53-bit significand M in
 * [2^52, 2^53), and a = |rst| <= 2^31. The exact product is P * 2^-s with
 * P = a * M < 2^84, its integer part N = P >> s and its fraction
 * F = P mod 2^s. fl() rounds the exact product to 53 significant bits, which
 * moves it by at most half a unit in the last place, so the truncated result
 * is N or N + 1, and it is N + 1 exactly when the product lies within half a
 * unit in the last place below N + 1 (a tie rounds to N + 1, whose significand
 * is even). With b the bit length of N that half unit is 2^(b - 54), so
 *
 *     round up  <=>  2^s - F <= 2^(s + b - 54)
 *               <=>  G <= 2^b,   G = (2^s - F) * 2^(54 - s)
 *               <=>  bitlen(G - 1) <= bitlen(N)
 *
 * and "bitlen(x) <= bitlen(y)" is "not (y < x and y < (x xor y))", which needs
 * no bit scan. An integral limit (the 1 and 100 of the shipped models) gives
 * F = 0 and never rounds.
 */

#ifndef LIBVMAF_FEATURE_ADM_GAIN_LIMIT_H_
#define LIBVMAF_FEATURE_ADM_GAIN_LIMIT_H_

#include <math.h>
#include <stdint.h>

/* The gain limit as significand * 2^-frac_bits, the significand in two halves
 * so that a device without 128-bit integers can multiply by it. No typedef:
 * the header is included from C and from C++ (SYCL) translation units, and
 * `struct AdmGainLimit` reads the same in both. */
struct AdmGainLimit {
    uint32_t m_hi;     /* significand bits 52..32 */
    uint32_t m_lo;     /* significand bits 31..0 */
    int32_t frac_bits; /* 46 for a limit of 100, 52 for a limit of 1 */
};

/* Splits `gain` on the host. adm_gain_limit_product() needs
 * 32 <= frac_bits <= 54, that is a limit in [0.25, 2^21); the option admits
 * [1, 100]. */
static inline struct AdmGainLimit adm_gain_limit_split(double gain)
{
    int exponent = 0;
    const double fraction = frexp(gain, &exponent); /* gain = fraction * 2^exponent */
    const uint64_t significand = (uint64_t)ldexp(fraction, 53);
    const struct AdmGainLimit g = {.m_hi = (uint32_t)(significand >> 32),
                                   .m_lo = (uint32_t)(significand & 0xFFFFFFFFu),
                                   .frac_bits = 53 - exponent};
    return g;
}

/* (int64_t)((double)rst * gain): the double product of a sample and the gain
 * limit, truncated toward zero. Integer arithmetic only. */
static inline int64_t adm_gain_limit_product(int32_t rst, struct AdmGainLimit g)
{
    const uint64_t a = (rst < 0) ? (uint64_t)(-(int64_t)rst) : (uint64_t)rst;
    /* P = a * M as hi * 2^32 + (p_lo mod 2^32). */
    const uint64_t p_lo = a * g.m_lo;
    const uint64_t hi = (a * g.m_hi) + (p_lo >> 32);
    const int s_hi = g.frac_bits - 32;
    const uint64_t n = hi >> s_hi;
    const uint64_t frac = ((hi & ((UINT64_C(1) << s_hi) - 1u)) << 32) | (p_lo & 0xFFFFFFFFu);
    /* G - 1, at most 2^54 - 1. */
    const uint64_t gap = (((UINT64_C(1) << g.frac_bits) - frac) << (54 - g.frac_bits)) - 1u;
    const int stays = (n < gap) && (n < (n ^ gap));
    const int64_t mag = (int64_t)(n + (stays ? 0u : 1u));
    return (rst < 0) ? -mag : mag;
}

#endif /* LIBVMAF_FEATURE_ADM_GAIN_LIMIT_H_ */
