/**
 *
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 */

/*
 * AVX2 host-kernel variants for the ssimulacra2 Vulkan extractor (ADR-0242).
 *
 * These are structurally identical to `ssimulacra2_linear_rgb_to_xyb_avx2`
 * and `ssimulacra2_downsample_2x2_avx2` in ssimulacra2_avx2.c, with one
 * difference: channel pointers are computed as `base + plane_stride` rather
 * than `base + w*h`. This allows the Vulkan pyramid to keep a fixed per-plane
 * slot size (= full-resolution frame pixels) across all downsampled scales,
 * matching the GPU shader's `c * full_plane` channel-offset convention.
 *
 * Bit-exact contract: ADR-0161 / ADR-0242 — lane-commutative pointwise
 * arithmetic, `cbrtf` applied per-lane via scalar libm, addition order
 * preserved left-to-right, `#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#pragma STDC FP_CONTRACT OFF
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif` + build flag
 * `-ffp-contract=off`.
 */

#include <assert.h>
#include <immintrin.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "feature/ssimulacra2_math.h"
#include "ssimulacra2_host_avx2.h"

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

static const float kM00 = 0.30f;
static const float kM02 = 0.078f;
static const float kM10 = 0.23f;
static const float kM12 = 0.078f;
static const float kM20 = 0.24342268924547819f;
static const float kM21 = 0.20476744424496821f;
static const float kOpsinBias = 0.0037930732552754493f;

/* Per-lane scalar cbrtf — preserves bit-exactness with the scalar reference
 * (ADR-0161 pattern). */
static inline __m256 cbrtf_lane8(const __m256 v)
{
    alignas(32) float tmp[8];
    _mm256_store_ps(tmp, v);
    for (int k = 0; k < 8; k++) {
        tmp[k] = vmaf_ss2_cbrtf(tmp[k]);
    }
    return _mm256_load_ps(tmp);
}

/* The helpers below hold the exact statements the single kernel used to
 * carry, in the same order; every operation is a separate IEEE-754 float
 * operation (this TU is built with contraction off), so splitting the kernel
 * does not change a bit. */
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
    /* `cbrtf` is applied per lane through scalar libm; see `cbrtf_lane8`. */
    const __m256 L = _mm256_sub_ps(cbrtf_lane8(l), vcbrt_bias);
    const __m256 M = _mm256_sub_ps(cbrtf_lane8(m), vcbrt_bias);
    const __m256 S = _mm256_sub_ps(cbrtf_lane8(sv), vcbrt_bias);
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

void ssimulacra2_host_linear_rgb_to_xyb_avx2(const float *lin, float *xyb, unsigned w, unsigned h,
                                             size_t plane_stride)
{
    assert(lin != NULL);
    assert(xyb != NULL);
    assert(w > 0 && h > 0);
    assert(plane_stride >= (size_t)w * (size_t)h);

    const float *const rgb[3] = {lin, lin + plane_stride, lin + 2u * plane_stride};
    float *const out[3] = {xyb, xyb + plane_stride, xyb + 2u * plane_stride};
    const xyb_coefs_avx2_t k = {
        .m01 = 1.0f - kM00 - kM02,
        .m11 = 1.0f - kM10 - kM12,
        .m22 = 1.0f - kM20 - kM21,
        .cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias),
    };

    const size_t scale_pixels = (size_t)w * (size_t)h;
    size_t i = 0;
    for (; i + 8 <= scale_pixels; i += 8) {
        xyb_block_avx2(&k, rgb, out, i);
    }
    for (; i < scale_pixels; i++) {
        xyb_pixel_scalar(&k, rgb, out, i);
    }
}

/* Deinterleave the even / odd pairs of two 8-float vectors into one vector
 * each, in pixel order. `_mm256_shuffle_ps` works inside 128-bit lanes, so
 * `_mm256_permute4x64_pd` restores lanes [0,1,2,3] = output pixels
 * [ox..ox+7], the same pattern as ssimulacra2_downsample_2x2_avx2. */
static inline void deinterleave8_avx2(__m256 a, __m256 b, __m256 *even, __m256 *odd)
{
    const __m256 e_raw = _mm256_shuffle_ps(a, b, 0x88);
    const __m256 o_raw = _mm256_shuffle_ps(a, b, 0xDD);
    *even = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(e_raw), 0xD8));
    *odd = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(o_raw), 0xD8));
}

/* 8 output pixels at `ox`. Sequential summation preserves the scalar
 * left-to-right order `((r0e + r0o) + r1e) + r1o`. */
static inline void downsample_block8_avx2(const float *row0, const float *row1, float *orow,
                                          unsigned ox)
{
    const size_t base = (size_t)ox * 2u;
    __m256 r0e;
    __m256 r0o;
    __m256 r1e;
    __m256 r1o;
    deinterleave8_avx2(_mm256_loadu_ps(row0 + base), _mm256_loadu_ps(row0 + base + 8), &r0e, &r0o);
    deinterleave8_avx2(_mm256_loadu_ps(row1 + base), _mm256_loadu_ps(row1 + base + 8), &r1e, &r1o);
    __m256 acc = _mm256_add_ps(r0e, r0o);
    acc = _mm256_add_ps(acc, r1e);
    acc = _mm256_add_ps(acc, r1o);
    _mm256_storeu_ps(orow + ox, _mm256_mul_ps(acc, _mm256_set1_ps(0.25f)));
}

/* Scalar tail pixel, bit-identical to ss2v_downsample_2x2. */
static inline void downsample_pixel_scalar(const float *row0, const float *row1, float *orow,
                                           unsigned ox, unsigned iw)
{
    const unsigned ix0 = ox * 2;
    const unsigned ix1 = (ix0 + 1 < iw) ? ix0 + 1 : iw - 1;
    const float sum = row0[ix0] + row0[ix1] + row1[ix0] + row1[ix1];
    orow[ox] = sum * 0.25f;
}

void ssimulacra2_host_downsample_2x2_avx2(const float *in, unsigned iw, unsigned ih, float *out,
                                          unsigned ow, unsigned oh, size_t plane_stride)
{
    assert(in != NULL);
    assert(out != NULL);
    assert(iw > 0 && ih > 0);
    assert(plane_stride >= (size_t)iw * (size_t)ih);

    for (int c = 0; c < 3; c++) {
        const float *ip = in + (size_t)c * plane_stride;
        float *op = out + (size_t)c * plane_stride;
        for (unsigned oy = 0; oy < oh; oy++) {
            const unsigned iy0 = oy * 2;
            const unsigned iy1 = (iy0 + 1 < ih) ? iy0 + 1 : ih - 1;
            const float *row0 = ip + (size_t)iy0 * iw;
            const float *row1 = ip + (size_t)iy1 * iw;
            float *orow = op + (size_t)oy * ow;
            unsigned ox = 0;
            /* SIMD interior: 8 output lanes at a time. */
            const unsigned interior_end = (ow > 0u && iw >= 2u) ? (((ow - 1u) / 8u) * 8u) : 0u;
            for (; ox < interior_end; ox += 8) {
                downsample_block8_avx2(row0, row1, orow, ox);
            }
            for (; ox < ow; ox++) {
                downsample_pixel_scalar(row0, row1, orow, ox, iw);
            }
        }
    }
}
