/**
 *
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 */

/*
 * aarch64 NEON port of the SSIMULACRA 2 SIMD kernels. Structural
 * mirror of the AVX2 / AVX-512 TUs (ADR-0161) — 4-wide float lanes.
 *
 * `cbrtf` applied per-lane via scalar libm. The 2x2 downsample's
 * deinterleave uses `vuzp1q_f32` / `vuzp2q_f32` to pull even / odd
 * positions into separate vectors; sequential adds preserve the
 * scalar left-to-right summation order.
 */

#include <arm_neon.h>
#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "feature/ssimulacra2_math.h"
#include "feature/ssimulacra2_score.h"
#include "ssimulacra2_neon.h"

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

#include "ssimulacra2_arm64_common.h"

static const float kC2 = 0.0009f;

void ssimulacra2_multiply_3plane_neon(const float *a, const float *b, float *mul, unsigned w,
                                      unsigned h)
{
    const size_t n = 3u * (size_t)w * (size_t)h;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const float32x4_t va = vld1q_f32(a + i);
        const float32x4_t vb = vld1q_f32(b + i);
        vst1q_f32(mul + i, vmulq_f32(va, vb));
    }
    for (; i < n; i++) {
        mul[i] = a[i] * b[i];
    }
}

void ssimulacra2_linear_rgb_to_xyb_neon(const float *lin, float *xyb, unsigned w, unsigned h)
{
    assert(lin != NULL);
    assert(xyb != NULL);
    assert(w > 0 && h > 0);
    const size_t plane_sz = (size_t)w * (size_t)h;
    const float *rp = lin;
    const float *gp = lin + plane_sz;
    const float *bp = lin + 2u * plane_sz;
    float *xp = xyb;
    float *yp = xyb + plane_sz;
    float *bxp = xyb + 2u * plane_sz;
    const Ss2XybK k = ss2_xyb_k_init();
    const size_t pixels = (size_t)w * (size_t)h;

    size_t i = 0;
    for (; i + 4 <= pixels; i += 4) {
        ss2_xyb_block_neon(&k, rp + i, gp + i, bp + i, xp + i, yp + i, bxp + i);
    }
    for (; i < pixels; i++) {
        ss2_xyb_pixel(&k, rp[i], gp[i], bp[i], xp + i, yp + i, bxp + i);
    }
}

void ssimulacra2_downsample_2x2_neon(const float *in, unsigned iw, unsigned ih, float *out,
                                     unsigned *ow_out, unsigned *oh_out)
{
    const unsigned ow = (iw + 1) / 2;
    const unsigned oh = (ih + 1) / 2;
    *ow_out = ow;
    *oh_out = oh;

    const size_t in_plane = (size_t)iw * (size_t)ih;
    const size_t out_plane = (size_t)ow * (size_t)oh;

    for (int c = 0; c < 3; c++) {
        const float *ip = in + (size_t)c * in_plane;
        float *op = out + (size_t)c * out_plane;
        for (unsigned oy = 0; oy < oh; oy++) {
            const unsigned iy0 = oy * 2;
            const unsigned iy1 = (iy0 + 1 < ih) ? iy0 + 1 : ih - 1;
            ss2_downsample_row_2x2_neon(ip + (size_t)iy0 * iw, ip + (size_t)iy1 * iw,
                                        op + (size_t)oy * ow, iw, ow);
        }
    }
}

void ssimulacra2_ssim_map_neon(const float *m1, const float *m2, const float *s11, const float *s22,
                               const float *s12, unsigned w, unsigned h, double plane_averages[6])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    const float32x4_t vc2 = vdupq_n_f32(kC2);
    const float32x4_t vone = vdupq_n_f32(1.0f);
    const float32x4_t vtwo = vdupq_n_f32(2.0f);

    for (int c = 0; c < 3; c++) {
        Ss2SsimSums sums = {0.0, 0.0};
        const float *rm1 = m1 + (size_t)c * plane;
        const float *rm2 = m2 + (size_t)c * plane;
        const float *rs11 = s11 + (size_t)c * plane;
        const float *rs22 = s22 + (size_t)c * plane;
        const float *rs12 = s12 + (size_t)c * plane;

        size_t i = 0;
        for (; i + 4 <= plane; i += 4) {
            const float32x4_t mu1 = vld1q_f32(rm1 + i);
            const float32x4_t mu2 = vld1q_f32(rm2 + i);
            const float32x4_t mu11 = vmulq_f32(mu1, mu1);
            const float32x4_t mu22 = vmulq_f32(mu2, mu2);
            const float32x4_t mu12 = vmulq_f32(mu1, mu2);
            const float32x4_t diff = vsubq_f32(mu1, mu2);
            const float32x4_t num_m = vsubq_f32(vone, vmulq_f32(diff, diff));
            const float32x4_t num_s =
                vaddq_f32(vmulq_f32(vtwo, vsubq_f32(vld1q_f32(rs12 + i), mu12)), vc2);
            const float32x4_t denom_s = vaddq_f32(vaddq_f32(vsubq_f32(vld1q_f32(rs11 + i), mu11),
                                                            vsubq_f32(vld1q_f32(rs22 + i), mu22)),
                                                  vc2);
            alignas(16) float num_m_f[4];
            alignas(16) float num_s_f[4];
            alignas(16) float denom_s_f[4];
            vst1q_f32(num_m_f, num_m);
            vst1q_f32(num_s_f, num_s);
            vst1q_f32(denom_s_f, denom_s);
            for (int k = 0; k < 4; k++) {
                ss2_ssim_accumulate(&sums, num_m_f[k], num_s_f[k], denom_s_f[k]);
            }
        }
        for (; i < plane; i++) {
            float mu1 = rm1[i];
            float mu2 = rm2[i];
            float mu11 = mu1 * mu1;
            float mu22 = mu2 * mu2;
            float mu12 = mu1 * mu2;
            float num_m = 1.0f - (mu1 - mu2) * (mu1 - mu2);
            float num_s = 2.0f * (rs12[i] - mu12) + kC2;
            float denom_s = (rs11[i] - mu11) + (rs22[i] - mu22) + kC2;
            ss2_ssim_accumulate(&sums, num_m, num_s, denom_s);
        }
        plane_averages[c * 2 + 0] = one_per_pixels * sums.l1;
        plane_averages[c * 2 + 1] = sqrt(sqrt(one_per_pixels * sums.l4));
    }
}

void ssimulacra2_edge_diff_map_neon(const float *img1, const float *mu1, const float *img2,
                                    const float *mu2, unsigned w, unsigned h,
                                    double plane_averages[12])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;

    for (int c = 0; c < 3; c++) {
        Ss2EdgeSums sums = {0.0, 0.0, 0.0, 0.0};
        const float *r1 = img1 + (size_t)c * plane;
        const float *rm1 = mu1 + (size_t)c * plane;
        const float *r2 = img2 + (size_t)c * plane;
        const float *rm2 = mu2 + (size_t)c * plane;

        size_t i = 0;
        for (; i + 4 <= plane; i += 4) {
            const float32x4_t a1 = vld1q_f32(r1 + i);
            const float32x4_t a2 = vld1q_f32(r2 + i);
            const float32x4_t am1 = vld1q_f32(rm1 + i);
            const float32x4_t am2 = vld1q_f32(rm2 + i);
            alignas(16) float a1f[4];
            alignas(16) float am1f[4];
            alignas(16) float a2f[4];
            alignas(16) float am2f[4];
            vst1q_f32(a1f, a1);
            vst1q_f32(am1f, am1);
            vst1q_f32(a2f, a2);
            vst1q_f32(am2f, am2);
            for (int k = 0; k < 4; k++) {
                ss2_edge_accumulate(&sums, a1f[k], am1f[k], a2f[k], am2f[k]);
            }
        }
        for (; i < plane; i++) {
            ss2_edge_accumulate(&sums, r1[i], rm1[i], r2[i], rm2[i]);
        }
        ss2_edge_store(plane_averages, c, one_per_pixels, &sums);
    }
}

typedef struct {
    float32x4_t n2[3];
    float32x4_t d1[3];
} Ss2IirK;

static inline Ss2IirK ss2_iir_k_init(const float rg_n2[3], const float rg_d1[3])
{
    Ss2IirK k;
    for (int p = 0; p < 3; p++) {
        k.n2[p] = vdupq_n_f32(rg_n2[p]);
        k.d1[p] = vdupq_n_f32(rg_d1[p]);
    }
    return k;
}

/* One pole of the recursive Gaussian: n2 * sum - d1 * prev1 - prev2, in the
 * scalar's order. */
static inline float32x4_t ss2_iir_pole_neon(const Ss2IirK *k, int p, float32x4_t sum,
                                            float32x4_t prev1, float32x4_t prev2)
{
    const float32x4_t o = vsubq_f32(vmulq_f32(k->n2[p], sum), vmulq_f32(k->d1[p], prev1));
    return vsubq_f32(o, prev2);
}

/* Lane i = row_bases[i][idx] for i < row_count, zero elsewhere. NEON has no
 * gather so the load is assembled lane by lane. */
static inline float32x4_t ss2_hblur_gather_neon(const float *const row_bases[4], unsigned row_count,
                                                ptrdiff_t idx)
{
    float32x4_t v = vdupq_n_f32(0.f);
    if (row_count > 0) {
        v = vsetq_lane_f32(row_bases[0][idx], v, 0);
    }
    if (row_count > 1) {
        v = vsetq_lane_f32(row_bases[1][idx], v, 1);
    }
    if (row_count > 2) {
        v = vsetq_lane_f32(row_bases[2][idx], v, 2);
    }
    if (row_count > 3) {
        v = vsetq_lane_f32(row_bases[3][idx], v, 3);
    }
    return v;
}

/* Three poles of the IIR over one input sum: advances the two state
 * vectors and returns (o0 + o1) + o2. */
static inline float32x4_t ss2_iir_step3_neon(const Ss2IirK *k, float32x4_t sum,
                                             float32x4_t prev1[3], float32x4_t prev2[3])
{
    float32x4_t o[3];
    for (int p = 0; p < 3; p++) {
        o[p] = ss2_iir_pole_neon(k, p, sum, prev1[p], prev2[p]);
        prev2[p] = prev1[p];
        prev1[p] = o[p];
    }
    return vaddq_f32(vaddq_f32(o[0], o[1]), o[2]);
}

static void hblur_4rows_neon(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                             const float *in, float *out, unsigned w, unsigned y_base,
                             unsigned row_count)
{
    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t W = (ptrdiff_t)w;
    const Ss2IirK k = ss2_iir_k_init(rg_n2, rg_d1);
    float32x4_t prev1[3] = {vdupq_n_f32(0.f), vdupq_n_f32(0.f), vdupq_n_f32(0.f)};
    float32x4_t prev2[3] = {vdupq_n_f32(0.f), vdupq_n_f32(0.f), vdupq_n_f32(0.f)};
    alignas(16) float store_tmp[4];

    /* Per-lane row base pointer addresses. */
    /* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
     * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
     * documented /std:clatest C23 feature set does not include `nullptr` and the
     * required Windows builds compile C with cl.exe (C2065). ADR-1138. */
    const float *row_bases[4] = {NULL, NULL, NULL, NULL};
    /* NOLINTEND(modernize-use-nullptr) */
    for (unsigned i = 0; i < row_count && i < 4; i++) {
        row_bases[i] = in + ((size_t)y_base + i) * w;
    }

    for (ptrdiff_t n = -N + 1; n < W; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float32x4_t lv =
            (left >= 0) ? ss2_hblur_gather_neon(row_bases, row_count, left) : vdupq_n_f32(0.f);
        const float32x4_t rv =
            (right < W) ? ss2_hblur_gather_neon(row_bases, row_count, right) : vdupq_n_f32(0.f);
        const float32x4_t res = ss2_iir_step3_neon(&k, vaddq_f32(lv, rv), prev1, prev2);

        if (n >= 0) {
            vst1q_f32(store_tmp, res);
            for (unsigned i = 0; i < row_count; i++) {
                out[((size_t)y_base + i) * w + (size_t)n] = store_tmp[i];
            }
        }
    }
}

/* The six per-column IIR state rows of the vertical blur. */
typedef struct {
    float *prev1[3];
    float *prev2[3];
} Ss2ColState;

/* Four columns at x: the rows above and below (NULL outside the image) are
 * summed, the state rows advance and, with an output row, (o0 + o1) + o2 is
 * stored. */
static inline void ss2_vblur_block_neon(const Ss2IirK *k, const Ss2ColState *st, const float *lrow,
                                        const float *rrow, float *orow, size_t x)
{
    const float32x4_t lv = lrow ? vld1q_f32(lrow + x) : vdupq_n_f32(0.f);
    const float32x4_t rv = rrow ? vld1q_f32(rrow + x) : vdupq_n_f32(0.f);
    const float32x4_t sum = vaddq_f32(lv, rv);
    float32x4_t p1[3];
    float32x4_t o[3];
    for (int p = 0; p < 3; p++) {
        p1[p] = vld1q_f32(st->prev1[p] + x);
        o[p] = ss2_iir_pole_neon(k, p, sum, p1[p], vld1q_f32(st->prev2[p] + x));
    }
    for (int p = 0; p < 3; p++) {
        vst1q_f32(st->prev2[p] + x, p1[p]);
        vst1q_f32(st->prev1[p] + x, o[p]);
    }
    if (orow) {
        vst1q_f32(orow + x, vaddq_f32(vaddq_f32(o[0], o[1]), o[2]));
    }
}

/* One column: the scalar tail of ss2_vblur_block_neon(), same operations. */
static inline void ss2_vblur_pixel(const float rg_n2[3], const float rg_d1[3],
                                   const Ss2ColState *st, const float *lrow, const float *rrow,
                                   float *orow, size_t x)
{
    const float lv = lrow ? lrow[x] : 0.f;
    const float rv = rrow ? rrow[x] : 0.f;
    const float sum = lv + rv;
    const float o0 = rg_n2[0] * sum - rg_d1[0] * st->prev1[0][x] - st->prev2[0][x];
    const float o1 = rg_n2[1] * sum - rg_d1[1] * st->prev1[1][x] - st->prev2[1][x];
    const float o2 = rg_n2[2] * sum - rg_d1[2] * st->prev1[2][x] - st->prev2[2][x];
    for (int p = 0; p < 3; p++) {
        st->prev2[p][x] = st->prev1[p][x];
    }
    st->prev1[0][x] = o0;
    st->prev1[1][x] = o1;
    st->prev1[2][x] = o2;
    if (orow) {
        orow[x] = o0 + o1 + o2;
    }
}

static void vblur_simd_4cols_neon(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                  float *col_state, const float *in, float *out, unsigned w,
                                  unsigned h)
{
    const size_t xsize = (size_t)w;
    Ss2ColState st;
    for (int p = 0; p < 3; p++) {
        st.prev1[p] = col_state + (size_t)p * xsize;
        st.prev2[p] = col_state + (size_t)(p + 3) * xsize;
    }
    memset(col_state, 0, 6u * xsize * sizeof(float));
    const Ss2IirK k = ss2_iir_k_init(rg_n2, rg_d1);

    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t ysize = (ptrdiff_t)h;

    for (ptrdiff_t n = -N + 1; n < ysize; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        /* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
         * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
         * documented /std:clatest C23 feature set does not include `nullptr` and the
         * required Windows builds compile C with cl.exe (C2065). ADR-1138. */
        const float *lrow = (left >= 0) ? (in + (size_t)left * xsize) : NULL;
        const float *rrow = (right < ysize) ? (in + (size_t)right * xsize) : NULL;
        float *orow = (n >= 0) ? (out + (size_t)n * xsize) : NULL;
        /* NOLINTEND(modernize-use-nullptr) */

        size_t x = 0;
        for (; x + 4 <= xsize; x += 4) {
            ss2_vblur_block_neon(&k, &st, lrow, rrow, orow, x);
        }
        for (; x < xsize; x++) {
            ss2_vblur_pixel(rg_n2, rg_d1, &st, lrow, rrow, orow, x);
        }
    }
}

void ssimulacra2_blur_plane_neon(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                 float *col_state, const float *in, float *out, float *scratch,
                                 unsigned w, unsigned h)
{
    assert(col_state != NULL);
    assert(in != NULL);
    assert(out != NULL);
    assert(scratch != NULL);
    assert(w > 0 && h > 0);

    unsigned y = 0;
    for (; y + 4 <= h; y += 4) {
        hblur_4rows_neon(rg_n2, rg_d1, rg_radius, in, scratch, w, y, 4);
    }
    if (y < h) {
        hblur_4rows_neon(rg_n2, rg_d1, rg_radius, in, scratch, w, y, h - y);
    }
    vblur_simd_4cols_neon(rg_n2, rg_d1, rg_radius, col_state, scratch, out, w, h);
}

/* YUV → linear RGB (ADR-0163). 4-wide aarch64 NEON mirror of the AVX2 port. */

static inline float32x4_t srgb_to_linear_lane_neon(float32x4_t v)
{
    alignas(16) float tmp[4];
    vst1q_f32(tmp, v);
    for (int k = 0; k < 4; k++) {
        const float x = tmp[k];
        tmp[k] = vmaf_ss2_srgb_eotf(x);
    }
    return vld1q_f32(tmp);
}

/* Four pixels at (x, y): the samples are read one by one (the planes may be
 * subsampled), the arithmetic is the four-wide form of ss2_yuv_pixel(). */
static inline void ss2_yuv_block_neon(const Ss2YuvK *k, const simd_plane_t planes[3], unsigned w,
                                      unsigned h, unsigned bpc, unsigned x, unsigned y, float *rdst,
                                      float *gdst, float *bdst)
{
    alignas(16) float y_tmp[4];
    alignas(16) float u_tmp[4];
    alignas(16) float v_tmp[4];
    for (int i = 0; i < 4; i++) {
        y_tmp[i] = ss2_read_plane_scalar(&planes[0], w, h, (int)(x + (unsigned)i), (int)y, bpc);
        u_tmp[i] = ss2_read_plane_scalar(&planes[1], w, h, (int)(x + (unsigned)i), (int)y, bpc);
        v_tmp[i] = ss2_read_plane_scalar(&planes[2], w, h, (int)(x + (unsigned)i), (int)y, bpc);
    }
    const float32x4_t vinv_peak = vdupq_n_f32(k->inv_peak);
    const float32x4_t vc_off = vdupq_n_f32(k->c_off);
    const float32x4_t vc_scale = vdupq_n_f32(k->c_scale);
    const float32x4_t Y = vmulq_f32(vld1q_f32(y_tmp), vinv_peak);
    const float32x4_t U = vmulq_f32(vld1q_f32(u_tmp), vinv_peak);
    const float32x4_t V = vmulq_f32(vld1q_f32(v_tmp), vinv_peak);
    const float32x4_t Yn = vmulq_f32(vsubq_f32(Y, vdupq_n_f32(k->y_off)), vdupq_n_f32(k->y_scale));
    const float32x4_t Un = vmulq_f32(vsubq_f32(U, vc_off), vc_scale);
    const float32x4_t Vn = vmulq_f32(vsubq_f32(V, vc_off), vc_scale);
    const float32x4_t vzero = vdupq_n_f32(0.0f);
    const float32x4_t vone = vdupq_n_f32(1.0f);
    /* ADR-0891: vfmaq_f32 — single-rounding FMA matches fmaf() in scalar ref. */
    float32x4_t R = vfmaq_f32(Yn, vdupq_n_f32(k->cr_r), Vn);
    float32x4_t G = vfmaq_f32(Yn, vdupq_n_f32(k->cb_g), Un);
    G = vfmaq_f32(G, vdupq_n_f32(k->cr_g), Vn);
    float32x4_t B = vfmaq_f32(Yn, vdupq_n_f32(k->cb_b), Un);
    R = vmaxq_f32(vminq_f32(R, vone), vzero);
    G = vmaxq_f32(vminq_f32(G, vone), vzero);
    B = vmaxq_f32(vminq_f32(B, vone), vzero);
    vst1q_f32(rdst, srgb_to_linear_lane_neon(R));
    vst1q_f32(gdst, srgb_to_linear_lane_neon(G));
    vst1q_f32(bdst, srgb_to_linear_lane_neon(B));
}

void ssimulacra2_picture_to_linear_rgb_neon(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
                                            const simd_plane_t planes[3], float *out)
{
    assert(planes != NULL);
    assert(out != NULL);
    assert(w > 0 && h > 0);

    const size_t plane_sz = (size_t)w * (size_t)h;
    float *rp = out;
    float *gp = out + plane_sz;
    float *bp = out + 2 * plane_sz;
    const Ss2YuvK k = ss2_yuv_k_init(yuv_matrix, bpc);

    for (unsigned y = 0; y < h; y++) {
        unsigned x = 0;
        for (; x + 4 <= w; x += 4) {
            const size_t idx = (size_t)y * w + x;
            ss2_yuv_block_neon(&k, planes, w, h, bpc, x, y, rp + idx, gp + idx, bp + idx);
        }
        for (; x < w; x++) {
            const size_t idx = (size_t)y * w + x;
            ss2_yuv_pixel(&k, planes, w, h, bpc, x, y, rp + idx, gp + idx, bp + idx);
        }
    }
}
