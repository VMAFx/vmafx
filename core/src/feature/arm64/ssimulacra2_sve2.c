/**
 *
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
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

/* ADR-0141 carve-out: lock-step mirror of the NEON XYB loop; splitting
 * mid-iteration would duplicate the per-channel matrix-coefficient
 * setup. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
void ssimulacra2_linear_rgb_to_xyb_sve2(const float *lin, float *xyb, unsigned w, unsigned h)
{
    assert(lin != NULL);
    assert(xyb != NULL);
    assert(w > 0 && h > 0);
    const size_t plane_sz = (size_t)w * (size_t)h;
    const float *rp = lin;
    const float *gp = lin + plane_sz;
    const float *bp = lin + 2 * plane_sz;
    float *xp = xyb;
    float *yp = xyb + plane_sz;
    float *bxp = xyb + 2 * plane_sz;

    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);

    const svbool_t pg = pg4();
    const svfloat32_t vm00 = svdup_f32(kM00);
    const svfloat32_t vm01 = svdup_f32(m01);
    const svfloat32_t vm02 = svdup_f32(kM02);
    const svfloat32_t vm10 = svdup_f32(kM10);
    const svfloat32_t vm11 = svdup_f32(m11);
    const svfloat32_t vm12 = svdup_f32(kM12);
    const svfloat32_t vm20 = svdup_f32(kM20);
    const svfloat32_t vm21 = svdup_f32(kM21);
    const svfloat32_t vm22 = svdup_f32(m22);
    const svfloat32_t vbias = svdup_f32(kOpsinBias);
    const svfloat32_t vzero = svdup_f32(0.0f);
    const svfloat32_t vcbrt_bias = svdup_f32(cbrt_bias);
    const svfloat32_t vhalf = svdup_f32(0.5f);
    const svfloat32_t v14 = svdup_f32(14.0f);
    const svfloat32_t v42 = svdup_f32(0.42f);
    const svfloat32_t v55 = svdup_f32(0.55f);
    const svfloat32_t v01 = svdup_f32(0.01f);

    size_t i = 0;
    for (; i + 4 <= plane_sz; i += 4) {
        const svfloat32_t r = svld1_f32(pg, rp + i);
        const svfloat32_t g = svld1_f32(pg, gp + i);
        const svfloat32_t b = svld1_f32(pg, bp + i);
        svfloat32_t l = svadd_f32_x(pg, svmul_f32_x(pg, vm00, r), svmul_f32_x(pg, vm01, g));
        l = svadd_f32_x(pg, l, svmul_f32_x(pg, vm02, b));
        l = svadd_f32_x(pg, l, vbias);
        svfloat32_t m = svadd_f32_x(pg, svmul_f32_x(pg, vm10, r), svmul_f32_x(pg, vm11, g));
        m = svadd_f32_x(pg, m, svmul_f32_x(pg, vm12, b));
        m = svadd_f32_x(pg, m, vbias);
        svfloat32_t sv = svadd_f32_x(pg, svmul_f32_x(pg, vm20, r), svmul_f32_x(pg, vm21, g));
        sv = svadd_f32_x(pg, sv, svmul_f32_x(pg, vm22, b));
        sv = svadd_f32_x(pg, sv, vbias);
        l = svmax_f32_x(pg, l, vzero);
        m = svmax_f32_x(pg, m, vzero);
        sv = svmax_f32_x(pg, sv, vzero);
        const svfloat32_t L = svsub_f32_x(pg, cbrtf_lane_sve2(pg, l), vcbrt_bias);
        const svfloat32_t M = svsub_f32_x(pg, cbrtf_lane_sve2(pg, m), vcbrt_bias);
        const svfloat32_t S = svsub_f32_x(pg, cbrtf_lane_sve2(pg, sv), vcbrt_bias);
        const svfloat32_t X = svmul_f32_x(pg, vhalf, svsub_f32_x(pg, L, M));
        const svfloat32_t Y = svmul_f32_x(pg, vhalf, svadd_f32_x(pg, L, M));
        const svfloat32_t B = S;
        const svfloat32_t Bfinal = svadd_f32_x(pg, svsub_f32_x(pg, B, Y), v55);
        const svfloat32_t Xfinal = svadd_f32_x(pg, svmul_f32_x(pg, X, v14), v42);
        const svfloat32_t Yfinal = svadd_f32_x(pg, Y, v01);
        svst1_f32(pg, xp + i, Xfinal);
        svst1_f32(pg, yp + i, Yfinal);
        svst1_f32(pg, bxp + i, Bfinal);
    }

    for (; i < plane_sz; i++) {
        float r = rp[i];
        float g = gp[i];
        float bb = bp[i];
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
        xp[i] = X;
        yp[i] = Y;
        bxp[i] = B;
    }
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

// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
void ssimulacra2_ssim_map_sve2(const float *m1, const float *m2, const float *s11, const float *s22,
                               const float *s12, unsigned w, unsigned h, double plane_averages[6])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    const svbool_t pg = pg4();
    const svfloat32_t vc2 = svdup_f32(kC2);
    const svfloat32_t vone = svdup_f32(1.0f);
    const svfloat32_t vtwo = svdup_f32(2.0f);

    for (int c = 0; c < 3; c++) {
        double sum_l1 = 0.0;
        double sum_l4 = 0.0;
        const float *rm1 = m1 + (size_t)c * plane;
        const float *rm2 = m2 + (size_t)c * plane;
        const float *rs11 = s11 + (size_t)c * plane;
        const float *rs22 = s22 + (size_t)c * plane;
        const float *rs12 = s12 + (size_t)c * plane;

        size_t i = 0;
        for (; i + 4 <= plane; i += 4) {
            const svfloat32_t mu1 = svld1_f32(pg, rm1 + i);
            const svfloat32_t mu2 = svld1_f32(pg, rm2 + i);
            const svfloat32_t mu11 = svmul_f32_x(pg, mu1, mu1);
            const svfloat32_t mu22 = svmul_f32_x(pg, mu2, mu2);
            const svfloat32_t mu12 = svmul_f32_x(pg, mu1, mu2);
            const svfloat32_t diff = svsub_f32_x(pg, mu1, mu2);
            const svfloat32_t num_m = svsub_f32_x(pg, vone, svmul_f32_x(pg, diff, diff));
            const svfloat32_t num_s = svadd_f32_x(
                pg, svmul_f32_x(pg, vtwo, svsub_f32_x(pg, svld1_f32(pg, rs12 + i), mu12)), vc2);
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
                double d = 1.0 - ((double)num_m_f[k] * (double)num_s_f[k] / (double)denom_s_f[k]);
                if (d < 0.0)
                    d = 0.0;
                sum_l1 += d;
                sum_l4 += quartic_d(d);
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
            double d = 1.0 - ((double)num_m * (double)num_s / (double)denom_s);
            if (d < 0.0)
                d = 0.0;
            sum_l1 += d;
            sum_l4 += quartic_d(d);
        }
        plane_averages[c * 2 + 0] = one_per_pixels * sum_l1;
        plane_averages[c * 2 + 1] = sqrt(sqrt(one_per_pixels * sum_l4));
    }
}

void ssimulacra2_edge_diff_map_sve2(const float *img1, const float *mu1, const float *img2,
                                    const float *mu2, unsigned w, unsigned h,
                                    double plane_averages[12])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    const svbool_t pg = pg4();

    for (int c = 0; c < 3; c++) {
        double s0 = 0.0;
        double s1 = 0.0;
        double s2 = 0.0;
        double s3 = 0.0;
        const float *r1 = img1 + (size_t)c * plane;
        const float *rm1 = mu1 + (size_t)c * plane;
        const float *r2 = img2 + (size_t)c * plane;
        const float *rm2 = mu2 + (size_t)c * plane;

        size_t i = 0;
        for (; i + 4 <= plane; i += 4) {
            const svfloat32_t a1 = svld1_f32(pg, r1 + i);
            const svfloat32_t a2 = svld1_f32(pg, r2 + i);
            const svfloat32_t am1 = svld1_f32(pg, rm1 + i);
            const svfloat32_t am2 = svld1_f32(pg, rm2 + i);
            /* ADR-1208: the reference difference is taken in DOUBLE.
             * `a` and `am` are floats, so `(double)a - (double)am` is exact,
             * whereas subtracting in float rounds first. The scalar
             * `edge_diff_map`, this function's own scalar tail, and the test's
             * reference all promote before subtracting; vectorising the
             * subtract in float made the ssimulacra2 score depend on whether
             * the host had SIMD. The per-lane loop below is scalar anyway, so
             * nothing is lost by folding the subtraction into it. */
            alignas(16) float a1f[4] = {0.f, 0.f, 0.f, 0.f};
            alignas(16) float am1f[4] = {0.f, 0.f, 0.f, 0.f};
            alignas(16) float a2f[4] = {0.f, 0.f, 0.f, 0.f};
            alignas(16) float am2f[4] = {0.f, 0.f, 0.f, 0.f};
            svst1_f32(pg, a1f, a1);
            svst1_f32(pg, am1f, am1);
            svst1_f32(pg, a2f, a2);
            svst1_f32(pg, am2f, am2);
            for (int k = 0; k < 4; k++) {
                double ed1 = fabs((double)a1f[k] - (double)am1f[k]);
                double ed2 = fabs((double)a2f[k] - (double)am2f[k]);
                double d = (1.0 + ed2) / (1.0 + ed1) - 1.0;
                double art = d > 0.0 ? d : 0.0;
                double det = d < 0.0 ? -d : 0.0;
                s0 += art;
                s1 += quartic_d(art);
                s2 += det;
                s3 += quartic_d(det);
            }
        }
        for (; i < plane; i++) {
            double ed1 = fabs((double)r1[i] - (double)rm1[i]);
            double ed2 = fabs((double)r2[i] - (double)rm2[i]);
            double d = (1.0 + ed2) / (1.0 + ed1) - 1.0;
            double art = d > 0.0 ? d : 0.0;
            double det = d < 0.0 ? -d : 0.0;
            s0 += art;
            s1 += quartic_d(art);
            s2 += det;
            s3 += quartic_d(det);
        }
        plane_averages[c * 4 + 0] = one_per_pixels * s0;
        plane_averages[c * 4 + 1] = sqrt(sqrt(one_per_pixels * s1));
        plane_averages[c * 4 + 2] = one_per_pixels * s2;
        plane_averages[c * 4 + 3] = sqrt(sqrt(one_per_pixels * s3));
    }
}

/* ADR-0141 carve-out: gather via lane-loads + 3-pole IIR + scalar-store
 * — IIR state and per-row gather share locality; splitting forces a
 * memory round-trip that defeats the vectorisation. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
static void hblur_4rows_sve2(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                             const float *in, float *out, unsigned w, unsigned y_base,
                             unsigned row_count)
{
    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t W = (ptrdiff_t)w;
    const svbool_t pg = pg4();

    const svfloat32_t vn2_0 = svdup_f32(rg_n2[0]);
    const svfloat32_t vn2_1 = svdup_f32(rg_n2[1]);
    const svfloat32_t vn2_2 = svdup_f32(rg_n2[2]);
    const svfloat32_t vd1_0 = svdup_f32(rg_d1[0]);
    const svfloat32_t vd1_1 = svdup_f32(rg_d1[1]);
    const svfloat32_t vd1_2 = svdup_f32(rg_d1[2]);

    svfloat32_t prev1_0 = svdup_f32(0.f);
    svfloat32_t prev1_1 = svdup_f32(0.f);
    svfloat32_t prev1_2 = svdup_f32(0.f);
    svfloat32_t prev2_0 = svdup_f32(0.f);
    svfloat32_t prev2_1 = svdup_f32(0.f);
    svfloat32_t prev2_2 = svdup_f32(0.f);

    alignas(16) float store_tmp[4] = {0.f, 0.f, 0.f, 0.f};
    alignas(16) float lane_tmp[4] = {0.f, 0.f, 0.f, 0.f};

    /* Per-lane row base pointers — SVE2 has gather but the byte-exact
     * contract pins us to NEON's lane-by-lane assemble pattern. */
    const float *row_bases[4] = {NULL, NULL, NULL, NULL};
    for (unsigned i = 0; i < row_count && i < 4; i++) {
        row_bases[i] = in + ((size_t)y_base + i) * w;
    }

    for (ptrdiff_t n = -N + 1; n < W; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        svfloat32_t lv = svdup_f32(0.f);
        svfloat32_t rv = svdup_f32(0.f);
        if (left >= 0) {
            for (unsigned i = 0; i < 4; i++) {
                lane_tmp[i] = (i < row_count) ? row_bases[i][left] : 0.f;
            }
            lv = svld1_f32(pg, lane_tmp);
        }
        if (right < W) {
            for (unsigned i = 0; i < 4; i++) {
                lane_tmp[i] = (i < row_count) ? row_bases[i][right] : 0.f;
            }
            rv = svld1_f32(pg, lane_tmp);
        }
        const svfloat32_t sum = svadd_f32_x(pg, lv, rv);

        svfloat32_t o0 =
            svsub_f32_x(pg, svmul_f32_x(pg, vn2_0, sum), svmul_f32_x(pg, vd1_0, prev1_0));
        o0 = svsub_f32_x(pg, o0, prev2_0);
        svfloat32_t o1 =
            svsub_f32_x(pg, svmul_f32_x(pg, vn2_1, sum), svmul_f32_x(pg, vd1_1, prev1_1));
        o1 = svsub_f32_x(pg, o1, prev2_1);
        svfloat32_t o2 =
            svsub_f32_x(pg, svmul_f32_x(pg, vn2_2, sum), svmul_f32_x(pg, vd1_2, prev1_2));
        o2 = svsub_f32_x(pg, o2, prev2_2);

        prev2_0 = prev1_0;
        prev2_1 = prev1_1;
        prev2_2 = prev1_2;
        prev1_0 = o0;
        prev1_1 = o1;
        prev1_2 = o2;

        if (n >= 0) {
            const svfloat32_t res = svadd_f32_x(pg, svadd_f32_x(pg, o0, o1), o2);
            svst1_f32(pg, store_tmp, res);
            for (unsigned i = 0; i < row_count; i++) {
                out[((size_t)y_base + i) * w + (size_t)n] = store_tmp[i];
            }
        }
    }
}

/* ADR-0141 carve-out: SIMD main loop + scalar tail share IIR state. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
static void vblur_simd_4cols_sve2(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                  float *col_state, const float *in, float *out, unsigned w,
                                  unsigned h)
{
    const size_t xsize = (size_t)w;
    float *prev1_0 = col_state + 0u * xsize;
    float *prev1_1 = col_state + 1u * xsize;
    float *prev1_2 = col_state + 2u * xsize;
    float *prev2_0 = col_state + 3u * xsize;
    float *prev2_1 = col_state + 4u * xsize;
    float *prev2_2 = col_state + 5u * xsize;
    memset(col_state, 0, 6u * xsize * sizeof(float));

    const svbool_t pg = pg4();
    const svfloat32_t vn2_0 = svdup_f32(rg_n2[0]);
    const svfloat32_t vn2_1 = svdup_f32(rg_n2[1]);
    const svfloat32_t vn2_2 = svdup_f32(rg_n2[2]);
    const svfloat32_t vd1_0 = svdup_f32(rg_d1[0]);
    const svfloat32_t vd1_1 = svdup_f32(rg_d1[1]);
    const svfloat32_t vd1_2 = svdup_f32(rg_d1[2]);

    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t ysize = (ptrdiff_t)h;

    for (ptrdiff_t n = -N + 1; n < ysize; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float *lrow = (left >= 0) ? (in + (size_t)left * xsize) : NULL;
        const float *rrow = (right < ysize) ? (in + (size_t)right * xsize) : NULL;
        float *orow = (n >= 0) ? (out + (size_t)n * xsize) : NULL;

        size_t x = 0;
        for (; x + 4 <= xsize; x += 4) {
            const svfloat32_t lv = lrow ? svld1_f32(pg, lrow + x) : svdup_f32(0.f);
            const svfloat32_t rv = rrow ? svld1_f32(pg, rrow + x) : svdup_f32(0.f);
            const svfloat32_t sum = svadd_f32_x(pg, lv, rv);
            const svfloat32_t p1_0 = svld1_f32(pg, prev1_0 + x);
            const svfloat32_t p1_1 = svld1_f32(pg, prev1_1 + x);
            const svfloat32_t p1_2 = svld1_f32(pg, prev1_2 + x);
            const svfloat32_t p2_0 = svld1_f32(pg, prev2_0 + x);
            const svfloat32_t p2_1 = svld1_f32(pg, prev2_1 + x);
            const svfloat32_t p2_2 = svld1_f32(pg, prev2_2 + x);
            svfloat32_t o0 =
                svsub_f32_x(pg, svmul_f32_x(pg, vn2_0, sum), svmul_f32_x(pg, vd1_0, p1_0));
            o0 = svsub_f32_x(pg, o0, p2_0);
            svfloat32_t o1 =
                svsub_f32_x(pg, svmul_f32_x(pg, vn2_1, sum), svmul_f32_x(pg, vd1_1, p1_1));
            o1 = svsub_f32_x(pg, o1, p2_1);
            svfloat32_t o2 =
                svsub_f32_x(pg, svmul_f32_x(pg, vn2_2, sum), svmul_f32_x(pg, vd1_2, p1_2));
            o2 = svsub_f32_x(pg, o2, p2_2);
            svst1_f32(pg, prev2_0 + x, p1_0);
            svst1_f32(pg, prev2_1 + x, p1_1);
            svst1_f32(pg, prev2_2 + x, p1_2);
            svst1_f32(pg, prev1_0 + x, o0);
            svst1_f32(pg, prev1_1 + x, o1);
            svst1_f32(pg, prev1_2 + x, o2);
            if (orow) {
                const svfloat32_t res = svadd_f32_x(pg, svadd_f32_x(pg, o0, o1), o2);
                svst1_f32(pg, orow + x, res);
            }
        }
        for (; x < xsize; x++) {
            const float lv = lrow ? lrow[x] : 0.f;
            const float rv = rrow ? rrow[x] : 0.f;
            const float sum = lv + rv;
            const float o0 = rg_n2[0] * sum - rg_d1[0] * prev1_0[x] - prev2_0[x];
            const float o1 = rg_n2[1] * sum - rg_d1[1] * prev1_1[x] - prev2_1[x];
            const float o2 = rg_n2[2] * sum - rg_d1[2] * prev1_2[x] - prev2_2[x];
            prev2_0[x] = prev1_0[x];
            prev2_1[x] = prev1_1[x];
            prev2_2[x] = prev1_2[x];
            prev1_0[x] = o0;
            prev1_1[x] = o1;
            prev1_2[x] = o2;
            if (orow) {
                orow[x] = o0 + o1 + o2;
            }
        }
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

// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
void ssimulacra2_picture_to_linear_rgb_sve2(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
                                            const simd_plane_t planes[3], float *out)
{
    assert(planes != NULL);
    assert(out != NULL);
    assert(w > 0 && h > 0);

    const size_t plane_sz = (size_t)w * (size_t)h;
    float *rp = out;
    float *gp = out + plane_sz;
    float *bp = out + 2 * plane_sz;

    const float peak = (float)((1u << bpc) - 1u);
    const float inv_peak = 1.0f / peak;

    float kr;
    float kg;
    float kb;
    int limited;
    compute_matrix_coefs_sve2(yuv_matrix, &kr, &kg, &kb, &limited);

    const float cr_r = 2.0f * (1.0f - kr);
    const float cb_b = 2.0f * (1.0f - kb);
    const float cb_g = -(2.0f * kb * (1.0f - kb)) / kg;
    const float cr_g = -(2.0f * kr * (1.0f - kr)) / kg;
    const float y_scale = limited ? (255.0f / 219.0f) : 1.0f;
    const float c_scale = limited ? (255.0f / 224.0f) : 1.0f;
    const float y_off = limited ? (16.0f / 255.0f) : 0.0f;
    const float c_off = 0.5f;

    const svbool_t pg = pg4();
    const svfloat32_t vinv_peak = svdup_f32(inv_peak);
    const svfloat32_t vy_scale = svdup_f32(y_scale);
    const svfloat32_t vc_scale = svdup_f32(c_scale);
    const svfloat32_t vy_off = svdup_f32(y_off);
    const svfloat32_t vc_off = svdup_f32(c_off);
    const svfloat32_t vcr_r = svdup_f32(cr_r);
    const svfloat32_t vcb_b = svdup_f32(cb_b);
    const svfloat32_t vcb_g = svdup_f32(cb_g);
    const svfloat32_t vcr_g = svdup_f32(cr_g);
    const svfloat32_t vzero = svdup_f32(0.0f);
    const svfloat32_t vone = svdup_f32(1.0f);

    alignas(16) float y_tmp[4] = {0.f, 0.f, 0.f, 0.f};
    alignas(16) float u_tmp[4] = {0.f, 0.f, 0.f, 0.f};
    alignas(16) float v_tmp[4] = {0.f, 0.f, 0.f, 0.f};

    for (unsigned y = 0; y < h; y++) {
        unsigned x = 0;
        for (; x + 4 <= w; x += 4) {
            for (int i = 0; i < 4; i++) {
                y_tmp[i] = read_plane_scalar_s2_sve2(&planes[0], w, h, (int)(x + (unsigned)i),
                                                     (int)y, bpc);
                u_tmp[i] = read_plane_scalar_s2_sve2(&planes[1], w, h, (int)(x + (unsigned)i),
                                                     (int)y, bpc);
                v_tmp[i] = read_plane_scalar_s2_sve2(&planes[2], w, h, (int)(x + (unsigned)i),
                                                     (int)y, bpc);
            }
            const svfloat32_t Y = svmul_f32_x(pg, svld1_f32(pg, y_tmp), vinv_peak);
            const svfloat32_t U = svmul_f32_x(pg, svld1_f32(pg, u_tmp), vinv_peak);
            const svfloat32_t V = svmul_f32_x(pg, svld1_f32(pg, v_tmp), vinv_peak);
            const svfloat32_t Yn = svmul_f32_x(pg, svsub_f32_x(pg, Y, vy_off), vy_scale);
            const svfloat32_t Un = svmul_f32_x(pg, svsub_f32_x(pg, U, vc_off), vc_scale);
            const svfloat32_t Vn = svmul_f32_x(pg, svsub_f32_x(pg, V, vc_off), vc_scale);
            /* ADR-0891: svmla_f32_x — single-rounding FMA matches fmaf() in scalar ref. */
            svfloat32_t R = svmla_f32_x(pg, Yn, vcr_r, Vn);
            svfloat32_t G = svmla_f32_x(pg, Yn, vcb_g, Un);
            G = svmla_f32_x(pg, G, vcr_g, Vn);
            svfloat32_t B = svmla_f32_x(pg, Yn, vcb_b, Un);
            R = svmax_f32_x(pg, svmin_f32_x(pg, R, vone), vzero);
            G = svmax_f32_x(pg, svmin_f32_x(pg, G, vone), vzero);
            B = svmax_f32_x(pg, svmin_f32_x(pg, B, vone), vzero);
            R = srgb_to_linear_lane_sve2(pg, R);
            G = srgb_to_linear_lane_sve2(pg, G);
            B = srgb_to_linear_lane_sve2(pg, B);
            svst1_f32(pg, rp + (size_t)y * w + x, R);
            svst1_f32(pg, gp + (size_t)y * w + x, G);
            svst1_f32(pg, bp + (size_t)y * w + x, B);
        }
        for (; x < w; x++) {
            const float Ys =
                read_plane_scalar_s2_sve2(&planes[0], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float Us =
                read_plane_scalar_s2_sve2(&planes[1], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float Vs =
                read_plane_scalar_s2_sve2(&planes[2], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float Yn = (Ys - y_off) * y_scale;
            const float Un = (Us - c_off) * c_scale;
            const float Vn = (Vs - c_off) * c_scale;
            /* ADR-0891: fmaf() matches svmla_f32_x single-rounding contract. */
            float R = fmaf(cr_r, Vn, Yn);
            float G = fmaf(cb_g, Un, Yn);
            G = fmaf(cr_g, Vn, G);
            float B = fmaf(cb_b, Un, Yn);
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
            rp[idx] = Rl;
            gp[idx] = Gl;
            bp[idx] = Bl;
        }
    }
}
