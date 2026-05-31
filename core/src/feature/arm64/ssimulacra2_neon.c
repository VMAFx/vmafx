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

// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
void ssimulacra2_linear_rgb_to_xyb_neon(const float *lin, float *xyb, unsigned w, unsigned h)
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

    const float32x4_t vm00 = vdupq_n_f32(kM00);
    const float32x4_t vm01 = vdupq_n_f32(m01);
    const float32x4_t vm02 = vdupq_n_f32(kM02);
    const float32x4_t vm10 = vdupq_n_f32(kM10);
    const float32x4_t vm11 = vdupq_n_f32(m11);
    const float32x4_t vm12 = vdupq_n_f32(kM12);
    const float32x4_t vm20 = vdupq_n_f32(kM20);
    const float32x4_t vm21 = vdupq_n_f32(kM21);
    const float32x4_t vm22 = vdupq_n_f32(m22);
    const float32x4_t vbias = vdupq_n_f32(kOpsinBias);
    const float32x4_t vzero = vdupq_n_f32(0.0f);
    const float32x4_t vcbrt_bias = vdupq_n_f32(cbrt_bias);
    const float32x4_t vhalf = vdupq_n_f32(0.5f);
    const float32x4_t v14 = vdupq_n_f32(14.0f);
    const float32x4_t v42 = vdupq_n_f32(0.42f);
    const float32x4_t v55 = vdupq_n_f32(0.55f);
    const float32x4_t v01 = vdupq_n_f32(0.01f);

    size_t i = 0;
    for (; i + 4 <= plane_sz; i += 4) {
        const float32x4_t r = vld1q_f32(rp + i);
        const float32x4_t g = vld1q_f32(gp + i);
        const float32x4_t b = vld1q_f32(bp + i);
        float32x4_t l = vaddq_f32(vmulq_f32(vm00, r), vmulq_f32(vm01, g));
        l = vaddq_f32(l, vmulq_f32(vm02, b));
        l = vaddq_f32(l, vbias);
        float32x4_t m = vaddq_f32(vmulq_f32(vm10, r), vmulq_f32(vm11, g));
        m = vaddq_f32(m, vmulq_f32(vm12, b));
        m = vaddq_f32(m, vbias);
        float32x4_t sv = vaddq_f32(vmulq_f32(vm20, r), vmulq_f32(vm21, g));
        sv = vaddq_f32(sv, vmulq_f32(vm22, b));
        sv = vaddq_f32(sv, vbias);
        l = vmaxq_f32(l, vzero);
        m = vmaxq_f32(m, vzero);
        sv = vmaxq_f32(sv, vzero);
        const float32x4_t L = vsubq_f32(cbrtf_lane_neon(l), vcbrt_bias);
        const float32x4_t M = vsubq_f32(cbrtf_lane_neon(m), vcbrt_bias);
        const float32x4_t S = vsubq_f32(cbrtf_lane_neon(sv), vcbrt_bias);
        const float32x4_t X = vmulq_f32(vhalf, vsubq_f32(L, M));
        const float32x4_t Y = vmulq_f32(vhalf, vaddq_f32(L, M));
        const float32x4_t B = S;
        const float32x4_t Bfinal = vaddq_f32(vsubq_f32(B, Y), v55);
        const float32x4_t Xfinal = vaddq_f32(vmulq_f32(X, v14), v42);
        const float32x4_t Yfinal = vaddq_f32(Y, v01);
        vst1q_f32(xp + i, Xfinal);
        vst1q_f32(yp + i, Yfinal);
        vst1q_f32(bxp + i, Bfinal);
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

// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
void ssimulacra2_ssim_map_neon(const float *m1, const float *m2, const float *s11, const float *s22,
                               const float *s12, unsigned w, unsigned h, double plane_averages[6])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    const float32x4_t vc2 = vdupq_n_f32(kC2);
    const float32x4_t vone = vdupq_n_f32(1.0f);
    const float32x4_t vtwo = vdupq_n_f32(2.0f);

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

void ssimulacra2_edge_diff_map_neon(const float *img1, const float *mu1, const float *img2,
                                    const float *mu2, unsigned w, unsigned h,
                                    double plane_averages[12])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;

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
            const float32x4_t a1 = vld1q_f32(r1 + i);
            const float32x4_t a2 = vld1q_f32(r2 + i);
            const float32x4_t am1 = vld1q_f32(rm1 + i);
            const float32x4_t am2 = vld1q_f32(rm2 + i);
            const float32x4_t d1 = vabsq_f32(vsubq_f32(a1, am1));
            const float32x4_t d2 = vabsq_f32(vsubq_f32(a2, am2));
            alignas(16) float d1f[4];
            alignas(16) float d2f[4];
            vst1q_f32(d1f, d1);
            vst1q_f32(d2f, d2);
            for (int k = 0; k < 4; k++) {
                double ed1 = (double)d1f[k];
                double ed2 = (double)d2f[k];
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

/* ADR-0141 carve-out: gather via lane-loads + 3-pole IIR + scalar-store. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
static void hblur_4rows_neon(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                             const float *in, float *out, unsigned w, unsigned y_base,
                             unsigned row_count)
{
    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t W = (ptrdiff_t)w;

    const float32x4_t vn2_0 = vdupq_n_f32(rg_n2[0]);
    const float32x4_t vn2_1 = vdupq_n_f32(rg_n2[1]);
    const float32x4_t vn2_2 = vdupq_n_f32(rg_n2[2]);
    const float32x4_t vd1_0 = vdupq_n_f32(rg_d1[0]);
    const float32x4_t vd1_1 = vdupq_n_f32(rg_d1[1]);
    const float32x4_t vd1_2 = vdupq_n_f32(rg_d1[2]);

    float32x4_t prev1_0 = vdupq_n_f32(0.f);
    float32x4_t prev1_1 = vdupq_n_f32(0.f);
    float32x4_t prev1_2 = vdupq_n_f32(0.f);
    float32x4_t prev2_0 = vdupq_n_f32(0.f);
    float32x4_t prev2_1 = vdupq_n_f32(0.f);
    float32x4_t prev2_2 = vdupq_n_f32(0.f);

    alignas(16) float store_tmp[4];

    /* Per-lane row base pointer addresses. NEON has no gather so we
     * assemble the load lane-by-lane. */
    const float *row_bases[4] = {NULL, NULL, NULL, NULL};
    for (unsigned i = 0; i < row_count && i < 4; i++) {
        row_bases[i] = in + ((size_t)y_base + i) * w;
    }

    for (ptrdiff_t n = -N + 1; n < W; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        float32x4_t lv = vdupq_n_f32(0.f);
        float32x4_t rv = vdupq_n_f32(0.f);
        if (left >= 0) {
            if (row_count > 0)
                lv = vsetq_lane_f32(row_bases[0][left], lv, 0);
            if (row_count > 1)
                lv = vsetq_lane_f32(row_bases[1][left], lv, 1);
            if (row_count > 2)
                lv = vsetq_lane_f32(row_bases[2][left], lv, 2);
            if (row_count > 3)
                lv = vsetq_lane_f32(row_bases[3][left], lv, 3);
        }
        if (right < W) {
            if (row_count > 0)
                rv = vsetq_lane_f32(row_bases[0][right], rv, 0);
            if (row_count > 1)
                rv = vsetq_lane_f32(row_bases[1][right], rv, 1);
            if (row_count > 2)
                rv = vsetq_lane_f32(row_bases[2][right], rv, 2);
            if (row_count > 3)
                rv = vsetq_lane_f32(row_bases[3][right], rv, 3);
        }
        const float32x4_t sum = vaddq_f32(lv, rv);

        float32x4_t o0 = vsubq_f32(vmulq_f32(vn2_0, sum), vmulq_f32(vd1_0, prev1_0));
        o0 = vsubq_f32(o0, prev2_0);
        float32x4_t o1 = vsubq_f32(vmulq_f32(vn2_1, sum), vmulq_f32(vd1_1, prev1_1));
        o1 = vsubq_f32(o1, prev2_1);
        float32x4_t o2 = vsubq_f32(vmulq_f32(vn2_2, sum), vmulq_f32(vd1_2, prev1_2));
        o2 = vsubq_f32(o2, prev2_2);

        prev2_0 = prev1_0;
        prev2_1 = prev1_1;
        prev2_2 = prev1_2;
        prev1_0 = o0;
        prev1_1 = o1;
        prev1_2 = o2;

        if (n >= 0) {
            const float32x4_t res = vaddq_f32(vaddq_f32(o0, o1), o2);
            vst1q_f32(store_tmp, res);
            for (unsigned i = 0; i < row_count; i++) {
                out[((size_t)y_base + i) * w + (size_t)n] = store_tmp[i];
            }
        }
    }
}

/* ADR-0141 carve-out: SIMD main loop + scalar tail share IIR state. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
static void vblur_simd_4cols_neon(const float rg_n2[3], const float rg_d1[3], int rg_radius,
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

    const float32x4_t vn2_0 = vdupq_n_f32(rg_n2[0]);
    const float32x4_t vn2_1 = vdupq_n_f32(rg_n2[1]);
    const float32x4_t vn2_2 = vdupq_n_f32(rg_n2[2]);
    const float32x4_t vd1_0 = vdupq_n_f32(rg_d1[0]);
    const float32x4_t vd1_1 = vdupq_n_f32(rg_d1[1]);
    const float32x4_t vd1_2 = vdupq_n_f32(rg_d1[2]);

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
            const float32x4_t lv = lrow ? vld1q_f32(lrow + x) : vdupq_n_f32(0.f);
            const float32x4_t rv = rrow ? vld1q_f32(rrow + x) : vdupq_n_f32(0.f);
            const float32x4_t sum = vaddq_f32(lv, rv);
            const float32x4_t p1_0 = vld1q_f32(prev1_0 + x);
            const float32x4_t p1_1 = vld1q_f32(prev1_1 + x);
            const float32x4_t p1_2 = vld1q_f32(prev1_2 + x);
            const float32x4_t p2_0 = vld1q_f32(prev2_0 + x);
            const float32x4_t p2_1 = vld1q_f32(prev2_1 + x);
            const float32x4_t p2_2 = vld1q_f32(prev2_2 + x);
            float32x4_t o0 = vsubq_f32(vmulq_f32(vn2_0, sum), vmulq_f32(vd1_0, p1_0));
            o0 = vsubq_f32(o0, p2_0);
            float32x4_t o1 = vsubq_f32(vmulq_f32(vn2_1, sum), vmulq_f32(vd1_1, p1_1));
            o1 = vsubq_f32(o1, p2_1);
            float32x4_t o2 = vsubq_f32(vmulq_f32(vn2_2, sum), vmulq_f32(vd1_2, p1_2));
            o2 = vsubq_f32(o2, p2_2);
            vst1q_f32(prev2_0 + x, p1_0);
            vst1q_f32(prev2_1 + x, p1_1);
            vst1q_f32(prev2_2 + x, p1_2);
            vst1q_f32(prev1_0 + x, o0);
            vst1q_f32(prev1_1 + x, o1);
            vst1q_f32(prev1_2 + x, o2);
            if (orow) {
                const float32x4_t res = vaddq_f32(vaddq_f32(o0, o1), o2);
                vst1q_f32(orow + x, res);
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

// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
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

    const float peak = (float)((1u << bpc) - 1u);
    const float inv_peak = 1.0f / peak;

    float kr;
    float kg;
    float kb;
    int limited;
    compute_matrix_coefs_neon(yuv_matrix, &kr, &kg, &kb, &limited);

    const float cr_r = 2.0f * (1.0f - kr);
    const float cb_b = 2.0f * (1.0f - kb);
    const float cb_g = -(2.0f * kb * (1.0f - kb)) / kg;
    const float cr_g = -(2.0f * kr * (1.0f - kr)) / kg;
    const float y_scale = limited ? (255.0f / 219.0f) : 1.0f;
    const float c_scale = limited ? (255.0f / 224.0f) : 1.0f;
    const float y_off = limited ? (16.0f / 255.0f) : 0.0f;
    const float c_off = 0.5f;

    const float32x4_t vinv_peak = vdupq_n_f32(inv_peak);
    const float32x4_t vy_scale = vdupq_n_f32(y_scale);
    const float32x4_t vc_scale = vdupq_n_f32(c_scale);
    const float32x4_t vy_off = vdupq_n_f32(y_off);
    const float32x4_t vc_off = vdupq_n_f32(c_off);
    const float32x4_t vcr_r = vdupq_n_f32(cr_r);
    const float32x4_t vcb_b = vdupq_n_f32(cb_b);
    const float32x4_t vcb_g = vdupq_n_f32(cb_g);
    const float32x4_t vcr_g = vdupq_n_f32(cr_g);
    const float32x4_t vzero = vdupq_n_f32(0.0f);
    const float32x4_t vone = vdupq_n_f32(1.0f);

    alignas(16) float y_tmp[4];
    alignas(16) float u_tmp[4];
    alignas(16) float v_tmp[4];

    for (unsigned y = 0; y < h; y++) {
        unsigned x = 0;
        for (; x + 4 <= w; x += 4) {
            for (int i = 0; i < 4; i++) {
                y_tmp[i] = read_plane_scalar_s2_neon(&planes[0], w, h, (int)(x + (unsigned)i),
                                                     (int)y, bpc);
                u_tmp[i] = read_plane_scalar_s2_neon(&planes[1], w, h, (int)(x + (unsigned)i),
                                                     (int)y, bpc);
                v_tmp[i] = read_plane_scalar_s2_neon(&planes[2], w, h, (int)(x + (unsigned)i),
                                                     (int)y, bpc);
            }
            const float32x4_t Y = vmulq_f32(vld1q_f32(y_tmp), vinv_peak);
            const float32x4_t U = vmulq_f32(vld1q_f32(u_tmp), vinv_peak);
            const float32x4_t V = vmulq_f32(vld1q_f32(v_tmp), vinv_peak);
            const float32x4_t Yn = vmulq_f32(vsubq_f32(Y, vy_off), vy_scale);
            const float32x4_t Un = vmulq_f32(vsubq_f32(U, vc_off), vc_scale);
            const float32x4_t Vn = vmulq_f32(vsubq_f32(V, vc_off), vc_scale);
            /* ADR-0891: vfmaq_f32 — single-rounding FMA matches fmaf() in scalar ref. */
            float32x4_t R = vfmaq_f32(Yn, vcr_r, Vn);
            float32x4_t G = vfmaq_f32(Yn, vcb_g, Un);
            G = vfmaq_f32(G, vcr_g, Vn);
            float32x4_t B = vfmaq_f32(Yn, vcb_b, Un);
            R = vmaxq_f32(vminq_f32(R, vone), vzero);
            G = vmaxq_f32(vminq_f32(G, vone), vzero);
            B = vmaxq_f32(vminq_f32(B, vone), vzero);
            R = srgb_to_linear_lane_neon(R);
            G = srgb_to_linear_lane_neon(G);
            B = srgb_to_linear_lane_neon(B);
            vst1q_f32(rp + (size_t)y * w + x, R);
            vst1q_f32(gp + (size_t)y * w + x, G);
            vst1q_f32(bp + (size_t)y * w + x, B);
        }
        for (; x < w; x++) {
            const float Ys =
                read_plane_scalar_s2_neon(&planes[0], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float Us =
                read_plane_scalar_s2_neon(&planes[1], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float Vs =
                read_plane_scalar_s2_neon(&planes[2], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float Yn = (Ys - y_off) * y_scale;
            const float Un = (Us - c_off) * c_scale;
            const float Vn = (Vs - c_off) * c_scale;
            /* ADR-0891: fmaf() matches vfmaq_f32 single-rounding contract. */
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
