/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2 AND BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  The decimation and the scale combine of float_ms_ssim_metal (ADR-1498),
 *  valid as Metal Shading Language (float_ms_ssim.metal) and as host C and
 *  C++, where test_metal_float_ms_ssim_math holds them against
 *  ms_ssim_decimate.c and ms_ssim.c.
 *
 *  Decimation: ms_ssim_decimate.c's two separable passes with the 9-tap
 *  biorthogonal 9/7 low-pass, every tap one fused multiply-add,
 *  `acc = fma(sample, tap, acc)`, because the reference accumulates with
 *  vmaf_fmaf_exact() and its AVX2, AVX-512 and NEON twins with a fused
 *  multiply-add (ADR-0891). The fusion is part of the arithmetic, so it is
 *  spelled VMAF_MTL_FMA() and does not depend on the compiler contracting
 *  `a * b + c`, which the kernels' build forbids (-ffp-contract=off). The
 *  horizontal pass fills a (w / 2 + (w & 1)) x h plane and the vertical pass
 *  reads it at mirrored rows, as the reference does (ADR-1414 for SYCL,
 *  ADR-1403 for CUDA).
 *
 *  Combine (host side): ms_ssim.c::ms_ssim_score_scales(), fabs() of all
 *  three fp32 scale means, the exponents promoted from fp32, the three
 *  powers multiplied left to right.
 *
 *  The window sums and the l / c / s terms of the scales are
 *  metal_ssim_terms.h's, shared with float_ssim_metal. Name mapping to
 *  sycl/integer_ms_ssim_sycl.cpp: mirror_idx() -> vmaf_mtl_msdec_mirror(),
 *  LPF -> vmaf_mtl_msdec_lpf, the sycl::fma() tap -> vmaf_mtl_msdec_tap(),
 *  combine_ms_ssim() -> vmaf_mtl_ms_ssim_combine().
 */

#ifndef VMAF_FEATURE_METAL_METAL_MS_SSIM_MATH_H_
#define VMAF_FEATURE_METAL_METAL_MS_SSIM_MATH_H_

#include "metal_portable.h"

#define VMAF_MTL_MSDEC_TAPS 9
#define VMAF_MTL_MSDEC_HALF 4

/* ms_ssim_decimate.c's ms_ssim_lpf_h and ms_ssim_lpf_v: the same nine taps. */
VMAF_MTL_CONSTANT float vmaf_mtl_msdec_lpf[VMAF_MTL_MSDEC_TAPS] = {
    0.026727f, -0.016828f, -0.078201f, 0.266846f, 0.602914f,
    0.266846f, -0.078201f, -0.016828f, 0.026727f};

/* The arguments of a decimation kernel, laid out alike in MSL and on the
 * host: the plane it reads and the plane it writes. */
typedef struct VmafMtlMsdecParams {
    vmaf_mtl_u32 width;
    vmaf_mtl_u32 height;
    vmaf_mtl_u32 output_width;
    vmaf_mtl_u32 output_height;
} VmafMtlMsdecParams;

/* ms_ssim_decimate_mirror(): period-2n mirror with the edge sample repeated,
 * also valid for an offset beyond one reflection. */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_msdec_mirror(vmaf_mtl_i32 idx, vmaf_mtl_i32 n)
{
    const vmaf_mtl_i32 period = 2 * n;
    vmaf_mtl_i32 r = idx % period;
    if (r < 0) {
        r += period;
    }
    if (r >= n) {
        r = period - r - 1;
    }
    return r;
}

/* One tap of either pass: `acc = fmaf(sample, tap, acc)`. */
VMAF_MTL_FUNC float vmaf_mtl_msdec_tap(float acc, float sample, float tap)
{
    return VMAF_MTL_FMA(sample, tap, acc);
}

/* The extent of a decimated line: `(n / 2) + (n & 1)`. */
VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_msdec_extent(vmaf_mtl_u32 n)
{
    return (n / 2u) + (n & 1u);
}

#if !defined(__METAL_VERSION__)

#include <math.h>

#define VMAF_MTL_MS_SSIM_SCALES 5

/* ms_ssim.c's g_alphas, g_betas and g_gammas (Wang weights). */
static const float vmaf_mtl_ms_ssim_alphas[VMAF_MTL_MS_SSIM_SCALES] = {0.0000f, 0.0000f, 0.0000f,
                                                                       0.0000f, 0.1333f};
static const float vmaf_mtl_ms_ssim_betas[VMAF_MTL_MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f,
                                                                      0.2363f, 0.1333f};
static const float vmaf_mtl_ms_ssim_gammas[VMAF_MTL_MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f,
                                                                       0.2363f, 0.1333f};

/* iqa_ssim()'s mean of one kind of term, `(float)(sum / (double)(w * h))`
 * (host side). */
static inline double vmaf_mtl_ms_ssim_scale_mean(double sum, double pixels)
{
    return (double)(float)(sum / pixels);
}

/* ms_ssim.c::ms_ssim_score_scales() (host side): fabs() of all three fp32
 * means, the exponents promoted from fp32, the three powers multiplied left
 * to right. */
static inline double vmaf_mtl_ms_ssim_combine(const double luminance[VMAF_MTL_MS_SSIM_SCALES],
                                              const double contrast[VMAF_MTL_MS_SSIM_SCALES],
                                              const double structure[VMAF_MTL_MS_SSIM_SCALES])
{
    double score = 1.0;
    for (int scale = 0; scale < VMAF_MTL_MS_SSIM_SCALES; scale++) {
        score *= pow(fabs(luminance[scale]), (double)vmaf_mtl_ms_ssim_alphas[scale]) *
                 pow(fabs(contrast[scale]), (double)vmaf_mtl_ms_ssim_betas[scale]) *
                 pow(fabs(structure[scale]), (double)vmaf_mtl_ms_ssim_gammas[scale]);
    }
    return score;
}

#endif /* !__METAL_VERSION__ */

#endif /* VMAF_FEATURE_METAL_METAL_MS_SSIM_MATH_H_ */
