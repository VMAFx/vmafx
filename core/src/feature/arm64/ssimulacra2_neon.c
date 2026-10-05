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
static const float kC2 = 0.0009f;

static inline float32x4_t cbrtf_lane_neon(float32x4_t v)
{
    alignas(16) float tmp[4];
    vst1q_f32(tmp, v);
    for (int k = 0; k < 4; k++) {
        tmp[k] = vmaf_ss2_cbrtf(tmp[k]);
    }
    return vld1q_f32(tmp);
}

static inline double quartic_d(double x)
{
    x *= x;
    return x * x;
}

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

typedef struct {
    float32x4_t m00, m01, m02, m10, m11, m12, m20, m21, m22;
    float32x4_t bias, zero, cbrt_bias, half, c14, c42, c55, c01;
} xyb_vecs_neon;

typedef struct {
    const float *rp;
    const float *gp;
    const float *bp;
    float *xp;
    float *yp;
    float *bxp;
} xyb_planes_neon;

static xyb_vecs_neon xyb_vecs_init_neon(float m01, float m11, float m22, float cbrt_bias)
{
    const xyb_vecs_neon c = {
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
static inline float32x4_t lms_mix_neon(float32x4_t k0, float32x4_t k1, float32x4_t k2,
                                       float32x4_t r, float32x4_t g, float32x4_t b,
                                       const xyb_vecs_neon *c)
{
    float32x4_t v = vaddq_f32(vmulq_f32(k0, r), vmulq_f32(k1, g));
    v = vaddq_f32(v, vmulq_f32(k2, b));
    v = vaddq_f32(v, c->bias);
    return vmaxq_f32(v, c->zero);
}

static void xyb_block_neon(const xyb_vecs_neon *c, const xyb_planes_neon *p, size_t i)
{
    const float32x4_t r = vld1q_f32(p->rp + i);
    const float32x4_t g = vld1q_f32(p->gp + i);
    const float32x4_t b = vld1q_f32(p->bp + i);
    const float32x4_t l = lms_mix_neon(c->m00, c->m01, c->m02, r, g, b, c);
    const float32x4_t m = lms_mix_neon(c->m10, c->m11, c->m12, r, g, b, c);
    const float32x4_t sv = lms_mix_neon(c->m20, c->m21, c->m22, r, g, b, c);
    const float32x4_t L = vsubq_f32(cbrtf_lane_neon(l), c->cbrt_bias);
    const float32x4_t M = vsubq_f32(cbrtf_lane_neon(m), c->cbrt_bias);
    const float32x4_t S = vsubq_f32(cbrtf_lane_neon(sv), c->cbrt_bias);
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

static void xyb_tail_neon(const xyb_planes_neon *p, size_t i, size_t n, float m01, float m11,
                          float m22, float cbrt_bias)
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

void ssimulacra2_linear_rgb_to_xyb_neon(const float *lin, float *xyb, unsigned w, unsigned h)
{
    assert(lin != NULL);
    assert(xyb != NULL);
    assert(w > 0 && h > 0);
    const size_t plane_sz = (size_t)w * (size_t)h;
    const xyb_planes_neon planes = {lin, lin + plane_sz, lin + 2 * plane_sz,
                                    xyb, xyb + plane_sz, xyb + 2 * plane_sz};

    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);
    const xyb_vecs_neon vecs = xyb_vecs_init_neon(m01, m11, m22, cbrt_bias);

    size_t i = 0;
    for (; i + 4 <= plane_sz; i += 4) {
        xyb_block_neon(&vecs, &planes, i);
    }
    xyb_tail_neon(&planes, i, plane_sz, m01, m11, m22, cbrt_bias);
}

/* ADR-0141 carve-out: the outer loop iterates per-plane × per-row ×
 * per-tile and keeps the SIMD-deinterleave + scalar-tail together for
 * the line-for-line scalar diff audit. Splitting mid-iteration would
 * duplicate the bounds-clamp logic. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
void ssimulacra2_downsample_2x2_neon(const float *in, unsigned iw, unsigned ih, float *out,
                                     unsigned *ow_out, unsigned *oh_out)
{
    const unsigned ow = (iw + 1) / 2;
    const unsigned oh = (ih + 1) / 2;
    *ow_out = ow;
    *oh_out = oh;

    const size_t in_plane = (size_t)iw * (size_t)ih;
    const size_t out_plane = (size_t)ow * (size_t)oh;
    const float32x4_t vquarter = vdupq_n_f32(0.25f);

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
            const unsigned interior_end = (ow > 0 && iw >= 2) ? ((ow - 1) / 4) * 4 : 0;
            for (; ox < interior_end; ox += 4) {
                const size_t base = (size_t)ox * 2u;
                const float32x4_t r00 = vld1q_f32(row0 + base);
                const float32x4_t r01 = vld1q_f32(row0 + base + 4);
                const float32x4_t r10 = vld1q_f32(row1 + base);
                const float32x4_t r11 = vld1q_f32(row1 + base + 4);
                const float32x4_t r0e = vuzp1q_f32(r00, r01);
                const float32x4_t r0o = vuzp2q_f32(r00, r01);
                const float32x4_t r1e = vuzp1q_f32(r10, r11);
                const float32x4_t r1o = vuzp2q_f32(r10, r11);
                float32x4_t acc = vaddq_f32(r0e, r0o);
                acc = vaddq_f32(acc, r1e);
                acc = vaddq_f32(acc, r1o);
                vst1q_f32(orow + ox, vmulq_f32(acc, vquarter));
            }
            for (; ox < ow; ox++) {
                unsigned ix0 = ox * 2;
                unsigned ix1 = (ix0 + 1 < iw) ? ix0 + 1 : iw - 1;
                float sum = row0[ix0] + row0[ix1] + row1[ix0] + row1[ix1];
                orow[ox] = sum * 0.25f;
            }
        }
    }
}

/* One pixel's SSIM term in double, clamped at zero; shared by the vector
 * lanes and the scalar tail. */
static inline double ssim_term_d_neon(float num_m, float num_s, float denom_s)
{
    double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
    if (d < 0.0)
        d = 0.0;
    return d;
}

static void ssim_block_neon(const float *rm1, const float *rm2, const float *rs11,
                            const float *rs22, const float *rs12, size_t i, double *sum_l1,
                            double *sum_l4)
{
    const float32x4_t vc2 = vdupq_n_f32(kC2);
    const float32x4_t vone = vdupq_n_f32(1.0f);
    const float32x4_t vtwo = vdupq_n_f32(2.0f);
    const float32x4_t mu1 = vld1q_f32(rm1 + i);
    const float32x4_t mu2 = vld1q_f32(rm2 + i);
    const float32x4_t mu11 = vmulq_f32(mu1, mu1);
    const float32x4_t mu22 = vmulq_f32(mu2, mu2);
    const float32x4_t mu12 = vmulq_f32(mu1, mu2);
    const float32x4_t diff = vsubq_f32(mu1, mu2);
    const float32x4_t num_m = vsubq_f32(vone, vmulq_f32(diff, diff));
    const float32x4_t num_s = vaddq_f32(vmulq_f32(vtwo, vsubq_f32(vld1q_f32(rs12 + i), mu12)), vc2);
    const float32x4_t denom_s = vaddq_f32(
        vaddq_f32(vsubq_f32(vld1q_f32(rs11 + i), mu11), vsubq_f32(vld1q_f32(rs22 + i), mu22)), vc2);
    alignas(16) float num_m_f[4];
    alignas(16) float num_s_f[4];
    alignas(16) float denom_s_f[4];
    vst1q_f32(num_m_f, num_m);
    vst1q_f32(num_s_f, num_s);
    vst1q_f32(denom_s_f, denom_s);
    for (int k = 0; k < 4; k++) {
        const double d = ssim_term_d_neon(num_m_f[k], num_s_f[k], denom_s_f[k]);
        *sum_l1 += d;
        *sum_l4 += quartic_d(d);
    }
}

static void ssim_plane_sums_neon(const float *rm1, const float *rm2, const float *rs11,
                                 const float *rs22, const float *rs12, size_t plane, double *sum_l1,
                                 double *sum_l4)
{
    size_t i = 0;
    for (; i + 4 <= plane; i += 4) {
        ssim_block_neon(rm1, rm2, rs11, rs22, rs12, i, sum_l1, sum_l4);
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
        const double d = ssim_term_d_neon(num_m, num_s, denom_s);
        *sum_l1 += d;
        *sum_l4 += quartic_d(d);
    }
}

void ssimulacra2_ssim_map_neon(const float *m1, const float *m2, const float *s11, const float *s22,
                               const float *s12, unsigned w, unsigned h, double plane_averages[6])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;

    for (int c = 0; c < 3; c++) {
        double sum_l1 = 0.0;
        double sum_l4 = 0.0;
        const size_t off = (size_t)c * plane;
        ssim_plane_sums_neon(m1 + off, m2 + off, s11 + off, s22 + off, s12 + off, plane, &sum_l1,
                             &sum_l4);
        plane_averages[c * 2 + 0] = one_per_pixels * sum_l1;
        plane_averages[c * 2 + 1] = sqrt(sqrt(one_per_pixels * sum_l4));
    }
}

typedef struct {
    double s0;
    double s1;
    double s2;
    double s3;
} edge_sums_neon;

/* ADR-1208: the reference difference is taken in DOUBLE. `a` and `am` are
 * floats, so `(double)a - (double)am` is exact, whereas subtracting in float
 * rounds first. The scalar `edge_diff_map`, this kernel's scalar tail, and the
 * test's reference all promote before subtracting; vectorising the subtract in
 * float made the ssimulacra2 score depend on whether the host had SIMD. The
 * per-lane work is scalar, so the subtraction is folded into it. */
static inline void edge_add_neon(edge_sums_neon *s, float a1, float am1, float a2, float am2)
{
    double ed1 = fabs((double)a1 - (double)am1);
    double ed2 = fabs((double)a2 - (double)am2);
    double d = (1.0 + ed2) / (1.0 + ed1) - 1.0;
    double art;
    double det;
    vmaf_ss2_split_edge_difference(d, &art, &det);
    s->s0 += art;
    s->s1 += quartic_d(art);
    s->s2 += det;
    s->s3 += quartic_d(det);
}

static void edge_block_neon(edge_sums_neon *s, const float *r1, const float *rm1, const float *r2,
                            const float *rm2, size_t i)
{
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
        edge_add_neon(s, a1f[k], am1f[k], a2f[k], am2f[k]);
    }
}

static edge_sums_neon edge_plane_sums_neon(const float *r1, const float *rm1, const float *r2,
                                           const float *rm2, size_t plane)
{
    edge_sums_neon s = {0.0, 0.0, 0.0, 0.0};
    size_t i = 0;
    for (; i + 4 <= plane; i += 4) {
        edge_block_neon(&s, r1, rm1, r2, rm2, i);
    }
    for (; i < plane; i++) {
        edge_add_neon(&s, r1[i], rm1[i], r2[i], rm2[i]);
    }
    return s;
}

void ssimulacra2_edge_diff_map_neon(const float *img1, const float *mu1, const float *img2,
                                    const float *mu2, unsigned w, unsigned h,
                                    double plane_averages[12])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;

    for (int c = 0; c < 3; c++) {
        const size_t off = (size_t)c * plane;
        const edge_sums_neon s =
            edge_plane_sums_neon(img1 + off, mu1 + off, img2 + off, mu2 + off, plane);
        plane_averages[c * 4 + 0] = one_per_pixels * s.s0;
        plane_averages[c * 4 + 1] = sqrt(sqrt(one_per_pixels * s.s1));
        plane_averages[c * 4 + 2] = one_per_pixels * s.s2;
        plane_averages[c * 4 + 3] = sqrt(sqrt(one_per_pixels * s.s3));
    }
}

typedef struct {
    float32x4_t n2[3];
    float32x4_t d1[3];
} iir_coefs_neon;

typedef struct {
    float32x4_t p1[3];
    float32x4_t p2[3];
} iir_state_neon;

static iir_coefs_neon iir_coefs_init_neon(const float rg_n2[3], const float rg_d1[3])
{
    iir_coefs_neon c;
    for (int k = 0; k < 3; k++) {
        c.n2[k] = vdupq_n_f32(rg_n2[k]);
        c.d1[k] = vdupq_n_f32(rg_d1[k]);
    }
    return c;
}

/* One 3-pole IIR step for four lanes: o = n2*sum - d1*prev1 - prev2 per pole,
 * the state shifts, and the result is (o0 + o1) + o2. */
static float32x4_t iir_advance_neon(const iir_coefs_neon *c, iir_state_neon *st, float32x4_t sum)
{
    float32x4_t o[3];
    for (int k = 0; k < 3; k++) {
        o[k] = vsubq_f32(vmulq_f32(c->n2[k], sum), vmulq_f32(c->d1[k], st->p1[k]));
        o[k] = vsubq_f32(o[k], st->p2[k]);
    }
    for (int k = 0; k < 3; k++) {
        st->p2[k] = st->p1[k];
        st->p1[k] = o[k];
    }
    return vaddq_f32(vaddq_f32(o[0], o[1]), o[2]);
}

/* NEON has no gather: assemble the four row samples lane by lane. */
static float32x4_t gather_rows_neon(const float *const row_bases[4], unsigned row_count,
                                    ptrdiff_t idx)
{
    float32x4_t v = vdupq_n_f32(0.f);
    if (row_count > 0)
        v = vsetq_lane_f32(row_bases[0][idx], v, 0);
    if (row_count > 1)
        v = vsetq_lane_f32(row_bases[1][idx], v, 1);
    if (row_count > 2)
        v = vsetq_lane_f32(row_bases[2][idx], v, 2);
    if (row_count > 3)
        v = vsetq_lane_f32(row_bases[3][idx], v, 3);
    return v;
}

static void hblur_store_rows_neon(float *out, unsigned w, unsigned y_base, unsigned row_count,
                                  ptrdiff_t n, float32x4_t res)
{
    alignas(16) float store_tmp[4];
    vst1q_f32(store_tmp, res);
    for (unsigned i = 0; i < row_count; i++) {
        out[((size_t)y_base + i) * w + (size_t)n] = store_tmp[i];
    }
}

static void hblur_4rows_neon(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                             const float *in, float *out, unsigned w, unsigned y_base,
                             unsigned row_count)
{
    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t W = (ptrdiff_t)w;
    const iir_coefs_neon coefs = iir_coefs_init_neon(rg_n2, rg_d1);
    iir_state_neon st;
    for (int k = 0; k < 3; k++) {
        st.p1[k] = vdupq_n_f32(0.f);
        st.p2[k] = vdupq_n_f32(0.f);
    }

    /* Per-lane row base pointer addresses. */
    const float *row_bases[4] = {NULL, NULL, NULL, NULL};
    for (unsigned i = 0; i < row_count && i < 4; i++) {
        row_bases[i] = in + ((size_t)y_base + i) * w;
    }

    for (ptrdiff_t n = -N + 1; n < W; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float32x4_t lv =
            (left >= 0) ? gather_rows_neon(row_bases, row_count, left) : vdupq_n_f32(0.f);
        const float32x4_t rv =
            (right < W) ? gather_rows_neon(row_bases, row_count, right) : vdupq_n_f32(0.f);
        const float32x4_t res = iir_advance_neon(&coefs, &st, vaddq_f32(lv, rv));
        if (n >= 0) {
            hblur_store_rows_neon(out, w, y_base, row_count, n, res);
        }
    }
}

/* Four-column SIMD pass over one row; the IIR state lives in col_state
 * (prev1 poles 0..2, then prev2 poles 0..2, each `xsize` floats). Returns the
 * first column left for the scalar tail. */
static size_t vblur_vec_neon(const iir_coefs_neon *c, float *col_state, size_t xsize,
                             const float *lrow, const float *rrow, float *orow)
{
    size_t x = 0;
    for (; x + 4 <= xsize; x += 4) {
        const float32x4_t lv = lrow ? vld1q_f32(lrow + x) : vdupq_n_f32(0.f);
        const float32x4_t rv = rrow ? vld1q_f32(rrow + x) : vdupq_n_f32(0.f);
        iir_state_neon st;
        for (int k = 0; k < 3; k++) {
            st.p1[k] = vld1q_f32(col_state + (size_t)k * xsize + x);
            st.p2[k] = vld1q_f32(col_state + (size_t)(3 + k) * xsize + x);
        }
        const float32x4_t res = iir_advance_neon(c, &st, vaddq_f32(lv, rv));
        for (int k = 0; k < 3; k++) {
            vst1q_f32(col_state + (size_t)(3 + k) * xsize + x, st.p2[k]);
            vst1q_f32(col_state + (size_t)k * xsize + x, st.p1[k]);
        }
        if (orow) {
            vst1q_f32(orow + x, res);
        }
    }
    return x;
}

static void vblur_tail_neon(const float rg_n2[3], const float rg_d1[3], float *col_state,
                            size_t xsize, size_t x, const float *lrow, const float *rrow,
                            float *orow)
{
    for (; x < xsize; x++) {
        const float lv = lrow ? lrow[x] : 0.f;
        const float rv = rrow ? rrow[x] : 0.f;
        const float sum = lv + rv;
        float o[3];
        for (size_t k = 0; k < 3; k++) {
            const float p1 = col_state[k * xsize + x];
            const float p2 = col_state[(3 + k) * xsize + x];
            o[k] = rg_n2[k] * sum - rg_d1[k] * p1 - p2;
        }
        for (size_t k = 0; k < 3; k++) {
            col_state[(3 + k) * xsize + x] = col_state[k * xsize + x];
            col_state[k * xsize + x] = o[k];
        }
        if (orow) {
            orow[x] = o[0] + o[1] + o[2];
        }
    }
}

static void vblur_simd_4cols_neon(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                  float *col_state, const float *in, float *out, unsigned w,
                                  unsigned h)
{
    const size_t xsize = (size_t)w;
    memset(col_state, 0, 6u * xsize * sizeof(float));
    const iir_coefs_neon coefs = iir_coefs_init_neon(rg_n2, rg_d1);
    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t ysize = (ptrdiff_t)h;

    for (ptrdiff_t n = -N + 1; n < ysize; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float *lrow = (left >= 0) ? (in + (size_t)left * xsize) : NULL;
        const float *rrow = (right < ysize) ? (in + (size_t)right * xsize) : NULL;
        float *orow = (n >= 0) ? (out + (size_t)n * xsize) : NULL;
        const size_t x = vblur_vec_neon(&coefs, col_state, xsize, lrow, rrow, orow);
        vblur_tail_neon(rg_n2, rg_d1, col_state, xsize, x, lrow, rrow, orow);
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

static inline float read_plane_scalar_s2_neon(const simd_plane_t *p, unsigned lw, unsigned lh,
                                              int x, int y, unsigned bpc)
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

static inline void compute_matrix_coefs_neon(int yuv_matrix, float *kr_out, float *kg_out,
                                             float *kb_out, int *limited_out)
{
    switch (yuv_matrix) {
    case 2:
        *limited_out = 0;
        *kr_out = 0.2126f;
        *kg_out = 0.7152f;
        *kb_out = 0.0722f;
        break;
    case 0:
        *limited_out = 1;
        *kr_out = 0.2126f;
        *kg_out = 0.7152f;
        *kb_out = 0.0722f;
        break;
    case 3:
        *limited_out = 0;
        *kr_out = 0.299f;
        *kg_out = 0.587f;
        *kb_out = 0.114f;
        break;
    case 1:
    default:
        *limited_out = 1;
        *kr_out = 0.299f;
        *kg_out = 0.587f;
        *kb_out = 0.114f;
        break;
    }
}

typedef struct {
    float inv_peak;
    float y_scale;
    float c_scale;
    float y_off;
    float c_off;
    float cr_r;
    float cb_b;
    float cb_g;
    float cr_g;
} ptlr_coefs_neon;

typedef struct {
    const simd_plane_t *planes;
    unsigned w;
    unsigned h;
    unsigned bpc;
    float *rp;
    float *gp;
    float *bp;
} ptlr_ctx_neon;

static ptlr_coefs_neon ptlr_coefs_init_neon(int yuv_matrix, unsigned bpc)
{
    const float peak = (float)((1u << bpc) - 1u);
    float kr;
    float kg;
    float kb;
    int limited;
    compute_matrix_coefs_neon(yuv_matrix, &kr, &kg, &kb, &limited);

    ptlr_coefs_neon c;
    c.inv_peak = 1.0f / peak;
    c.cr_r = 2.0f * (1.0f - kr);
    c.cb_b = 2.0f * (1.0f - kb);
    c.cb_g = -(2.0f * kb * (1.0f - kb)) / kg;
    c.cr_g = -(2.0f * kr * (1.0f - kr)) / kg;
    c.y_scale = limited ? (255.0f / 219.0f) : 1.0f;
    c.c_scale = limited ? (255.0f / 224.0f) : 1.0f;
    c.y_off = limited ? (16.0f / 255.0f) : 0.0f;
    c.c_off = 0.5f;
    return c;
}

/* Four pixels: per-lane scalar plane reads, vector YUV -> RGB with fused
 * multiply-add, per-lane scalar sRGB EOTF. */
static void ptlr_block_neon(const ptlr_coefs_neon *c, const ptlr_ctx_neon *ctx, unsigned x,
                            unsigned y)
{
    alignas(16) float y_tmp[4];
    alignas(16) float u_tmp[4];
    alignas(16) float v_tmp[4];
    for (int i = 0; i < 4; i++) {
        const int px = (int)(x + (unsigned)i);
        y_tmp[i] = read_plane_scalar_s2_neon(&ctx->planes[0], ctx->w, ctx->h, px, (int)y, ctx->bpc);
        u_tmp[i] = read_plane_scalar_s2_neon(&ctx->planes[1], ctx->w, ctx->h, px, (int)y, ctx->bpc);
        v_tmp[i] = read_plane_scalar_s2_neon(&ctx->planes[2], ctx->w, ctx->h, px, (int)y, ctx->bpc);
    }
    const float32x4_t vinv_peak = vdupq_n_f32(c->inv_peak);
    const float32x4_t vc_off = vdupq_n_f32(c->c_off);
    const float32x4_t vc_scale = vdupq_n_f32(c->c_scale);
    const float32x4_t vzero = vdupq_n_f32(0.0f);
    const float32x4_t vone = vdupq_n_f32(1.0f);
    const float32x4_t Y = vmulq_f32(vld1q_f32(y_tmp), vinv_peak);
    const float32x4_t U = vmulq_f32(vld1q_f32(u_tmp), vinv_peak);
    const float32x4_t V = vmulq_f32(vld1q_f32(v_tmp), vinv_peak);
    const float32x4_t Yn = vmulq_f32(vsubq_f32(Y, vdupq_n_f32(c->y_off)), vdupq_n_f32(c->y_scale));
    const float32x4_t Un = vmulq_f32(vsubq_f32(U, vc_off), vc_scale);
    const float32x4_t Vn = vmulq_f32(vsubq_f32(V, vc_off), vc_scale);
    /* ADR-0891: vfmaq_f32 — single-rounding FMA matches fmaf() in scalar ref. */
    float32x4_t R = vfmaq_f32(Yn, vdupq_n_f32(c->cr_r), Vn);
    float32x4_t G = vfmaq_f32(Yn, vdupq_n_f32(c->cb_g), Un);
    G = vfmaq_f32(G, vdupq_n_f32(c->cr_g), Vn);
    float32x4_t B = vfmaq_f32(Yn, vdupq_n_f32(c->cb_b), Un);
    R = vmaxq_f32(vminq_f32(R, vone), vzero);
    G = vmaxq_f32(vminq_f32(G, vone), vzero);
    B = vmaxq_f32(vminq_f32(B, vone), vzero);
    R = srgb_to_linear_lane_neon(R);
    G = srgb_to_linear_lane_neon(G);
    B = srgb_to_linear_lane_neon(B);
    const size_t idx = (size_t)y * ctx->w + x;
    vst1q_f32(ctx->rp + idx, R);
    vst1q_f32(ctx->gp + idx, G);
    vst1q_f32(ctx->bp + idx, B);
}

static void ptlr_pixel_neon(const ptlr_coefs_neon *c, const ptlr_ctx_neon *ctx, unsigned x,
                            unsigned y)
{
    const unsigned w = ctx->w;
    const unsigned h = ctx->h;
    const float Ys =
        read_plane_scalar_s2_neon(&ctx->planes[0], w, h, (int)x, (int)y, ctx->bpc) * c->inv_peak;
    const float Us =
        read_plane_scalar_s2_neon(&ctx->planes[1], w, h, (int)x, (int)y, ctx->bpc) * c->inv_peak;
    const float Vs =
        read_plane_scalar_s2_neon(&ctx->planes[2], w, h, (int)x, (int)y, ctx->bpc) * c->inv_peak;
    const float Yn = (Ys - c->y_off) * c->y_scale;
    const float Un = (Us - c->c_off) * c->c_scale;
    const float Vn = (Vs - c->c_off) * c->c_scale;
    /* ADR-0891: fmaf() matches vfmaq_f32 single-rounding contract. */
    float R = fmaf(c->cr_r, Vn, Yn);
    float G = fmaf(c->cb_g, Un, Yn);
    G = fmaf(c->cr_g, Vn, G);
    float B = fmaf(c->cb_b, Un, Yn);
    if (R < 0.0f)
        R = 0.0f;
    if (R > 1.0f)
        R = 1.0f;
    if (G < 0.0f)
        G = 0.0f;
    if (G > 1.0f)
        G = 1.0f;
    if (B < 0.0f)
        B = 0.0f;
    if (B > 1.0f)
        B = 1.0f;
    const float Rl = vmaf_ss2_srgb_eotf(R);
    const float Gl = vmaf_ss2_srgb_eotf(G);
    const float Bl = vmaf_ss2_srgb_eotf(B);
    const size_t idx = (size_t)y * w + x;
    ctx->rp[idx] = Rl;
    ctx->gp[idx] = Gl;
    ctx->bp[idx] = Bl;
}

void ssimulacra2_picture_to_linear_rgb_neon(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
                                            const simd_plane_t planes[3], float *out)
{
    assert(planes != NULL);
    assert(out != NULL);
    assert(w > 0 && h > 0);

    const size_t plane_sz = (size_t)w * (size_t)h;
    const ptlr_ctx_neon ctx = {planes, w, h, bpc, out, out + plane_sz, out + 2 * plane_sz};
    const ptlr_coefs_neon coefs = ptlr_coefs_init_neon(yuv_matrix, bpc);

    for (unsigned y = 0; y < h; y++) {
        unsigned x = 0;
        for (; x + 4 <= w; x += 4) {
            ptlr_block_neon(&coefs, &ctx, x, y);
        }
        for (; x < w; x++) {
            ptlr_pixel_neon(&coefs, &ctx, x, y);
        }
    }
}
