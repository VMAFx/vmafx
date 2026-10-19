/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  The per-sample arithmetic of float_ms_ssim_hip (ADR-1403): everything the
 *  kernels in ms_ssim_score.hip compute for one output sample, written as the
 *  CPU extractor computes it, operation for operation and type for type.
 *
 *    - vmaf_hip_ms_ssim_decimate_sample(): ms_ssim_decimate.c. Every tap is
 *      one fused multiply-add (vmaf_fmaf_exact() there, fmaf() here), first
 *      along the row, then down the nine row sums.
 *    - vmaf_hip_ms_ssim_horizontal() / vmaf_hip_ms_ssim_vertical():
 *      iqa/convolve.c. Each tap is an fp32 product, the products are added
 *      in fp64, and the sum is rounded to fp32 once per pass; the fp64 sum is
 *      carried as an exact fp32 pair (VmafHipMsPair). The squares and the
 *      cross product are fp32 products formed first, as
 *      ssim_precompute_scalar() stores them.
 *    - vmaf_hip_ms_ssim_lcs(): ssim_variance_scalar() and
 *      ssim_accumulate_default_scalar() (iqa/ssim_tools.c) with their mixed
 *      operands: fp32 variances with the reference's clamp, fp64 numerators
 *      over fp32 denominators for l and c, and s as an fp32 quotient.
 *
 *  None of it may be contracted or approximated: the HSACO builds with
 *  `hip_strict_fp_args` (ADR-1407: no FMA contraction, correctly rounded fp32
 *  division and square root), and the one fusion the reference has is spelled
 *  fmaf(). The kernels add nothing on top: the sum of l / c / s over a scale
 *  is formed on the host in the CPU's raster order, because another order is
 *  another fp64 sum and its mean can round to the neighbouring float
 *  (T-GPU-FLOAT-SSIM-FRAME-SUM-ORDER-2026-10-02).
 *
 *  Plain C as well as HIP C++: ms_ssim_score.hip and the device-free
 *  test_hip_ms_ssim_arith.c compile the same lines, and the test holds them
 *  against the CPU extractor itself.
 */

#ifndef VMAF_SRC_FEATURE_HIP_INTEGER_MS_SSIM_MS_SSIM_ARITH_H_
#define VMAF_SRC_FEATURE_HIP_INTEGER_MS_SSIM_MS_SSIM_ARITH_H_

#ifdef __cplusplus
#include <cmath>
#include <cstddef>
#else
#include <math.h>
#include <stddef.h>
#endif

#include "../hip_tile_index.h"

/* Taps of the Gaussian window (iqa/ssim_tools.h::g_gaussian_window_h). */
#define VMAF_HIP_MS_SSIM_WINDOW 11
/* Taps of the 9/7 biorthogonal low-pass (ms_ssim_decimate.c). */
#define VMAF_HIP_MS_SSIM_LPF_LEN 9
#define VMAF_HIP_MS_SSIM_LPF_HALF 4

/* The five window statistics of one sample after a convolution pass. */
struct VmafHipMsWindow {
    float ref_mu;
    float cmp_mu;
    float ref_sq;
    float cmp_sq;
    float refcmp;
};
#ifndef __cplusplus
typedef struct VmafHipMsWindow VmafHipMsWindow;
#endif

/* The five horizontal-pass planes the vertical pass reads. */
struct VmafHipMsPlanes {
    const float *ref_mu;
    const float *cmp_mu;
    const float *ref_sq;
    const float *cmp_sq;
    const float *refcmp;
};
#ifndef __cplusplus
typedef struct VmafHipMsPlanes VmafHipMsPlanes;
#endif

/* One sample's luminance, contrast and structure terms. */
struct VmafHipMsLcs {
    double l;
    double c;
    double s;
};
#ifndef __cplusplus
typedef struct VmafHipMsLcs VmafHipMsLcs;
#endif

/* ms_ssim_decimate.c::ms_ssim_decimate_mirror(): period-2n mirror, edge
 * sample repeated. */
VMAF_HIP_HOST_DEVICE int vmaf_hip_ms_ssim_mirror(int idx, int n)
{
    const int period = 2 * n;
    int r = idx % period;
    if (r < 0) {
        r += period;
    }
    return (r >= n) ? period - r - 1 : r;
}

/* ms_ssim_decimate.c::ms_ssim_lpf_h / ms_ssim_lpf_v (the same nine taps). */
VMAF_HIP_HOST_DEVICE float vmaf_hip_ms_ssim_lpf_tap(int k)
{
    static const float lpf[VMAF_HIP_MS_SSIM_LPF_LEN] = {
        0.026727f, -0.016828f, -0.078201f, 0.266846f, 0.602914f,
        0.266846f, -0.078201f, -0.016828f, 0.026727f,
    };
    return lpf[k];
}

/* iqa/ssim_tools.h::g_gaussian_window_h / g_gaussian_window_v. */
VMAF_HIP_HOST_DEVICE float vmaf_hip_ms_ssim_window_tap(int k)
{
    static const float window[VMAF_HIP_MS_SSIM_WINDOW] = {
        0.001028f, 0.007599f, 0.036001f, 0.109361f, 0.213006f, 0.266012f,
        0.213006f, 0.109361f, 0.036001f, 0.007599f, 0.001028f,
    };
    return window[k];
}

/* Output sample (x_out, y_out) of ms_ssim_decimate() over the w x h plane
 * `src`: the h-pass value of each of the nine source rows, then the v-pass
 * over them, every tap one fused multiply-add in the reference's order. */
VMAF_HIP_HOST_DEVICE float vmaf_hip_ms_ssim_decimate_sample(const float *src, int w, int h,
                                                            int x_out, int y_out)
{
    const int x_src = x_out * 2;
    const int y_src = y_out * 2;
    float acc = 0.0f;
    for (int kv = 0; kv < VMAF_HIP_MS_SSIM_LPF_LEN; kv++) {
        const int yi = vmaf_hip_ms_ssim_mirror(y_src + kv - VMAF_HIP_MS_SSIM_LPF_HALF, h);
        const float *row = src + (size_t)yi * (size_t)w;
        float row_acc = 0.0f;
        for (int ku = 0; ku < VMAF_HIP_MS_SSIM_LPF_LEN; ku++) {
            const int xi = vmaf_hip_ms_ssim_mirror(x_src + ku - VMAF_HIP_MS_SSIM_LPF_HALF, w);
            row_acc = fmaf(row[xi], vmaf_hip_ms_ssim_lpf_tap(ku), row_acc);
        }
        acc = fmaf(row_acc, vmaf_hip_ms_ssim_lpf_tap(kv), acc);
    }
    return acc;
}

/* A window sum as iqa_convolve() forms it: fp32 terms added in fp64 and
 * rounded to fp32 once. The sum is carried as an fp32 pair instead of an fp64
 * value: `hi` is the running fp32 sum and `lo` collects the exact rounding
 * error of every step (Knuth's two-sum, six fp32 operations), so hi + lo
 * holds the eleven-term sum to about 48 bits and rounds to the fp32 value the
 * reference's 53-bit sum rounds to, unless the exact sum lies within about
 * 2^-18 ulp of an fp32 rounding boundary. An fp64 accumulator gives the same
 * scores on a gfx1036 and costs 126 ms more per 3840x2160 frame (173 -> 299):
 * the device runs fp64 at a fraction of its fp32 rate. The six operations
 * must stay six: no contraction, no reassociation (`hip_strict_fp_args`). */
struct VmafHipMsPair {
    float hi;
    float lo;
};
#ifndef __cplusplus
typedef struct VmafHipMsPair VmafHipMsPair;
#endif

VMAF_HIP_HOST_DEVICE void vmaf_hip_ms_pair_add(VmafHipMsPair *sum, float term)
{
    const float total = sum->hi + term;
    const float term_virtual = total - sum->hi;
    const float hi_virtual = total - term_virtual;
    const float error = (sum->hi - hi_virtual) + (term - term_virtual);
    sum->hi = total;
    sum->lo = sum->lo + error;
}

VMAF_HIP_HOST_DEVICE float vmaf_hip_ms_pair_round(const VmafHipMsPair *sum)
{
    return sum->hi + sum->lo;
}

/* Five window sums in progress, and their rounded values. */
struct VmafHipMsSums {
    VmafHipMsPair ref_mu;
    VmafHipMsPair cmp_mu;
    VmafHipMsPair ref_sq;
    VmafHipMsPair cmp_sq;
    VmafHipMsPair refcmp;
};
#ifndef __cplusplus
typedef struct VmafHipMsSums VmafHipMsSums;
#endif

VMAF_HIP_HOST_DEVICE VmafHipMsWindow vmaf_hip_ms_sums_round(const VmafHipMsSums *sums)
{
    VmafHipMsWindow out;
    out.ref_mu = vmaf_hip_ms_pair_round(&sums->ref_mu);
    out.cmp_mu = vmaf_hip_ms_pair_round(&sums->cmp_mu);
    out.ref_sq = vmaf_hip_ms_pair_round(&sums->ref_sq);
    out.cmp_sq = vmaf_hip_ms_pair_round(&sums->cmp_sq);
    out.refcmp = vmaf_hip_ms_pair_round(&sums->refcmp);
    return out;
}

/* iqa_convolve_horizontal_pass() at one sample: `ref` and `cmp` point at the
 * first of the eleven taps of their row. `prod = img * tap` in fp32 and
 * `sum += prod` in fp64; the window is normalised, so the reference's
 * `* scale` multiplies by 1. */
VMAF_HIP_HOST_DEVICE VmafHipMsWindow vmaf_hip_ms_ssim_horizontal(const float *ref, const float *cmp)
{
    VmafHipMsSums sums = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    for (int u = 0; u < VMAF_HIP_MS_SSIM_WINDOW; u++) {
        const float r = ref[u];
        const float c = cmp[u];
        const float tap = vmaf_hip_ms_ssim_window_tap(u);
        const float r_sq = r * r;
        const float c_sq = c * c;
        const float r_c = r * c;
        vmaf_hip_ms_pair_add(&sums.ref_mu, r * tap);
        vmaf_hip_ms_pair_add(&sums.cmp_mu, c * tap);
        vmaf_hip_ms_pair_add(&sums.ref_sq, r_sq * tap);
        vmaf_hip_ms_pair_add(&sums.cmp_sq, c_sq * tap);
        vmaf_hip_ms_pair_add(&sums.refcmp, r_c * tap);
    }
    return vmaf_hip_ms_sums_round(&sums);
}

/* iqa_convolve_vertical_pass() at one sample: `first` is the index of the top
 * tap in the horizontal-pass planes and `stride` their row pitch. */
VMAF_HIP_HOST_DEVICE VmafHipMsWindow vmaf_hip_ms_ssim_vertical(const VmafHipMsPlanes *planes,
                                                               size_t first, size_t stride)
{
    VmafHipMsSums sums = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    for (int v = 0; v < VMAF_HIP_MS_SSIM_WINDOW; v++) {
        const size_t idx = first + (size_t)v * stride;
        const float tap = vmaf_hip_ms_ssim_window_tap(v);
        vmaf_hip_ms_pair_add(&sums.ref_mu, planes->ref_mu[idx] * tap);
        vmaf_hip_ms_pair_add(&sums.cmp_mu, planes->cmp_mu[idx] * tap);
        vmaf_hip_ms_pair_add(&sums.ref_sq, planes->ref_sq[idx] * tap);
        vmaf_hip_ms_pair_add(&sums.cmp_sq, planes->cmp_sq[idx] * tap);
        vmaf_hip_ms_pair_add(&sums.refcmp, planes->refcmp[idx] * tap);
    }
    return vmaf_hip_ms_sums_round(&sums);
}

/* ssim_variance_scalar() then ssim_accumulate_default_scalar() for one
 * sample. `c1`, `c2` and `c3` are iqa_ssim()'s fp32 stabilisation constants.
 * The variances and the covariance are fp32 with the reference's
 * `MAX(0.0, x)` clamp, `2.0 *` promotes the numerators of l and c to fp64,
 * their denominators are fp32 sums, and s is an fp32 quotient. */
VMAF_HIP_HOST_DEVICE VmafHipMsLcs vmaf_hip_ms_ssim_lcs(const VmafHipMsWindow *st, float c1,
                                                       float c2, float c3)
{
    const float ref_mu = st->ref_mu;
    const float cmp_mu = st->cmp_mu;
    const float ref_diff = st->ref_sq - ref_mu * ref_mu;
    const float cmp_diff = st->cmp_sq - cmp_mu * cmp_mu;
    const float ref_var = (0.0f > ref_diff) ? 0.0f : ref_diff;
    const float cmp_var = (0.0f > cmp_diff) ? 0.0f : cmp_diff;
    const float covar = st->refcmp - ref_mu * cmp_mu;
    const float sigma = sqrtf(ref_var * cmp_var);
    /* zli-nflx: a flat region of an identical pair can leave the covariance
     * slightly negative while sigma is zero; clamp so s stays 1. */
    const float clamped_covar = (covar < 0.0f && sigma <= 0.0f) ? 0.0f : covar;
    VmafHipMsLcs out;
    out.l = (2.0 * (double)ref_mu * (double)cmp_mu + (double)c1) /
            (double)(ref_mu * ref_mu + cmp_mu * cmp_mu + c1);
    out.c = (2.0 * (double)sigma + (double)c2) / (double)(ref_var + cmp_var + c2);
    out.s = (double)((clamped_covar + c3) / (sigma + c3));
    return out;
}

#if !defined(__HIPCC__)

/* The host part of the score: what integer_ms_ssim_hip.c does with the
 * device's sums, and the device-free test with its own. */

#define VMAF_HIP_MS_SSIM_SCALES 5

/* iqa_ssim()'s stabilisation constants: fp32 arithmetic on fp32 K1 / K2. */
static inline void vmaf_hip_ms_ssim_constants(float *c1, float *c2, float *c3)
{
    const int L = 255;
    const float K1 = 0.01f;
    const float K2 = 0.03f;
    *c1 = (K1 * (float)L) * (K1 * (float)L);
    *c2 = (K2 * (float)L) * (K2 * (float)L);
    *c3 = *c2 / 2.0f;
}

/* iqa_ssim()'s per-scale mean of l, c or s: the fp64 sum over the scale's
 * `samples` window positions, divided, and returned as a float. `sum` has to
 * be the reference's raster-order sum: the rounding to fp32 does not hide a
 * sum formed in another order on every input. */
static inline double vmaf_hip_ms_ssim_scale_mean(double sum, double samples)
{
    return (double)(float)(sum / samples);
}

/* ms_ssim.c::ms_ssim_score_scales(): the Wang product of the per-scale means,
 * with fabs() on all three terms. */
static inline double vmaf_hip_ms_ssim_combine(const double *l_means, const double *c_means,
                                              const double *s_means)
{
    static const float alphas[VMAF_HIP_MS_SSIM_SCALES] = {0.0000f, 0.0000f, 0.0000f, 0.0000f,
                                                          0.1333f};
    static const float betas[VMAF_HIP_MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f,
                                                         0.1333f};
    static const float gammas[VMAF_HIP_MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f,
                                                          0.1333f};
    double msssim = 1.0;
    for (int i = 0; i < VMAF_HIP_MS_SSIM_SCALES; i++) {
        msssim *= pow(fabs(l_means[i]), (double)alphas[i]) *
                  pow(fabs(c_means[i]), (double)betas[i]) *
                  pow(fabs(s_means[i]), (double)gammas[i]);
    }
    return msssim;
}

#endif /* !defined(__HIPCC__) */

#endif /* VMAF_SRC_FEATURE_HIP_INTEGER_MS_SSIM_MS_SSIM_ARITH_H_ */
