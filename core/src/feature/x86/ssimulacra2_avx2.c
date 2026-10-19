/**
 *
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 */

/*
 * AVX2 port of the SSIMULACRA 2 pointwise, reduction, and downsampling
 * kernels. The scalar reference is libvmaf/src/feature/ssimulacra2.c
 * — bit-exactness contract per ADR-0161 mirrors ADR-0159/ADR-0160:
 *
 *   - Pointwise float arithmetic is vectorised lane-wise (single-
 *     precision). Because IEEE-754 add/mul/sub are lane-commutative
 *     (same op applied independently to each lane = same result as
 *     scalar per-element), the vector result equals the scalar
 *     result byte-for-byte under FLT_EVAL_METHOD == 0.
 *
 *   - Transcendental calls (`cbrtf`, `powf`) are NOT vectorised.
 *     They are applied per-lane via scalar libm inside a SIMD loop,
 *     matching the scalar reference byte-for-byte. The surrounding
 *     float arithmetic (matmul, bias, scale) is vectorised.
 *
 *   - Reductions over large buffers (ssim_map, edge_diff_map) accumulate
 *     into `double` via per-lane scalar tails (ADR-0139 pattern) to
 *     preserve the scalar summation tree. Vectorising the per-pixel
 *     arithmetic and then reducing lane-parallel would change the
 *     summation order.
 *
 * `#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#pragma STDC FP_CONTRACT OFF
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif` disables FMA contraction to match
 * the scalar reference compiled without FMA fusion.
 */

#include <assert.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */
#include <immintrin.h>
#include <math.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "feature/ssimulacra2_math.h"
#include "feature/ssimulacra2_score.h"
#include "feature/ssimulacra2_simd_common.h"
#include "ssimulacra2_avx2.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#if !defined(_MSC_VER) /* cl.exe has no contraction pragma; /fp:precise does not contract */
#pragma STDC FP_CONTRACT OFF
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* Scalar constants mirrored from ssimulacra2.c. Keeping them local
 * avoids dragging `Ssimu2State` into this TU. */
static const float kM00 = 0.30f;
static const float kM02 = 0.078f;
static const float kM10 = 0.23f;
static const float kM12 = 0.078f;
static const float kM20 = 0.24342268924547819f;
static const float kM21 = 0.20476744424496821f;
static const float kOpsinBias = 0.0037930732552754493f;
static const float kC2 = 0.0009f;

/* Per-lane scalar cbrtf on an __m256. Applying libm `cbrtf` on each
 * lane preserves byte-for-byte parity with the scalar reference. */
static inline __m256 cbrtf_lane_avx2(__m256 v)
{
    alignas(32) float tmp[8];
    _mm256_store_ps(tmp, v);
    for (int k = 0; k < 8; k++) {
        tmp[k] = vmaf_ss2_cbrtf(tmp[k]);
    }
    return _mm256_load_ps(tmp);
}

void ssimulacra2_multiply_3plane_avx2(const float *a, const float *b, float *mul, unsigned w,
                                      unsigned h)
{
    const size_t n = 3u * (size_t)w * (size_t)h;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 va = _mm256_loadu_ps(a + i);
        const __m256 vb = _mm256_loadu_ps(b + i);
        _mm256_storeu_ps(mul + i, _mm256_mul_ps(va, vb));
    }
    for (; i < n; i++) {
        mul[i] = a[i] * b[i];
    }
}

/* One 8-pixel block of the linear RGB -> XYB conversion. The helpers below
 * hold the exact statements the single kernel used to carry, in the same
 * order; every operation is a separate IEEE-754 float operation (this TU is
 * built with contraction off), so splitting the kernel does not change a
 * bit. */
typedef struct {
    float m01;
    float m11;
    float m22;
    float cbrt_bias;
} xyb_coefs_avx2_t;

/* LMS mixing matrix + opsin bias, clamped at zero. Addition order MUST match
 * scalar left-to-right: ((a + b) + c) + d. IEEE-754 add is non-associative
 * and test_xyb catches the drift. */
static inline void xyb_lms_avx2(const xyb_coefs_avx2_t *k, __m256 r, __m256 g, __m256 b, __m256 *l,
                                __m256 *m, __m256 *sv)
{
    const __m256 vbias = _mm256_set1_ps(kOpsinBias);
    const __m256 vzero = _mm256_setzero_ps();
    __m256 lv = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(kM00), r),
                              _mm256_mul_ps(_mm256_set1_ps(k->m01), g));
    lv = _mm256_add_ps(lv, _mm256_mul_ps(_mm256_set1_ps(kM02), b));
    lv = _mm256_add_ps(lv, vbias);
    __m256 mv = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(kM10), r),
                              _mm256_mul_ps(_mm256_set1_ps(k->m11), g));
    mv = _mm256_add_ps(mv, _mm256_mul_ps(_mm256_set1_ps(kM12), b));
    mv = _mm256_add_ps(mv, vbias);
    __m256 sw = _mm256_add_ps(_mm256_mul_ps(_mm256_set1_ps(kM20), r),
                              _mm256_mul_ps(_mm256_set1_ps(kM21), g));
    sw = _mm256_add_ps(sw, _mm256_mul_ps(_mm256_set1_ps(k->m22), b));
    sw = _mm256_add_ps(sw, vbias);
    /* Clamp to zero below, as the scalar `if (l < 0.0f) l = 0.0f`. */
    *l = _mm256_max_ps(lv, vzero);
    *m = _mm256_max_ps(mv, vzero);
    *sv = _mm256_max_ps(sw, vzero);
}

/* MakePositiveXYB rescale in libjxl order (B uses Y before the Y offset).
 * X = 0.5 * (L - M), then X * 14 + 0.42: the folded (L - M) * 7 rounds
 * differently and fails the bit-exact assertion in test_xyb. */
static inline void xyb_rescale_avx2(__m256 L, __m256 M, __m256 S, __m256 *xf, __m256 *yf,
                                    __m256 *bf)
{
    const __m256 Y = _mm256_mul_ps(_mm256_set1_ps(0.5f), _mm256_add_ps(L, M));
    *bf = _mm256_add_ps(_mm256_sub_ps(S, Y), _mm256_set1_ps(0.55f));
    const __m256 X_half = _mm256_mul_ps(_mm256_set1_ps(0.5f), _mm256_sub_ps(L, M));
    *xf = _mm256_add_ps(_mm256_mul_ps(X_half, _mm256_set1_ps(14.0f)), _mm256_set1_ps(0.42f));
    *yf = _mm256_add_ps(Y, _mm256_set1_ps(0.01f));
}

static inline void xyb_block_avx2(const xyb_coefs_avx2_t *k, const float *const rgb[3],
                                  float *const xyb[3], size_t i)
{
    __m256 l;
    __m256 m;
    __m256 sv;
    xyb_lms_avx2(k, _mm256_loadu_ps(rgb[0] + i), _mm256_loadu_ps(rgb[1] + i),
                 _mm256_loadu_ps(rgb[2] + i), &l, &m, &sv);
    const __m256 vcbrt_bias = _mm256_set1_ps(k->cbrt_bias);
    /* `cbrtf` is applied per lane through scalar libm; see `cbrtf_lane_avx2`. */
    const __m256 L = _mm256_sub_ps(cbrtf_lane_avx2(l), vcbrt_bias);
    const __m256 M = _mm256_sub_ps(cbrtf_lane_avx2(m), vcbrt_bias);
    const __m256 S = _mm256_sub_ps(cbrtf_lane_avx2(sv), vcbrt_bias);
    __m256 xf;
    __m256 yf;
    __m256 bf;
    xyb_rescale_avx2(L, M, S, &xf, &yf, &bf);
    _mm256_storeu_ps(xyb[0] + i, xf);
    _mm256_storeu_ps(xyb[1] + i, yf);
    _mm256_storeu_ps(xyb[2] + i, bf);
}

/* Scalar tail pixel, identical to the scalar reference body. */
static inline void xyb_pixel_scalar(const xyb_coefs_avx2_t *k, const float *const rgb[3],
                                    float *const xyb[3], size_t i)
{
    const float r = rgb[0][i];
    const float g = rgb[1][i];
    const float bb = rgb[2][i];
    float l = kM00 * r + k->m01 * g + kM02 * bb + kOpsinBias;
    float m = kM10 * r + k->m11 * g + kM12 * bb + kOpsinBias;
    float s = kM20 * r + kM21 * g + k->m22 * bb + kOpsinBias;
    if (l < 0.0f)
        l = 0.0f;
    if (m < 0.0f)
        m = 0.0f;
    if (s < 0.0f)
        s = 0.0f;
    const float L = vmaf_ss2_cbrtf(l) - k->cbrt_bias;
    const float M = vmaf_ss2_cbrtf(m) - k->cbrt_bias;
    const float S = vmaf_ss2_cbrtf(s) - k->cbrt_bias;
    float X = 0.5f * (L - M);
    float Y = 0.5f * (L + M);
    float B = S;
    B = (B - Y) + 0.55f;
    X = X * 14.0f + 0.42f;
    Y = Y + 0.01f;
    xyb[0][i] = X;
    xyb[1][i] = Y;
    xyb[2][i] = B;
}

void ssimulacra2_linear_rgb_to_xyb_avx2(const float *lin, float *xyb, unsigned w, unsigned h)
{
    assert(lin != NULL);
    assert(xyb != NULL);
    assert(w > 0 && h > 0);
    const size_t plane_sz = (size_t)w * (size_t)h;
    const float *const rgb[3] = {lin, lin + plane_sz, lin + 2 * plane_sz};
    float *const out[3] = {xyb, xyb + plane_sz, xyb + 2 * plane_sz};
    const xyb_coefs_avx2_t k = {
        .m01 = 1.0f - kM00 - kM02,
        .m11 = 1.0f - kM10 - kM12,
        .m22 = 1.0f - kM20 - kM21,
        .cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias),
    };

    size_t i = 0;
    for (; i + 8 <= plane_sz; i += 8) {
        xyb_block_avx2(&k, rgb, out, i);
    }
    for (; i < plane_sz; i++) {
        xyb_pixel_scalar(&k, rgb, out, i);
    }
}

void ssimulacra2_downsample_2x2_avx2(const float *in, unsigned iw, unsigned ih, float *out,
                                     unsigned *ow_out, unsigned *oh_out)
{
    const unsigned ow = (iw + 1) / 2;
    const unsigned oh = (ih + 1) / 2;
    *ow_out = ow;
    *oh_out = oh;

    const size_t in_plane = (size_t)iw * (size_t)ih;
    const size_t out_plane = (size_t)ow * (size_t)oh;
    /* The scalar reference clamps ix/iy to (iw-1)/(ih-1) when they
     * exceed the input bounds. Vectorising this requires handling
     * the last column / last row carefully; we stay scalar on those
     * edges and SIMD on the interior. */
    for (int c = 0; c < 3; c++) {
        const float *ip = in + (size_t)c * in_plane;
        float *op = out + (size_t)c * out_plane;
        for (unsigned oy = 0; oy < oh; oy++) {
            const unsigned iy0 = oy * 2;
            const unsigned iy1 = (iy0 + 1 < ih) ? iy0 + 1 : ih - 1;
            const float *row0 = ip + (size_t)iy0 * iw;
            const float *row1 = ip + (size_t)iy1 * iw;
            float *orow = op + (size_t)oy * ow;
            unsigned ox = 0;
            /* SIMD interior: 8 output lanes per iter. To preserve scalar's
             * left-to-right summation order `((r0e + r0o) + r1e) + r1o`,
             * deinterleave even/odd pairs then sum sequentially. */
            const unsigned interior_end = (ow > 0 && iw >= 2) ? ((ow - 1) / 8) * 8 : 0;
            const __m256 vquarter = _mm256_set1_ps(0.25f);
            for (; ox < interior_end; ox += 8) {
                const size_t base = (size_t)ox * 2u;
                const __m256 r00 = _mm256_loadu_ps(row0 + base);
                const __m256 r01 = _mm256_loadu_ps(row0 + base + 8);
                const __m256 r10 = _mm256_loadu_ps(row1 + base);
                const __m256 r11 = _mm256_loadu_ps(row1 + base + 8);
                const __m256 r0e_raw = _mm256_shuffle_ps(r00, r01, 0x88);
                const __m256 r0o_raw = _mm256_shuffle_ps(r00, r01, 0xDD);
                const __m256 r1e_raw = _mm256_shuffle_ps(r10, r11, 0x88);
                const __m256 r1o_raw = _mm256_shuffle_ps(r10, r11, 0xDD);
                const __m256 r0e =
                    _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r0e_raw), 0xD8));
                const __m256 r0o =
                    _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r0o_raw), 0xD8));
                const __m256 r1e =
                    _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r1e_raw), 0xD8));
                const __m256 r1o =
                    _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r1o_raw), 0xD8));
                __m256 acc = _mm256_add_ps(r0e, r0o);
                acc = _mm256_add_ps(acc, r1e);
                acc = _mm256_add_ps(acc, r1o);
                _mm256_storeu_ps(orow + ox, _mm256_mul_ps(acc, vquarter));
            }
            /* Scalar tail for the last-column wraparound + leftover lanes. */
            for (; ox < ow; ox++) {
                unsigned ix0 = ox * 2;
                unsigned ix1 = (ix0 + 1 < iw) ? ix0 + 1 : iw - 1;
                float sum = row0[ix0] + row0[ix1] + row1[ix0] + row1[ix1];
                orow[ox] = sum * 0.25f;
            }
        }
    }
}

static inline double quartic_d(double x)
{
    x *= x;
    return x * x;
}

/* One pixel of the ADR-1208 edge-diff accumulation, taken in double.
 *
 * The vector body and the scalar tail below used to carry identical copies of
 * these ten lines. Sharing one definition is what stops them drifting apart,
 * which is precisely the defect ADR-1208 records: the SIMD path subtracted in
 * float while every scalar reference promoted first, and the ssimulacra2 score
 * came to depend on whether the host had SIMD. The operations and their order
 * are unchanged by the extraction, so the accumulated bits are unchanged.
 *
 * `a1`/`am1`/`a2`/`am2` are `double` parameters on purpose: the callers pass
 * floats and the promotion happens at the call, exactly as the explicit
 * `(double)` casts did before. */
typedef struct {
    double artifact;
    double artifact_quartic;
    double detail;
    double detail_quartic;
} edge_diff_acc_t;

static inline void edge_diff_accum_d(edge_diff_acc_t *acc, double a1, double am1, double a2,
                                     double am2)
{
    const double ed1 = fabs(a1 - am1);
    const double ed2 = fabs(a2 - am2);
    const double d = (1.0 + ed2) / (1.0 + ed1) - 1.0;
    double art;
    double det;
    vmaf_ss2_split_edge_difference(d, &art, &det);
    acc->artifact += art;
    acc->artifact_quartic += quartic_d(art);
    acc->detail += det;
    acc->detail_quartic += quartic_d(det);
}

/* d = 1.0 - (num_m * num_s / denom_s), taken in double to match the scalar
 * reference's (double)num_m * (double)num_s / (double)denom_s (ADR-0139). */
static inline void ssim_accum_d(double *sum_l1, double *sum_l4, float num_m, float num_s,
                                float denom_s)
{
    double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
    if (d < 0.0)
        d = 0.0;
    *sum_l1 += d;
    *sum_l4 += quartic_d(d);
}

/* One 8-pixel block: pointwise float terms in SIMD, then a per-lane double
 * accumulate (spill + scalar tail, ADR-0139) to keep the scalar summation
 * tree. */
static inline void ssim_block_avx2(const float *const p[5], size_t i, double *sum_l1,
                                   double *sum_l4)
{
    const __m256 vc2 = _mm256_set1_ps(kC2);
    const __m256 mu1 = _mm256_loadu_ps(p[0] + i);
    const __m256 mu2 = _mm256_loadu_ps(p[1] + i);
    const __m256 mu11 = _mm256_mul_ps(mu1, mu1);
    const __m256 mu22 = _mm256_mul_ps(mu2, mu2);
    const __m256 mu12 = _mm256_mul_ps(mu1, mu2);
    const __m256 diff = _mm256_sub_ps(mu1, mu2);
    const __m256 num_m = _mm256_sub_ps(_mm256_set1_ps(1.0f), _mm256_mul_ps(diff, diff));
    const __m256 num_s = _mm256_add_ps(
        _mm256_mul_ps(_mm256_set1_ps(2.0f), _mm256_sub_ps(_mm256_loadu_ps(p[4] + i), mu12)), vc2);
    const __m256 denom_s =
        _mm256_add_ps(_mm256_add_ps(_mm256_sub_ps(_mm256_loadu_ps(p[2] + i), mu11),
                                    _mm256_sub_ps(_mm256_loadu_ps(p[3] + i), mu22)),
                      vc2);
    alignas(32) float num_m_f[8];
    alignas(32) float num_s_f[8];
    alignas(32) float denom_s_f[8];
    _mm256_store_ps(num_m_f, num_m);
    _mm256_store_ps(num_s_f, num_s);
    _mm256_store_ps(denom_s_f, denom_s);
    for (int k = 0; k < 8; k++) {
        ssim_accum_d(sum_l1, sum_l4, num_m_f[k], num_s_f[k], denom_s_f[k]);
    }
}

/* Scalar tail pixel, identical to the scalar reference. */
static inline void ssim_pixel_scalar(const float *const p[5], size_t i, double *sum_l1,
                                     double *sum_l4)
{
    const float mu1 = p[0][i];
    const float mu2 = p[1][i];
    const float mu11 = mu1 * mu1;
    const float mu22 = mu2 * mu2;
    const float mu12 = mu1 * mu2;
    const float num_m = 1.0f - (mu1 - mu2) * (mu1 - mu2);
    const float num_s = 2.0f * (p[4][i] - mu12) + kC2;
    const float denom_s = (p[2][i] - mu11) + (p[3][i] - mu22) + kC2;
    ssim_accum_d(sum_l1, sum_l4, num_m, num_s, denom_s);
}

void ssimulacra2_ssim_map_avx2(const float *m1, const float *m2, const float *s11, const float *s22,
                               const float *s12, unsigned w, unsigned h, double plane_averages[6])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;

    for (int c = 0; c < 3; c++) {
        double sum_l1 = 0.0;
        double sum_l4 = 0.0;
        const size_t off = (size_t)c * plane;
        const float *const p[5] = {m1 + off, m2 + off, s11 + off, s22 + off, s12 + off};

        size_t i = 0;
        for (; i + 8 <= plane; i += 8) {
            ssim_block_avx2(p, i, &sum_l1, &sum_l4);
        }
        for (; i < plane; i++) {
            ssim_pixel_scalar(p, i, &sum_l1, &sum_l4);
        }
        plane_averages[c * 2 + 0] = one_per_pixels * sum_l1;
        plane_averages[c * 2 + 1] = sqrt(sqrt(one_per_pixels * sum_l4));
    }
}

void ssimulacra2_edge_diff_map_avx2(const float *img1, const float *mu1, const float *img2,
                                    const float *mu2, unsigned w, unsigned h,
                                    double plane_averages[12])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;

    for (int c = 0; c < 3; c++) {
        edge_diff_acc_t acc = {0.0, 0.0, 0.0, 0.0};
        const float *r1 = img1 + (size_t)c * plane;
        const float *rm1 = mu1 + (size_t)c * plane;
        const float *r2 = img2 + (size_t)c * plane;
        const float *rm2 = mu2 + (size_t)c * plane;

        size_t i = 0;
        for (; i + 8 <= plane; i += 8) {
            const __m256 a1 = _mm256_loadu_ps(r1 + i);
            const __m256 a2 = _mm256_loadu_ps(r2 + i);
            const __m256 am1 = _mm256_loadu_ps(rm1 + i);
            const __m256 am2 = _mm256_loadu_ps(rm2 + i);
            /* ADR-1208: the difference is taken in double, per-lane — see
             * edge_diff_accum_d above. */
            alignas(32) float a1f[8];
            alignas(32) float am1f[8];
            alignas(32) float a2f[8];
            alignas(32) float am2f[8];
            _mm256_store_ps(a1f, a1);
            _mm256_store_ps(am1f, am1);
            _mm256_store_ps(a2f, a2);
            _mm256_store_ps(am2f, am2);
            for (int k = 0; k < 8; k++)
                edge_diff_accum_d(&acc, a1f[k], am1f[k], a2f[k], am2f[k]);
        }
        /* Scalar tail. */
        for (; i < plane; i++)
            edge_diff_accum_d(&acc, r1[i], rm1[i], r2[i], rm2[i]);
        plane_averages[c * 4 + 0] = one_per_pixels * acc.artifact;
        plane_averages[c * 4 + 1] = sqrt(sqrt(one_per_pixels * acc.artifact_quartic));
        plane_averages[c * 4 + 2] = one_per_pixels * acc.detail;
        plane_averages[c * 4 + 3] = sqrt(sqrt(one_per_pixels * acc.detail_quartic));
    }
}

/*
 * FastGaussian separable IIR — horizontal pass.
 *
 * Scalar `fast_gaussian_1d` is a 3-pole serial recurrence per row;
 * the recurrence can't vectorise within a single row. We batch 8
 * rows in parallel: each AVX2 vector lane holds a column of the 8
 * rows being processed together.
 *
 * Data layout: scalar input is row-major (`in[y * w + x]`). To
 * feed 8 rows into SIMD lanes, we use `vpgatherdd`-style strided
 * loads — lane i reads `in[(y_batch + i) * w + x]` via a stride-w
 * index vector. Output is written lane-by-lane (AVX2 has no
 * scatter) via a spill + 8 scalar stores per iteration. The
 * per-lane IIR state vectors hold prev1/prev2 for 8 independent
 * rows in lock-step.
 *
 * Bit-exact to scalar: each lane computes the same recurrence on
 * the same row of input, in the same left-to-right order. IEEE-754
 * lane-commutative arithmetic yields byte-identical results.
 */
/* Output of one IIR step: o = n2 * sum - d1 * prev1 - prev2, in the scalar
 * reference's order. Shared by the horizontal and the vertical pass. */
static inline __m256 iir_out_avx2(__m256 vn2, __m256 vd1, __m256 sum, __m256 p1, __m256 p2)
{
    const __m256 o = _mm256_sub_ps(_mm256_mul_ps(vn2, sum), _mm256_mul_ps(vd1, p1));
    return _mm256_sub_ps(o, p2);
}

/* Scalar order: (o0 + o1) + o2. */
static inline __m256 iir_sum3_avx2(__m256 o0, __m256 o1, __m256 o2)
{
    return _mm256_add_ps(_mm256_add_ps(o0, o1), o2);
}

/* Gather the left and right taps of column `n` for the 8 rows; a tap outside
 * the row contributes zero. */
static inline __m256 hblur_taps_avx2(const float *in, __m256i vindices, ptrdiff_t n, ptrdiff_t N,
                                     ptrdiff_t W)
{
    const ptrdiff_t left = n - N - 1;
    const ptrdiff_t right = n + N - 1;
    const __m256 lv =
        (left >= 0) ? _mm256_i32gather_ps(in + left, vindices, sizeof(float)) : _mm256_setzero_ps();
    const __m256 rv = (right < W) ? _mm256_i32gather_ps(in + right, vindices, sizeof(float)) :
                                    _mm256_setzero_ps();
    return _mm256_add_ps(lv, rv);
}

/* AVX2 has no scatter: spill the 8 results and store the valid rows. */
static inline void hblur_store_rows_avx2(float *out, unsigned w, unsigned y_base,
                                         unsigned row_count, ptrdiff_t n, __m256 res)
{
    alignas(32) float store_tmp[8];
    _mm256_store_ps(store_tmp, res);
    for (unsigned i = 0; i < row_count; i++) {
        out[((size_t)y_base + i) * w + (size_t)n] = store_tmp[i];
    }
}

static void hblur_8rows_avx2(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                             const float *in, float *out, unsigned w, unsigned y_base,
                             unsigned row_count)
{
    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t W = (ptrdiff_t)w;

    /* Per-lane gather index vector: lane i reads at offset i * w. Lanes
     * beyond `row_count` read row 0 and are never stored. */
    int32_t idx_tmp[8];
    for (int i = 0; i < 8; i++) {
        idx_tmp[i] = (i < (int)row_count) ? (int32_t)((y_base + (unsigned)i) * w) : 0;
    }
    const __m256i vindices = _mm256_loadu_si256((const __m256i *)idx_tmp);

    __m256 vn2[3];
    __m256 vd1[3];
    __m256 prev1[3];
    __m256 prev2[3];
    for (int k = 0; k < 3; k++) {
        vn2[k] = _mm256_set1_ps(rg_n2[k]);
        vd1[k] = _mm256_set1_ps(rg_d1[k]);
        prev1[k] = _mm256_setzero_ps();
        prev2[k] = _mm256_setzero_ps();
    }

    for (ptrdiff_t n = -N + 1; n < W; n++) {
        const __m256 sum = hblur_taps_avx2(in, vindices, n, N, W);
        __m256 o[3];
        for (int k = 0; k < 3; k++) {
            o[k] = iir_out_avx2(vn2[k], vd1[k], sum, prev1[k], prev2[k]);
        }
        for (int k = 0; k < 3; k++) {
            prev2[k] = prev1[k];
            prev1[k] = o[k];
        }
        if (n >= 0) {
            hblur_store_rows_avx2(out, w, y_base, row_count, n, iir_sum3_avx2(o[0], o[1], o[2]));
        }
    }
}

/*
 * FastGaussian separable IIR — vertical pass.
 *
 * The per-column IIR state (`prev1_*`, `prev2_*`) already lives in
 * contiguous 6 × w float arrays (`col_state`), so 8 consecutive
 * columns' state load/store naturally with aligned vectors. No
 * gather required.
 *
 * Bit-exact to scalar: same summation order, lane-commutative.
 */
/* Per-column IIR state: three `prev1` and three `prev2` rows of `w` floats,
 * contiguous in `col_state` as [prev1_0|prev1_1|prev1_2|prev2_0|prev2_1|prev2_2]. */
typedef struct {
    float *prev1[3];
    float *prev2[3];
} vblur_state_t;

static inline void vblur_state_init(vblur_state_t *st, float *col_state, size_t xsize)
{
    for (size_t k = 0; k < 3; k++) {
        st->prev1[k] = col_state + k * xsize;
        st->prev2[k] = col_state + (3u + k) * xsize;
    }
}

/* 8 columns at `x`: the three poles in SIMD, state updated in place. */
static inline void vblur_cols8_avx2(const __m256 vn2[3], const __m256 vd1[3],
                                    const vblur_state_t *st, const float *lrow, const float *rrow,
                                    float *orow, size_t x)
{
    const __m256 lv = lrow ? _mm256_loadu_ps(lrow + x) : _mm256_setzero_ps();
    const __m256 rv = rrow ? _mm256_loadu_ps(rrow + x) : _mm256_setzero_ps();
    const __m256 sum = _mm256_add_ps(lv, rv);
    __m256 p1[3];
    __m256 p2[3];
    __m256 o[3];
    for (int k = 0; k < 3; k++) {
        p1[k] = _mm256_loadu_ps(st->prev1[k] + x);
        p2[k] = _mm256_loadu_ps(st->prev2[k] + x);
    }
    for (int k = 0; k < 3; k++) {
        o[k] = iir_out_avx2(vn2[k], vd1[k], sum, p1[k], p2[k]);
    }
    for (int k = 0; k < 3; k++) {
        _mm256_storeu_ps(st->prev2[k] + x, p1[k]);
        _mm256_storeu_ps(st->prev1[k] + x, o[k]);
    }
    if (orow) {
        _mm256_storeu_ps(orow + x, iir_sum3_avx2(o[0], o[1], o[2]));
    }
}

/* One column of the scalar tail, identical to the scalar reference body. */
static inline void vblur_col_scalar(const float rg_n2[3], const float rg_d1[3],
                                    const vblur_state_t *st, const float *lrow, const float *rrow,
                                    float *orow, size_t x)
{
    const float lv = lrow ? lrow[x] : 0.f;
    const float rv = rrow ? rrow[x] : 0.f;
    const float sum = lv + rv;
    const float o0 = rg_n2[0] * sum - rg_d1[0] * st->prev1[0][x] - st->prev2[0][x];
    const float o1 = rg_n2[1] * sum - rg_d1[1] * st->prev1[1][x] - st->prev2[1][x];
    const float o2 = rg_n2[2] * sum - rg_d1[2] * st->prev1[2][x] - st->prev2[2][x];
    st->prev2[0][x] = st->prev1[0][x];
    st->prev2[1][x] = st->prev1[1][x];
    st->prev2[2][x] = st->prev1[2][x];
    st->prev1[0][x] = o0;
    st->prev1[1][x] = o1;
    st->prev1[2][x] = o2;
    if (orow) {
        orow[x] = o0 + o1 + o2;
    }
}

static void vblur_simd_8cols_avx2(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                  float *col_state, const float *in, float *out, unsigned w,
                                  unsigned h)
{
    const size_t xsize = (size_t)w;
    vblur_state_t st;
    vblur_state_init(&st, col_state, xsize);
    memset(col_state, 0, 6u * xsize * sizeof(float));

    __m256 vn2[3];
    __m256 vd1[3];
    for (int k = 0; k < 3; k++) {
        vn2[k] = _mm256_set1_ps(rg_n2[k]);
        vd1[k] = _mm256_set1_ps(rg_d1[k]);
    }

    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t ysize = (ptrdiff_t)h;

    for (ptrdiff_t n = -N + 1; n < ysize; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float *lrow = (left >= 0) ? (in + (size_t)left * xsize) : NULL;
        const float *rrow = (right < ysize) ? (in + (size_t)right * xsize) : NULL;
        float *orow = (n >= 0) ? (out + (size_t)n * xsize) : NULL;

        size_t x = 0;
        for (; x + 8 <= xsize; x += 8) {
            vblur_cols8_avx2(vn2, vd1, &st, lrow, rrow, orow, x);
        }
        for (; x < xsize; x++) {
            vblur_col_scalar(rg_n2, rg_d1, &st, lrow, rrow, orow, x);
        }
    }
}

void ssimulacra2_blur_plane_avx2(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                 float *col_state, const float *in, float *out, float *scratch,
                                 unsigned w, unsigned h)
{
    assert(col_state != NULL);
    assert(in != NULL);
    assert(out != NULL);
    assert(scratch != NULL);
    assert(w > 0 && h > 0);

    /* Horizontal pass: 8-row batches through `scratch`. */
    unsigned y = 0;
    for (; y + 8 <= h; y += 8) {
        hblur_8rows_avx2(rg_n2, rg_d1, rg_radius, in, scratch, w, y, 8);
    }
    if (y < h) {
        /* Tail: fewer than 8 rows. Still use the 8-wide path with a
         * masked-store row_count — bit-exact because lanes beyond
         * `row_count` don't write to output. */
        hblur_8rows_avx2(rg_n2, rg_d1, rg_radius, in, scratch, w, y, h - y);
    }

    /* Vertical pass: 8-column SIMD with scalar tail. */
    vblur_simd_8cols_avx2(rg_n2, rg_d1, rg_radius, col_state, scratch, out, w, h);
}

/*
 * YUV → linear RGB SIMD port (ADR-0163).
 *
 * Strategy: per-lane scalar `read_plane`-equivalent reads (handles
 * all chroma subsampling ratios + 8/16-bit uniformly), SIMD matmul
 * + normalise + clamp, per-lane scalar `srgb_to_linear` (branch +
 * libm `powf` — bit-exact to scalar reference).
 *
 * The scalar-read-per-lane pattern mirrors the ADR-0161 `cbrtf_lane`
 * approach: SIMD arithmetic is the win; the libm transcendental
 * and the pixel-format dispatch stay scalar to preserve
 * bit-exactness and handle arbitrary chroma ratios.
 */

/* Scalar pixel read with bounds clamping — mirror of scalar read_plane. */
static inline float read_plane_scalar_s2(const simd_plane_t *p, unsigned lw, unsigned lh, int x,
                                         int y, unsigned bpc)
{
    const unsigned pw = p->w;
    const unsigned ph = p->h;
    int sx;
    int sy;
    if (pw == lw) {
        sx = x;
    } else if (pw * 2 == lw) {
        sx = x >> 1;
    } else {
        sx = (int)((int64_t)x * (int64_t)pw / (int64_t)lw);
    }
    if (ph == lh) {
        sy = y;
    } else if (ph * 2 == lh) {
        sy = y >> 1;
    } else {
        sy = (int)((int64_t)y * (int64_t)ph / (int64_t)lh);
    }
    if (sx < 0)
        sx = 0;
    if (sy < 0)
        sy = 0;
    if ((unsigned)sx >= pw)
        sx = (int)pw - 1;
    if ((unsigned)sy >= ph)
        sy = (int)ph - 1;

    if (bpc > 8) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)p->data + (size_t)sy * p->stride);
        return (float)row[sx];
    }
    const uint8_t *row = (const uint8_t *)p->data + (size_t)sy * p->stride;
    return (float)row[sx];
}

/* Per-lane scalar sRGB EOTF. Branch + `powf` per lane; bit-exact to
 * the scalar reference's `srgb_to_linear`. */
static inline __m256 srgb_to_linear_lane_avx2(__m256 v)
{
    alignas(32) float tmp[8];
    _mm256_store_ps(tmp, v);
    for (int k = 0; k < 8; k++) {
        const float x = tmp[k];
        tmp[k] = vmaf_ss2_srgb_eotf(x);
    }
    return _mm256_load_ps(tmp);
}

/* Extract YUV→RGB matrix coefficients for a given enum value. The
 * scalar reference uses the same formula; we materialise the
 * derived coefficients here so SIMD callers avoid the scalar
 * switch inside the hot loop. */
static inline void compute_matrix_coefs(int yuv_matrix, float *kr_out, float *kg_out, float *kb_out,
                                        int *limited_out)
{
    switch (yuv_matrix) {
    case 2: /* BT709_FULL */
        *limited_out = 0;
        *kr_out = 0.2126f;
        *kg_out = 0.7152f;
        *kb_out = 0.0722f;
        break;
    case 0: /* BT709_LIMITED */
        *limited_out = 1;
        *kr_out = 0.2126f;
        *kg_out = 0.7152f;
        *kb_out = 0.0722f;
        break;
    case 3: /* BT601_FULL */
        *limited_out = 0;
        *kr_out = 0.299f;
        *kg_out = 0.587f;
        *kb_out = 0.114f;
        break;
    case 1: /* BT601_LIMITED */
    default:
        *limited_out = 1;
        *kr_out = 0.299f;
        *kg_out = 0.587f;
        *kb_out = 0.114f;
        break;
    }
}

/* Derived YUV -> RGB coefficients, mirroring the scalar reference. */
typedef struct {
    float inv_peak;
    float cr_r;
    float cb_b;
    float cb_g;
    float cr_g;
    float y_scale;
    float c_scale;
    float y_off;
    float c_off;
} ycc_coefs_t;

static inline void ycc_coefs_init(ycc_coefs_t *k, int yuv_matrix, unsigned bpc)
{
    const float peak = (float)((1u << bpc) - 1u);
    float kr;
    float kg;
    float kb;
    int limited;
    compute_matrix_coefs(yuv_matrix, &kr, &kg, &kb, &limited);
    k->inv_peak = 1.0f / peak;
    k->cr_r = 2.0f * (1.0f - kr);
    k->cb_b = 2.0f * (1.0f - kb);
    k->cb_g = -(2.0f * kb * (1.0f - kb)) / kg;
    k->cr_g = -(2.0f * kr * (1.0f - kr)) / kg;
    k->y_scale = limited ? (255.0f / 219.0f) : 1.0f;
    k->c_scale = limited ? (255.0f / 224.0f) : 1.0f;
    k->y_off = limited ? (16.0f / 255.0f) : 0.0f;
    k->c_off = 0.5f;
}

/* Per-lane scalar reads of 8 pixels from the three planes, scaled to [0, 1]. */
static inline void ycc_read8_avx2(const simd_plane_t planes[3], unsigned w, unsigned h,
                                  unsigned bpc, unsigned x, unsigned y, __m256 inv_peak,
                                  __m256 yuv[3])
{
    alignas(32) float tmp[3][8];
    for (int i = 0; i < 8; i++) {
        for (int c = 0; c < 3; c++) {
            tmp[c][i] = read_plane_scalar_s2(&planes[c], w, h, (int)(x + (unsigned)i), (int)y, bpc);
        }
    }
    for (int c = 0; c < 3; c++) {
        yuv[c] = _mm256_mul_ps(_mm256_load_ps(tmp[c]), inv_peak);
    }
}

/* Normalise, FMA matmul, clamp and sRGB EOTF for 8 lanes.
 *
 * ADR-0891 round-2 fix: explicit FMA intrinsics pair with `fmaf()` in the
 * scalar tail and test reference. Under icx + `-mfma`, a separate
 * `_mm256_add_ps(_, _mm256_mul_ps(_, _))` was auto-fused despite
 * `-fp-model=precise` while gcc kept it as mul + add. Forcing FMA on both
 * sides unifies the rounding for every compiler, and the left-to-right
 * association of the G computation is preserved. */
static inline void ycc_to_rgb8_avx2(const ycc_coefs_t *k, const __m256 yuv[3], __m256 rgb[3])
{
    const __m256 vzero = _mm256_setzero_ps();
    const __m256 vone = _mm256_set1_ps(1.0f);
    const __m256 vc_off = _mm256_set1_ps(k->c_off);
    const __m256 vc_scale = _mm256_set1_ps(k->c_scale);
    const __m256 Yn =
        _mm256_mul_ps(_mm256_sub_ps(yuv[0], _mm256_set1_ps(k->y_off)), _mm256_set1_ps(k->y_scale));
    const __m256 Un = _mm256_mul_ps(_mm256_sub_ps(yuv[1], vc_off), vc_scale);
    const __m256 Vn = _mm256_mul_ps(_mm256_sub_ps(yuv[2], vc_off), vc_scale);
    __m256 R = _mm256_fmadd_ps(_mm256_set1_ps(k->cr_r), Vn, Yn);
    __m256 G = _mm256_fmadd_ps(_mm256_set1_ps(k->cb_g), Un, Yn);
    G = _mm256_fmadd_ps(_mm256_set1_ps(k->cr_g), Vn, G);
    __m256 B = _mm256_fmadd_ps(_mm256_set1_ps(k->cb_b), Un, Yn);
    R = _mm256_max_ps(_mm256_min_ps(R, vone), vzero);
    G = _mm256_max_ps(_mm256_min_ps(G, vone), vzero);
    B = _mm256_max_ps(_mm256_min_ps(B, vone), vzero);
    rgb[0] = srgb_to_linear_lane_avx2(R);
    rgb[1] = srgb_to_linear_lane_avx2(G);
    rgb[2] = srgb_to_linear_lane_avx2(B);
}

static inline float clamp01_s2(float v)
{
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    return v;
}

/* Scalar tail pixel, identical to the scalar reference body.
 *
 * ADR-0891: explicit fmaf(); icx + `-mfma` may contract plain `a + b*c` to
 * FMA even under `-fp-model=precise`, diverging from the SIMD reference. The
 * left-to-right association of the G computation is preserved. */
static inline void ycc_pixel_scalar(const ycc_coefs_t *k, const simd_plane_t planes[3], unsigned w,
                                    unsigned h, unsigned bpc, unsigned x, unsigned y,
                                    float *const rgb[3])
{
    const float Ys = read_plane_scalar_s2(&planes[0], w, h, (int)x, (int)y, bpc) * k->inv_peak;
    const float Us = read_plane_scalar_s2(&planes[1], w, h, (int)x, (int)y, bpc) * k->inv_peak;
    const float Vs = read_plane_scalar_s2(&planes[2], w, h, (int)x, (int)y, bpc) * k->inv_peak;
    const float Yn = (Ys - k->y_off) * k->y_scale;
    const float Un = (Us - k->c_off) * k->c_scale;
    const float Vn = (Vs - k->c_off) * k->c_scale;
    float R = fmaf(k->cr_r, Vn, Yn);
    float G = fmaf(k->cb_g, Un, Yn);
    G = fmaf(k->cr_g, Vn, G);
    float B = fmaf(k->cb_b, Un, Yn);
    R = clamp01_s2(R);
    G = clamp01_s2(G);
    B = clamp01_s2(B);
    const size_t idx = (size_t)y * w + x;
    rgb[0][idx] = vmaf_ss2_srgb_eotf(R);
    rgb[1][idx] = vmaf_ss2_srgb_eotf(G);
    rgb[2][idx] = vmaf_ss2_srgb_eotf(B);
}

void ssimulacra2_picture_to_linear_rgb_avx2(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
                                            const simd_plane_t planes[3], float *out)
{
    assert(planes != NULL);
    assert(out != NULL);
    assert(w > 0 && h > 0);

    const size_t plane_sz = (size_t)w * (size_t)h;
    float *const rgb[3] = {out, out + plane_sz, out + 2 * plane_sz};
    ycc_coefs_t k;
    ycc_coefs_init(&k, yuv_matrix, bpc);
    const __m256 vinv_peak = _mm256_set1_ps(k.inv_peak);

    for (unsigned y = 0; y < h; y++) {
        unsigned x = 0;
        for (; x + 8 <= w; x += 8) {
            __m256 yuv[3];
            __m256 lin[3];
            ycc_read8_avx2(planes, w, h, bpc, x, y, vinv_peak, yuv);
            ycc_to_rgb8_avx2(&k, yuv, lin);
            for (int c = 0; c < 3; c++) {
                _mm256_storeu_ps(rgb[c] + (size_t)y * w + x, lin[c]);
            }
        }
        for (; x < w; x++) {
            ycc_pixel_scalar(&k, planes, w, h, bpc, x, y, rgb);
        }
    }
}

/* NOLINTEND(modernize-use-nullptr) */
