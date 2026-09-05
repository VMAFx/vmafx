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

/* ADR-0252 carve-out: matmul + per-lane cbrtf + XYB rescale stay together
 * for line-for-line diff against the host scalar reference in
 * ss2v_host_linear_rgb_to_xyb.  Splitting would break the bit-exact audit. */
// NOLINTNEXTLINE(readability-function-size,google-readability-function-size) — ADR-0141 §2 /
// ADR-0252 / ADR-0161 / ADR-0278: host XYB kernel kept whole for the bit-exact audit.
void ssimulacra2_host_linear_rgb_to_xyb_avx2(const float *lin, float *xyb, unsigned w, unsigned h,
                                             size_t plane_stride)
{
    assert(lin != NULL);
    assert(xyb != NULL);
    assert(w > 0 && h > 0);
    assert(plane_stride >= (size_t)w * (size_t)h);

    const float *rp = lin;
    const float *gp = lin + plane_stride;
    const float *bp = lin + 2u * plane_stride;
    float *xp = xyb;
    float *yp = xyb + plane_stride;
    float *bxp = xyb + 2u * plane_stride;

    const float m01 = 1.0f - kM00 - kM02;
    const float m11 = 1.0f - kM10 - kM12;
    const float m22 = 1.0f - kM20 - kM21;
    const float cbrt_bias = vmaf_ss2_cbrtf(kOpsinBias);

    const __m256 vm00 = _mm256_set1_ps(kM00);
    const __m256 vm01 = _mm256_set1_ps(m01);
    const __m256 vm02 = _mm256_set1_ps(kM02);
    const __m256 vm10 = _mm256_set1_ps(kM10);
    const __m256 vm11 = _mm256_set1_ps(m11);
    const __m256 vm12 = _mm256_set1_ps(kM12);
    const __m256 vm20 = _mm256_set1_ps(kM20);
    const __m256 vm21 = _mm256_set1_ps(kM21);
    const __m256 vm22 = _mm256_set1_ps(m22);
    const __m256 vbias = _mm256_set1_ps(kOpsinBias);
    const __m256 vzero = _mm256_setzero_ps();
    const __m256 vcbrt_bias = _mm256_set1_ps(cbrt_bias);

    const size_t scale_pixels = (size_t)w * (size_t)h;
    size_t i = 0;

    for (; i + 8 <= scale_pixels; i += 8) {
        const __m256 r = _mm256_loadu_ps(rp + i);
        const __m256 g = _mm256_loadu_ps(gp + i);
        const __m256 b = _mm256_loadu_ps(bp + i);
        /* LMS mixing: left-to-right addition order matches scalar reference
         * ss2v_host_linear_rgb_to_xyb exactly — IEEE-754 add is
         * non-associative and test_ssimulacra2_simd catches drift. */
        __m256 l = _mm256_add_ps(_mm256_mul_ps(vm00, r), _mm256_mul_ps(vm01, g));
        l = _mm256_add_ps(l, _mm256_mul_ps(vm02, b));
        l = _mm256_add_ps(l, vbias);
        __m256 m = _mm256_add_ps(_mm256_mul_ps(vm10, r), _mm256_mul_ps(vm11, g));
        m = _mm256_add_ps(m, _mm256_mul_ps(vm12, b));
        m = _mm256_add_ps(m, vbias);
        __m256 sv = _mm256_add_ps(_mm256_mul_ps(vm20, r), _mm256_mul_ps(vm21, g));
        sv = _mm256_add_ps(sv, _mm256_mul_ps(vm22, b));
        sv = _mm256_add_ps(sv, vbias);
        l = _mm256_max_ps(l, vzero);
        m = _mm256_max_ps(m, vzero);
        sv = _mm256_max_ps(sv, vzero);

        const __m256 L = _mm256_sub_ps(cbrtf_lane8(l), vcbrt_bias);
        const __m256 M = _mm256_sub_ps(cbrtf_lane8(m), vcbrt_bias);
        const __m256 S = _mm256_sub_ps(cbrtf_lane8(sv), vcbrt_bias);

        const __m256 X = _mm256_mul_ps(_mm256_set1_ps(0.5f), _mm256_sub_ps(L, M));
        const __m256 Y = _mm256_mul_ps(_mm256_set1_ps(0.5f), _mm256_add_ps(L, M));
        /* MakePositiveXYB rescale — order matches scalar:
         *   B = (S - Y) + 0.55f;   X = X * 14.0f + 0.42f;   Y = Y + 0.01f */
        const __m256 Bfinal = _mm256_add_ps(_mm256_sub_ps(S, Y), _mm256_set1_ps(0.55f));
        const __m256 Xfinal =
            _mm256_add_ps(_mm256_mul_ps(X, _mm256_set1_ps(14.0f)), _mm256_set1_ps(0.42f));
        const __m256 Yfinal = _mm256_add_ps(Y, _mm256_set1_ps(0.01f));

        _mm256_storeu_ps(xp + i, Xfinal);
        _mm256_storeu_ps(yp + i, Yfinal);
        _mm256_storeu_ps(bxp + i, Bfinal);
    }

    /* Scalar tail — bit-identical to ss2v_host_linear_rgb_to_xyb body. */
    for (; i < scale_pixels; i++) {
        float r = rp[i];
        float g = gp[i];
        float bb = bp[i];
        float lv = kM00 * r + m01 * g + kM02 * bb + kOpsinBias;
        float mv = kM10 * r + m11 * g + kM12 * bb + kOpsinBias;
        float sv = kM20 * r + kM21 * g + m22 * bb + kOpsinBias;
        if (lv < 0.0f)
            lv = 0.0f;
        if (mv < 0.0f)
            mv = 0.0f;
        if (sv < 0.0f)
            sv = 0.0f;
        float L = vmaf_ss2_cbrtf(lv) - cbrt_bias;
        float M = vmaf_ss2_cbrtf(mv) - cbrt_bias;
        float S = vmaf_ss2_cbrtf(sv) - cbrt_bias;
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

void ssimulacra2_host_downsample_2x2_avx2(const float *in, unsigned iw, unsigned ih, float *out,
                                          unsigned ow, unsigned oh, size_t plane_stride)
{
    assert(in != NULL);
    assert(out != NULL);
    assert(iw > 0 && ih > 0);
    assert(plane_stride >= (size_t)iw * (size_t)ih);

    const __m256 vquarter = _mm256_set1_ps(0.25f);

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
            /* SIMD interior: 8 output lanes at a time.
             * Deinterleave even/odd pairs and add sequentially to
             * preserve scalar left-to-right summation order
             * `((r0e + r0o) + r1e) + r1o`. Lane crossing from
             * `_mm256_shuffle_ps` is corrected with `_mm256_permute4x64_pd`
             * — the same pattern as ssimulacra2_downsample_2x2_avx2. */
            const unsigned interior_end = (ow > 0u && iw >= 2u) ? (((ow - 1u) / 8u) * 8u) : 0u;
            for (; ox < interior_end; ox += 8) {
                const size_t base = (size_t)ox * 2u;
                const __m256 r00 = _mm256_loadu_ps(row0 + base);
                const __m256 r01 = _mm256_loadu_ps(row0 + base + 8);
                const __m256 r10 = _mm256_loadu_ps(row1 + base);
                const __m256 r11 = _mm256_loadu_ps(row1 + base + 8);
                /* Deinterleave even / odd positions. */
                const __m256 r0e_raw = _mm256_shuffle_ps(r00, r01, 0x88);
                const __m256 r0o_raw = _mm256_shuffle_ps(r00, r01, 0xDD);
                const __m256 r1e_raw = _mm256_shuffle_ps(r10, r11, 0x88);
                const __m256 r1o_raw = _mm256_shuffle_ps(r10, r11, 0xDD);
                /* Fix 128-bit lane crossing: permute so lanes [0,1,2,3]
                 * correspond to output pixels [ox..ox+7]. */
                const __m256 r0e =
                    _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r0e_raw), 0xD8));
                const __m256 r0o =
                    _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r0o_raw), 0xD8));
                const __m256 r1e =
                    _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r1e_raw), 0xD8));
                const __m256 r1o =
                    _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(r1o_raw), 0xD8));
                /* Sequential summation: (r0e + r0o) + r1e + r1o. */
                __m256 acc = _mm256_add_ps(r0e, r0o);
                acc = _mm256_add_ps(acc, r1e);
                acc = _mm256_add_ps(acc, r1o);
                _mm256_storeu_ps(orow + ox, _mm256_mul_ps(acc, vquarter));
            }
            /* Scalar tail — bit-identical to ss2v_downsample_2x2. */
            for (; ox < ow; ox++) {
                unsigned ix0 = ox * 2;
                unsigned ix1 = (ix0 + 1 < iw) ? ix0 + 1 : iw - 1;
                float sum = row0[ix0] + row0[ix1] + row1[ix0] + row1[ix1];
                orow[ox] = sum * 0.25f;
            }
        }
    }
}
