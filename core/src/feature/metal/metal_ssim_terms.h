/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2 AND BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  The CPU's SSIM window arithmetic for Metal kernels, without fp64
 *  (ADR-1498): iqa/convolve.c's two Gaussian passes and iqa/ssim_tools.c's
 *  per-pixel luminance, contrast and structure terms, operation for operation
 *  and type for type. The CPU's fp64 values of the convolution are carried as
 *  exact fp32 pairs; its fp64 terms are the doubles themselves, computed in
 *  64-bit integers (vmaf_mtl_ssim_double_terms()), stored per window and
 *  added on the host in the CPU's raster order (vmaf_mtl_ssim_frame_sums(),
 *  vmaf_mtl_ssim_lcs_sums(); ADR-1463 and ADR-1464 for float_ssim, ADR-1414,
 *  ADR-1465 and ADR-1466 for float_ms_ssim).
 *
 *  Shared by float_ssim_metal (float_ssim.metal) and float_ms_ssim_metal
 *  (float_ms_ssim.metal), as sycl/sycl_ssim_terms.h is shared by its twins.
 *  This is that header statement for statement; the two are one behaviour in
 *  two files until T-METAL-SYCL-FP64-FREE-ARITHMETIC-COPIES-2026-10-03
 *  merges them. Name mapping (namespace vmaf_sycl_ssim -> Metal):
 *    struct SsimMoments, MomentPairs, SsimFloatParts, SsimDoubleTerms,
 *           SsimFrameSums           -> VmafMtlSsimMoments, VmafMtlSsimPairs,
 *                                      VmafMtlSsimFloatParts,
 *                                      VmafMtlSsimDoubleTerms,
 *                                      VmafMtlSsimFrameSums
 *    Ff, two_sum, quick_two_sum,
 *    ff_add                         -> VmafMtlFf, vmaf_mtl_ff_*
 *    add_tap, add_horizontal_tap,
 *    add_vertical_tap, round_moments,
 *    ssim_float_parts,
 *    ssim_double_terms,
 *    ssim_product_bits,
 *    ssim_frame_sums, ssim_lcs_sums -> vmaf_mtl_ssim_<name>
 *    float_ssim_constants()         -> vmaf_mtl_ssim_constants()
 *    div_rn(), sqrt_rn()            -> `/` and VMAF_MTL_SQRT: with fast math off
 *                                      both are correctly rounded (MSL 4.1,
 *                                      Table 8.1), and the SYCL helpers exist
 *                                      only because a SYCL device's are not
 *    a function that fills a
 *    reference parameter            -> a function returning the value
 *
 *  Valid as Metal Shading Language and as host C and C++
 *  (core/test/test_metal_float_ssim_math.c holds it against iqa_ssim() and
 *  ssim_tools.c): values in and values out, no pointer or reference in the
 *  device part, no `double`. It assumes contraction off, which every Metal
 *  kernel has (-ffp-contract=off, core/src/metal/meson.build) and the host
 *  build has (ADR-1461): vmaf_mtl_ff_two_sum() and vmaf_mtl_ff_quick_two_sum()
 *  are exact only when every add rounds on its own.
 */

#ifndef VMAF_FEATURE_METAL_METAL_SSIM_TERMS_H_
#define VMAF_FEATURE_METAL_METAL_SSIM_TERMS_H_

#include "metal_portable.h"
#include "metal_soft_signed.h"

/* iqa/ssim_tools.h's g_gaussian_window_h and _v: eleven taps, both passes. */
#define VMAF_MTL_SSIM_TAPS 11

VMAF_MTL_CONSTANT float vmaf_mtl_ssim_gauss[VMAF_MTL_SSIM_TAPS] = {
    0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
    0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f};

/* The arguments of a window-term kernel, laid out alike in MSL and on the
 * host: the horizontal pass's plane, the windows' plane, where this
 * (plane, scale) starts in the term buffers, and C1 and C2. */
typedef struct VmafMtlSsimWindowParams {
    vmaf_mtl_u32 horizontal_width;
    vmaf_mtl_u32 horizontal_height;
    vmaf_mtl_u32 final_width;
    vmaf_mtl_u32 final_height;
    vmaf_mtl_u32 offset;
    vmaf_mtl_u32 reserved;
    float c1;
    float c2;
} VmafMtlSsimWindowParams;

/* An unevaluated sum hi + lo of two fp32 values, about 48 significant bits. */
typedef struct VmafMtlFf {
    float hi;
    float lo;
} VmafMtlFf;

VMAF_MTL_FUNC VmafMtlFf vmaf_mtl_ff_make(float hi, float lo)
{
    const VmafMtlFf value = {hi, lo};
    return value;
}

/* a + b exactly: the rounded sum and what the rounding dropped. */
VMAF_MTL_FUNC VmafMtlFf vmaf_mtl_ff_two_sum(float a, float b)
{
    const float sum = a + b;
    const float b_virtual = sum - a;
    const float a_virtual = sum - b_virtual;
    const float b_error = b - b_virtual;
    const float a_error = a - a_virtual;
    return vmaf_mtl_ff_make(sum, a_error + b_error);
}

/* two_sum for |a| >= |b|. */
VMAF_MTL_FUNC VmafMtlFf vmaf_mtl_ff_quick_two_sum(float a, float b)
{
    const float sum = a + b;
    const float rebuilt = sum - a;
    return vmaf_mtl_ff_make(sum, b - rebuilt);
}

VMAF_MTL_FUNC VmafMtlFf vmaf_mtl_ff_add(VmafMtlFf a, VmafMtlFf b)
{
    const VmafMtlFf high = vmaf_mtl_ff_two_sum(a.hi, b.hi);
    const VmafMtlFf low = vmaf_mtl_ff_two_sum(a.lo, b.lo);
    const VmafMtlFf first = vmaf_mtl_ff_quick_two_sum(high.hi, high.lo + low.hi);
    return vmaf_mtl_ff_quick_two_sum(first.hi, low.lo + first.lo);
}

/* The five values of one convolution pass, as fp32. */
typedef struct VmafMtlSsimMoments {
    float reference_mean;
    float comparison_mean;
    float reference_square;
    float comparison_square;
    float cross_product;
} VmafMtlSsimMoments;

/* The five sums of one convolution pass, as pairs. */
typedef struct VmafMtlSsimPairs {
    VmafMtlFf reference_mean;
    VmafMtlFf comparison_mean;
    VmafMtlFf reference_square;
    VmafMtlFf comparison_square;
    VmafMtlFf cross_product;
} VmafMtlSsimPairs;

VMAF_MTL_FUNC VmafMtlSsimMoments vmaf_mtl_ssim_moments_make(float reference_mean,
                                                            float comparison_mean,
                                                            float reference_square,
                                                            float comparison_square,
                                                            float cross_product)
{
    const VmafMtlSsimMoments m = {reference_mean, comparison_mean, reference_square,
                                  comparison_square, cross_product};
    return m;
}

VMAF_MTL_FUNC VmafMtlSsimPairs vmaf_mtl_ssim_pairs_zero(void)
{
    const VmafMtlFf zero = {0.0f, 0.0f};
    const VmafMtlSsimPairs p = {zero, zero, zero, zero, zero};
    return p;
}

/* One tap of iqa/convolve.c: the fp32 product `img * kernel`, added to the
 * pass's sum. The CPU adds these products in double and rounds the sum to
 * fp32 once; a pair sum carries it to about 2^-46, so the rounded pair is the
 * CPU's fp32 value except within that distance of a rounding midpoint. */
VMAF_MTL_FUNC VmafMtlFf vmaf_mtl_ssim_add_tap(VmafMtlFf sum, float sample, float weight)
{
    const float product = sample * weight;
    return vmaf_mtl_ff_add(sum, vmaf_mtl_ff_make(product, 0.0f));
}

/* Horizontal pass: the CPU convolves ref, cmp and the fp32 products
 * ref * ref, cmp * cmp and ref * cmp (ssim_precompute). */
VMAF_MTL_FUNC VmafMtlSsimPairs vmaf_mtl_ssim_add_horizontal_tap(VmafMtlSsimPairs sums, float ref,
                                                                float cmp, float weight)
{
    const float ref_sq = ref * ref;
    const float cmp_sq = cmp * cmp;
    const float ref_cmp = ref * cmp;
    VmafMtlSsimPairs next = sums;
    next.reference_mean = vmaf_mtl_ssim_add_tap(sums.reference_mean, ref, weight);
    next.comparison_mean = vmaf_mtl_ssim_add_tap(sums.comparison_mean, cmp, weight);
    next.reference_square = vmaf_mtl_ssim_add_tap(sums.reference_square, ref_sq, weight);
    next.comparison_square = vmaf_mtl_ssim_add_tap(sums.comparison_square, cmp_sq, weight);
    next.cross_product = vmaf_mtl_ssim_add_tap(sums.cross_product, ref_cmp, weight);
    return next;
}

/* Vertical pass over the five horizontal results. */
VMAF_MTL_FUNC VmafMtlSsimPairs vmaf_mtl_ssim_add_vertical_tap(VmafMtlSsimPairs sums,
                                                              VmafMtlSsimMoments row, float weight)
{
    VmafMtlSsimPairs next = sums;
    next.reference_mean = vmaf_mtl_ssim_add_tap(sums.reference_mean, row.reference_mean, weight);
    next.comparison_mean = vmaf_mtl_ssim_add_tap(sums.comparison_mean, row.comparison_mean, weight);
    next.reference_square =
        vmaf_mtl_ssim_add_tap(sums.reference_square, row.reference_square, weight);
    next.comparison_square =
        vmaf_mtl_ssim_add_tap(sums.comparison_square, row.comparison_square, weight);
    next.cross_product = vmaf_mtl_ssim_add_tap(sums.cross_product, row.cross_product, weight);
    return next;
}

/* A pass result, `(float)(sum * scale)` with scale 1 on the CPU: the pair is
 * normalised, so hi is hi + lo rounded to fp32. */
VMAF_MTL_FUNC VmafMtlSsimMoments vmaf_mtl_ssim_round_moments(VmafMtlSsimPairs sums)
{
    return vmaf_mtl_ssim_moments_make(sums.reference_mean.hi, sums.comparison_mean.hi,
                                      sums.reference_square.hi, sums.comparison_square.hi,
                                      sums.cross_product.hi);
}

/* The fp32 values iqa/ssim_tools.c forms for one pixel before its fp64
 * terms: the arguments of ssim_accumulate_lane() (ssim_accumulate_lane.h). */
typedef struct VmafMtlSsimFloatParts {
    float reference_mean;
    float comparison_mean;
    float srsc;      /* sqrtf(ref_sigma_sqd * cmp_sigma_sqd) */
    float l_den;     /* ref_mu^2 + cmp_mu^2 + C1 */
    float c_den;     /* ref_sigma_sqd + cmp_sigma_sqd + C2 */
    float structure; /* sv_f, the fp32 quotient S */
} VmafMtlSsimFloatParts;

/* iqa/ssim_tools.c for one pixel (ssim_variance_scalar, then the fp32 part of
 * ssim_accumulate_default_scalar, which ssim_accumulate_lane.h shares with
 * the SIMD paths): fp32 variances clamped at zero, the covariance, one fp32
 * square root of their product, the fp32 denominators and S. There is no
 * identical-window shortcut: on a flat identical window the fp32 l_den
 * rounds below 2 * mu^2 + C1, and the CPU keeps that. */
VMAF_MTL_FUNC VmafMtlSsimFloatParts vmaf_mtl_ssim_float_parts(VmafMtlSsimMoments m, float c1,
                                                              float c2)
{
    const float ref_mean_sq = m.reference_mean * m.reference_mean;
    const float cmp_mean_sq = m.comparison_mean * m.comparison_mean;
    const float mean_product = m.reference_mean * m.comparison_mean;
    const float ref_var_raw = m.reference_square - ref_mean_sq;
    const float cmp_var_raw = m.comparison_square - cmp_mean_sq;
    const float ref_var = ref_var_raw < 0.0f ? 0.0f : ref_var_raw;
    const float cmp_var = cmp_var_raw < 0.0f ? 0.0f : cmp_var_raw;
    const float covariance = m.cross_product - mean_product;
    const float var_product = ref_var * cmp_var;
    const float srsc = VMAF_MTL_SQRT(var_product);
    const float l_den_sum = ref_mean_sq + cmp_mean_sq;
    const float l_den = l_den_sum + c1;
    const float c_den_sum = ref_var + cmp_var;
    const float c_den = c_den_sum + c2;
    const float c3 = c2 / 2.0f;
    const float flat_covariance = (covariance < 0.0f && srsc <= 0.0f) ? 0.0f : covariance;
    const float s_num = flat_covariance + c3;
    const float s_den = srsc + c3;
    const VmafMtlSsimFloatParts parts = {m.reference_mean, m.comparison_mean, srsc, l_den, c_den,
                                         s_num / s_den};
    return parts;
}

/* One pixel's terms as the CPU's own values: `lv` and `cv` of
 * ssim_accumulate_lane() are doubles, held here as a significand and an
 * exponent in integers (metal_soft_signed.h; a kernel has no fp64 type), and
 * `sv` is the fp32 quotient converted. */
typedef struct VmafMtlSsimDoubleTerms {
    VmafMtlSoftSigned luminance;
    VmafMtlSoftSigned contrast;
    float structure;
} VmafMtlSsimDoubleTerms;

/* ssim_accumulate_lane()'s two fp64 expressions, operation for operation:
 *
 *     const double lv = (2.0 * rm * cm + C1) / l_den;
 *     const double cv = (2.0 * srsc + C2) / c_den;
 *
 * `2.0 * rm * cm` is exact in fp64 (two 24-bit significands), so lv is one
 * rounded sum and one rounded quotient, and so is cv. Both numerators and
 * denominators are positive: the means and srsc are not negative and C1 and
 * C2 are positive. */
VMAF_MTL_FUNC VmafMtlSsimDoubleTerms vmaf_mtl_ssim_double_terms(VmafMtlSsimFloatParts p, float c1,
                                                                float c2)
{
    const VmafMtlSoftSigned mean_product =
        vmaf_mtl_signed_mul(vmaf_mtl_signed_from_float(p.reference_mean),
                            vmaf_mtl_signed_from_float(p.comparison_mean));
    const VmafMtlSoftSigned l_num =
        vmaf_mtl_signed_add(vmaf_mtl_signed_twice(mean_product), vmaf_mtl_signed_from_float(c1));
    const VmafMtlSoftSigned c_num = vmaf_mtl_signed_add(
        vmaf_mtl_signed_twice(vmaf_mtl_signed_from_float(p.srsc)), vmaf_mtl_signed_from_float(c2));
    const VmafMtlSsimDoubleTerms terms = {
        vmaf_mtl_signed_div(l_num, vmaf_mtl_signed_from_float(p.l_den)),
        vmaf_mtl_signed_div(c_num, vmaf_mtl_signed_from_float(p.c_den)), p.structure};
    return terms;
}

/* The fp64 bit pattern of the pixel's SSIM term, ssim_accumulate_lane()'s
 * `lv * cv * sv`: two rounded fp64 products, left to right. */
VMAF_MTL_FUNC vmaf_mtl_u64 vmaf_mtl_ssim_product_bits(VmafMtlSsimDoubleTerms t)
{
    return vmaf_mtl_signed_bits(vmaf_mtl_signed_mul(vmaf_mtl_signed_mul(t.luminance, t.contrast),
                                                    vmaf_mtl_signed_from_float(t.structure)));
}

/* ssim_init_args' C1 and C2 for L = 255, K1 = 0.01, K2 = 0.03, in fp32. */
typedef struct VmafMtlSsimConstants {
    float c1;
    float c2;
} VmafMtlSsimConstants;

VMAF_MTL_FUNC VmafMtlSsimConstants vmaf_mtl_ssim_constants(void)
{
    const float range = 255.0f;
    const float k1 = 0.01f;
    const float k2 = 0.03f;
    const VmafMtlSsimConstants c = {(k1 * range) * (k1 * range), (k2 * range) * (k2 * range)};
    return c;
}

#if !defined(__METAL_VERSION__)

#include <stddef.h>

/* iqa_ssim()'s four frame sums (host side). */
typedef struct VmafMtlSsimFrameSums {
    double ssim;
    double luminance;
    double contrast;
    double structure;
} VmafMtlSsimFrameSums;

static inline double vmaf_mtl_ssim_double_of(vmaf_mtl_u64 bits)
{
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* ssim_accumulate_lane() for one window, with its lv, cv and sv (host side):
 * each sum is one double that takes the windows in the caller's order. */
static inline void vmaf_mtl_ssim_accumulate_window(VmafMtlSsimFrameSums *sums, double lv, double cv,
                                                   double sv)
{
    sums->ssim += lv * cv * sv;
    sums->luminance += lv;
    sums->contrast += cv;
    sums->structure += sv;
}

/* The four sums over a plane's windows in raster order (host side), from the
 * per-window lv and cv bit patterns and the fp32 sv a kernel stored: the
 * CPU's sums, since the terms and the order are the CPU's. */
static inline VmafMtlSsimFrameSums vmaf_mtl_ssim_frame_sums(const vmaf_mtl_u64 *luminance,
                                                            const vmaf_mtl_u64 *contrast,
                                                            const float *structure, size_t count)
{
    VmafMtlSsimFrameSums sums = {0.0, 0.0, 0.0, 0.0};
    for (size_t i = 0u; i < count; i++) {
        vmaf_mtl_ssim_accumulate_window(&sums, vmaf_mtl_ssim_double_of(luminance[i]),
                                        vmaf_mtl_ssim_double_of(contrast[i]), (double)structure[i]);
    }
    return sums;
}

/* iqa_ssim()'s lv, cv and sv sums without the product (host side), for a
 * caller that uses the three means alone, as ms_ssim.c does per scale. Each
 * sum is one double that takes the windows in raster order; `ssim` stays 0. */
static inline VmafMtlSsimFrameSums vmaf_mtl_ssim_lcs_sums(const vmaf_mtl_u64 *luminance,
                                                          const vmaf_mtl_u64 *contrast,
                                                          const float *structure, size_t count)
{
    VmafMtlSsimFrameSums sums = {0.0, 0.0, 0.0, 0.0};
    for (size_t i = 0u; i < count; i++) {
        sums.luminance += vmaf_mtl_ssim_double_of(luminance[i]);
        sums.contrast += vmaf_mtl_ssim_double_of(contrast[i]);
        sums.structure += (double)structure[i];
    }
    return sums;
}

/* iqa_ssim()'s `ssim` sum alone (host side): the stored product bits of
 * every window added into one double in raster order. */
static inline double vmaf_mtl_ssim_product_sum(const vmaf_mtl_u64 *terms, size_t count)
{
    double sum = 0.0;
    for (size_t i = 0u; i < count; i++) {
        sum += vmaf_mtl_ssim_double_of(terms[i]);
    }
    return sum;
}

#endif /* !__METAL_VERSION__ */

#endif /* VMAF_FEATURE_METAL_METAL_SSIM_TERMS_H_ */
