/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  The arithmetic of float_vif_metal (ADR-1498): vif_tools.c's filter tap,
 *  log2f_approx() and vif_pixel_statistic_s(), operation for operation,
 *  without an fp64 type (Metal has none, Metal Shading Language
 *  Specification 4.1, section 2.1).
 *
 *  This is core/src/feature/sycl/sycl_float_vif_math.h statement for
 *  statement (ADR-1422; the CUDA and HIP twins compute the same values with
 *  fp64, ADR-1412, ADR-1444). The SYCL header is written against sycl:: and
 *  C++20 and cannot be included from Metal; the two are one behaviour in two
 *  files until T-METAL-SYCL-FP64-FREE-ARITHMETIC-COPIES-2026-10-03 merges
 *  them, and a change to one changes the other in the same PR.
 *
 *  Two expressions of the reference are fp64, because `vif_sigma_nsq` is a
 *  `double` parameter:
 *
 *      1.0f + (g * g * sigma1_sq) / (sv_sq + vif_sigma_nsq)
 *      1.0f + (sigma1_sq) / (vif_sigma_nsq)
 *
 *  and each is rounded to fp32 once, when it is passed to log2f(). Here both
 *  are evaluated as exact fp32 pairs, good to about 2^-44, and rounded to
 *  fp32. That is the reference's value unless the pair lies within 2^-12 of
 *  an fp32 unit of a rounding boundary; for those samples (about one in 2000)
 *  the reference's own sequence of fp64 operations is replayed in 64-bit
 *  integers (metal_soft_double.h), so the result is the reference's on every
 *  sample by construction.
 *
 *  The kernels of float_vif.metal compute every value through this header;
 *  core/test/test_metal_float_vif_math.cpp compiles the same lines on the
 *  host and holds them against vif_tools.c and vif.c, value by value. Every
 *  operation is one of the correctly rounded fp32 operations of the
 *  specification's Table 8.1 under the kernels' -fno-fast-math
 *  -ffp-contract=off; each product is named before it is added so that the
 *  rounding stays in the source. Written on metal_portable.h: values in and
 *  out, typedef'd structs built by _make() functions, no `double` outside the
 *  host-only block at the end. The polynomial's coefficients are bit
 *  patterns, not decimal literals: a decimal literal without a suffix has no
 *  fp64 type to round through in Metal.
 */

#ifndef VMAF_FEATURE_METAL_METAL_FLOAT_VIF_MATH_H_
#define VMAF_FEATURE_METAL_METAL_FLOAT_VIF_MATH_H_

#include "metal_portable.h"
#include "metal_soft_double.h"

#if !defined(__METAL_VERSION__) && !defined(__cplusplus)
#include <stdbool.h>
#endif

/* ------------------------------------------------------------------ */
/* Geometry and kernel argument blocks                                 */
/* ------------------------------------------------------------------ */

#define VMAF_MTL_FVIF_SCALES 4
/* The largest filter vif_get_filter() can return: float_vif.c's filter cache
 * is float[128]; at vif_kernelscale = 4.0 scale 0 is 69 taps wide. */
#define VMAF_MTL_FVIF_MAX_TAPS 128
/* The five filtered moments of a pixel: mu1, mu2, ref^2, dis^2, ref*dis. */
#define VMAF_MTL_FVIF_MOMENTS 5u
/* The two terms of a pixel and of a row sum: num, den. */
#define VMAF_MTL_FVIF_TERM_FLOATS 2u

/* float_vif_vertical and float_vif_compute: the plane filtered at this
 * scale, its filter width and the floats of one moment plane. */
typedef struct VmafMtlFvifFilterArgs {
    vmaf_mtl_u32 width;
    vmaf_mtl_u32 height;
    vmaf_mtl_u32 taps;
    vmaf_mtl_u32 plane;
} VmafMtlFvifFilterArgs;

/* float_vif_compute: `vif_sigma_nsq` in the forms the statistic reads (see
 * VmafMtlFvifNoise; the fp64 significand split in two 32-bit halves so that
 * every field is 4 bytes and the layout is the same in C++ and in Metal),
 * (float)vif_enhn_gain_limit and vif_statistic_s()'s sigma_max_inv. */
typedef struct VmafMtlFvifStatisticArgs {
    vmaf_mtl_u32 noise_mant_hi;
    vmaf_mtl_u32 noise_mant_lo;
    vmaf_mtl_i32 noise_exp;
    float noise_hi;
    float noise_lo;
    float noise_above;
    float gain_limit;
    float sigma_max_inv;
} VmafMtlFvifStatisticArgs;

/* The plane the filter and decimate kernels read: the raw 8- or 16-bit
 * frame (`raw` != 0, `raw_stride` bytes per row, `bpc` as picture_copy()
 * reads it) or a float plane of the scale's own width (the decimated planes,
 * and scale 0 after a host prescale). */
typedef struct VmafMtlFvifInputArgs {
    vmaf_mtl_u32 raw;
    vmaf_mtl_u32 raw_stride;
    vmaf_mtl_u32 bpc;
} VmafMtlFvifInputArgs;

/* float_vif_decimate: this scale's filter over the previous scale's plane
 * (in_width x in_height), sampled at even positions. */
typedef struct VmafMtlFvifDecimateArgs {
    vmaf_mtl_u32 in_width;
    vmaf_mtl_u32 in_height;
    vmaf_mtl_u32 out_width;
    vmaf_mtl_u32 out_height;
    vmaf_mtl_u32 taps;
} VmafMtlFvifDecimateArgs;

/* float_vif_row_sums: the plane of terms of this scale. */
typedef struct VmafMtlFvifRowArgs {
    vmaf_mtl_u32 width;
    vmaf_mtl_u32 height;
} VmafMtlFvifRowArgs;

/* ------------------------------------------------------------------ */
/* Filter and layout                                                   */
/* ------------------------------------------------------------------ */

/* vif_tools.c::vif_mirror_index(): reflect-101 of `idx` into [0, n). Every
 * index a consumed output reads lies within a filter half-width of the
 * plane, which vif_get_min_dim() keeps inside one reflection. */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_fvif_mirror(vmaf_mtl_i32 idx, vmaf_mtl_i32 n)
{
    if (idx < 0) {
        return -idx;
    }
    if (idx >= n) {
        return 2 * n - idx - 2;
    }
    return idx;
}

/* One tap of vif_filter1d_*_s(): acc + coeff * sample, the product and the
 * sum each rounded to fp32. */
VMAF_MTL_FUNC float vmaf_mtl_fvif_tap(float acc, float coeff, float sample)
{
    const float product = coeff * sample;
    return acc + product;
}

/* One raw sample as picture_copy(dst, stride, pic, -128, bpc, 0) reads it:
 * an 8-bit sample minus 128, a 10-, 12- or 16-bit sample divided by 4, 16 or
 * 256 (the divide is the correctly rounded fp32 one) minus 128. */
VMAF_MTL_FUNC float vmaf_mtl_fvif_raw_to_float(vmaf_mtl_u32 sample, vmaf_mtl_u32 bpc)
{
    if (bpc <= 8u) {
        return (float)sample - 128.0f;
    }
    float scaler = 1.0f;
    if (bpc == 10u) {
        scaler = 4.0f;
    } else if (bpc == 12u) {
        scaler = 16.0f;
    } else if (bpc == 16u) {
        scaler = 256.0f;
    }
    return (float)sample / scaler - 128.0f;
}

/* Where the terms of pixel (x, y) are stored: column by column, so that the
 * threads of the row kernel, one per row, read neighbouring addresses at
 * every step (float_vif_gpu_common.h::fvif_term_index()). */
VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_fvif_term_index(vmaf_mtl_u32 x, vmaf_mtl_u32 y,
                                                    vmaf_mtl_u32 height)
{
    return (x * height + y) * VMAF_MTL_FVIF_TERM_FLOATS;
}

/* ------------------------------------------------------------------ */
/* log2f_approx()                                                      */
/* ------------------------------------------------------------------ */

/* vif_tools.c::log2_poly_s[], each decimal literal rounded to fp32 as the C
 * initialiser of its `float` array rounds it (test_metal_float_vif_math.cpp
 * checks every pattern against that conversion). */
#define VMAF_MTL_FVIF_LOG2_C0 0xBC4F9CB1u /* (float)-0.012671635276421 */
#define VMAF_MTL_FVIF_LOG2_C1 0x3D84CB74u /* (float)0.064841182402670 */
#define VMAF_MTL_FVIF_LOG2_C2 0xBE20D169u /* (float)-0.157048836463065 */
#define VMAF_MTL_FVIF_LOG2_C3 0x3E83AB7Du /* (float)0.257167726303123 */
#define VMAF_MTL_FVIF_LOG2_C4 0xBEB52559u /* (float)-0.353800560300520 */
#define VMAF_MTL_FVIF_LOG2_C5 0x3EF5D3C9u /* (float)0.480131410397451 */
#define VMAF_MTL_FVIF_LOG2_C6 0xBF38A80Eu /* (float)-0.721314327952201 */
#define VMAF_MTL_FVIF_LOG2_C7 0x3FB8AA39u /* (float)1.442694803896991 */
#define VMAF_MTL_FVIF_LOG2_C8 0x00000000u /* 0 */

/* One step of horner_s(): var * x + poly[i], each operation rounded. */
VMAF_MTL_FUNC float vmaf_mtl_fvif_horner_step(float var, float x, vmaf_mtl_u32 coeff_bits)
{
    const float scaled = var * x;
    return scaled + VMAF_MTL_U2F(coeff_bits);
}

/* vif_tools.c::log2f_approx(): the exponent plus horner_s() over
 * log2_poly_s in `mantissa - 1`, from 0 and over all nine coefficients. */
VMAF_MTL_FUNC float vmaf_mtl_fvif_log2(float x)
{
    if (x == 0.0f) {
        return VMAF_MTL_U2F(0xFF800000u); /* -INFINITY */
    }
    if (x < 0.0f) {
        return VMAF_MTL_U2F(0x7FC00000u); /* NAN */
    }
    const vmaf_mtl_u32 bits = VMAF_MTL_F2U(x);
    const vmaf_mtl_u32 exponent = (bits & 0x7F800000u) >> 23;
    const float remain = VMAF_MTL_U2F((bits & 0x007FFFFFu) | 0x3F800000u);
    const float log_base = (float)((vmaf_mtl_i32)exponent - 127);
    const float t = remain - 1.0f;
    float var = 0.0f;
    var = vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C0);
    var = vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C1);
    var = vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C2);
    var = vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C3);
    var = vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C4);
    var = vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C5);
    var = vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C6);
    var = vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C7);
    var = vmaf_mtl_fvif_horner_step(var, t, VMAF_MTL_FVIF_LOG2_C8);
    return log_base + var;
}

/* ------------------------------------------------------------------ */
/* Exact fp32 pairs (sycl_exact_fp.h's, on the correctly rounded       */
/* operators of the kernels' strict FP line)                           */
/* ------------------------------------------------------------------ */

typedef struct VmafMtlFvifFf {
    float hi;
    float lo;
} VmafMtlFvifFf;

VMAF_MTL_FUNC VmafMtlFvifFf vmaf_mtl_fvif_ff_make(float hi, float lo)
{
    const VmafMtlFvifFf value = {hi, lo};
    return value;
}

/* Neither an infinity nor a NaN (sycl::isfinite()). */
VMAF_MTL_FUNC bool vmaf_mtl_fvif_finite(float x)
{
    return (VMAF_MTL_F2U(x) & 0x7F800000u) != 0x7F800000u;
}

/* a + b, exactly: the rounded sum and what the rounding dropped. */
VMAF_MTL_FUNC VmafMtlFvifFf vmaf_mtl_fvif_two_sum(float a, float b)
{
    const float sum = a + b;
    const float b_virtual = sum - a;
    const float a_virtual = sum - b_virtual;
    const float b_error = b - b_virtual;
    const float a_error = a - a_virtual;
    return vmaf_mtl_fvif_ff_make(sum, a_error + b_error);
}

/* two_sum() for |a| >= |b|. */
VMAF_MTL_FUNC VmafMtlFvifFf vmaf_mtl_fvif_quick_two_sum(float a, float b)
{
    const float sum = a + b;
    const float rebuilt = sum - a;
    return vmaf_mtl_fvif_ff_make(sum, b - rebuilt);
}

/* a * b, exactly. */
VMAF_MTL_FUNC VmafMtlFvifFf vmaf_mtl_fvif_two_prod(float a, float b)
{
    const float product = a * b;
    return vmaf_mtl_fvif_ff_make(product, VMAF_MTL_FMA(a, b, -product));
}

VMAF_MTL_FUNC VmafMtlFvifFf vmaf_mtl_fvif_ff_add(VmafMtlFvifFf a, VmafMtlFvifFf b)
{
    const VmafMtlFvifFf high = vmaf_mtl_fvif_two_sum(a.hi, b.hi);
    const VmafMtlFvifFf low = vmaf_mtl_fvif_two_sum(a.lo, b.lo);
    const VmafMtlFvifFf first = vmaf_mtl_fvif_quick_two_sum(high.hi, high.lo + low.hi);
    return vmaf_mtl_fvif_quick_two_sum(first.hi, low.lo + first.lo);
}

VMAF_MTL_FUNC VmafMtlFvifFf vmaf_mtl_fvif_ff_mul(VmafMtlFvifFf a, VmafMtlFvifFf b)
{
    const VmafMtlFvifFf product = vmaf_mtl_fvif_two_prod(a.hi, b.hi);
    const float cross1 = a.hi * b.lo;
    const float cross2 = a.lo * b.hi;
    const float cross = cross1 + cross2;
    return vmaf_mtl_fvif_quick_two_sum(product.hi, product.lo + cross);
}

VMAF_MTL_FUNC VmafMtlFvifFf vmaf_mtl_fvif_ff_neg(VmafMtlFvifFf a)
{
    return vmaf_mtl_fvif_ff_make(-a.hi, -a.lo);
}

/* a / b as a pair, relative error about 2^-46 (one quotient digit per step,
 * the residual a - q1 * b formed with ff_mul / ff_add). Both partial
 * quotients are the correctly rounded fp32 `/` of the strict FP line. A
 * non-finite first quotient is returned as is (lo = 0) so infinities and NaN
 * reach the caller unchanged. */
VMAF_MTL_FUNC VmafMtlFvifFf vmaf_mtl_fvif_ff_div(VmafMtlFvifFf a, VmafMtlFvifFf b)
{
    const float q1 = a.hi / b.hi;
    if (!vmaf_mtl_fvif_finite(q1)) {
        return vmaf_mtl_fvif_ff_make(q1, 0.0f);
    }
    const VmafMtlFvifFf scaled = vmaf_mtl_fvif_ff_mul(b, vmaf_mtl_fvif_ff_make(q1, 0.0f));
    const VmafMtlFvifFf residual = vmaf_mtl_fvif_ff_add(a, vmaf_mtl_fvif_ff_neg(scaled));
    const float q2 = residual.hi / b.hi;
    return vmaf_mtl_fvif_quick_two_sum(q1, q2);
}

/* ------------------------------------------------------------------ */
/* `vif_sigma_nsq` as the kernels take it                              */
/* ------------------------------------------------------------------ */

/* The fp64 value as a pair (to about 2^-48), the smallest fp32 value that is
 * not below it, and the value itself as a VmafMtlSoftDouble significand and
 * exponent (mant 0 when it is zero). */
typedef struct VmafMtlFvifNoise {
    float hi;
    float lo;
    float above;
    vmaf_mtl_u64 mant;
    vmaf_mtl_i32 exp;
} VmafMtlFvifNoise;

/* vif_statistic_s()'s per-call constants. */
typedef struct VmafMtlFvifStatParams {
    VmafMtlFvifNoise noise;
    float gain_limit;    /* (float)vif_enhn_gain_limit */
    float sigma_max_inv; /* powf(vif_sigma_nsq, 2.0f) / (255.0 * 255.0) */
} VmafMtlFvifStatParams;

/* The statistic's constants from the kernel argument block's fields. */
VMAF_MTL_FUNC VmafMtlFvifStatParams vmaf_mtl_fvif_stat_params_make(
    vmaf_mtl_u32 noise_mant_hi, vmaf_mtl_u32 noise_mant_lo, vmaf_mtl_i32 noise_exp, float noise_hi,
    float noise_lo, float noise_above, float gain_limit, float sigma_max_inv)
{
    VmafMtlFvifStatParams p;
    p.noise.hi = noise_hi;
    p.noise.lo = noise_lo;
    p.noise.above = noise_above;
    p.noise.mant = (VMAF_MTL_U64(noise_mant_hi) << 32) | VMAF_MTL_U64(noise_mant_lo);
    p.noise.exp = noise_exp;
    p.gain_limit = gain_limit;
    p.sigma_max_inv = sigma_max_inv;
    return p;
}

/* ------------------------------------------------------------------ */
/* 1.0f + numerator / denominator, in the reference's fp64             */
/* ------------------------------------------------------------------ */

/* 2^-12 and the smallest normal fp32 value. */
#define VMAF_MTL_FVIF_TWO_POW_M12 0x39800000u
#define VMAF_MTL_FVIF_FLT_MIN 0x00800000u

/* True when the pair is within 2^-12 of an fp32 unit of the point where its
 * rounding to fp32 changes: fl32(hi + lo) is then not certain to be the
 * reference's value. `sum` is normalised (|lo| is at most half a unit of
 * hi) and at least 1. */
VMAF_MTL_FUNC bool vmaf_mtl_fvif_near_rounding_boundary(VmafMtlFvifFf sum)
{
    const vmaf_mtl_u32 bits = VMAF_MTL_F2U(sum.hi);
    const vmaf_mtl_u32 exponent = bits & 0x7F800000u;
    const float unit = VMAF_MTL_U2F(exponent - (23u << 23));
    /* Below a power of two the spacing halves. */
    const bool lower_binade = (bits & 0x007FFFFFu) == 0u && sum.lo < 0.0f;
    const float half = lower_binade ? 0.25f * unit : 0.5f * unit;
    const float margin = unit * VMAF_MTL_U2F(VMAF_MTL_FVIF_TWO_POW_M12);
    return VMAF_MTL_FABS(sum.lo) >= half - margin;
}

/* A positive fp64 denominator in the two forms the quotient needs: as a
 * pair, and exactly. `exact.mant` is 0 for a zero denominator. */
typedef struct VmafMtlFvifDenominator {
    VmafMtlFvifFf pair;
    VmafMtlSoftDouble exact;
} VmafMtlFvifDenominator;

VMAF_MTL_FUNC VmafMtlFvifDenominator vmaf_mtl_fvif_denominator_make(VmafMtlFvifFf pair,
                                                                    VmafMtlSoftDouble exact)
{
    const VmafMtlFvifDenominator value = {pair, exact};
    return value;
}

/* (double)vif_sigma_nsq. */
VMAF_MTL_FUNC VmafMtlFvifDenominator vmaf_mtl_fvif_noise_denominator(VmafMtlFvifNoise n)
{
    return vmaf_mtl_fvif_denominator_make(vmaf_mtl_fvif_ff_make(n.hi, n.lo),
                                          vmaf_mtl_soft_make(n.mant, n.exp));
}

/* fl64(sv_sq + vif_sigma_nsq), sv_sq positive and normal. */
VMAF_MTL_FUNC VmafMtlFvifDenominator vmaf_mtl_fvif_noise_plus(float sv_sq, VmafMtlFvifNoise n)
{
    const VmafMtlFvifFf pair =
        vmaf_mtl_fvif_ff_add(vmaf_mtl_fvif_ff_make(sv_sq, 0.0f), vmaf_mtl_fvif_ff_make(n.hi, n.lo));
    const VmafMtlSoftDouble addend = vmaf_mtl_soft_from_float(sv_sq);
    /* A zero variance adds nothing. Scalar selects, not a select of the
     * structs, as in the SYCL header. */
    const bool zero = n.mant == 0u;
    const VmafMtlSoftDouble sum = vmaf_mtl_soft_add(
        addend, vmaf_mtl_soft_make(zero ? VMAF_MTL_SOFT_DOUBLE_TOP : n.mant, n.exp));
    return vmaf_mtl_fvif_denominator_make(
        pair, vmaf_mtl_soft_make(zero ? addend.mant : sum.mant, zero ? addend.exp : sum.exp));
}

/* 1.0 + numerator / denominator as an exact pair: good to about 2^-44. */
VMAF_MTL_FUNC VmafMtlFvifFf vmaf_mtl_fvif_one_plus_ratio_pair(float numerator,
                                                              VmafMtlFvifFf denominator)
{
    const VmafMtlFvifFf ratio =
        vmaf_mtl_fvif_ff_div(vmaf_mtl_fvif_ff_make(numerator, 0.0f), denominator);
    if (!vmaf_mtl_fvif_finite(ratio.hi)) {
        /* A zero denominator: the sum is the quotient's infinity or NaN. */
        return vmaf_mtl_fvif_ff_make(1.0f + ratio.hi, 0.0f);
    }
    return vmaf_mtl_fvif_ff_add(vmaf_mtl_fvif_ff_make(1.0f, 0.0f), ratio);
}

/* fl32(fl64(1.0 + fl64(numerator / denominator))) by replaying the two fp64
 * operations in integers. numerator and denominator are positive and
 * normal. */
VMAF_MTL_FUNC float vmaf_mtl_fvif_one_plus_ratio_replayed(float numerator,
                                                          VmafMtlSoftDouble denominator)
{
    const VmafMtlSoftDouble ratio =
        vmaf_mtl_soft_div(vmaf_mtl_soft_from_float(numerator), denominator);
    return vmaf_mtl_soft_to_float(
        vmaf_mtl_soft_add(vmaf_mtl_soft_make(VMAF_MTL_SOFT_DOUBLE_TOP, -52), ratio));
}

/* True when the pair does not decide the rounding. A zero, subnormal or
 * non-finite quotient is far from every boundary above 1 or not a number at
 * all, so the pair's value is the reference's there. */
VMAF_MTL_FUNC bool vmaf_mtl_fvif_needs_replay(float numerator, VmafMtlFvifFf sum,
                                              VmafMtlFvifDenominator denominator)
{
    return vmaf_mtl_fvif_finite(sum.hi) && vmaf_mtl_fvif_near_rounding_boundary(sum) &&
           denominator.exact.mant != 0u && numerator >= VMAF_MTL_U2F(VMAF_MTL_FVIF_FLT_MIN);
}

/* fl32(fl64(1.0 + fl64(numerator / denominator))); numerator is not
 * negative. The one NaN the reference can form here is 0 / 0 (a zero
 * `vif_sigma_nsq`), whose fp64 default NaN rounds to the fp32 pattern
 * 0x7FC00000 or 0xFFC00000 on the CPUs the reference runs on; log2f_approx()
 * reads its significand. The NaN a Metal device generates has no specified
 * payload, so it is given the reference's: the sign does not reach the
 * significand log2f_approx() reads. */
VMAF_MTL_FUNC float vmaf_mtl_fvif_one_plus_ratio(float numerator,
                                                 VmafMtlFvifDenominator denominator)
{
    const VmafMtlFvifFf sum = vmaf_mtl_fvif_one_plus_ratio_pair(numerator, denominator.pair);
    if (sum.hi != sum.hi) {
        return VMAF_MTL_U2F(0x7FC00000u);
    }
    return vmaf_mtl_fvif_needs_replay(numerator, sum, denominator) ?
               vmaf_mtl_fvif_one_plus_ratio_replayed(numerator, denominator.exact) :
               sum.hi;
}

/* ------------------------------------------------------------------ */
/* vif_pixel_statistic_s()                                             */
/* ------------------------------------------------------------------ */

/* A pixel's variances and covariance, as vif_pixel_statistic_s() derives
 * them from the filtered moments (the two variances clamped at 0). */
typedef struct VmafMtlFvifSigmas {
    float sigma1_sq;
    float sigma2_sq;
    float sigma12;
} VmafMtlFvifSigmas;

/* The numerator and denominator term of a pixel. */
typedef struct VmafMtlFvifTerm {
    float num;
    float den;
} VmafMtlFvifTerm;

/* The first half of vif_pixel_statistic_s(): fp32 throughout. The
 * reference's MAX() macro, not fmax(): it picks the second operand for a NaN
 * and for -0 against +0. */
VMAF_MTL_FUNC VmafMtlFvifSigmas vmaf_mtl_fvif_pixel_sigmas(float mu1, float mu2, float xx, float yy,
                                                           float xy)
{
    const float mu1_sq = mu1 * mu1;
    const float mu2_sq = mu2 * mu2;
    const float mu1_mu2 = mu1 * mu2;
    const float sigma1_sq = xx - mu1_sq;
    const float sigma2_sq = yy - mu2_sq;
    VmafMtlFvifSigmas s;
    s.sigma1_sq = sigma1_sq > 0.0f ? sigma1_sq : 0.0f;
    s.sigma2_sq = sigma2_sq > 0.0f ? sigma2_sq : 0.0f;
    s.sigma12 = xy - mu1_mu2;
    return s;
}

/* The second half: the gain, the two log terms and their overrides. */
VMAF_MTL_FUNC VmafMtlFvifTerm vmaf_mtl_fvif_pixel_statistic(VmafMtlFvifSigmas sigmas,
                                                            VmafMtlFvifStatParams p)
{
    const float eps = 1.0e-10f;
    float sigma1_sq = sigmas.sigma1_sq;
    const float sigma2_sq = sigmas.sigma2_sq;
    const float sigma12 = sigmas.sigma12;

    const float gain_den = sigma1_sq + eps;
    float g = sigma12 / gain_den;
    const float g_sigma12 = g * sigma12;
    float sv_sq = sigma2_sq - g_sigma12;
    if (sigma1_sq < eps) {
        g = 0.0f;
        sv_sq = sigma2_sq;
        sigma1_sq = 0.0f;
    }
    if (sigma2_sq < eps) {
        g = 0.0f;
        sv_sq = 0.0f;
    }
    if (g < 0.0f) {
        sv_sq = sigma2_sq;
        g = 0.0f;
    }
    sv_sq = sv_sq > eps ? sv_sq : eps;
    g = g < p.gain_limit ? g : p.gain_limit;

    /* The product is fp32; the sum, the quotient and the `1.0f +` are fp64. */
    const float gain_sq = g * g;
    const float product = gain_sq * sigma1_sq;
    VmafMtlFvifTerm term;
    term.num = vmaf_mtl_fvif_log2(
        vmaf_mtl_fvif_one_plus_ratio(product, vmaf_mtl_fvif_noise_plus(sv_sq, p.noise)));
    term.den = vmaf_mtl_fvif_log2(
        vmaf_mtl_fvif_one_plus_ratio(sigma1_sq, vmaf_mtl_fvif_noise_denominator(p.noise)));
    if (sigma12 < 0.0f) {
        term.num = 0.0f;
    }
    if (sigma1_sq < p.noise.above) {
        const float scaled = sigma2_sq * p.sigma_max_inv;
        term.num = 1.0f - scaled;
        term.den = 1.0f;
    }
    return term;
}

/* vif_pixel_statistic_s() of one pixel from its five filtered moments. */
VMAF_MTL_FUNC VmafMtlFvifTerm vmaf_mtl_fvif_pixel_term(float mu1, float mu2, float xx, float yy,
                                                       float xy, VmafMtlFvifStatParams p)
{
    return vmaf_mtl_fvif_pixel_statistic(vmaf_mtl_fvif_pixel_sigmas(mu1, mu2, xx, yy, xy), p);
}

/* ------------------------------------------------------------------ */
/* Host code (fp64): the statistic's argument block                    */
/* ------------------------------------------------------------------ */

#if !defined(__METAL_VERSION__)

/* `vif_sigma_nsq` and `vif_enhn_gain_limit` in the forms the kernels read
 * (sycl_float_vif_math.h::make_statistic_params()): a pair, the smallest fp32
 * value not below the variance (which turns `sigma1_sq < vif_sigma_nsq` into
 * an fp32 comparison with the same outcome), the fp64 value as an integer
 * significand and exponent, and sigma_max_inv exactly as vif_statistic_s()
 * derives it: powf() in fp32, divided by 255.0 * 255.0 in fp64. */
static inline VmafMtlFvifStatisticArgs vmaf_mtl_fvif_statistic_args(double sigma_nsq,
                                                                    double enhn_gain_limit)
{
    VmafMtlFvifStatisticArgs a;
    memset(&a, 0, sizeof(a));
    a.noise_hi = (float)sigma_nsq;
    a.noise_lo = (float)(sigma_nsq - (double)a.noise_hi);
    a.noise_above = a.noise_hi;
    if ((double)a.noise_above < sigma_nsq) {
        a.noise_above = nextafterf(a.noise_above, INFINITY);
    }
    if (sigma_nsq > 0.0) {
        int exponent = 0;
        const double fraction = frexp(sigma_nsq, &exponent);
        const uint64_t mant = (uint64_t)ldexp(fraction, 53);
        a.noise_mant_hi = (uint32_t)(mant >> 32);
        a.noise_mant_lo = (uint32_t)(mant & 0xFFFFFFFFu);
        a.noise_exp = exponent - 53;
    }
    a.gain_limit = (float)enhn_gain_limit;
    a.sigma_max_inv = (float)(powf((float)sigma_nsq, 2.0f) / (255.0 * 255.0));
    return a;
}

/* The statistic's constants from the argument block, as a kernel builds
 * them. */
static inline VmafMtlFvifStatParams vmaf_mtl_fvif_stat_params(VmafMtlFvifStatisticArgs a)
{
    return vmaf_mtl_fvif_stat_params_make(a.noise_mant_hi, a.noise_mant_lo, a.noise_exp, a.noise_hi,
                                          a.noise_lo, a.noise_above, a.gain_limit, a.sigma_max_inv);
}

#endif /* !__METAL_VERSION__ */

#endif /* VMAF_FEATURE_METAL_METAL_FLOAT_VIF_MATH_H_ */
