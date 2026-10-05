/**
 *
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 */

/*
 * aarch64 SVE2 port of the SSIMULACRA 2 SIMD kernels (T7-38). Matches
 * the NEON sibling lane-for-lane: every kernel processes 4 float lanes
 * at a time under an `svwhilelt_b32(0, 4)` fixed-width predicate so the
 * arithmetic order is identical to `float32x4_t` regardless of the
 * runtime vector length. This preserves the ADR-0138 / ADR-0139 /
 * ADR-0140 byte-exact contract: the SVE2 path produces output that is
 * memcmp-equal to both NEON and the scalar reference.
 *
 * Tails (loop bound n % 4 != 0) are handled by tightening the predicate
 * via `svwhilelt_b32(i, n)`. All `cbrtf` / `srgb_eotf` libm calls stay
 * scalar (per-lane spill + reload) — same as the NEON port.
 *
 * Research-0016 / Research-0017 captured the design path; this TU
 * supersedes the "deferred pending CI hardware" footnote — local
 * validation runs under qemu-aarch64-static with `-cpu max,sve=on,
 * sve2=on`.
 */

#include <arm_neon.h>
#include <arm_sve.h>
#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "feature/ssimulacra2_math.h"
#include "feature/ssimulacra2_score.h"
#include "ssimulacra2_sve2.h"

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

/* All kernels lock to 4 active lanes (`PG4`) to mirror NEON arithmetic
 * order exactly. The runtime vector length on SVE2 hardware is always
 * >= 128 bits (4 floats), so this is universally safe. The wider
 * lanes simply stay false in the predicate. */
static inline svbool_t pg4(void)
{
    return svwhilelt_b32((uint32_t)0, (uint32_t)4);
}

static inline svfloat32_t cbrtf_lane_sve2(svbool_t pg, svfloat32_t v)
{
    alignas(16) float tmp[4] = {0.f, 0.f, 0.f, 0.f};
    svst1_f32(pg, tmp, v);
    for (int k = 0; k < 4; k++) {
        tmp[k] = vmaf_ss2_cbrtf(tmp[k]);
    }
    return svld1_f32(pg, tmp);
}

static inline double quartic_d(double x)
{
    x *= x;
    return x * x;
}

void ssimulacra2_multiply_3plane_sve2(const float *a, const float *b, float *mul, unsigned w,
                                      unsigned h)
{
    const size_t n = 3u * (size_t)w * (size_t)h;
    const svbool_t pg = pg4();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const svfloat32_t va = svld1_f32(pg, a + i);
        const svfloat32_t vb = svld1_f32(pg, b + i);
        svst1_f32(pg, mul + i, svmul_f32_x(pg, va, vb));
    }
    for (; i < n; i++) {
        mul[i] = a[i] * b[i];
    }
}

/* sizeless SVE vectors cannot live in structs: the constants stay scalar and
 * every helper broadcasts them with svdup_f32(). */
typedef struct {
    float m01;
    float m11;
    float m22;
    float cbrt_bias;
} xyb_coefs_sve2;

typedef struct {
    const float *rp;
    const float *gp;
    const float *bp;
    float *xp;
    float *yp;
    float *bxp;
} xyb_planes_sve2;

/* One LMS channel: (k0*r + k1*g) + k2*b + bias, clamped at zero. */
static inline svfloat32_t lms_mix_sve2(svbool_t pg, float k0, float k1, float k2, svfloat32_t r,
                                       svfloat32_t g, svfloat32_t b)
{
    svfloat32_t v =
        svadd_f32_x(pg, svmul_f32_x(pg, svdup_f32(k0), r), svmul_f32_x(pg, svdup_f32(k1), g));
    v = svadd_f32_x(pg, v, svmul_f32_x(pg, svdup_f32(k2), b));
    v = svadd_f32_x(pg, v, svdup_f32(kOpsinBias));
    return svmax_f32_x(pg, v, svdup_f32(0.0f));
}

static void xyb_block_sve2(svbool_t pg, const xyb_coefs_sve2 *c, const xyb_planes_sve2 *p, size_t i)
{
    const svfloat32_t r = svld1_f32(pg, p->rp + i);
    const svfloat32_t g = svld1_f32(pg, p->gp + i);
    const svfloat32_t b = svld1_f32(pg, p->bp + i);
    const svfloat32_t l = lms_mix_sve2(pg, kM00, c->m01, kM02, r, g, b);
    const svfloat32_t m = lms_mix_sve2(pg, kM10, c->m11, kM12, r, g, b);
    const svfloat32_t sv = lms_mix_sve2(pg, kM20, kM21, c->m22, r, g, b);
    const svfloat32_t vcbrt_bias = svdup_f32(c->cbrt_bias);
    const svfloat32_t L = svsub_f32_x(pg, cbrtf_lane_sve2(pg, l), vcbrt_bias);
    const svfloat32_t M = svsub_f32_x(pg, cbrtf_lane_sve2(pg, m), vcbrt_bias);
    const svfloat32_t S = svsub_f32_x(pg, cbrtf_lane_sve2(pg, sv), vcbrt_bias);
    const svfloat32_t vhalf = svdup_f32(0.5f);
    const svfloat32_t X = svmul_f32_x(pg, vhalf, svsub_f32_x(pg, L, M));
    const svfloat32_t Y = svmul_f32_x(pg, vhalf, svadd_f32_x(pg, L, M));
    const svfloat32_t B = S;
    const svfloat32_t Bfinal = svadd_f32_x(pg, svsub_f32_x(pg, B, Y), svdup_f32(0.55f));
    const svfloat32_t Xfinal =
        svadd_f32_x(pg, svmul_f32_x(pg, X, svdup_f32(14.0f)), svdup_f32(0.42f));
    const svfloat32_t Yfinal = svadd_f32_x(pg, Y, svdup_f32(0.01f));
    svst1_f32(pg, p->xp + i, Xfinal);
    svst1_f32(pg, p->yp + i, Yfinal);
    svst1_f32(pg, p->bxp + i, Bfinal);
}

static void xyb_tail_sve2(const xyb_planes_sve2 *p, size_t i, size_t n, const xyb_coefs_sve2 *c)
{
    const float m01 = c->m01;
    const float m11 = c->m11;
    const float m22 = c->m22;
    const float cbrt_bias = c->cbrt_bias;
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

void ssimulacra2_linear_rgb_to_xyb_sve2(const float *lin, float *xyb, unsigned w, unsigned h)
{
    assert(lin != NULL);
    assert(xyb != NULL);
    assert(w > 0 && h > 0);
    const size_t plane_sz = (size_t)w * (size_t)h;
    const xyb_planes_sve2 planes = {lin, lin + plane_sz, lin + 2 * plane_sz,
                                    xyb, xyb + plane_sz, xyb + 2 * plane_sz};
    const xyb_coefs_sve2 coefs = {1.0f - kM00 - kM02, 1.0f - kM10 - kM12, 1.0f - kM20 - kM21,
                                  vmaf_ss2_cbrtf(kOpsinBias)};

    const svbool_t pg = pg4();
    size_t i = 0;
    for (; i + 4 <= plane_sz; i += 4) {
        xyb_block_sve2(pg, &coefs, &planes, i);
    }
    xyb_tail_sve2(&planes, i, plane_sz, &coefs);
}

/* ADR-0141 carve-out — same rationale as the NEON sibling: the outer
 * loop iterates per-plane × per-row × per-tile and keeps the
 * deinterleave + scalar-tail together for the line-for-line scalar
 * diff audit. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
void ssimulacra2_downsample_2x2_sve2(const float *in, unsigned iw, unsigned ih, float *out,
                                     unsigned *ow_out, unsigned *oh_out)
{
    const unsigned ow = (iw + 1) / 2;
    const unsigned oh = (ih + 1) / 2;
    *ow_out = ow;
    *oh_out = oh;

    const size_t in_plane = (size_t)iw * (size_t)ih;
    const size_t out_plane = (size_t)ow * (size_t)oh;
    const svbool_t pg = pg4();
    const svfloat32_t vquarter = svdup_f32(0.25f);

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
                /* Use NEON deinterleave to mirror the NEON port byte
                 * for byte; SVE2 has uzp1/uzp2 too but the NEON
                 * intrinsic is the audited reference. */
                const float32x4_t r00 = vld1q_f32(row0 + base);
                const float32x4_t r01 = vld1q_f32(row0 + base + 4);
                const float32x4_t r10 = vld1q_f32(row1 + base);
                const float32x4_t r11 = vld1q_f32(row1 + base + 4);
                const float32x4_t r0e = vuzp1q_f32(r00, r01);
                const float32x4_t r0o = vuzp2q_f32(r00, r01);
                const float32x4_t r1e = vuzp1q_f32(r10, r11);
                const float32x4_t r1o = vuzp2q_f32(r10, r11);
                const svfloat32_t s_r0e = svld1_f32(pg, (const float *)&r0e);
                const svfloat32_t s_r0o = svld1_f32(pg, (const float *)&r0o);
                const svfloat32_t s_r1e = svld1_f32(pg, (const float *)&r1e);
                const svfloat32_t s_r1o = svld1_f32(pg, (const float *)&r1o);
                svfloat32_t acc = svadd_f32_x(pg, s_r0e, s_r0o);
                acc = svadd_f32_x(pg, acc, s_r1e);
                acc = svadd_f32_x(pg, acc, s_r1o);
                svst1_f32(pg, orow + ox, svmul_f32_x(pg, acc, vquarter));
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
static inline double ssim_term_d_sve2(float num_m, float num_s, float denom_s)
{
    double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
    if (d < 0.0)
        d = 0.0;
    return d;
}

static void ssim_block_sve2(svbool_t pg, const float *rm1, const float *rm2, const float *rs11,
                            const float *rs22, const float *rs12, size_t i, double *sum_l1,
                            double *sum_l4)
{
    const svfloat32_t vc2 = svdup_f32(kC2);
    const svfloat32_t vone = svdup_f32(1.0f);
    const svfloat32_t vtwo = svdup_f32(2.0f);
    const svfloat32_t mu1 = svld1_f32(pg, rm1 + i);
    const svfloat32_t mu2 = svld1_f32(pg, rm2 + i);
    const svfloat32_t mu11 = svmul_f32_x(pg, mu1, mu1);
    const svfloat32_t mu22 = svmul_f32_x(pg, mu2, mu2);
    const svfloat32_t mu12 = svmul_f32_x(pg, mu1, mu2);
    const svfloat32_t diff = svsub_f32_x(pg, mu1, mu2);
    const svfloat32_t num_m = svsub_f32_x(pg, vone, svmul_f32_x(pg, diff, diff));
    const svfloat32_t num_s =
        svadd_f32_x(pg, svmul_f32_x(pg, vtwo, svsub_f32_x(pg, svld1_f32(pg, rs12 + i), mu12)), vc2);
    const svfloat32_t denom_s =
        svadd_f32_x(pg,
                    svadd_f32_x(pg, svsub_f32_x(pg, svld1_f32(pg, rs11 + i), mu11),
                                svsub_f32_x(pg, svld1_f32(pg, rs22 + i), mu22)),
                    vc2);
    alignas(16) float num_m_f[4] = {0.f, 0.f, 0.f, 0.f};
    alignas(16) float num_s_f[4] = {0.f, 0.f, 0.f, 0.f};
    alignas(16) float denom_s_f[4] = {0.f, 0.f, 0.f, 0.f};
    svst1_f32(pg, num_m_f, num_m);
    svst1_f32(pg, num_s_f, num_s);
    svst1_f32(pg, denom_s_f, denom_s);
    for (int k = 0; k < 4; k++) {
        const double d = ssim_term_d_sve2(num_m_f[k], num_s_f[k], denom_s_f[k]);
        *sum_l1 += d;
        *sum_l4 += quartic_d(d);
    }
}

static void ssim_plane_sums_sve2(svbool_t pg, const float *rm1, const float *rm2, const float *rs11,
                                 const float *rs22, const float *rs12, size_t plane, double *sum_l1,
                                 double *sum_l4)
{
    size_t i = 0;
    for (; i + 4 <= plane; i += 4) {
        ssim_block_sve2(pg, rm1, rm2, rs11, rs22, rs12, i, sum_l1, sum_l4);
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
        const double d = ssim_term_d_sve2(num_m, num_s, denom_s);
        *sum_l1 += d;
        *sum_l4 += quartic_d(d);
    }
}

void ssimulacra2_ssim_map_sve2(const float *m1, const float *m2, const float *s11, const float *s22,
                               const float *s12, unsigned w, unsigned h, double plane_averages[6])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    const svbool_t pg = pg4();

    for (int c = 0; c < 3; c++) {
        double sum_l1 = 0.0;
        double sum_l4 = 0.0;
        const size_t off = (size_t)c * plane;
        ssim_plane_sums_sve2(pg, m1 + off, m2 + off, s11 + off, s22 + off, s12 + off, plane,
                             &sum_l1, &sum_l4);
        plane_averages[c * 2 + 0] = one_per_pixels * sum_l1;
        plane_averages[c * 2 + 1] = sqrt(sqrt(one_per_pixels * sum_l4));
    }
}

typedef struct {
    double s0;
    double s1;
    double s2;
    double s3;
} edge_sums_sve2;

/* ADR-1208: the reference difference is taken in DOUBLE. `a` and `am` are
 * floats, so `(double)a - (double)am` is exact, whereas subtracting in float
 * rounds first. The scalar `edge_diff_map`, this kernel's scalar tail, and the
 * test's reference all promote before subtracting; vectorising the subtract in
 * float made the ssimulacra2 score depend on whether the host had SIMD. The
 * per-lane work is scalar, so the subtraction is folded into it. */
static inline void edge_add_sve2(edge_sums_sve2 *s, float a1, float am1, float a2, float am2)
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

static void edge_block_sve2(svbool_t pg, edge_sums_sve2 *s, const float *r1, const float *rm1,
                            const float *r2, const float *rm2, size_t i)
{
    const svfloat32_t a1 = svld1_f32(pg, r1 + i);
    const svfloat32_t a2 = svld1_f32(pg, r2 + i);
    const svfloat32_t am1 = svld1_f32(pg, rm1 + i);
    const svfloat32_t am2 = svld1_f32(pg, rm2 + i);
    alignas(16) float a1f[4] = {0.f, 0.f, 0.f, 0.f};
    alignas(16) float am1f[4] = {0.f, 0.f, 0.f, 0.f};
    alignas(16) float a2f[4] = {0.f, 0.f, 0.f, 0.f};
    alignas(16) float am2f[4] = {0.f, 0.f, 0.f, 0.f};
    svst1_f32(pg, a1f, a1);
    svst1_f32(pg, am1f, am1);
    svst1_f32(pg, a2f, a2);
    svst1_f32(pg, am2f, am2);
    for (int k = 0; k < 4; k++) {
        edge_add_sve2(s, a1f[k], am1f[k], a2f[k], am2f[k]);
    }
}

static edge_sums_sve2 edge_plane_sums_sve2(svbool_t pg, const float *r1, const float *rm1,
                                           const float *r2, const float *rm2, size_t plane)
{
    edge_sums_sve2 s = {0.0, 0.0, 0.0, 0.0};
    size_t i = 0;
    for (; i + 4 <= plane; i += 4) {
        edge_block_sve2(pg, &s, r1, rm1, r2, rm2, i);
    }
    for (; i < plane; i++) {
        edge_add_sve2(&s, r1[i], rm1[i], r2[i], rm2[i]);
    }
    return s;
}

void ssimulacra2_edge_diff_map_sve2(const float *img1, const float *mu1, const float *img2,
                                    const float *mu2, unsigned w, unsigned h,
                                    double plane_averages[12])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    const svbool_t pg = pg4();

    for (int c = 0; c < 3; c++) {
        const size_t off = (size_t)c * plane;
        const edge_sums_sve2 s =
            edge_plane_sums_sve2(pg, img1 + off, mu1 + off, img2 + off, mu2 + off, plane);
        plane_averages[c * 4 + 0] = one_per_pixels * s.s0;
        plane_averages[c * 4 + 1] = sqrt(sqrt(one_per_pixels * s.s1));
        plane_averages[c * 4 + 2] = one_per_pixels * s.s2;
        plane_averages[c * 4 + 3] = sqrt(sqrt(one_per_pixels * s.s3));
    }
}

/* One pole of the 3-pole IIR for four lanes, state in memory: o = n2*sum -
 * d1*prev1 - prev2; prev2 takes the old prev1, prev1 takes o. */
static svfloat32_t iir_pole_sve2(svbool_t pg, float n2, float d1, float *p1, float *p2,
                                 svfloat32_t sum)
{
    const svfloat32_t v1 = svld1_f32(pg, p1);
    const svfloat32_t v2 = svld1_f32(pg, p2);
    svfloat32_t o =
        svsub_f32_x(pg, svmul_f32_x(pg, svdup_f32(n2), sum), svmul_f32_x(pg, svdup_f32(d1), v1));
    o = svsub_f32_x(pg, o, v2);
    svst1_f32(pg, p2, v1);
    svst1_f32(pg, p1, o);
    return o;
}

/* The three poles of one IIR step; the result is (o0 + o1) + o2. */
static svfloat32_t iir_advance_sve2(svbool_t pg, const float rg_n2[3], const float rg_d1[3],
                                    float *const p1[3], float *const p2[3], svfloat32_t sum)
{
    const svfloat32_t o0 = iir_pole_sve2(pg, rg_n2[0], rg_d1[0], p1[0], p2[0], sum);
    const svfloat32_t o1 = iir_pole_sve2(pg, rg_n2[1], rg_d1[1], p1[1], p2[1], sum);
    const svfloat32_t o2 = iir_pole_sve2(pg, rg_n2[2], rg_d1[2], p1[2], p2[2], sum);
    return svadd_f32_x(pg, svadd_f32_x(pg, o0, o1), o2);
}

/* Assemble the four row samples at `idx`; rows past row_count read zero. */
static svfloat32_t gather_rows_sve2(svbool_t pg, const float *const row_bases[4],
                                    unsigned row_count, ptrdiff_t idx)
{
    alignas(16) float lane_tmp[4] = {0.f, 0.f, 0.f, 0.f};
    for (unsigned i = 0; i < 4; i++) {
        lane_tmp[i] = (i < row_count) ? row_bases[i][idx] : 0.f;
    }
    return svld1_f32(pg, lane_tmp);
}

static void hblur_store_rows_sve2(svbool_t pg, float *out, unsigned w, unsigned y_base,
                                  unsigned row_count, ptrdiff_t n, svfloat32_t res)
{
    alignas(16) float store_tmp[4] = {0.f, 0.f, 0.f, 0.f};
    svst1_f32(pg, store_tmp, res);
    for (unsigned i = 0; i < row_count; i++) {
        out[((size_t)y_base + i) * w + (size_t)n] = store_tmp[i];
    }
}

static void hblur_4rows_sve2(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                             const float *in, float *out, unsigned w, unsigned y_base,
                             unsigned row_count)
{
    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t W = (ptrdiff_t)w;
    const svbool_t pg = pg4();

    /* IIR state, four lanes per pole: prev1 poles 0..2, then prev2 poles. */
    alignas(16) float state[6][4];
    float *p1[3];
    float *p2[3];
    memset(state, 0, sizeof(state));
    for (int k = 0; k < 3; k++) {
        p1[k] = state[k];
        p2[k] = state[3 + k];
    }

    /* Per-lane row base pointers — SVE2 has gather but the byte-exact
     * contract pins us to NEON's lane-by-lane assemble pattern. */
    const float *row_bases[4] = {NULL, NULL, NULL, NULL};
    for (unsigned i = 0; i < row_count && i < 4; i++) {
        row_bases[i] = in + ((size_t)y_base + i) * w;
    }

    for (ptrdiff_t n = -N + 1; n < W; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const svfloat32_t lv =
            (left >= 0) ? gather_rows_sve2(pg, row_bases, row_count, left) : svdup_f32(0.f);
        const svfloat32_t rv =
            (right < W) ? gather_rows_sve2(pg, row_bases, row_count, right) : svdup_f32(0.f);
        const svfloat32_t res = iir_advance_sve2(pg, rg_n2, rg_d1, p1, p2, svadd_f32_x(pg, lv, rv));
        if (n >= 0) {
            hblur_store_rows_sve2(pg, out, w, y_base, row_count, n, res);
        }
    }
}

/* Four-column SIMD pass over one row; the IIR state lives in col_state
 * (prev1 poles 0..2, then prev2 poles 0..2, each `xsize` floats). Returns the
 * first column left for the scalar tail. */
static size_t vblur_vec_sve2(svbool_t pg, const float rg_n2[3], const float rg_d1[3],
                             float *col_state, size_t xsize, const float *lrow, const float *rrow,
                             float *orow)
{
    size_t x = 0;
    for (; x + 4 <= xsize; x += 4) {
        const svfloat32_t lv = lrow ? svld1_f32(pg, lrow + x) : svdup_f32(0.f);
        const svfloat32_t rv = rrow ? svld1_f32(pg, rrow + x) : svdup_f32(0.f);
        float *p1[3];
        float *p2[3];
        for (size_t k = 0; k < 3; k++) {
            p1[k] = col_state + k * xsize + x;
            p2[k] = col_state + (3 + k) * xsize + x;
        }
        const svfloat32_t res = iir_advance_sve2(pg, rg_n2, rg_d1, p1, p2, svadd_f32_x(pg, lv, rv));
        if (orow) {
            svst1_f32(pg, orow + x, res);
        }
    }
    return x;
}

static void vblur_tail_sve2(const float rg_n2[3], const float rg_d1[3], float *col_state,
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

static void vblur_simd_4cols_sve2(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                  float *col_state, const float *in, float *out, unsigned w,
                                  unsigned h)
{
    const size_t xsize = (size_t)w;
    memset(col_state, 0, 6u * xsize * sizeof(float));
    const svbool_t pg = pg4();
    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t ysize = (ptrdiff_t)h;

    for (ptrdiff_t n = -N + 1; n < ysize; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float *lrow = (left >= 0) ? (in + (size_t)left * xsize) : NULL;
        const float *rrow = (right < ysize) ? (in + (size_t)right * xsize) : NULL;
        float *orow = (n >= 0) ? (out + (size_t)n * xsize) : NULL;
        const size_t x = vblur_vec_sve2(pg, rg_n2, rg_d1, col_state, xsize, lrow, rrow, orow);
        vblur_tail_sve2(rg_n2, rg_d1, col_state, xsize, x, lrow, rrow, orow);
    }
}

void ssimulacra2_blur_plane_sve2(const float rg_n2[3], const float rg_d1[3], int rg_radius,
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
        hblur_4rows_sve2(rg_n2, rg_d1, rg_radius, in, scratch, w, y, 4);
    }
    if (y < h) {
        hblur_4rows_sve2(rg_n2, rg_d1, rg_radius, in, scratch, w, y, h - y);
    }
    vblur_simd_4cols_sve2(rg_n2, rg_d1, rg_radius, col_state, scratch, out, w, h);
}

/* YUV → linear RGB (ADR-0163). 4-wide aarch64 SVE2 mirror of the NEON
 * port. Per-lane scalar reads + per-lane scalar `srgb_eotf` keep
 * byte-exact parity with both the scalar and NEON outputs. */

static inline float read_plane_scalar_s2_sve2(const simd_plane_t *p, unsigned lw, unsigned lh,
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

static inline svfloat32_t srgb_to_linear_lane_sve2(svbool_t pg, svfloat32_t v)
{
    alignas(16) float tmp[4] = {0.f, 0.f, 0.f, 0.f};
    svst1_f32(pg, tmp, v);
    for (int k = 0; k < 4; k++) {
        const float x = tmp[k];
        tmp[k] = vmaf_ss2_srgb_eotf(x);
    }
    return svld1_f32(pg, tmp);
}

static inline void compute_matrix_coefs_sve2(int yuv_matrix, float *kr_out, float *kg_out,
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
} ptlr_coefs_sve2;

typedef struct {
    const simd_plane_t *planes;
    unsigned w;
    unsigned h;
    unsigned bpc;
    float *rp;
    float *gp;
    float *bp;
} ptlr_ctx_sve2;

static ptlr_coefs_sve2 ptlr_coefs_init_sve2(int yuv_matrix, unsigned bpc)
{
    const float peak = (float)((1u << bpc) - 1u);
    float kr;
    float kg;
    float kb;
    int limited;
    compute_matrix_coefs_sve2(yuv_matrix, &kr, &kg, &kb, &limited);

    ptlr_coefs_sve2 c;
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
static void ptlr_block_sve2(svbool_t pg, const ptlr_coefs_sve2 *c, const ptlr_ctx_sve2 *ctx,
                            unsigned x, unsigned y)
{
    alignas(16) float y_tmp[4] = {0.f, 0.f, 0.f, 0.f};
    alignas(16) float u_tmp[4] = {0.f, 0.f, 0.f, 0.f};
    alignas(16) float v_tmp[4] = {0.f, 0.f, 0.f, 0.f};
    for (int i = 0; i < 4; i++) {
        const int px = (int)(x + (unsigned)i);
        y_tmp[i] = read_plane_scalar_s2_sve2(&ctx->planes[0], ctx->w, ctx->h, px, (int)y, ctx->bpc);
        u_tmp[i] = read_plane_scalar_s2_sve2(&ctx->planes[1], ctx->w, ctx->h, px, (int)y, ctx->bpc);
        v_tmp[i] = read_plane_scalar_s2_sve2(&ctx->planes[2], ctx->w, ctx->h, px, (int)y, ctx->bpc);
    }
    const svfloat32_t vinv_peak = svdup_f32(c->inv_peak);
    const svfloat32_t vc_off = svdup_f32(c->c_off);
    const svfloat32_t vc_scale = svdup_f32(c->c_scale);
    const svfloat32_t vzero = svdup_f32(0.0f);
    const svfloat32_t vone = svdup_f32(1.0f);
    const svfloat32_t Y = svmul_f32_x(pg, svld1_f32(pg, y_tmp), vinv_peak);
    const svfloat32_t U = svmul_f32_x(pg, svld1_f32(pg, u_tmp), vinv_peak);
    const svfloat32_t V = svmul_f32_x(pg, svld1_f32(pg, v_tmp), vinv_peak);
    const svfloat32_t Yn =
        svmul_f32_x(pg, svsub_f32_x(pg, Y, svdup_f32(c->y_off)), svdup_f32(c->y_scale));
    const svfloat32_t Un = svmul_f32_x(pg, svsub_f32_x(pg, U, vc_off), vc_scale);
    const svfloat32_t Vn = svmul_f32_x(pg, svsub_f32_x(pg, V, vc_off), vc_scale);
    /* ADR-0891: svmla_f32_x — single-rounding FMA matches fmaf() in scalar ref. */
    svfloat32_t R = svmla_f32_x(pg, Yn, svdup_f32(c->cr_r), Vn);
    svfloat32_t G = svmla_f32_x(pg, Yn, svdup_f32(c->cb_g), Un);
    G = svmla_f32_x(pg, G, svdup_f32(c->cr_g), Vn);
    svfloat32_t B = svmla_f32_x(pg, Yn, svdup_f32(c->cb_b), Un);
    R = svmax_f32_x(pg, svmin_f32_x(pg, R, vone), vzero);
    G = svmax_f32_x(pg, svmin_f32_x(pg, G, vone), vzero);
    B = svmax_f32_x(pg, svmin_f32_x(pg, B, vone), vzero);
    R = srgb_to_linear_lane_sve2(pg, R);
    G = srgb_to_linear_lane_sve2(pg, G);
    B = srgb_to_linear_lane_sve2(pg, B);
    const size_t idx = (size_t)y * ctx->w + x;
    svst1_f32(pg, ctx->rp + idx, R);
    svst1_f32(pg, ctx->gp + idx, G);
    svst1_f32(pg, ctx->bp + idx, B);
}

static void ptlr_pixel_sve2(const ptlr_coefs_sve2 *c, const ptlr_ctx_sve2 *ctx, unsigned x,
                            unsigned y)
{
    const unsigned w = ctx->w;
    const unsigned h = ctx->h;
    const float Ys =
        read_plane_scalar_s2_sve2(&ctx->planes[0], w, h, (int)x, (int)y, ctx->bpc) * c->inv_peak;
    const float Us =
        read_plane_scalar_s2_sve2(&ctx->planes[1], w, h, (int)x, (int)y, ctx->bpc) * c->inv_peak;
    const float Vs =
        read_plane_scalar_s2_sve2(&ctx->planes[2], w, h, (int)x, (int)y, ctx->bpc) * c->inv_peak;
    const float Yn = (Ys - c->y_off) * c->y_scale;
    const float Un = (Us - c->c_off) * c->c_scale;
    const float Vn = (Vs - c->c_off) * c->c_scale;
    /* ADR-0891: fmaf() matches svmla_f32_x single-rounding contract. */
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

void ssimulacra2_picture_to_linear_rgb_sve2(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
                                            const simd_plane_t planes[3], float *out)
{
    assert(planes != NULL);
    assert(out != NULL);
    assert(w > 0 && h > 0);

    const size_t plane_sz = (size_t)w * (size_t)h;
    const ptlr_ctx_sve2 ctx = {planes, w, h, bpc, out, out + plane_sz, out + 2 * plane_sz};
    const ptlr_coefs_sve2 coefs = ptlr_coefs_init_sve2(yuv_matrix, bpc);

    const svbool_t pg = pg4();

    for (unsigned y = 0; y < h; y++) {
        unsigned x = 0;
        for (; x + 4 <= w; x += 4) {
            ptlr_block_sve2(pg, &coefs, &ctx, x, y);
        }
        for (; x < w; x++) {
            ptlr_pixel_sve2(&coefs, &ctx, x, y);
        }
    }
}
