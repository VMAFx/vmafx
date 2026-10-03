/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  The two integers integer_vif.c derives from a pixel's gain, for the
 *  integer VIF Metal kernels, without an fp64 type (Metal has none, MSL
 *  specification 4.1, section 2.1). The reference
 *  (integer_vif.c::vif_accumulate_pixel(), and the same lines in
 *  x86/vif_avx2.c and x86/vif_avx512.c) computes
 *
 *      const double eps = 65536 * 1.0e-10;
 *      double g = sigma12 / (sigma1_sq + eps);
 *      int32_t sv_sq = sigma2_sq - g * sigma12;
 *      sv_sq = (uint32_t)(MAX(sv_sq, 0));
 *      g = MIN(g, vif_enhn_gain_limit);
 *      ... (int64_t)((g * g * sigma1_sq)) ...
 *
 *  Both results are truncations of fp64 values. This is
 *  core/src/feature/sycl/sycl_integer_vif_math.h statement for statement
 *  (ADR-1432, ported by ADR-1498): one integer division gives both integer
 *  parts, unless the exact value lies within the fp64 chain's own rounding
 *  error of an integer; those samples (one pixel in about 300 000 on real
 *  content) replay the reference's fp64 operations in 64-bit integers
 *  (metal_soft_double.h), so the result is the reference's on every sample
 *  by construction.
 *
 *  Preconditions, which are integer_vif.c's for these lines: sigma1_sq is at
 *  least 2 * 65536, sigma2_sq and sigma12 are positive, all three are below
 *  2^31, and the gain limit is at least 1.
 *
 *  Written on metal_portable.h in what Metal Shading Language, C and C++
 *  share (values in and values out, typedef'd structs built by _make()
 *  functions, no `double`, no `long long`, no 64-bit multiply-high: the
 *  products are 32x32 bit or written in limbs by metal_soft_double.h).
 *  vmaf_mtl_ivif_make_gain_limit() is host code and the only place that
 *  names `double`; it is not part of what a kernel compiles.
 *
 *  Name mapping, SYCL (namespace vmaf_sycl_ivif) -> Metal:
 *    kEpsMant, kEpsExp             -> VMAF_MTL_IVIF_EPS_MANT, _EPS_EXP
 *    Divisor, divisor_of           -> VmafMtlIvifDivisor, vmaf_mtl_ivif_divisor_of
 *    GainLimit, make_gain_limit    -> VmafMtlGainLimit, vmaf_mtl_ivif_make_gain_limit
 *                                     (no `squared` member: the SYCL header
 *                                     never reads it)
 *    GainTerms                     -> VmafMtlGainTerms
 *    gain_terms_replayed           -> vmaf_mtl_ivif_gain_terms_replayed
 *    Quotient, divide              -> VmafMtlIvifQuotient, vmaf_mtl_ivif_divide
 *    GainResult, LimitTest         -> VmafMtlIvifGainResult, VmafMtlIvifLimitTest
 *    limit_test                    -> vmaf_mtl_ivif_limit_test
 *    kSub                          -> VMAF_MTL_IVIF_SUB
 *    gain_terms_integer            -> vmaf_mtl_ivif_gain_terms_integer
 *    gain_terms                    -> vmaf_mtl_ivif_gain_terms
 */

#ifndef VMAF_FEATURE_METAL_METAL_INTEGER_VIF_GAIN_H_
#define VMAF_FEATURE_METAL_METAL_INTEGER_VIF_GAIN_H_

#include "metal_portable.h"
#include "metal_soft_double.h"

/* 65536 * 1.0e-10, the reference's `eps`, as EPS_MANT * 2^EPS_EXP. */
#define VMAF_MTL_IVIF_EPS_MANT VMAF_MTL_U64(0x1b7cdfd9d7bdbb)
#define VMAF_MTL_IVIF_EPS_EXP (-70)

/* Fractional bits of the fixed-point remainders. */
#define VMAF_MTL_IVIF_SUB 8u

/* 2^-18, written out: a hexadecimal float literal is C++17 and C99 only. */
#define VMAF_MTL_IVIF_MARGIN 3.814697265625e-06f

/* fl64(sigma1_sq + eps): the sum stays in sigma1_sq's binade, so it is
 * sigma1_sq with eps rounded to that binade's last place. */
typedef struct VmafMtlIvifDivisor {
    VmafMtlSoftDouble value;
    vmaf_mtl_u32 eps_units; /* the rounded eps, in units of the last place */
    vmaf_mtl_i32 binade;    /* floor(log2(sigma1_sq)) */
} VmafMtlIvifDivisor;

/* The gain limit as the kernels take it. 32 bytes, 8-aligned, the same
 * layout in Metal Shading Language and in the host's C and C++. */
typedef struct VmafMtlGainLimit {
    VmafMtlSoftDouble value; /* vif_enhn_gain_limit */
    /* The limit when it is an integer below 2^11, else 0. `g < limit` is then
     * an integer comparison, and limit * limit * sigma1_sq an integer below
     * 2^53, which fp64 holds exactly. */
    vmaf_mtl_u32 integer;
    float hi; /* the limit as a pair, for the other limits */
    float lo;
    vmaf_mtl_u32 reserved; /* keeps the size a multiple of 8 */
} VmafMtlGainLimit;

typedef struct VmafMtlGainTerms {
    vmaf_mtl_u32 sv_sq;    /* (uint32_t)MAX((int32_t)(sigma2_sq - g * sigma12), 0) */
    vmaf_mtl_i64 gg_sigma; /* (int64_t)(g * g * sigma1_sq), g after the limit */
} VmafMtlGainTerms;

typedef struct VmafMtlIvifQuotient {
    vmaf_mtl_u64 quot;
    vmaf_mtl_u32 rem;
} VmafMtlIvifQuotient;

typedef struct VmafMtlIvifGainResult {
    VmafMtlGainTerms terms;
    bool replay; /* the integer evaluation does not decide: take the replay */
} VmafMtlIvifGainResult;

/* `g < limit` and whether that decision is certain. */
typedef struct VmafMtlIvifLimitTest {
    bool unlimited;
    bool certain;
} VmafMtlIvifLimitTest;

VMAF_MTL_FUNC VmafMtlIvifDivisor vmaf_mtl_ivif_divisor_of(vmaf_mtl_u32 sigma1_sq)
{
    const vmaf_mtl_i32 binade = 31 - (vmaf_mtl_i32)vmaf_mtl_soft_clz32(sigma1_sq);
    /* The last place of the sum is 2^(binade - 52). */
    const vmaf_mtl_u32 shift = (vmaf_mtl_u32)(binade - 52 - VMAF_MTL_IVIF_EPS_EXP);
    const vmaf_mtl_u64 mant = VMAF_MTL_IVIF_EPS_MANT;
    const bool half_bit = ((mant >> (shift - 1u)) & VMAF_MTL_U64(1)) != VMAF_MTL_U64(0);
    const bool sticky =
        (mant & ((VMAF_MTL_U64(1) << (shift - 1u)) - VMAF_MTL_U64(1))) != VMAF_MTL_U64(0);
    const vmaf_mtl_u64 units =
        vmaf_mtl_round_kept(vmaf_mtl_shifted_make(mant >> shift, half_bit, sticky));
    const vmaf_mtl_u64 sum = ((vmaf_mtl_u64)sigma1_sq << (vmaf_mtl_u32)(52 - binade)) + units;
    VmafMtlIvifDivisor out;
    out.value = vmaf_mtl_soft_make(sum, binade - 52);
    out.eps_units = (vmaf_mtl_u32)units;
    out.binade = binade;
    return out;
}

/* ------------------------------------------------------------------ */
/* The reference's operations, replayed                                */
/* ------------------------------------------------------------------ */

VMAF_MTL_FUNC VmafMtlGainTerms vmaf_mtl_ivif_gain_terms_replayed(vmaf_mtl_u32 sigma1_sq,
                                                                 vmaf_mtl_u32 sigma2_sq,
                                                                 vmaf_mtl_u32 sigma12,
                                                                 VmafMtlGainLimit limit)
{
    const VmafMtlSoftDouble covariance = vmaf_mtl_soft_from_u32(sigma12);
    const VmafMtlSoftDouble g =
        vmaf_mtl_soft_div(covariance, vmaf_mtl_ivif_divisor_of(sigma1_sq).value);
    const VmafMtlSoftDouble t = vmaf_mtl_soft_mul(g, covariance);
    const bool positive = vmaf_mtl_soft_less(t, vmaf_mtl_soft_from_u32(sigma2_sq));
    const vmaf_mtl_u32 sv_sq = positive ? vmaf_mtl_soft_sub_trunc(sigma2_sq, t) : 0u;
    const bool unlimited = vmaf_mtl_soft_less(g, limit.value);
    const vmaf_mtl_u64 g_mant = unlimited ? g.mant : limit.value.mant;
    const vmaf_mtl_i32 g_exp = unlimited ? g.exp : limit.value.exp;
    const VmafMtlSoftDouble g_limited = vmaf_mtl_soft_make(g_mant, g_exp);
    const VmafMtlSoftDouble squared = vmaf_mtl_soft_mul(g_limited, g_limited);
    const VmafMtlSoftDouble scaled = vmaf_mtl_soft_mul(squared, vmaf_mtl_soft_from_u32(sigma1_sq));
    VmafMtlGainTerms out;
    out.sv_sq = sv_sq;
    out.gg_sigma = vmaf_mtl_soft_trunc(scaled);
    return out;
}

/* ------------------------------------------------------------------ */
/* The integer evaluation                                              */
/* ------------------------------------------------------------------ */

/* floor(p / d) and the remainder for p below 2^62 and d in [2^17, 2^31).
 * Two fp32 estimates and an integer correction: a GPU has no 64-bit divider. */
VMAF_MTL_FUNC VmafMtlIvifQuotient vmaf_mtl_ivif_divide(vmaf_mtl_u64 p, vmaf_mtl_u32 d)
{
    const float df = (float)d;
    const vmaf_mtl_i64 divisor = (vmaf_mtl_i64)d;
    const vmaf_mtl_u64 first = (vmaf_mtl_u64)((float)p / df);
    vmaf_mtl_i64 rem = (vmaf_mtl_i64)p - (vmaf_mtl_i64)(first * (vmaf_mtl_u64)d);
    const vmaf_mtl_i64 second = (vmaf_mtl_i64)((float)rem / df);
    vmaf_mtl_i64 quot = (vmaf_mtl_i64)first + second;
    rem -= second * divisor;
    for (vmaf_mtl_i32 fix = 0; fix < 4; fix++) {
        const bool low = rem < 0;
        const bool high = rem >= divisor;
        quot += high ? VMAF_MTL_I64(1) : (low ? VMAF_MTL_I64(-1) : VMAF_MTL_I64(0));
        rem += low ? divisor : (high ? -divisor : VMAF_MTL_I64(0));
    }
    VmafMtlIvifQuotient out;
    out.quot = (vmaf_mtl_u64)quot;
    out.rem = (vmaf_mtl_u32)rem;
    return out;
}

VMAF_MTL_FUNC VmafMtlIvifLimitTest vmaf_mtl_ivif_limit_test(vmaf_mtl_u32 sigma1_sq,
                                                            vmaf_mtl_u32 sigma12, float eps_r,
                                                            VmafMtlGainLimit limit)
{
    VmafMtlIvifLimitTest out;
    if (limit.integer != 0u) {
        /* sigma12 / (sigma1_sq + eps) < L  <=>  sigma12 <= L * sigma1_sq: the
         * two sides of the first differ by at least eps / sigma1_sq, far more
         * than an fp64 rounding. */
        out.unlimited = (vmaf_mtl_u64)sigma12 <= (vmaf_mtl_u64)limit.integer * sigma1_sq;
        out.certain = true;
        return out;
    }
    /* Any other limit: compare in fp32 and trust the comparison only when it
     * is clear. limit * (sigma1_sq + eps) against sigma12. */
    const float bound = limit.hi * ((float)sigma1_sq + eps_r) + limit.lo * (float)sigma1_sq;
    const float value = (float)sigma12;
    const float margin = value * VMAF_MTL_IVIF_MARGIN;
    out.unlimited = value < bound;
    out.certain = VMAF_MTL_FABS(value - bound) > margin;
    return out;
}

/* 2^e as an fp32 value, for e in [-126, 127]: the exponent field written out,
 * so the value is exact and no library function is involved. */
VMAF_MTL_FUNC float vmaf_mtl_ivif_pow2(vmaf_mtl_i32 e)
{
    return VMAF_MTL_U2F((vmaf_mtl_u32)(127 + e) << 23);
}

VMAF_MTL_FUNC VmafMtlIvifGainResult vmaf_mtl_ivif_gain_terms_integer(vmaf_mtl_u32 sigma1_sq,
                                                                     vmaf_mtl_u32 sigma2_sq,
                                                                     vmaf_mtl_u32 sigma12,
                                                                     VmafMtlGainLimit limit)
{
    const VmafMtlIvifDivisor divisor = vmaf_mtl_ivif_divisor_of(sigma1_sq);
    const vmaf_mtl_u64 product = (vmaf_mtl_u64)sigma12 * sigma12;
    const VmafMtlIvifQuotient q = vmaf_mtl_ivif_divide(product, sigma1_sq);
    const vmaf_mtl_i64 divisor_fp = (vmaf_mtl_i64)sigma1_sq << VMAF_MTL_IVIF_SUB;
    /* eps, as rounded into the divisor, and c = product * eps / divisor: what
     * eps moves the two quotients by, in units of 2^-SUB / sigma1_sq. The
     * fp32 chain is good to 2^-21 of c; the conversion drops less than one
     * unit. */
    const float eps_r = (float)divisor.eps_units * vmaf_mtl_ivif_pow2(divisor.binade - 52);
    const float shift_f =
        (float)product / (float)sigma1_sq * eps_r * (float)(1u << VMAF_MTL_IVIF_SUB);
    const vmaf_mtl_i64 shift = (vmaf_mtl_i64)shift_f;
    const vmaf_mtl_i64 shift_error = (shift >> 21) + 2;

    /* sigma2_sq - product / (sigma1_sq + eps) = qa + (ra + c) / sigma1_sq.
     * The fp64 chain is within 2^-20 of it (three roundings of values below
     * 2^31). Only a positive integer is a boundary: around 0 the truncation
     * gives 0 from both sides, and a negative value is clamped. */
    const vmaf_mtl_i64 qa = (vmaf_mtl_i64)sigma2_sq - (vmaf_mtl_i64)q.quot - (q.rem != 0u ? 1 : 0);
    const vmaf_mtl_i64 ra = q.rem != 0u ? (vmaf_mtl_i64)sigma1_sq - (vmaf_mtl_i64)q.rem : 0;
    const vmaf_mtl_i64 sv_zone =
        ((vmaf_mtl_i64)sigma1_sq >> (20u - VMAF_MTL_IVIF_SUB)) + 1 + shift_error;
    const vmaf_mtl_i64 sv_frac = (ra << VMAF_MTL_IVIF_SUB) + shift;
    const bool carry = sv_frac >= divisor_fp;
    const bool sv_low = qa >= 1 && sv_frac < sv_zone;
    const vmaf_mtl_i64 sv_gap = sv_frac - divisor_fp;
    const bool sv_high = qa >= 0 && sv_gap < sv_zone && sv_gap > -sv_zone;
    const vmaf_mtl_u32 sv_sq = qa >= 0 ? (vmaf_mtl_u32)(qa + (carry ? 1 : 0)) : 0u;

    const VmafMtlIvifLimitTest test = vmaf_mtl_ivif_limit_test(sigma1_sq, sigma12, eps_r, limit);
    /* g * g * sigma1_sq = quot + (rem - 2c) / sigma1_sq below the limit; the
     * fp64 chain is within 2^-51 of it, relatively (four roundings). */
    const vmaf_mtl_i64 gg_zone =
        (vmaf_mtl_i64)(product >> (51u - VMAF_MTL_IVIF_SUB)) + 1 + 2 * shift_error;
    const vmaf_mtl_i64 gg_frac = ((vmaf_mtl_i64)q.rem << VMAF_MTL_IVIF_SUB) - 2 * shift;
    const bool borrow = gg_frac < 0;
    const bool gg_near = (gg_frac < gg_zone && gg_frac > -gg_zone) ||
                         gg_frac > divisor_fp - gg_zone || gg_frac < gg_zone - divisor_fp;
    const vmaf_mtl_i64 unlimited_gg = (vmaf_mtl_i64)q.quot - (borrow ? 1 : 0);
    /* At an integer limit the product is exact. Any other limit rounds, and
     * that product is left to the replay. */
    const vmaf_mtl_i64 limited_gg =
        (vmaf_mtl_i64)((vmaf_mtl_u64)limit.integer * limit.integer) * (vmaf_mtl_i64)sigma1_sq;
    const bool limited_rounds = !test.unlimited && limit.integer == 0u;

    VmafMtlIvifGainResult out;
    out.replay =
        sv_low || sv_high || !test.certain || (test.unlimited && gg_near) || limited_rounds;
    out.terms.sv_sq = sv_sq;
    out.terms.gg_sigma = test.unlimited ? unlimited_gg : limited_gg;
    return out;
}

/* The two integers of integer_vif.c for one pixel of the log branch. */
VMAF_MTL_FUNC VmafMtlGainTerms vmaf_mtl_ivif_gain_terms(vmaf_mtl_u32 sigma1_sq,
                                                        vmaf_mtl_u32 sigma2_sq,
                                                        vmaf_mtl_u32 sigma12,
                                                        VmafMtlGainLimit limit)
{
    const VmafMtlIvifGainResult quick =
        vmaf_mtl_ivif_gain_terms_integer(sigma1_sq, sigma2_sq, sigma12, limit);
    if (!quick.replay) {
        return quick.terms;
    }
    return vmaf_mtl_ivif_gain_terms_replayed(sigma1_sq, sigma2_sq, sigma12, limit);
}

#if !defined(__METAL_VERSION__)
/* Host code: the limit in the forms above. */
VMAF_MTL_FUNC VmafMtlGainLimit vmaf_mtl_ivif_make_gain_limit(double limit)
{
    VmafMtlGainLimit out;
    int exponent = 0;
    const double fraction = frexp(limit, &exponent);
    out.value = vmaf_mtl_soft_make((vmaf_mtl_u64)ldexp(fraction, 53), exponent - 53);
    const bool is_integer = limit == floor(limit) && limit < 2048.0;
    out.integer = is_integer ? (vmaf_mtl_u32)limit : 0u;
    out.hi = (float)limit;
    out.lo = (float)(limit - (double)out.hi);
    out.reserved = 0u;
    return out;
}
#endif

#endif /* VMAF_FEATURE_METAL_METAL_INTEGER_VIF_GAIN_H_ */
