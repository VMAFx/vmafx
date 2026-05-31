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
 * AVX-512 port of the SSIMULACRA 2 SIMD kernels. Mechanical 16-wide
 * widening of the AVX2 sister TU (x86/ssimulacra2_avx2.c, ADR-0161).
 * Same bit-exactness contract: byte-for-byte identical output to the
 * scalar reference in feature/ssimulacra2.c under FLT_EVAL_METHOD == 0.
 *
 * The shape of the ops is identical — only the lane count changes.
 * IEEE-754 lane-commutative adds/muls + per-lane scalar libm for
 * `cbrtf` preserve the summation tree byte-for-byte.
 *
 * Downsample_2x2's deinterleave uses AVX-512's `vpermt2ps` with
 * explicit index vectors, providing cleaner cross-lane rearrangement
 * than the AVX2 `vshufps + vpermpd` chain.
 */

#include <assert.h>
#include <immintrin.h>
#include <math.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "feature/ssimulacra2_math.h"
#include "feature/ssimulacra2_simd_common.h"
#include "ssimulacra2_avx512.h"

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

static inline __m512 cbrtf_lane_avx512(__m512 v)
{
    alignas(64) float tmp[16];
    _mm512_store_ps(tmp, v);
    for (int k = 0; k < 16; k++) {
        tmp[k] = vmaf_ss2_cbrtf(tmp[k]);
    }
    return _mm512_load_ps(tmp);
}

static inline double quartic_d(double x)
{
    x *= x;
    return x * x;
}

void ssimulacra2_multiply_3plane_avx512(const float *a, const float *b, float *mul, unsigned w,
                                        unsigned h)
{
    const size_t n = 3u * (size_t)w * (size_t)h;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m512 va = _mm512_loadu_ps(a + i);
        const __m512 vb = _mm512_loadu_ps(b + i);
        _mm512_storeu_ps(mul + i, _mm512_mul_ps(va, vb));
    }
    for (; i < n; i++) {
        mul[i] = a[i] * b[i];
    }
}

// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
void ssimulacra2_linear_rgb_to_xyb_avx512(const float *lin, float *xyb, unsigned w, unsigned h)
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

    const __m512 vm00 = _mm512_set1_ps(kM00);
    const __m512 vm01 = _mm512_set1_ps(m01);
    const __m512 vm02 = _mm512_set1_ps(kM02);
    const __m512 vm10 = _mm512_set1_ps(kM10);
    const __m512 vm11 = _mm512_set1_ps(m11);
    const __m512 vm12 = _mm512_set1_ps(kM12);
    const __m512 vm20 = _mm512_set1_ps(kM20);
    const __m512 vm21 = _mm512_set1_ps(kM21);
    const __m512 vm22 = _mm512_set1_ps(m22);
    const __m512 vbias = _mm512_set1_ps(kOpsinBias);
    const __m512 vzero = _mm512_setzero_ps();
    const __m512 vcbrt_bias = _mm512_set1_ps(cbrt_bias);
    const __m512 vhalf = _mm512_set1_ps(0.5f);
    const __m512 v14 = _mm512_set1_ps(14.0f);
    const __m512 v42 = _mm512_set1_ps(0.42f);
    const __m512 v55 = _mm512_set1_ps(0.55f);
    const __m512 v01 = _mm512_set1_ps(0.01f);

    size_t i = 0;
    for (; i + 16 <= plane_sz; i += 16) {
        const __m512 r = _mm512_loadu_ps(rp + i);
        const __m512 g = _mm512_loadu_ps(gp + i);
        const __m512 b = _mm512_loadu_ps(bp + i);
        __m512 l = _mm512_add_ps(_mm512_mul_ps(vm00, r), _mm512_mul_ps(vm01, g));
        l = _mm512_add_ps(l, _mm512_mul_ps(vm02, b));
        l = _mm512_add_ps(l, vbias);
        __m512 m = _mm512_add_ps(_mm512_mul_ps(vm10, r), _mm512_mul_ps(vm11, g));
        m = _mm512_add_ps(m, _mm512_mul_ps(vm12, b));
        m = _mm512_add_ps(m, vbias);
        __m512 sv = _mm512_add_ps(_mm512_mul_ps(vm20, r), _mm512_mul_ps(vm21, g));
        sv = _mm512_add_ps(sv, _mm512_mul_ps(vm22, b));
        sv = _mm512_add_ps(sv, vbias);
        l = _mm512_max_ps(l, vzero);
        m = _mm512_max_ps(m, vzero);
        sv = _mm512_max_ps(sv, vzero);
        const __m512 L = _mm512_sub_ps(cbrtf_lane_avx512(l), vcbrt_bias);
        const __m512 M = _mm512_sub_ps(cbrtf_lane_avx512(m), vcbrt_bias);
        const __m512 S = _mm512_sub_ps(cbrtf_lane_avx512(sv), vcbrt_bias);
        const __m512 X = _mm512_mul_ps(vhalf, _mm512_sub_ps(L, M));
        const __m512 Y = _mm512_mul_ps(vhalf, _mm512_add_ps(L, M));
        const __m512 B = S;
        const __m512 Bfinal = _mm512_add_ps(_mm512_sub_ps(B, Y), v55);
        const __m512 Xfinal = _mm512_add_ps(_mm512_mul_ps(X, v14), v42);
        const __m512 Yfinal = _mm512_add_ps(Y, v01);
        _mm512_storeu_ps(xp + i, Xfinal);
        _mm512_storeu_ps(yp + i, Yfinal);
        _mm512_storeu_ps(bxp + i, Bfinal);
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

void ssimulacra2_downsample_2x2_avx512(const float *in, unsigned iw, unsigned ih, float *out,
                                       unsigned *ow_out, unsigned *oh_out)
{
    const unsigned ow = (iw + 1) / 2;
    const unsigned oh = (ih + 1) / 2;
    *ow_out = ow;
    *oh_out = oh;

    const size_t in_plane = (size_t)iw * (size_t)ih;
    const size_t out_plane = (size_t)ow * (size_t)oh;

    /* Index vectors for `vpermt2ps` even-/odd-lane deinterleave across
     * two __m512 source vectors (32 consecutive floats). */
    const __m512i idx_even =
        _mm512_set_epi32(30, 28, 26, 24, 22, 20, 18, 16, 14, 12, 10, 8, 6, 4, 2, 0);
    const __m512i idx_odd =
        _mm512_set_epi32(31, 29, 27, 25, 23, 21, 19, 17, 15, 13, 11, 9, 7, 5, 3, 1);
    const __m512 vquarter = _mm512_set1_ps(0.25f);

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
            const unsigned interior_end = (ow > 0 && iw >= 2) ? ((ow - 1) / 16) * 16 : 0;
            for (; ox < interior_end; ox += 16) {
                const size_t base = (size_t)ox * 2u;
                const __m512 r00 = _mm512_loadu_ps(row0 + base);
                const __m512 r01 = _mm512_loadu_ps(row0 + base + 16);
                const __m512 r10 = _mm512_loadu_ps(row1 + base);
                const __m512 r11 = _mm512_loadu_ps(row1 + base + 16);
                const __m512 r0e = _mm512_permutex2var_ps(r00, idx_even, r01);
                const __m512 r0o = _mm512_permutex2var_ps(r00, idx_odd, r01);
                const __m512 r1e = _mm512_permutex2var_ps(r10, idx_even, r11);
                const __m512 r1o = _mm512_permutex2var_ps(r10, idx_odd, r11);
                __m512 acc = _mm512_add_ps(r0e, r0o);
                acc = _mm512_add_ps(acc, r1e);
                acc = _mm512_add_ps(acc, r1o);
                _mm512_storeu_ps(orow + ox, _mm512_mul_ps(acc, vquarter));
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
void ssimulacra2_ssim_map_avx512(const float *m1, const float *m2, const float *s11,
                                 const float *s22, const float *s12, unsigned w, unsigned h,
                                 double plane_averages[6])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    const __m512 vc2 = _mm512_set1_ps(kC2);
    const __m512 vone = _mm512_set1_ps(1.0f);
    const __m512 vtwo = _mm512_set1_ps(2.0f);

    for (int c = 0; c < 3; c++) {
        double sum_l1 = 0.0;
        double sum_l4 = 0.0;
        const float *rm1 = m1 + (size_t)c * plane;
        const float *rm2 = m2 + (size_t)c * plane;
        const float *rs11 = s11 + (size_t)c * plane;
        const float *rs22 = s22 + (size_t)c * plane;
        const float *rs12 = s12 + (size_t)c * plane;

        size_t i = 0;
        for (; i + 16 <= plane; i += 16) {
            const __m512 mu1 = _mm512_loadu_ps(rm1 + i);
            const __m512 mu2 = _mm512_loadu_ps(rm2 + i);
            const __m512 mu11 = _mm512_mul_ps(mu1, mu1);
            const __m512 mu22 = _mm512_mul_ps(mu2, mu2);
            const __m512 mu12 = _mm512_mul_ps(mu1, mu2);
            const __m512 diff = _mm512_sub_ps(mu1, mu2);
            const __m512 num_m = _mm512_sub_ps(vone, _mm512_mul_ps(diff, diff));
            const __m512 num_s = _mm512_add_ps(
                _mm512_mul_ps(vtwo, _mm512_sub_ps(_mm512_loadu_ps(rs12 + i), mu12)), vc2);
            const __m512 denom_s =
                _mm512_add_ps(_mm512_add_ps(_mm512_sub_ps(_mm512_loadu_ps(rs11 + i), mu11),
                                            _mm512_sub_ps(_mm512_loadu_ps(rs22 + i), mu22)),
                              vc2);
            alignas(64) float num_m_f[16];
            alignas(64) float num_s_f[16];
            alignas(64) float denom_s_f[16];
            _mm512_store_ps(num_m_f, num_m);
            _mm512_store_ps(num_s_f, num_s);
            _mm512_store_ps(denom_s_f, denom_s);
            for (int k = 0; k < 16; k++) {
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

void ssimulacra2_edge_diff_map_avx512(const float *img1, const float *mu1, const float *img2,
                                      const float *mu2, unsigned w, unsigned h,
                                      double plane_averages[12])
{
    const size_t plane = (size_t)w * (size_t)h;
    const double one_per_pixels = 1.0 / (double)plane;
    const __m512 vsignmask = _mm512_castsi512_ps(_mm512_set1_epi32(0x7FFFFFFF));

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
        for (; i + 16 <= plane; i += 16) {
            const __m512 a1 = _mm512_loadu_ps(r1 + i);
            const __m512 a2 = _mm512_loadu_ps(r2 + i);
            const __m512 am1 = _mm512_loadu_ps(rm1 + i);
            const __m512 am2 = _mm512_loadu_ps(rm2 + i);
            const __m512 d1 = _mm512_and_ps(vsignmask, _mm512_sub_ps(a1, am1));
            const __m512 d2 = _mm512_and_ps(vsignmask, _mm512_sub_ps(a2, am2));
            alignas(64) float d1f[16];
            alignas(64) float d2f[16];
            _mm512_store_ps(d1f, d1);
            _mm512_store_ps(d2f, d2);
            for (int k = 0; k < 16; k++) {
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

/* ADR-0141 carve-out: gather loads + 3-pole IIR + scalar-store scatter. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
static void hblur_16rows_avx512(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                const float *in, float *out, unsigned w, unsigned y_base,
                                unsigned row_count)
{
    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t W = (ptrdiff_t)w;

    int32_t idx_tmp[16];
    for (int i = 0; i < 16; i++) {
        idx_tmp[i] = (i < (int)row_count) ? (int32_t)(((size_t)y_base + (size_t)i) * w) : 0;
    }
    const __m512i vindices = _mm512_loadu_si512((const __m512i *)idx_tmp);

    const __m512 vn2_0 = _mm512_set1_ps(rg_n2[0]);
    const __m512 vn2_1 = _mm512_set1_ps(rg_n2[1]);
    const __m512 vn2_2 = _mm512_set1_ps(rg_n2[2]);
    const __m512 vd1_0 = _mm512_set1_ps(rg_d1[0]);
    const __m512 vd1_1 = _mm512_set1_ps(rg_d1[1]);
    const __m512 vd1_2 = _mm512_set1_ps(rg_d1[2]);

    __m512 prev1_0 = _mm512_setzero_ps();
    __m512 prev1_1 = _mm512_setzero_ps();
    __m512 prev1_2 = _mm512_setzero_ps();
    __m512 prev2_0 = _mm512_setzero_ps();
    __m512 prev2_1 = _mm512_setzero_ps();
    __m512 prev2_2 = _mm512_setzero_ps();

    alignas(64) float store_tmp[16];

    for (ptrdiff_t n = -N + 1; n < W; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const __m512 lv = (left >= 0) ? _mm512_i32gather_ps(vindices, in + left, sizeof(float)) :
                                        _mm512_setzero_ps();
        const __m512 rv = (right < W) ? _mm512_i32gather_ps(vindices, in + right, sizeof(float)) :
                                        _mm512_setzero_ps();
        const __m512 sum = _mm512_add_ps(lv, rv);

        __m512 o0 = _mm512_sub_ps(_mm512_mul_ps(vn2_0, sum), _mm512_mul_ps(vd1_0, prev1_0));
        o0 = _mm512_sub_ps(o0, prev2_0);
        __m512 o1 = _mm512_sub_ps(_mm512_mul_ps(vn2_1, sum), _mm512_mul_ps(vd1_1, prev1_1));
        o1 = _mm512_sub_ps(o1, prev2_1);
        __m512 o2 = _mm512_sub_ps(_mm512_mul_ps(vn2_2, sum), _mm512_mul_ps(vd1_2, prev1_2));
        o2 = _mm512_sub_ps(o2, prev2_2);

        prev2_0 = prev1_0;
        prev2_1 = prev1_1;
        prev2_2 = prev1_2;
        prev1_0 = o0;
        prev1_1 = o1;
        prev1_2 = o2;

        if (n >= 0) {
            const __m512 res = _mm512_add_ps(_mm512_add_ps(o0, o1), o2);
            _mm512_store_ps(store_tmp, res);
            for (unsigned i = 0; i < row_count; i++) {
                out[((size_t)y_base + i) * w + (size_t)n] = store_tmp[i];
            }
        }
    }
}

/* ADR-0141 carve-out: SIMD main loop + scalar tail share IIR state. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
static void vblur_simd_16cols_avx512(const float rg_n2[3], const float rg_d1[3], int rg_radius,
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

    const __m512 vn2_0 = _mm512_set1_ps(rg_n2[0]);
    const __m512 vn2_1 = _mm512_set1_ps(rg_n2[1]);
    const __m512 vn2_2 = _mm512_set1_ps(rg_n2[2]);
    const __m512 vd1_0 = _mm512_set1_ps(rg_d1[0]);
    const __m512 vd1_1 = _mm512_set1_ps(rg_d1[1]);
    const __m512 vd1_2 = _mm512_set1_ps(rg_d1[2]);

    const ptrdiff_t N = (ptrdiff_t)rg_radius;
    const ptrdiff_t ysize = (ptrdiff_t)h;

    for (ptrdiff_t n = -N + 1; n < ysize; n++) {
        const ptrdiff_t left = n - N - 1;
        const ptrdiff_t right = n + N - 1;
        const float *lrow = (left >= 0) ? (in + (size_t)left * xsize) : NULL;
        const float *rrow = (right < ysize) ? (in + (size_t)right * xsize) : NULL;
        float *orow = (n >= 0) ? (out + (size_t)n * xsize) : NULL;

        size_t x = 0;
        for (; x + 16 <= xsize; x += 16) {
            const __m512 lv = lrow ? _mm512_loadu_ps(lrow + x) : _mm512_setzero_ps();
            const __m512 rv = rrow ? _mm512_loadu_ps(rrow + x) : _mm512_setzero_ps();
            const __m512 sum = _mm512_add_ps(lv, rv);
            const __m512 p1_0 = _mm512_loadu_ps(prev1_0 + x);
            const __m512 p1_1 = _mm512_loadu_ps(prev1_1 + x);
            const __m512 p1_2 = _mm512_loadu_ps(prev1_2 + x);
            const __m512 p2_0 = _mm512_loadu_ps(prev2_0 + x);
            const __m512 p2_1 = _mm512_loadu_ps(prev2_1 + x);
            const __m512 p2_2 = _mm512_loadu_ps(prev2_2 + x);
            __m512 o0 = _mm512_sub_ps(_mm512_mul_ps(vn2_0, sum), _mm512_mul_ps(vd1_0, p1_0));
            o0 = _mm512_sub_ps(o0, p2_0);
            __m512 o1 = _mm512_sub_ps(_mm512_mul_ps(vn2_1, sum), _mm512_mul_ps(vd1_1, p1_1));
            o1 = _mm512_sub_ps(o1, p2_1);
            __m512 o2 = _mm512_sub_ps(_mm512_mul_ps(vn2_2, sum), _mm512_mul_ps(vd1_2, p1_2));
            o2 = _mm512_sub_ps(o2, p2_2);
            _mm512_storeu_ps(prev2_0 + x, p1_0);
            _mm512_storeu_ps(prev2_1 + x, p1_1);
            _mm512_storeu_ps(prev2_2 + x, p1_2);
            _mm512_storeu_ps(prev1_0 + x, o0);
            _mm512_storeu_ps(prev1_1 + x, o1);
            _mm512_storeu_ps(prev1_2 + x, o2);
            if (orow) {
                const __m512 res = _mm512_add_ps(_mm512_add_ps(o0, o1), o2);
                _mm512_storeu_ps(orow + x, res);
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

void ssimulacra2_blur_plane_avx512(const float rg_n2[3], const float rg_d1[3], int rg_radius,
                                   float *col_state, const float *in, float *out, float *scratch,
                                   unsigned w, unsigned h)
{
    assert(col_state != NULL);
    assert(in != NULL);
    assert(out != NULL);
    assert(scratch != NULL);
    assert(w > 0 && h > 0);

    unsigned y = 0;
    for (; y + 16 <= h; y += 16) {
        hblur_16rows_avx512(rg_n2, rg_d1, rg_radius, in, scratch, w, y, 16);
    }
    if (y < h) {
        hblur_16rows_avx512(rg_n2, rg_d1, rg_radius, in, scratch, w, y, h - y);
    }
    vblur_simd_16cols_avx512(rg_n2, rg_d1, rg_radius, col_state, scratch, out, w, h);
}

/* YUV → linear RGB (ADR-0163). 16-wide widening of the AVX2 path in
 * ssimulacra2_avx2.c. Per-lane scalar pixel reads + per-lane scalar
 * sRGB EOTF; the matmul / normalise / clamp block is true SIMD. */

static inline float read_plane_scalar_s2_av512(const simd_plane_t *p, unsigned lw, unsigned lh,
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

static inline __m512 srgb_to_linear_lane_avx512(__m512 v)
{
    alignas(64) float tmp[16];
    _mm512_store_ps(tmp, v);
    for (int k = 0; k < 16; k++) {
        const float x = tmp[k];
        tmp[k] = vmaf_ss2_srgb_eotf(x);
    }
    return _mm512_load_ps(tmp);
}

static inline void compute_matrix_coefs_avx512(int yuv_matrix, float *kr_out, float *kg_out,
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

/**
 * AVX-512 (16-wide) port of the SSIMULACRA2 YUV-to-linear-RGB conversion.
 *
 * Selected by `Ssimu2State::ptlr_fn` in `ssimulacra2.c` when the runtime
 * detects AVX-512F; see `init()` line ~946 of that file. Behavioural and
 * numerical contract is identical to the AVX2 reference implementation
 * declared in `ssimulacra2_avx2.h` — see ADR-0163 for the YUV→linear-RGB
 * pipeline definition.
 *
 * Why this kernel is one large function: bit-exactness vs the scalar
 * reference is enforced by ADR-0138/ADR-0139. Splitting the body into
 * helpers would perturb register allocation and the chroma-reconstruction
 * reduction order, which has been observed to introduce ULP drift in
 * `/cross-backend-diff` runs. The NOLINT below is the load-bearing
 * suppression mandated by ADR-0141.
 *
 * Caller contract: `planes` must hold three `simd_plane_t` planes laid
 * out in source picture order (Y, U, V) of size `w * h`. `out` must
 * point to a buffer of `3 * w * h` floats, written as three planar
 * R/G/B planes in [0, 1] linear-light. `bpc` is bits per component
 * (8 / 10 / 12); peak / scaling derive from it. `yuv_matrix` selects
 * BT.601/709/2020 coefficients via `compute_matrix_coefs_avx512`.
 */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — bit-exactness invariant: splitting would perturb register allocation + reduction order vs scalar (ADR-0138/0139, ADR-0141)
void ssimulacra2_picture_to_linear_rgb_avx512(int yuv_matrix, unsigned bpc, unsigned w, unsigned h,
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
    compute_matrix_coefs_avx512(yuv_matrix, &kr, &kg, &kb, &limited);

    const float cr_r = 2.0f * (1.0f - kr);
    const float cb_b = 2.0f * (1.0f - kb);
    const float cb_g = -(2.0f * kb * (1.0f - kb)) / kg;
    const float cr_g = -(2.0f * kr * (1.0f - kr)) / kg;
    const float y_scale = limited ? (255.0f / 219.0f) : 1.0f;
    const float c_scale = limited ? (255.0f / 224.0f) : 1.0f;
    const float y_off = limited ? (16.0f / 255.0f) : 0.0f;
    const float c_off = 0.5f;

    const __m512 vinv_peak = _mm512_set1_ps(inv_peak);
    const __m512 vy_scale = _mm512_set1_ps(y_scale);
    const __m512 vc_scale = _mm512_set1_ps(c_scale);
    const __m512 vy_off = _mm512_set1_ps(y_off);
    const __m512 vc_off = _mm512_set1_ps(c_off);
    const __m512 vcr_r = _mm512_set1_ps(cr_r);
    const __m512 vcb_b = _mm512_set1_ps(cb_b);
    const __m512 vcb_g = _mm512_set1_ps(cb_g);
    const __m512 vcr_g = _mm512_set1_ps(cr_g);
    const __m512 vzero = _mm512_setzero_ps();
    const __m512 vone = _mm512_set1_ps(1.0f);

    alignas(64) float y_tmp[16];
    alignas(64) float u_tmp[16];
    alignas(64) float v_tmp[16];

    for (unsigned y = 0; y < h; y++) {
        unsigned x = 0;
        for (; x + 16 <= w; x += 16) {
            for (int i = 0; i < 16; i++) {
                y_tmp[i] = read_plane_scalar_s2_av512(&planes[0], w, h, (int)(x + (unsigned)i),
                                                      (int)y, bpc);
                u_tmp[i] = read_plane_scalar_s2_av512(&planes[1], w, h, (int)(x + (unsigned)i),
                                                      (int)y, bpc);
                v_tmp[i] = read_plane_scalar_s2_av512(&planes[2], w, h, (int)(x + (unsigned)i),
                                                      (int)y, bpc);
            }
            const __m512 Y = _mm512_mul_ps(_mm512_load_ps(y_tmp), vinv_peak);
            const __m512 U = _mm512_mul_ps(_mm512_load_ps(u_tmp), vinv_peak);
            const __m512 V = _mm512_mul_ps(_mm512_load_ps(v_tmp), vinv_peak);
            const __m512 Yn = _mm512_mul_ps(_mm512_sub_ps(Y, vy_off), vy_scale);
            const __m512 Un = _mm512_mul_ps(_mm512_sub_ps(U, vc_off), vc_scale);
            const __m512 Vn = _mm512_mul_ps(_mm512_sub_ps(V, vc_off), vc_scale);
            /* ADR-0891 round-2 fix: explicit FMA intrinsics — see
             * sibling AVX2 TU for rationale (icx auto-fuses the
             * separate mul+add pattern under `-fp-model=precise`,
             * gcc does not; unifying on FMA via fmaf() in the
             * scalar reference is the bit-exact pairing). */
            __m512 R = _mm512_fmadd_ps(vcr_r, Vn, Yn);
            __m512 G = _mm512_fmadd_ps(vcb_g, Un, Yn);
            G = _mm512_fmadd_ps(vcr_g, Vn, G);
            __m512 B = _mm512_fmadd_ps(vcb_b, Un, Yn);
            R = _mm512_max_ps(_mm512_min_ps(R, vone), vzero);
            G = _mm512_max_ps(_mm512_min_ps(G, vone), vzero);
            B = _mm512_max_ps(_mm512_min_ps(B, vone), vzero);
            R = srgb_to_linear_lane_avx512(R);
            G = srgb_to_linear_lane_avx512(G);
            B = srgb_to_linear_lane_avx512(B);
            _mm512_storeu_ps(rp + (size_t)y * w + x, R);
            _mm512_storeu_ps(gp + (size_t)y * w + x, G);
            _mm512_storeu_ps(bp + (size_t)y * w + x, B);
        }
        for (; x < w; x++) {
            const float Ys =
                read_plane_scalar_s2_av512(&planes[0], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float Us =
                read_plane_scalar_s2_av512(&planes[1], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float Vs =
                read_plane_scalar_s2_av512(&planes[2], w, h, (int)x, (int)y, bpc) * inv_peak;
            const float Yn = (Ys - y_off) * y_scale;
            const float Un = (Us - c_off) * c_scale;
            const float Vn = (Vs - c_off) * c_scale;
            /* ADR-0891: explicit fmaf() — icx + `-mfma` may contract
             * plain `a + b*c` to FMA even under `-fp-model=precise`,
             * diverging from the SIMD reference. Preserves the
             * left-to-right associativity of the G computation. */
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
