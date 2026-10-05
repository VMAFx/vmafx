/**
 *
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 */

/*
 * aarch64 NEON host-kernel variants for the ssimulacra2 Vulkan extractor
 * (ADR-0242). 4-wide float lanes; structurally mirrors the AVX2 sibling
 * (ssimulacra2_host_avx2.c) and the standalone NEON kernels in
 * ssimulacra2_neon.c.
 *
 * The only difference from `ssimulacra2_linear_rgb_to_xyb_neon` is the
 * `plane_stride` parameter: channel pointers are `base + p * plane_stride`
 * instead of `base + p * w*h`.
 *
 * Bit-exact contract: ADR-0161 / ADR-0242 — per-lane scalar cbrtf,
 * `#pragma STDC FP_CONTRACT OFF` (gated by a -Wunknown-pragmas push
 * so older GCC keeps quiet), compiled with `-ffp-contract=off`.
 */

#include <arm_neon.h>
#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "feature/ssimulacra2_math.h"
#include "ssimulacra2_host_neon.h"

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

static inline float32x4_t cbrtf_lane4(float32x4_t v)
{
    alignas(16) float tmp[4];
    vst1q_f32(tmp, v);
    for (int k = 0; k < 4; k++) {
        tmp[k] = vmaf_ss2_cbrtf(tmp[k]);
    }
    return vld1q_f32(tmp);
}

typedef struct {
    float32x4_t m00, m01, m02, m10, m11, m12, m20, m21, m22;
    float32x4_t bias, zero, cbrt_bias, half, c14, c42, c55, c01;
} host_xyb_vecs_neon;

typedef struct {
    const float *rp;
    const float *gp;
    const float *bp;
    float *xp;
    float *yp;
    float *bxp;
} host_xyb_planes_neon;

static host_xyb_vecs_neon host_xyb_vecs_init_neon(float m01, float m11, float m22, float cbrt_bias)
{
    const host_xyb_vecs_neon c = {
        .m00 = vdupq_n_f32(kM00),
        .m01 = vdupq_n_f32(m01),
        .m02 = vdupq_n_f32(kM02),
        .m10 = vdupq_n_f32(kM10),
        .m11 = vdupq_n_f32(m11),
        .m12 = vdupq_n_f32(kM12),
        .m20 = vdupq_n_f32(kM20),
        .m21 = vdupq_n_f32(kM21),
        .m22 = vdupq_n_f32(m22),
        .bias = vdupq_n_f32(kOpsinBias),
        .zero = vdupq_n_f32(0.0f),
        .cbrt_bias = vdupq_n_f32(cbrt_bias),
        .half = vdupq_n_f32(0.5f),
        .c14 = vdupq_n_f32(14.0f),
        .c42 = vdupq_n_f32(0.42f),
        .c55 = vdupq_n_f32(0.55f),
        .c01 = vdupq_n_f32(0.01f),
    };
    return c;
}

/* One LMS channel: (k0*r + k1*g) + k2*b + bias, clamped at zero. */
static inline float32x4_t host_lms_mix_neon(float32x4_t k0, float32x4_t k1, float32x4_t k2,
                                            float32x4_t r, float32x4_t g, float32x4_t b,
                                            const host_xyb_vecs_neon *c)
{
    float32x4_t v = vaddq_f32(vmulq_f32(k0, r), vmulq_f32(k1, g));
    v = vaddq_f32(v, vmulq_f32(k2, b));
    v = vaddq_f32(v, c->bias);
    return vmaxq_f32(v, c->zero);
}

static void host_xyb_block_neon(const host_xyb_vecs_neon *c, const host_xyb_planes_neon *p,
                                size_t i)
{
    const float32x4_t r = vld1q_f32(p->rp + i);
    const float32x4_t g = vld1q_f32(p->gp + i);
    const float32x4_t b = vld1q_f32(p->bp + i);
    const float32x4_t l = host_lms_mix_neon(c->m00, c->m01, c->m02, r, g, b, c);
    const float32x4_t m = host_lms_mix_neon(c->m10, c->m11, c->m12, r, g, b, c);
    const float32x4_t sv = host_lms_mix_neon(c->m20, c->m21, c->m22, r, g, b, c);
    const float32x4_t L = vsubq_f32(cbrtf_lane4(l), c->cbrt_bias);
    const float32x4_t M = vsubq_f32(cbrtf_lane4(m), c->cbrt_bias);
    const float32x4_t S = vsubq_f32(cbrtf_lane4(sv), c->cbrt_bias);
    const float32x4_t X = vmulq_f32(c->half, vsubq_f32(L, M));
    const float32x4_t Y = vmulq_f32(c->half, vaddq_f32(L, M));
    const float32x4_t B = S;
    const float32x4_t Bfinal = vaddq_f32(vsubq_f32(B, Y), c->c55);
    const float32x4_t Xfinal = vaddq_f32(vmulq_f32(X, c->c14), c->c42);
    const float32x4_t Yfinal = vaddq_f32(Y, c->c01);
    vst1q_f32(p->xp + i, Xfinal);
    vst1q_f32(p->yp + i, Yfinal);
    vst1q_f32(p->bxp + i, Bfinal);
}

static void host_xyb_tail_neon(const host_xyb_planes_neon *p, size_t i, size_t n, float m01,
                               float m11, float m22, float cbrt_bias)
{
    for (; i < n; i++) {
        float r = p->rp[i];
        float g = p->gp[i];
        float bb = p->bp[i];
        float l = kM00 * r + m01 * g + kM02 * bb + kOpsinBias;
        float m = kM10 * r + m11 * g + kM12 * bb + kOpsinBias;
        float s = kM20 * r + kM21 * g + m22 * bb + kOpsinBias;
        if (l < 0.0f)
            l = 0.0f;
        if (m < 0.0f)
            m = 0.0f;
        if (s < 0.0f)
            s = 0.0f;
        float L = vmaf_ss2_cbrtf(l) - cbrt_bias;
        float M = vmaf_ss2_cbrtf(m) - cbrt_bias;
        float S = vmaf_ss2_cbrtf(s) - cbrt_bias;
        float X = 0.5f * (L - M);
        float Y = 0.5f * (L + M);
        float B = S;
        B = (B - Y) + 0.55f;
        X = X * 14.0f + 0.42f;
        Y = Y + 0.01f;
        p->xp[i] = X;
        p->yp[i] = Y;
        p->bxp[i] = B;
    }
}

void ssimulacra2_host_linear_rgb_to_xyb_neon(const float *lin, float *xyb, unsigned w, unsigned h,
                                             size_t plane_stride)
{
    assert(lin != NULL);
    assert(xyb != NULL);
    assert(w > 0 && h > 0);
    assert(plane_stride >= (size_t)w * (size_t)h);

    const host_xyb_planes_neon planes = {lin, lin + plane_stride, lin + 2u * plane_stride,
                                         xyb, xyb + plane_stride, xyb + 2u * plane_stride};

    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);
    const host_xyb_vecs_neon vecs = host_xyb_vecs_init_neon(m01, m11, m22, cbrt_bias);

    const size_t scale_pixels = (size_t)w * (size_t)h;
    size_t i = 0;
    for (; i + 4 <= scale_pixels; i += 4) {
        host_xyb_block_neon(&vecs, &planes, i);
    }
    host_xyb_tail_neon(&planes, i, scale_pixels, m01, m11, m22, cbrt_bias);
}

void ssimulacra2_host_downsample_2x2_neon(const float *in, unsigned iw, unsigned ih, float *out,
                                          unsigned ow, unsigned oh, size_t plane_stride)
{
    assert(in != NULL);
    assert(out != NULL);
    assert(iw > 0 && ih > 0);
    assert(plane_stride >= (size_t)iw * (size_t)ih);

    const float32x4_t vquarter = vdupq_n_f32(0.25f);

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
            /* SIMD interior: 4 output lanes at a time. `vuzp1q_f32` extracts
             * even positions, `vuzp2q_f32` extracts odd — equivalent to the
             * AVX2 shuffle+permute. Sequential adds preserve summation order. */
            const unsigned interior_end = (ow > 0u && iw >= 2u) ? (((ow - 1u) / 4u) * 4u) : 0u;
            for (; ox < interior_end; ox += 4) {
                const size_t base = (size_t)ox * 2u;
                const float32x4_t r0a = vld1q_f32(row0 + base);
                const float32x4_t r0b = vld1q_f32(row0 + base + 4);
                const float32x4_t r1a = vld1q_f32(row1 + base);
                const float32x4_t r1b = vld1q_f32(row1 + base + 4);
                /* Deinterleave even / odd sample pairs across r0a:r0b. */
                const float32x4_t r0e = vuzp1q_f32(r0a, r0b);
                const float32x4_t r0o = vuzp2q_f32(r0a, r0b);
                const float32x4_t r1e = vuzp1q_f32(r1a, r1b);
                const float32x4_t r1o = vuzp2q_f32(r1a, r1b);
                /* (r0e + r0o) + r1e + r1o — scalar summation order. */
                float32x4_t acc = vaddq_f32(r0e, r0o);
                acc = vaddq_f32(acc, r1e);
                acc = vaddq_f32(acc, r1o);
                vst1q_f32(orow + ox, vmulq_f32(acc, vquarter));
            }
            /* Scalar tail. */
            for (; ox < ow; ox++) {
                unsigned ix0 = ox * 2;
                unsigned ix1 = (ix0 + 1 < iw) ? ix0 + 1 : iw - 1;
                float sum = row0[ix0] + row0[ix1] + row1[ix0] + row1[ix1];
                orow[ox] = sum * 0.25f;
            }
        }
    }
}
