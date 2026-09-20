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

#include "vmaf_nullptr.h"

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
#pragma STDC FP_CONTRACT OFF
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

typedef struct Ssimulacra2XybAvx2 {
    const float *r;
    const float *g;
    const float *b;
    float *x;
    float *y;
    float *b_out;
    float m01;
    float m11;
    float m22;
    float cbrt_bias;
    __m256 m[9];
    __m256 bias;
    __m256 cbrt_bias_v;
} Ssimulacra2XybAvx2;

static Ssimulacra2XybAvx2 ssimulacra2_xyb_context_avx2(const float *lin, float *xyb,
                                                       size_t plane_stride)
{
    Ssimulacra2XybAvx2 ctx;
    ctx.r = lin;
    ctx.g = lin + plane_stride;
    ctx.b = lin + 2u * plane_stride;
    ctx.x = xyb;
    ctx.y = xyb + plane_stride;
    ctx.b_out = xyb + 2u * plane_stride;
    ctx.m01 = 1.0f - kM00 - kM02;
    ctx.m11 = 1.0f - kM10 - kM12;
    ctx.m22 = 1.0f - kM20 - kM21;
    ctx.cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);
    const float matrix[9] = {kM00, ctx.m01, kM02, kM10, ctx.m11, kM12, kM20, kM21, ctx.m22};
    for (int coefficient = 0; coefficient < 9; ++coefficient)
        ctx.m[coefficient] = _mm256_set1_ps(matrix[coefficient]);
    ctx.bias = _mm256_set1_ps(kOpsinBias);
    ctx.cbrt_bias_v = _mm256_set1_ps(ctx.cbrt_bias);
    return ctx;
}

static inline void ssimulacra2_xyb_block_avx2(const Ssimulacra2XybAvx2 *ctx, size_t i)
{
    const __m256 r = _mm256_loadu_ps(ctx->r + i);
    const __m256 g = _mm256_loadu_ps(ctx->g + i);
    const __m256 b = _mm256_loadu_ps(ctx->b + i);
    __m256 l = _mm256_add_ps(_mm256_mul_ps(ctx->m[0], r), _mm256_mul_ps(ctx->m[1], g));
    l = _mm256_add_ps(l, _mm256_mul_ps(ctx->m[2], b));
    l = _mm256_add_ps(l, ctx->bias);
    __m256 m = _mm256_add_ps(_mm256_mul_ps(ctx->m[3], r), _mm256_mul_ps(ctx->m[4], g));
    m = _mm256_add_ps(m, _mm256_mul_ps(ctx->m[5], b));
    m = _mm256_add_ps(m, ctx->bias);
    __m256 s = _mm256_add_ps(_mm256_mul_ps(ctx->m[6], r), _mm256_mul_ps(ctx->m[7], g));
    s = _mm256_add_ps(s, _mm256_mul_ps(ctx->m[8], b));
    s = _mm256_add_ps(s, ctx->bias);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 big_l = _mm256_sub_ps(cbrtf_lane8(_mm256_max_ps(l, zero)), ctx->cbrt_bias_v);
    const __m256 big_m = _mm256_sub_ps(cbrtf_lane8(_mm256_max_ps(m, zero)), ctx->cbrt_bias_v);
    const __m256 big_s = _mm256_sub_ps(cbrtf_lane8(_mm256_max_ps(s, zero)), ctx->cbrt_bias_v);
    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256 x = _mm256_mul_ps(half, _mm256_sub_ps(big_l, big_m));
    const __m256 y = _mm256_mul_ps(half, _mm256_add_ps(big_l, big_m));
    const __m256 b_out = _mm256_add_ps(_mm256_sub_ps(big_s, y), _mm256_set1_ps(0.55f));
    const __m256 x_out =
        _mm256_add_ps(_mm256_mul_ps(x, _mm256_set1_ps(14.0f)), _mm256_set1_ps(0.42f));
    const __m256 y_out = _mm256_add_ps(y, _mm256_set1_ps(0.01f));
    _mm256_storeu_ps(ctx->x + i, x_out);
    _mm256_storeu_ps(ctx->y + i, y_out);
    _mm256_storeu_ps(ctx->b_out + i, b_out);
}

static inline void ssimulacra2_xyb_scalar_avx2(const Ssimulacra2XybAvx2 *ctx, size_t i)
{
    const float r = ctx->r[i];
    const float g = ctx->g[i];
    const float b = ctx->b[i];
    float l = kM00 * r + ctx->m01 * g + kM02 * b + kOpsinBias;
    float m = kM10 * r + ctx->m11 * g + kM12 * b + kOpsinBias;
    float s = kM20 * r + kM21 * g + ctx->m22 * b + kOpsinBias;
    if (l < 0.0f)
        l = 0.0f;
    if (m < 0.0f)
        m = 0.0f;
    if (s < 0.0f)
        s = 0.0f;
    const float big_l = vmaf_ss2_cbrtf(l) - ctx->cbrt_bias;
    const float big_m = vmaf_ss2_cbrtf(m) - ctx->cbrt_bias;
    const float big_s = vmaf_ss2_cbrtf(s) - ctx->cbrt_bias;
    float x = 0.5f * (big_l - big_m);
    float y = 0.5f * (big_l + big_m);
    float b_out = big_s;
    b_out = (b_out - y) + 0.55f;
    x = x * 14.0f + 0.42f;
    y = y + 0.01f;
    ctx->x[i] = x;
    ctx->y[i] = y;
    ctx->b_out[i] = b_out;
}

void ssimulacra2_host_linear_rgb_to_xyb_avx2(const float *lin, float *xyb, unsigned w, unsigned h,
                                             size_t plane_stride)
{
    assert(lin != VMAF_NULLPTR);
    assert(xyb != VMAF_NULLPTR);
    assert(w > 0 && h > 0);
    assert(plane_stride >= (size_t)w * (size_t)h);

    const Ssimulacra2XybAvx2 ctx = ssimulacra2_xyb_context_avx2(lin, xyb, plane_stride);
    const size_t scale_pixels = (size_t)w * (size_t)h;
    size_t i = 0;
    for (; i + 8 <= scale_pixels; i += 8)
        ssimulacra2_xyb_block_avx2(&ctx, i);
    for (; i < scale_pixels; ++i)
        ssimulacra2_xyb_scalar_avx2(&ctx, i);
}

static inline void ssimulacra2_downsample_block_avx2(const float *row0, const float *row1,
                                                     float *out, unsigned column)
{
    const size_t base = (size_t)column * 2u;
    const __m256 r00 = _mm256_loadu_ps(row0 + base);
    const __m256 r01 = _mm256_loadu_ps(row0 + base + 8);
    const __m256 r10 = _mm256_loadu_ps(row1 + base);
    const __m256 r11 = _mm256_loadu_ps(row1 + base + 8);
    const __m256 r0e_raw = _mm256_shuffle_ps(r00, r01, 0x88);
    const __m256 r0o_raw = _mm256_shuffle_ps(r00, r01, 0xDD);
    const __m256 r1e_raw = _mm256_shuffle_ps(r10, r11, 0x88);
    const __m256 r1o_raw = _mm256_shuffle_ps(r10, r11, 0xDD);
    const __m256 r0e = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r0e_raw), 0xD8));
    const __m256 r0o = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r0o_raw), 0xD8));
    const __m256 r1e = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r1e_raw), 0xD8));
    const __m256 r1o = _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r1o_raw), 0xD8));
    __m256 accum = _mm256_add_ps(r0e, r0o);
    accum = _mm256_add_ps(accum, r1e);
    accum = _mm256_add_ps(accum, r1o);
    _mm256_storeu_ps(out + column, _mm256_mul_ps(accum, _mm256_set1_ps(0.25f)));
}

static void ssimulacra2_downsample_row_avx2(const float *row0, const float *row1, float *out,
                                            unsigned iw, unsigned ow)
{
    const unsigned interior_end = (ow > 0u && iw >= 2u) ? (((ow - 1u) / 8u) * 8u) : 0u;
    unsigned column = 0;
    for (; column < interior_end; column += 8)
        ssimulacra2_downsample_block_avx2(row0, row1, out, column);
    for (; column < ow; ++column) {
        const unsigned ix0 = column * 2;
        const unsigned ix1 = (ix0 + 1 < iw) ? ix0 + 1 : iw - 1;
        const float sum = row0[ix0] + row0[ix1] + row1[ix0] + row1[ix1];
        out[column] = sum * 0.25f;
    }
}

void ssimulacra2_host_downsample_2x2_avx2(const float *in, unsigned iw, unsigned ih, float *out,
                                          unsigned ow, unsigned oh, size_t plane_stride)
{
    assert(in != VMAF_NULLPTR);
    assert(out != VMAF_NULLPTR);
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
            ssimulacra2_downsample_row_avx2(row0, row1, op + (size_t)oy * ow, iw, ow);
        }
    }
}
