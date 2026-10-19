/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  The float ADM reference's per-sample arithmetic (adm_tools.c), operation
 *  for operation, for Metal kernels, which have no fp64 type (Metal Shading
 *  Language Specification 4.1, section 2.1). This is
 *  core/src/feature/sycl/sycl_float_adm_math.h (ADR-1434) written on
 *  metal_portable.h and metal_soft_double.h, so the one header compiles as
 *  Metal Shading Language in float_adm.metal and as C or C++ on the host,
 *  where core/test/test_metal_float_adm_math.cpp holds it against
 *  adm_decouple_s(), adm_csf_s(), adm_csf_den_scale_s() and adm_cm_s() value
 *  by value (ADR-1498). The SYCL header is written against sycl:: and cannot
 *  be included from Metal: the two are one behaviour in two files, and a
 *  change to one changes the other in the same PR.
 *
 *  What the header holds, as the SYCL header does:
 *
 *   - divs() is DIVS(): the IEEE fp32 quotient (ADR-1442), which `/` is
 *     under the kernels' -fno-fast-math (Table 8.1). Never a reciprocal.
 *   - angle_flag() and decouple_band() are adm_angle_flag_s() and
 *     adm_decouple_band_s(): the threshold is (cos^2 * |o|^2) * |t|^2 in that
 *     association, the clamp is the reference's two ternaries.
 *   - csf_flt() and thresh_band() are adm_csf_s() and adm_cm_thresh3x3_s().
 *   - den_term() and cm_term() are the terms adm_csf_den_scale_s() and
 *     adm_cm_s() accumulate. At adm_p_norm = 3 they are (x * x) * x in fp32
 *     and at 1 they are x. Any other adm_p_norm is pow() on both sides and
 *     the device's pow is not glibc's: that option is close to the reference,
 *     not equal.
 *
 *  Three expressions of the reference are fp64, because a constant or an
 *  argument in them is a `double`, and each is rounded to fp32 once:
 *
 *      rst * adm_enhn_gain_limit                    (the enhancement gain)
 *      FLOAT_ONE_BY_30 * fabsf(csf)                 (the filtered value)
 *      sum + FLOAT_ONE_BY_15 * fabsf(centre)        (the centre tap)
 *
 *  The first is an fp32 product whenever the limit is an fp32 value (the
 *  default 100 and the models' 1 are): the fp64 product is then exact. The
 *  other two are evaluated as exact fp32 pairs; a pair that lies next to an
 *  fp32 rounding boundary, and any limit that is not an fp32 value, replays
 *  the reference's fp64 operations in 64-bit integers
 *  (metal_soft_double.h). So every result is the reference's by
 *  construction.
 *
 *  The pair operations are written here, statement for statement as
 *  sycl_exact_fp.h's two_sum() / quick_two_sum() / two_prod() / ff_add():
 *  feature/ff_pair.h is namespace and designated-initializer C++ that Metal
 *  Shading Language (C++14-based) does not accept. They need every operation
 *  to round on its own, which the kernels' -ffp-contract=off and the host
 *  test's strict FP flags give.
 *
 *  Name mapping, SYCL (namespace vmaf_sycl_fadm / vmaf_sycl_exact) -> Metal:
 *    vmaf_sycl_soft::f / SoftDouble        -> vmaf_mtl_f / VmafMtlSoftDouble,
 *                                             built by vmaf_mtl_soft_make()
 *    kX                                    -> VMAF_MTL_SOFT_X (the kernels use
 *                                             kFastLow as VMAF_MTL_FADM_FAST_LOW)
 *    struct Ff                             -> VmafMtlFadmFf (vmaf_mtl_fadm_ff())
 *    two_sum, quick_two_sum, two_prod,
 *    ff_add                                -> vmaf_mtl_fadm_<name>
 *    struct DoubleValue, kOneBy30/15       -> VmafMtlFadmConst,
 *                                             vmaf_mtl_fadm_one_by_30() / _15()
 *    struct GainLimit, make_gain_limit()   -> VmafMtlFadmGainLimit (host only:
 *                                             vmaf_mtl_fadm_make_gain_limit())
 *    undecided, scaled_pair, scaled_exact,
 *    times_constant, add_scaled,
 *    divs, angle_flag, gain_limited,
 *    decouple_band, csf_flt, decouple_csf,
 *    thresh_band, before, after, pnorm,
 *    den_term, cm_term                     -> vmaf_mtl_fadm_<name>
 *    struct CsfSample, Neighbours          -> VmafMtlFadmCsfSample,
 *                                             VmafMtlFadmNeighbours
 *    decouple_sample(), store_terms()      -> vmaf_mtl_fadm_decouple_sample(),
 *                                             vmaf_mtl_fadm_band_terms(): values
 *                                             in, values out; the kernel does
 *                                             the buffer reads and writes
 *    sycl::fabs / sycl::pow / isfinite     -> VMAF_MTL_FABS / VMAF_MTL_FADM_POW /
 *                                             vmaf_mtl_fadm_finite()
 *
 *  A kernel-safe header: no array, no pointer, no select between structs, no
 *  double outside the host-only section at the end, no scratch memory.
 */

#ifndef VMAF_FEATURE_METAL_METAL_FLOAT_ADM_MATH_H_
#define VMAF_FEATURE_METAL_METAL_FLOAT_ADM_MATH_H_

#include "metal_portable.h"
#include "metal_soft_double.h"

#if defined(__METAL_VERSION__)
#define VMAF_MTL_FADM_POW(x, p) metal::pow((x), (p))
#else
#define VMAF_MTL_FADM_POW(x, p) powf((x), (p))
#endif

/* The decouple's epsilon: `const float eps = 1e-30`. */
#define VMAF_MTL_FADM_EPS 1e-30f

/* 2^-100: below it the pair arithmetic is not exact (sycl_exact_fp.h kFastLow). */
#define VMAF_MTL_FADM_FAST_LOW_BITS 0x0D800000u

/* 2^-18, the margin of undecided(). */
#define VMAF_MTL_FADM_MARGIN 3.814697265625e-06f

/* Term slots of one sample: |csf(ref)|^p, the masked restored signal, the
 * masked additive signal, three bands each. */
#define VMAF_MTL_FADM_SLOT_DEN 0u
#define VMAF_MTL_FADM_SLOT_CM 3u
#define VMAF_MTL_FADM_SLOT_AIM 6u
#define VMAF_MTL_FADM_TERM_SLOTS 9u

/* ------------------------------------------------------------------ */
/* Exact fp32 pairs                                                    */
/* ------------------------------------------------------------------ */

typedef struct VmafMtlFadmFf {
    float hi;
    float lo;
} VmafMtlFadmFf;

VMAF_MTL_FUNC VmafMtlFadmFf vmaf_mtl_fadm_ff(float hi, float lo)
{
    const VmafMtlFadmFf pair = {hi, lo};
    return pair;
}

VMAF_MTL_FUNC VmafMtlFadmFf vmaf_mtl_fadm_two_sum(float a, float b)
{
    const float sum = a + b;
    const float b_virtual = sum - a;
    const float a_virtual = sum - b_virtual;
    const float b_error = b - b_virtual;
    const float a_error = a - a_virtual;
    return vmaf_mtl_fadm_ff(sum, a_error + b_error);
}

VMAF_MTL_FUNC VmafMtlFadmFf vmaf_mtl_fadm_quick_two_sum(float a, float b)
{
    const float sum = a + b;
    const float rebuilt = sum - a;
    return vmaf_mtl_fadm_ff(sum, b - rebuilt);
}

VMAF_MTL_FUNC VmafMtlFadmFf vmaf_mtl_fadm_two_prod(float a, float b)
{
    const float product = a * b;
    return vmaf_mtl_fadm_ff(product, VMAF_MTL_FMA(a, b, -product));
}

VMAF_MTL_FUNC VmafMtlFadmFf vmaf_mtl_fadm_ff_add(VmafMtlFadmFf a, VmafMtlFadmFf b)
{
    const VmafMtlFadmFf high = vmaf_mtl_fadm_two_sum(a.hi, b.hi);
    const VmafMtlFadmFf low = vmaf_mtl_fadm_two_sum(a.lo, b.lo);
    const VmafMtlFadmFf first = vmaf_mtl_fadm_quick_two_sum(high.hi, high.lo + low.hi);
    return vmaf_mtl_fadm_quick_two_sum(first.hi, low.lo + first.lo);
}

/* True for a finite float (an infinity or NaN has every exponent bit set). */
VMAF_MTL_FUNC bool vmaf_mtl_fadm_finite(float x)
{
    return (VMAF_MTL_F2U(x) & 0x7F800000u) != 0x7F800000u;
}

VMAF_MTL_FUNC float vmaf_mtl_fadm_fast_low(void)
{
    return VMAF_MTL_U2F(VMAF_MTL_FADM_FAST_LOW_BITS);
}

/* ------------------------------------------------------------------ */
/* fp64 constants and the gain limit, as the kernels take them         */
/* ------------------------------------------------------------------ */

/* A positive fp64 value as a pair of floats (good to 2^-48) and exactly. */
typedef struct VmafMtlFadmConst {
    float hi;
    float lo;
    vmaf_mtl_u64 mant; /* the value is mant * 2^exp, mant in [2^52, 2^53) */
    vmaf_mtl_i32 exp;
} VmafMtlFadmConst;

VMAF_MTL_FUNC VmafMtlFadmConst vmaf_mtl_fadm_const(vmaf_mtl_u32 hi_bits, vmaf_mtl_u32 lo_bits,
                                                   vmaf_mtl_u32 mant_hi, vmaf_mtl_u32 mant_lo,
                                                   vmaf_mtl_i32 exp)
{
    const vmaf_mtl_u64 mant = (VMAF_MTL_U64(mant_hi) << 32) | VMAF_MTL_U64(mant_lo);
    const VmafMtlFadmConst c = {VMAF_MTL_U2F(hi_bits), VMAF_MTL_U2F(lo_bits), mant, exp};
    return c;
}

/* adm_tools.c's FLOAT_ONE_BY_30 (0.0333333351) and FLOAT_ONE_BY_15
 * (0.0666666701): double literals, so not the fp32 values of those names.
 * hi + lo are 0x1.111112p-5 + 0x1.f00fep-36 and 0x1.111112p-4 - 0x1.7f8c18p-35. */
VMAF_MTL_FUNC VmafMtlFadmConst vmaf_mtl_fadm_one_by_30(void)
{
    return vmaf_mtl_fadm_const(0x3d088889u, 0x2df807f0u, 0x00111111u, 0x203e01fcu, -57);
}

VMAF_MTL_FUNC VmafMtlFadmConst vmaf_mtl_fadm_one_by_15(void)
{
    return vmaf_mtl_fadm_const(0x3d888889u, 0xae3fc60cu, 0x00111111u, 0x1fd00e7du, -56);
}

/* adm_enhn_gain_limit. */
typedef struct VmafMtlFadmGainLimit {
    float value;           /* (float)limit */
    vmaf_mtl_i32 is_float; /* the limit is an fp32 value: rst * limit is exact in fp64 */
    vmaf_mtl_u64 mant;     /* the limit itself, for the others */
    vmaf_mtl_i32 exp;
} VmafMtlFadmGainLimit;

/* ------------------------------------------------------------------ */
/* The two fp64 products with a constant                               */
/* ------------------------------------------------------------------ */

/* True when the pair does not decide its rounding to fp32: it is within
 * 2^-18 of an fp32 step of the point where the rounding changes, or too small
 * for the pair to be exact. `sum` is positive and normalised. */
VMAF_MTL_FUNC bool vmaf_mtl_fadm_undecided(VmafMtlFadmFf sum)
{
    if (!(sum.hi >= vmaf_mtl_fadm_fast_low()) || !vmaf_mtl_fadm_finite(sum.hi)) {
        return true;
    }
    const vmaf_mtl_u32 bits = VMAF_MTL_F2U(sum.hi);
    const vmaf_mtl_u32 exponent = bits & 0x7F800000u;
    const float unit = VMAF_MTL_U2F(exponent - (23u << 23));
    /* Below a power of two the spacing halves. */
    const bool lower_binade = (bits & 0x007FFFFFu) == 0u && sum.lo < 0.0f;
    const float half_step = lower_binade ? 0.25f * unit : 0.5f * unit;
    const float margin = unit * VMAF_MTL_FADM_MARGIN;
    return VMAF_MTL_FABS(sum.lo) >= half_step - margin;
}

/* constant * a as a pair, for a positive normal a. */
VMAF_MTL_FUNC VmafMtlFadmFf vmaf_mtl_fadm_scaled_pair(float a, VmafMtlFadmConst c)
{
    const VmafMtlFadmFf product = vmaf_mtl_fadm_two_prod(a, c.hi);
    const float cross = a * c.lo;
    return vmaf_mtl_fadm_quick_two_sum(product.hi, product.lo + cross);
}

/* fl64(constant * a), exactly. */
VMAF_MTL_FUNC VmafMtlSoftDouble vmaf_mtl_fadm_scaled_exact(float a, VmafMtlFadmConst c)
{
    return vmaf_mtl_soft_mul(vmaf_mtl_soft_from_float_any(a), vmaf_mtl_soft_make(c.mant, c.exp));
}

/* (float)(constant * (double)a) by replaying the fp64 product, for a positive
 * finite a. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_times_constant_replayed(float a, VmafMtlFadmConst c)
{
    return vmaf_mtl_soft_to_float_any(vmaf_mtl_fadm_scaled_exact(a, c));
}

/* (float)((double)sum + constant * (double)a) by replaying the fp64 product
 * and sum, for positive finite operands. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_add_scaled_replayed(float sum, float a, VmafMtlFadmConst c)
{
    return vmaf_mtl_soft_to_float_any(
        vmaf_mtl_soft_add(vmaf_mtl_soft_from_float_any(sum), vmaf_mtl_fadm_scaled_exact(a, c)));
}

/* (float)(constant * (double)a) for a >= 0. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_times_constant(float a, VmafMtlFadmConst c)
{
    if (a == 0.0f) {
        return 0.0f;
    }
    if (!vmaf_mtl_fadm_finite(a)) {
        return a;
    }
    const VmafMtlFadmFf pair = vmaf_mtl_fadm_scaled_pair(a, c);
    if (!vmaf_mtl_fadm_undecided(pair)) {
        return pair.hi;
    }
    return vmaf_mtl_fadm_times_constant_replayed(a, c);
}

/* (float)((double)sum + constant * (double)a) for sum >= 0 and a >= 0. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_add_scaled(float sum, float a, VmafMtlFadmConst c)
{
    if (a == 0.0f) {
        return sum; /* the product is +0 and the fp64 sum is sum itself */
    }
    if (sum == 0.0f) {
        return vmaf_mtl_fadm_times_constant(a, c);
    }
    if (!vmaf_mtl_fadm_finite(a) || !vmaf_mtl_fadm_finite(sum)) {
        return sum + a;
    }
    const VmafMtlFadmFf pair =
        vmaf_mtl_fadm_ff_add(vmaf_mtl_fadm_ff(sum, 0.0f), vmaf_mtl_fadm_scaled_pair(a, c));
    const float low = vmaf_mtl_fadm_fast_low();
    const bool small = !(a >= low) || !(sum >= low);
    if (!small && !vmaf_mtl_fadm_undecided(pair)) {
        return pair.hi;
    }
    return vmaf_mtl_fadm_add_scaled_replayed(sum, a, c);
}

/* ------------------------------------------------------------------ */
/* The reference's arithmetic                                          */
/* ------------------------------------------------------------------ */

/* DIVS(n, d): the IEEE fp32 quotient (ADR-1442). */
VMAF_MTL_FUNC float vmaf_mtl_fadm_divs(float n, float d)
{
    return n / d;
}

/* adm_angle_flag_s() with ADM_OPT_AVOID_ATAN: the angle between (oh, ov) and
 * (th, tv) is below one degree. */
VMAF_MTL_FUNC bool vmaf_mtl_fadm_angle_flag(float oh, float ov, float th, float tv,
                                            float cos_1deg_sq)
{
    const float oh_th = oh * th;
    const float ov_tv = ov * tv;
    const float ot_dp = oh_th + ov_tv;
    const float oh_sq = oh * oh;
    const float ov_sq = ov * ov;
    const float o_mag_sq = oh_sq + ov_sq;
    const float th_sq = th * th;
    const float tv_sq = tv * tv;
    const float t_mag_sq = th_sq + tv_sq;
    const float lhs = ot_dp * ot_dp;
    const float scaled = cos_1deg_sq * o_mag_sq;
    const float rhs = scaled * t_mag_sq;
    return (ot_dp >= 0.0f) && (lhs >= rhs);
}

/* (float)MIN(rst * limit, t) for rst > 0, (float)MAX(rst * limit, t) for
 * rst < 0, the product and the comparison in fp64. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_gain_limited(float rst, float t, VmafMtlFadmGainLimit limit)
{
    const float gained_f = rst * limit.value;
    if (limit.is_float != 0) {
        /* The fp64 product is exact, so its rounding is this fp32 product's,
         * and comparing the rounded product with t selects as the exact
         * comparison does. */
        if (rst > 0.0f) {
            return gained_f < t ? gained_f : t;
        }
        return gained_f > t ? gained_f : t;
    }
    /* The product keeps rst's sign. It is selected when it is closer to zero
     * than t and t has its sign; otherwise t is. */
    const bool same_sign = (rst > 0.0f) == (t > 0.0f) && t != 0.0f;
    if (!same_sign || !vmaf_mtl_fadm_finite(rst) || !vmaf_mtl_fadm_finite(t)) {
        if (rst > 0.0f) {
            return gained_f < t ? gained_f : t;
        }
        return gained_f > t ? gained_f : t;
    }
    const VmafMtlSoftDouble gained =
        vmaf_mtl_soft_mul(vmaf_mtl_soft_from_float_any(VMAF_MTL_FABS(rst)),
                          vmaf_mtl_soft_make(limit.mant, limit.exp));
    if (!vmaf_mtl_soft_less(gained, vmaf_mtl_soft_from_float_any(VMAF_MTL_FABS(t)))) {
        return t;
    }
    const float magnitude = vmaf_mtl_soft_to_float_any(gained);
    return rst > 0.0f ? magnitude : -magnitude;
}

/* adm_decouple_band_s(): the restored signal of one band. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_decouple_band(VmafMtlFadmGainLimit limit, float o, float t,
                                                bool flag)
{
    const float denominator = o + VMAF_MTL_FADM_EPS;
    float k = vmaf_mtl_fadm_divs(t, denominator);
    k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
    float rst = k * o;
    if (flag && (rst > 0.0f)) {
        rst = vmaf_mtl_fadm_gain_limited(rst, t, limit);
    }
    if (flag && (rst < 0.0f)) {
        rst = vmaf_mtl_fadm_gain_limited(rst, t, limit);
    }
    return rst;
}

/* adm_csf_s()'s filtered value: FLOAT_ONE_BY_30 * fabsf(csf). */
VMAF_MTL_FUNC float vmaf_mtl_fadm_csf_flt(float csf)
{
    return vmaf_mtl_fadm_times_constant(VMAF_MTL_FABS(csf), vmaf_mtl_fadm_one_by_30());
}

/* What the decouple kernel stores for one band sample: the CSF-weighted
 * additive (a = t - rst) and restored (r = rst) signals, each with its
 * filtered magnitude. */
typedef struct VmafMtlFadmCsfSample {
    float csf_a;
    float csf_fa;
    float csf_r;
    float csf_fr;
} VmafMtlFadmCsfSample;

/* adm_decouple_s() and both adm_csf_s() calls of compute_adm() for one band
 * of one sample. */
VMAF_MTL_FUNC VmafMtlFadmCsfSample vmaf_mtl_fadm_decouple_csf(VmafMtlFadmGainLimit limit,
                                                              float rfactor, float o, float t,
                                                              bool flag)
{
    const float rst = vmaf_mtl_fadm_decouple_band(limit, o, t, flag);
    const float add = t - rst;
    const float csf_a = rfactor * add;
    const float csf_r = rfactor * rst;
    const VmafMtlFadmCsfSample sample = {csf_a, vmaf_mtl_fadm_csf_flt(csf_a), csf_r,
                                         vmaf_mtl_fadm_csf_flt(csf_r)};
    return sample;
}

/* The three sub-bands (h, v, d) of one sample. */
typedef struct VmafMtlFadmDecouple {
    VmafMtlFadmCsfSample h;
    VmafMtlFadmCsfSample v;
    VmafMtlFadmCsfSample d;
} VmafMtlFadmDecouple;

/* The decouple kernel for one sample: the angle test of the (h, v) vectors of
 * reference and distorted band, then the three sub-bands. */
VMAF_MTL_FUNC VmafMtlFadmDecouple vmaf_mtl_fadm_decouple_sample(VmafMtlFadmGainLimit limit,
                                                                float cos_1deg_sq, float rfactor_h,
                                                                float rfactor_v, float rfactor_d,
                                                                float oh, float ov, float od,
                                                                float th, float tv, float td)
{
    const bool flag = vmaf_mtl_fadm_angle_flag(oh, ov, th, tv, cos_1deg_sq);
    const VmafMtlFadmDecouple out = {vmaf_mtl_fadm_decouple_csf(limit, rfactor_h, oh, th, flag),
                                     vmaf_mtl_fadm_decouple_csf(limit, rfactor_v, ov, tv, flag),
                                     vmaf_mtl_fadm_decouple_csf(limit, rfactor_d, od, td, flag)};
    return out;
}

/* The eight filtered neighbours of one band in row order (above-left, above,
 * above-right, left, right, below-left, below, below-right). */
typedef struct VmafMtlFadmNeighbours {
    float above_left;
    float above;
    float above_right;
    float left;
    float right;
    float below_left;
    float below;
    float below_right;
} VmafMtlFadmNeighbours;

/* One band of adm_cm_thresh3x3_s(): the neighbours and the unfiltered centre,
 * which is added fifth, as an fp64 addend. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_thresh_band(VmafMtlFadmNeighbours n, float centre)
{
    float sum = 0.0f;
    sum += n.above_left;
    sum += n.above;
    sum += n.above_right;
    sum += n.left;
    sum = vmaf_mtl_fadm_add_scaled(sum, VMAF_MTL_FABS(centre), vmaf_mtl_fadm_one_by_15());
    sum += n.right;
    sum += n.below_left;
    sum += n.below;
    sum += n.below_right;
    return sum;
}

/* The taps of one band: its neighbours and its centre. */
typedef struct VmafMtlFadmBandTaps {
    VmafMtlFadmNeighbours n;
    float centre;
} VmafMtlFadmBandTaps;

/* adm_cm_thresh3x3_s(): one sum per band, the three added in band order. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_threshold(VmafMtlFadmBandTaps b0, VmafMtlFadmBandTaps b1,
                                            VmafMtlFadmBandTaps b2)
{
    float accum = 0.0f;
    accum += vmaf_mtl_fadm_thresh_band(b0.n, b0.centre);
    accum += vmaf_mtl_fadm_thresh_band(b1.n, b1.centre);
    accum += vmaf_mtl_fadm_thresh_band(b2.n, b2.centre);
    return accum;
}

/* adm_cm_thresh3x3_s()'s neighbour indices: the sample before the first one
 * mirrors to index 1, the sample past the last one clamps to the last index.
 * A one-sample band has no index 1; the reference reads past the band there
 * and the device reads the only sample it has. */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_fadm_before(vmaf_mtl_i32 i, vmaf_mtl_i32 n)
{
    if (i != 0) {
        return i - 1;
    }
    return (n > 1) ? 1 : 0;
}

VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_fadm_after(vmaf_mtl_i32 i, vmaf_mtl_i32 n)
{
    return (i == n - 1) ? n - 1 : i + 1;
}

/* The 3x3 window of adm_cm_thresh3x3_s() around (x, y), as indices. */
typedef struct VmafMtlFadmWindow {
    vmaf_mtl_i32 ym;
    vmaf_mtl_i32 y;
    vmaf_mtl_i32 yp;
    vmaf_mtl_i32 xm;
    vmaf_mtl_i32 x;
    vmaf_mtl_i32 xp;
} VmafMtlFadmWindow;

VMAF_MTL_FUNC VmafMtlFadmWindow vmaf_mtl_fadm_window(vmaf_mtl_i32 x, vmaf_mtl_i32 y,
                                                     vmaf_mtl_i32 half_w, vmaf_mtl_i32 half_h)
{
    const VmafMtlFadmWindow w = {
        vmaf_mtl_fadm_before(y, half_h), y, vmaf_mtl_fadm_after(y, half_h),
        vmaf_mtl_fadm_before(x, half_w), x, vmaf_mtl_fadm_after(x, half_w)};
    return w;
}

/* x^adm_p_norm as the reductions take it, for x >= 0. At 3 the reference's
 * _p3 routines multiply; at 1 its powf(x, 1) is x. Any other exponent is
 * powf() on both sides, and the device's is not the host's. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_pnorm(float x, bool is_cube, float p_norm)
{
    if (is_cube) {
        const float square = x * x;
        return square * x;
    }
    if (p_norm == 1.0f) {
        return x;
    }
    return VMAF_MTL_FADM_POW(x, p_norm);
}

/* The term adm_csf_den_scale_s() accumulates for one band sample. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_den_term(float rfactor, float src, bool is_cube, float p_norm)
{
    const float weighted = rfactor * src;
    return vmaf_mtl_fadm_pnorm(VMAF_MTL_FABS(weighted), is_cube, p_norm);
}

/* The term adm_cm_s() accumulates for one band sample: `csf` is the sample
 * times its CSF weight, `thr` the masking threshold. */
VMAF_MTL_FUNC float vmaf_mtl_fadm_cm_term(float csf, float thr, bool is_cube, float p_norm)
{
    float x = VMAF_MTL_FABS(csf) - thr;
    x = x < 0.0f ? 0.0f : x;
    return vmaf_mtl_fadm_pnorm(x, is_cube, p_norm);
}

/* What the term kernel stores for one band of one sample of the reduced
 * region. */
typedef struct VmafMtlFadmBandTerms {
    float den; /* |rfactor * ref|^p, the adm2 denominator */
    float cm;  /* decouple_r masked by the threshold of decouple_a (adm2) */
    float aim; /* decouple_a masked by the threshold of decouple_r (AIM) */
} VmafMtlFadmBandTerms;

VMAF_MTL_FUNC VmafMtlFadmBandTerms vmaf_mtl_fadm_band_terms(float rfactor, float src, float csf_r,
                                                            float csf_a, float thr_additive,
                                                            float thr_restored, bool is_cube,
                                                            float p_norm)
{
    const VmafMtlFadmBandTerms terms = {
        vmaf_mtl_fadm_den_term(rfactor, src, is_cube, p_norm),
        vmaf_mtl_fadm_cm_term(csf_r, thr_additive, is_cube, p_norm),
        vmaf_mtl_fadm_cm_term(csf_a, thr_restored, is_cube, p_norm)};
    return terms;
}

/* The per-sample terms are stored slot by slot and, inside a slot, column by
 * column: the row kernel runs one work-item per (slot, row), so at every step
 * neighbouring work-items read neighbouring addresses. */
VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_fadm_term_index(vmaf_mtl_u32 slot, vmaf_mtl_u32 x,
                                                    vmaf_mtl_u32 y, vmaf_mtl_u32 region_w,
                                                    vmaf_mtl_u32 region_h)
{
    return (slot * region_w + x) * region_h + y;
}

/* ------------------------------------------------------------------ */
/* What the host hands the kernels                                     */
/* ------------------------------------------------------------------ */

/* The decouple kernel's constant buffer. Both sides compile this one
 * definition, so the layouts cannot differ. */
typedef struct VmafMtlFadmDecoupleArgs {
    VmafMtlFadmGainLimit limit;
    vmaf_mtl_i32 half_w;
    vmaf_mtl_i32 half_h;
    vmaf_mtl_i32 buf_stride;
    float cos_1deg_sq; /* adm_decouple_cos_1deg_sq_s() */
    float rfactor_h;   /* adm_csf_rfactor_s() */
    float rfactor_v;
    float rfactor_d;
    vmaf_mtl_i32 pad;
} VmafMtlFadmDecoupleArgs;

/* The term kernel's constant buffer. */
typedef struct VmafMtlFadmTermArgs {
    vmaf_mtl_i32 half_w;
    vmaf_mtl_i32 half_h;
    vmaf_mtl_i32 buf_stride;
    vmaf_mtl_i32 left; /* the reduced region, adm_border_s() */
    vmaf_mtl_i32 top;
    vmaf_mtl_u32 region_w;
    vmaf_mtl_u32 region_h;
    float p_norm;           /* (float)adm_p_norm, read when !is_cube */
    vmaf_mtl_u32 is_cube;   /* adm_p_norm == 3.0 */
    vmaf_mtl_u32 bypass_cm; /* adm_bypass_cm: no masking threshold */
    float rfactor_h;
    float rfactor_v;
    float rfactor_d;
    vmaf_mtl_u32 pad;
} VmafMtlFadmTermArgs;

/* The row kernel's constant buffer. */
typedef struct VmafMtlFadmRowArgs {
    vmaf_mtl_u32 region_w;
    vmaf_mtl_u32 region_h;
} VmafMtlFadmRowArgs;

/* ------------------------------------------------------------------ */
/* Host code                                                           */
/* ------------------------------------------------------------------ */

#if !defined(__METAL_VERSION__)

/* The gain limit in the forms above. */
static inline VmafMtlFadmGainLimit vmaf_mtl_fadm_make_gain_limit(double limit)
{
    VmafMtlFadmGainLimit out;
    memset(&out, 0, sizeof(out));
    out.value = (float)limit;
    out.is_float = vmaf_mtl_f64_equal((double)out.value, limit) ? 1 : 0;
    int exponent = 0;
    const double fraction = frexp(limit, &exponent);
    out.mant = (vmaf_mtl_u64)ldexp(fraction, 53);
    out.exp = (vmaf_mtl_i32)exponent - 53;
    return out;
}

/* adm_fold3_s() over the rows of one slot, top to bottom, in fp32. */
static inline float vmaf_mtl_fadm_fold_rows(const float *rows, vmaf_mtl_u32 count)
{
    float accum = 0.0f;
    for (vmaf_mtl_u32 y = 0u; y < count; y++) {
        accum += rows[y];
    }
    return accum;
}

#endif /* host code */

#endif /* VMAF_FEATURE_METAL_METAL_FLOAT_ADM_MATH_H_ */
