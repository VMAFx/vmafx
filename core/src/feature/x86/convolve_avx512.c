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
 * AVX-512 bit-exact fast path for `iqa_convolve` — 1-D separable,
 * interior-only (no boundary reflection).
 *
 * Mirrors convolve_avx2.c exactly, widened from 4-lane double accumulate
 * (`__m256d`) to 8-lane (`__m512d`). Every bit-exactness invariant is
 * preserved:
 *   - 8-lane `__m256 * __m256 = __m256` for the single-rounded float
 *     multiply (`_mm256_mul_ps`). Mirrors the scalar's `float * float →
 *     float` at `sum += img[i] * k[j]` under FLT_EVAL_METHOD == 0.
 *   - Widen the float product to `__m512d` via `_mm512_cvtps_pd`.
 *   - Double add into a `__m512d` accumulator via `_mm512_add_pd`.
 *   - NO fused FMA — would collapse two rounding steps into one and
 *     diverge from scalar by ULPs.
 *   - Round to float at store via `_mm512_cvtpd_ps`, matching the
 *     scalar's `(float)(sum * 1.0)` store.
 *
 * Per-output-pixel reduction order is identical to both the scalar
 * reference and the AVX2 variant: for each output pixel the 11 (or 8)
 * taps are reduced in `k_offset = 0 .. kw-1` order, so the scalar's
 * `sum += a*b` sequence holds byte-for-byte.
 *
 * See docs/adr/0138-iqa-convolve-avx2-bitexact-double.md.
 */

#include <immintrin.h>
#include <stddef.h>
#include <stdlib.h>

#include "libvmaf/vmaf_assert.h"
#include "convolve_avx512.h"

/*
 * Per-output-chunk horizontal tap reduction: 8-lane float multiply,
 * widen to 8-lane double, accumulate. Mirrors scalar `sum += a*b`.
 */
static inline __m512d h_tap8_avx512(const float *img_row, int kx, int uc, int kw_even,
                                    const float *kh)
{
    __m512d acc = _mm512_setzero_pd();
    int k_offset = 0;
    for (int u = -uc; u <= uc - kw_even; ++u, ++k_offset) {
        const __m256 f8 = _mm256_loadu_ps(img_row + kx + u);
        const __m256 coeff_f = _mm256_set1_ps(kh[k_offset]);
        const __m256 prod_f = _mm256_mul_ps(f8, coeff_f);
        const __m512d prod = _mm512_cvtps_pd(prod_f);
        acc = _mm512_add_pd(prod, acc);
    }
    return acc;
}

/*
 * Masked variant of h_tap8_avx512 for the 1..7 tail columns.
 * `_mm256_maskz_loadu_ps` reads only masked lanes (AVX-512VL).
 */
static inline __m512d h_tap8_masked_avx512(const float *img_row, int kx, int uc, int kw_even,
                                           const float *kh, __mmask8 tail_mask)
{
    __m512d acc = _mm512_setzero_pd();
    int k_offset = 0;
    for (int u = -uc; u <= uc - kw_even; ++u, ++k_offset) {
        const __m256 f8 = _mm256_maskz_loadu_ps(tail_mask, img_row + kx + u);
        const __m256 coeff_f = _mm256_set1_ps(kh[k_offset]);
        const __m256 prod_f = _mm256_mul_ps(f8, coeff_f);
        const __m512d prod = _mm512_cvtps_pd(prod_f);
        acc = _mm512_add_pd(prod, acc);
    }
    return acc;
}

/*
 * One row of the horizontal pass: 8-lane SIMD inner loop plus a masked
 * 8-lane tail covering 1..7 remaining columns.
 */
static void h_row_avx512(const float *img_row, float *cache_row, int uc, int kw_even, int dst_w,
                         int dst_w_simd, const float *kh)
{
    for (int x = 0; x < dst_w_simd; x += 8) {
        const int kx = x + uc;
        const __m512d acc = h_tap8_avx512(img_row, kx, uc, kw_even, kh);
        _mm256_storeu_ps(cache_row + kx, _mm512_cvtpd_ps(acc));
    }

    const int tail_n = dst_w - dst_w_simd;
    if (tail_n > 0) {
        const __mmask8 tail_mask = (__mmask8)((1u << tail_n) - 1u);
        const int kx = dst_w_simd + uc;
        const __m512d acc = h_tap8_masked_avx512(img_row, kx, uc, kw_even, kh, tail_mask);
        _mm256_mask_storeu_ps(cache_row + kx, tail_mask, _mm512_cvtpd_ps(acc));
    }
}

/*
 * Horizontal pass — 8 output pixels per iteration plus a masked
 * 8-lane tail step for the final 1..7 columns.
 */
static void h_pass_avx512(const float *img, int w, int uc, int kw_even, int kh_even, int dst_h,
                          int dst_w, int vc, const float *kh, float *img_cache)
{
    VMAF_ASSERT_DEBUG(uc >= 1);
    VMAF_ASSERT_DEBUG(dst_w >= 1);
    VMAF_ASSERT_DEBUG(vc >= 0);
    VMAF_ASSERT_DEBUG(dst_h >= 1);

    const int y_lo = -vc;
    /* See convolve.c:iqa_convolve — stop at `dst_h + vc - kh_even` so
     * we don't write cache row ky = h on even-tap kernels (OOB by one
     * row). That row is never read by the v-pass. */
    const int y_hi = dst_h + vc - kh_even;
    const int dst_w_simd = (dst_w / 8) * 8;

    for (int y = y_lo; y < y_hi; ++y) {
        const int ky = y + vc;
        const float *img_row = img + (ptrdiff_t)ky * (ptrdiff_t)w;
        float *cache_row = img_cache + (ptrdiff_t)ky * (ptrdiff_t)w;
        h_row_avx512(img_row, cache_row, uc, kw_even, dst_w, dst_w_simd, kh);
    }
}

/*
 * Per-output-chunk vertical tap reduction for an 8-lane output chunk.
 */
static inline __m512d v_tap8_avx512(const float *img_cache, int w, int ky, int kx, int vc,
                                    int kh_even, const float *kv)
{
    __m512d acc = _mm512_setzero_pd();
    int k_offset = 0;
    for (int v = -vc; v <= vc - kh_even; ++v, ++k_offset) {
        const float *row_v = img_cache + (ptrdiff_t)(ky + v) * (ptrdiff_t)w + kx;
        const __m256 f8 = _mm256_loadu_ps(row_v);
        const __m256 coeff_f = _mm256_set1_ps(kv[k_offset]);
        const __m256 prod_f = _mm256_mul_ps(f8, coeff_f);
        const __m512d prod = _mm512_cvtps_pd(prod_f);
        acc = _mm512_add_pd(prod, acc);
    }
    return acc;
}

/*
 * Masked variant of v_tap8_avx512 for the 1..7 tail columns.
 */
static inline __m512d v_tap8_masked_avx512(const float *img_cache, int w, int ky, int kx, int vc,
                                           int kh_even, const float *kv, __mmask8 tail_mask)
{
    __m512d acc = _mm512_setzero_pd();
    int k_offset = 0;
    for (int v = -vc; v <= vc - kh_even; ++v, ++k_offset) {
        const float *row_v = img_cache + (ptrdiff_t)(ky + v) * (ptrdiff_t)w + kx;
        const __m256 f8 = _mm256_maskz_loadu_ps(tail_mask, row_v);
        const __m256 coeff_f = _mm256_set1_ps(kv[k_offset]);
        const __m256 prod_f = _mm256_mul_ps(f8, coeff_f);
        const __m512d prod = _mm512_cvtps_pd(prod_f);
        acc = _mm512_add_pd(prod, acc);
    }
    return acc;
}

/*
 * One row of the vertical pass: 8-lane SIMD inner loop plus masked tail.
 */
static void v_row_avx512(const float *img_cache, float *dst_row, int w, int ky, int uc, int vc,
                         int kh_even, int dst_w, int dst_w_simd, const float *kv)
{
    for (int x = 0; x < dst_w_simd; x += 8) {
        const int kx = x + uc;
        const __m512d acc = v_tap8_avx512(img_cache, w, ky, kx, vc, kh_even, kv);
        _mm256_storeu_ps(dst_row + x, _mm512_cvtpd_ps(acc));
    }

    const int tail_n = dst_w - dst_w_simd;
    if (tail_n > 0) {
        const __mmask8 tail_mask = (__mmask8)((1u << tail_n) - 1u);
        const int kx = dst_w_simd + uc;
        const __m512d acc = v_tap8_masked_avx512(img_cache, w, ky, kx, vc, kh_even, kv, tail_mask);
        _mm256_mask_storeu_ps(dst_row + dst_w_simd, tail_mask, _mm512_cvtpd_ps(acc));
    }
}

/*
 * Vertical pass — 8 output pixels per iteration.
 */
static void v_pass_avx512(const float *img_cache, int w, int vc, int kh_even, int dst_h, int dst_w,
                          int uc, const float *kv, float *dst)
{
    VMAF_ASSERT_DEBUG(dst_w >= 1);
    VMAF_ASSERT_DEBUG(dst_h >= 1);
    VMAF_ASSERT_DEBUG(uc >= 0);
    VMAF_ASSERT_DEBUG(vc >= 1);

    const int dst_w_simd = (dst_w / 8) * 8;

    for (int y = 0; y < dst_h; ++y) {
        const int ky = y + vc;
        float *dst_row = dst + (ptrdiff_t)y * (ptrdiff_t)dst_w;
        v_row_avx512(img_cache, dst_row, w, ky, uc, vc, kh_even, dst_w, dst_w_simd, kv);
    }
}

void iqa_convolve_avx512(float *img, int w, int h, const float *kernel_h, const float *kernel_v,
                         int kw, int kh, int normalized, float *workspace, float *result, int *rw,
                         int *rh)
{
    VMAF_ASSERT_DEBUG(img != NULL);
    VMAF_ASSERT_DEBUG(kernel_h != NULL);
    VMAF_ASSERT_DEBUG(kernel_v != NULL);
    VMAF_ASSERT_DEBUG(normalized == 1);
    VMAF_ASSERT_DEBUG(w >= kw);
    VMAF_ASSERT_DEBUG(h >= kh);
    (void)normalized;

    const int uc = kw / 2;
    const int vc = kh / 2;
    const int kw_even = (kw & 1) ? 0 : 1;
    const int kh_even = (kh & 1) ? 0 : 1;
    const int dst_w = w - kw + 1;
    const int dst_h = h - kh + 1;

    float *dst = result;
    if (!dst) {
        dst = img;
    }

    float *img_cache = workspace;
    int owned = 0;
    if (!img_cache) {
        img_cache = (float *)calloc((size_t)w * (size_t)h, sizeof(float));
        if (!img_cache) {
            if (rw)
                *rw = 0;
            if (rh)
                *rh = 0;
            return;
        }
        owned = 1;
    }

    h_pass_avx512(img, w, uc, kw_even, kh_even, dst_h, dst_w, vc, kernel_h, img_cache);
    v_pass_avx512(img_cache, w, vc, kh_even, dst_h, dst_w, uc, kernel_v, dst);

    if (owned) {
        free(img_cache);
    }

    if (rw)
        *rw = dst_w;
    if (rh)
        *rh = dst_h;
}
