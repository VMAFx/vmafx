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
 * AVX2 bit-exact fast path for `iqa_convolve` — 1-D separable,
 * interior-only (no boundary reflection).
 *
 * Specialised for MS-SSIM / SSIM invariants:
 *   - `normalized == 1` (so scalar's `scale == 1.0f`, elided here)
 *   - separable: `kernel_h` and `kernel_v` non-NULL
 *   - `IQA_CONVOLVE_1D` is set in iqa_options.h
 *   - `kw == kh ∈ { 8 (square), 11 (Gaussian) }`
 *
 * Bit-identical to the scalar reference under IQA_CONVOLVE_1D
 * (see ADR-0138 §Decision). The scalar inner body is:
 *
 *   double sum = 0.0;
 *   for (...) sum += img[i] * k[j];        (*)
 *   dst[out] = (float)(sum * scale);
 *
 * At (*), `img[i] * k[j]` is `float * float` → `float` (single-
 * rounded; `FLT_EVAL_METHOD == 0` on x86-64 with SSE2+), then the
 * float product is implicitly promoted to `double` before `+= sum`.
 * We mirror this exactly: single-rounded float multiply in SIMD,
 * then widen to double, then double add.
 *
 *   - 4-lane `__m128 * __m128 = __m128` for the single-rounded
 *     float multiply (`_mm_mul_ps`).
 *   - Widen the float product to `__m256d` via `_mm256_cvtps_pd`.
 *   - Double add into a `__m256d` accumulator via `_mm256_add_pd`.
 *   - NO fused FMA — that would collapse two rounding steps into
 *     one and diverge by ULPs.
 *   - Round to float at store via `_mm256_cvtpd_ps`, matching the
 *     scalar's `(float)(sum * 1.0)` store.
 *
 * Primitive-argument signature (no `struct iqa_kernel`) decouples this
 * translation unit from iqa/convolve.h — keeps the x86_avx2 static
 * library's include set narrow and matches ms_ssim_decimate_avx2.c.
 *
 * Right-edge tail (columns not filling a 4-lane output chunk) uses
 * a masked 4-lane step with the same float-mul-widen-add sequence,
 * so every output pixel is byte-identical to scalar.
 *
 * See docs/adr/0138-iqa-convolve-avx2-bitexact-double.md and
 * docs/research/0011-iqa-convolve-avx2.md.
 */

#include <immintrin.h>
#include <stddef.h>
#include <stdlib.h>

#include "libvmaf/vmaf_assert.h"
#include "convolve_avx2.h"

/*
 * Lane-mask storage for the 4-lane AVX2 tail — sliced by
 * `_mm_loadu_si128((const __m128i *)(tail_lane_mask + 4 - tail_n))`
 * to produce a mask with `tail_n` high-bit lanes set.
 * Read-only static: zero initialisation, no runtime cost.
 */
static const int tail_lane_mask[8] = {-1, -1, -1, -1, 0, 0, 0, 0};

/*
 * Per-output-chunk tap reduction: for one 4-lane chunk of output
 * columns, reduce `img[kx+u]` over the tap range with coefficients
 * `kh`, accumulating into `*acc` as double.
 *
 * The float-mul-widen-double-add pattern mirrors scalar
 * `sum += img[i] * k[j]` exactly (see file header).
 */
static inline __m256d h_tap4_avx2(const float *img_row, int kx, int uc, int kw_even,
                                  const float *kh)
{
    __m256d acc = _mm256_setzero_pd();
    int k_offset = 0;
    for (int u = -uc; u <= uc - kw_even; ++u, ++k_offset) {
        const __m128 f4 = _mm_loadu_ps(img_row + kx + u);
        const __m128 coeff_f = _mm_set1_ps(kh[k_offset]);
        const __m128 prod_f = _mm_mul_ps(f4, coeff_f);
        const __m256d prod = _mm256_cvtps_pd(prod_f);
        acc = _mm256_add_pd(prod, acc);
    }
    return acc;
}

/*
 * Masked variant of h_tap4_avx2 for the 1..3 tail columns.
 * `_mm_maskload_ps` reads only masked lanes, keeping memory access
 * safe past the scalar right boundary.
 */
static inline __m256d h_tap4_masked_avx2(const float *img_row, int kx, int uc, int kw_even,
                                         const float *kh, __m128i tail_mask)
{
    __m256d acc = _mm256_setzero_pd();
    int k_offset = 0;
    for (int u = -uc; u <= uc - kw_even; ++u, ++k_offset) {
        const __m128 f4 = _mm_maskload_ps(img_row + kx + u, tail_mask);
        const __m128 coeff_f = _mm_set1_ps(kh[k_offset]);
        const __m128 prod_f = _mm_mul_ps(f4, coeff_f);
        const __m256d prod = _mm256_cvtps_pd(prod_f);
        acc = _mm256_add_pd(prod, acc);
    }
    return acc;
}

/*
 * One row of the horizontal pass: SIMD inner loop (4 output pixels per
 * iteration) plus a masked 4-lane tail covering 1..3 remaining columns.
 */
static void h_row_avx2(const float *img_row, float *cache_row, int uc, int kw_even, int dst_w,
                       int dst_w_simd, const float *kh)
{
    for (int x = 0; x < dst_w_simd; x += 4) {
        const int kx = x + uc;
        const __m256d acc = h_tap4_avx2(img_row, kx, uc, kw_even, kh);
        _mm_storeu_ps(cache_row + kx, _mm256_cvtpd_ps(acc));
    }

    const int tail_n = dst_w - dst_w_simd;
    if (tail_n > 0) {
        const __m128i tail_mask = _mm_loadu_si128((const __m128i *)(tail_lane_mask + 4 - tail_n));
        const int kx = dst_w_simd + uc;
        const __m256d acc = h_tap4_masked_avx2(img_row, kx, uc, kw_even, kh, tail_mask);
        _mm_maskstore_ps(cache_row + kx, tail_mask, _mm256_cvtpd_ps(acc));
    }
}

/*
 * Horizontal pass: for each covered row `ky ∈ [0, h_covered)`, compute
 * the convolved pixel at `img_cache[ky*w + kx]` for `kx ∈ [uc, uc+dst_w)`.
 */
static void h_pass_avx2(const float *img, int w, int uc, int kw_even, int kh_even, int dst_h,
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
    const int dst_w_simd = (dst_w / 4) * 4;

    for (int y = y_lo; y < y_hi; ++y) {
        const int ky = y + vc;
        const float *img_row = img + (ptrdiff_t)ky * (ptrdiff_t)w;
        float *cache_row = img_cache + (ptrdiff_t)ky * (ptrdiff_t)w;
        h_row_avx2(img_row, cache_row, uc, kw_even, dst_w, dst_w_simd, kh);
    }
}

/*
 * Per-output-chunk vertical tap reduction: for one 4-lane chunk of
 * output columns at `kx`, reduce `img_cache[(ky+v)*w + kx]` over the
 * tap range with coefficients `kv`.
 */
static inline __m256d v_tap4_avx2(const float *img_cache, int w, int ky, int kx, int vc,
                                  int kh_even, const float *kv)
{
    __m256d acc = _mm256_setzero_pd();
    int k_offset = 0;
    for (int v = -vc; v <= vc - kh_even; ++v, ++k_offset) {
        const float *row_v = img_cache + (ptrdiff_t)(ky + v) * (ptrdiff_t)w + kx;
        const __m128 f4 = _mm_loadu_ps(row_v);
        const __m128 coeff_f = _mm_set1_ps(kv[k_offset]);
        const __m128 prod_f = _mm_mul_ps(f4, coeff_f);
        const __m256d prod = _mm256_cvtps_pd(prod_f);
        acc = _mm256_add_pd(prod, acc);
    }
    return acc;
}

/*
 * Masked variant of v_tap4_avx2 for the 1..3 tail columns.
 */
static inline __m256d v_tap4_masked_avx2(const float *img_cache, int w, int ky, int kx, int vc,
                                         int kh_even, const float *kv, __m128i tail_mask)
{
    __m256d acc = _mm256_setzero_pd();
    int k_offset = 0;
    for (int v = -vc; v <= vc - kh_even; ++v, ++k_offset) {
        const float *row_v = img_cache + (ptrdiff_t)(ky + v) * (ptrdiff_t)w + kx;
        const __m128 f4 = _mm_maskload_ps(row_v, tail_mask);
        const __m128 coeff_f = _mm_set1_ps(kv[k_offset]);
        const __m128 prod_f = _mm_mul_ps(f4, coeff_f);
        const __m256d prod = _mm256_cvtps_pd(prod_f);
        acc = _mm256_add_pd(prod, acc);
    }
    return acc;
}

/*
 * One row of the vertical pass: SIMD inner loop plus masked 4-lane tail.
 */
static void v_row_avx2(const float *img_cache, float *dst_row, int w, int ky, int uc, int vc,
                       int kh_even, int dst_w, int dst_w_simd, const float *kv)
{
    for (int x = 0; x < dst_w_simd; x += 4) {
        const int kx = x + uc;
        const __m256d acc = v_tap4_avx2(img_cache, w, ky, kx, vc, kh_even, kv);
        _mm_storeu_ps(dst_row + x, _mm256_cvtpd_ps(acc));
    }

    const int tail_n = dst_w - dst_w_simd;
    if (tail_n > 0) {
        const __m128i tail_mask = _mm_loadu_si128((const __m128i *)(tail_lane_mask + 4 - tail_n));
        const int kx = dst_w_simd + uc;
        const __m256d acc = v_tap4_masked_avx2(img_cache, w, ky, kx, vc, kh_even, kv, tail_mask);
        _mm_maskstore_ps(dst_row + dst_w_simd, tail_mask, _mm256_cvtpd_ps(acc));
    }
}

/*
 * Vertical pass: for each output row `y ∈ [0, dst_h)` and output
 * column `x ∈ [0, dst_w)`, compute `dst[y*dst_w + x]` by reducing
 * `img_cache[(y+vc+v)*w + (x+uc)]` over `v ∈ [-vc, vc-kh_even]`.
 *
 * Loop order swapped vs. scalar (y-outer for contiguous writes). The
 * per-output-pixel reduction order is identical, so the result is
 * bit-identical.
 */
static void v_pass_avx2(const float *img_cache, int w, int vc, int kh_even, int dst_h, int dst_w,
                        int uc, const float *kv, float *dst)
{
    VMAF_ASSERT_DEBUG(dst_w >= 1);
    VMAF_ASSERT_DEBUG(dst_h >= 1);
    VMAF_ASSERT_DEBUG(uc >= 0);
    VMAF_ASSERT_DEBUG(vc >= 1);

    const int dst_w_simd = (dst_w / 4) * 4;

    for (int y = 0; y < dst_h; ++y) {
        const int ky = y + vc;
        float *dst_row = dst + (ptrdiff_t)y * (ptrdiff_t)dst_w;
        v_row_avx2(img_cache, dst_row, w, ky, uc, vc, kh_even, dst_w, dst_w_simd, kv);
    }
}

void iqa_convolve_avx2(float *img, int w, int h, const float *kernel_h, const float *kernel_v,
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
        dst = img; /* Convolve in-place, matches scalar semantics */
    }

    /* Caller-owned workspace eliminates the per-call calloc (~1200 pairs
     * per 120-frame run at 1080p). NULL triggers an internal alloc for
     * standalone callers (unit tests); the hot path in iqa_ssim
     * allocates once and reuses. */
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

    h_pass_avx2(img, w, uc, kw_even, kh_even, dst_h, dst_w, vc, kernel_h, img_cache);
    v_pass_avx2(img_cache, w, vc, kh_even, dst_h, dst_w, uc, kernel_v, dst);

    if (owned) {
        free(img_cache);
    }

    if (rw)
        *rw = dst_w;
    if (rh)
        *rh = dst_h;
}
