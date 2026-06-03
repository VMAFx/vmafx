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
 * NEON bit-exact fast path for `iqa_convolve` — 1-D separable,
 * interior-only (no boundary reflection). Port of convolve_avx2.c
 * to aarch64 via the simd_dx.h macros (ADR-0140).
 *
 * Specialised for MS-SSIM / SSIM invariants:
 *   - `normalized == 1` (scalar's `scale == 1.0f`, elided here)
 *   - separable: `kernel_h` and `kernel_v` non-NULL
 *   - `IQA_CONVOLVE_1D` is set in iqa_options.h
 *   - `kw == kh ∈ { 8 (square), 11 (Gaussian) }`
 *
 * Bit-identical to the scalar reference under IQA_CONVOLVE_1D — per
 * scalar C `sum += img[i] * k[j]` with `sum` typed as `double`,
 * each tap is *single-rounded `float * float` → widen to double →
 * double add*. Mirrors x86's AVX2 implementation.
 *
 *   - 4-lane `float32x4_t * float32x4_t = float32x4_t`
 *     (single-rounded `vmulq_f32`).
 *   - Widen the float product to two `float64x2_t` halves via
 *     `vcvt_f64_f32(vget_low_f32(p))` + `vcvt_high_f64_f32(p)`.
 *   - Double add into paired `float64x2_t` accumulators via
 *     `vaddq_f64`.
 *   - NO FMA (`vfmaq_f32` / `vfmaq_f64`) — would collapse rounding
 *     and diverge by ULPs from scalar.
 *   - Round back to float at store via `vcvt_f32_f64` +
 *     `vcvt_high_f32_f64`, matching scalar's `(float)(sum * 1.0)`.
 *
 * The widen-add reduction pattern lives in
 * `SIMD_WIDEN_ADD_F32_F64_NEON_4L` (see simd_dx.h). Two `float64x2_t`
 * accumulators carry the low/high halves of each float32x4_t product.
 *
 * See docs/adr/0138-iqa-convolve-avx2-bitexact-double.md and
 * docs/adr/0140-simd-dx-framework.md.
 */

#include <arm_neon.h>
#include <stddef.h>
#include <stdlib.h>

#include "libvmaf/vmaf_assert.h"
#include "../simd_dx.h"
#include "convolve_neon.h"

/*
 * Per-output-chunk horizontal tap reduction: reduce `img[kx+u]`
 * over the tap range with coefficients `kh`, accumulating into the
 * paired double accumulators `(*acc_lo, *acc_hi)`.
 *
 * Uses the ADR-0138 widen-add pattern via the DX macro
 * `SIMD_WIDEN_ADD_F32_F64_NEON_4L`.
 */
static inline void h_tap4_neon(const float *img_row, int kx, int uc, int kw_even, const float *kh,
                               float64x2_t *acc_lo, float64x2_t *acc_hi)
{
    float64x2_t lo = vdupq_n_f64(0.0);
    float64x2_t hi = vdupq_n_f64(0.0);
    int k_offset = 0;
    for (int u = -uc; u <= uc - kw_even; ++u, ++k_offset) {
        const float32x4_t f4 = vld1q_f32(img_row + kx + u);
        const float32x4_t coeff_f = vdupq_n_f32(kh[k_offset]);
        SIMD_WIDEN_ADD_F32_F64_NEON_4L(lo, hi, f4, coeff_f);
    }
    *acc_lo = lo;
    *acc_hi = hi;
}

/*
 * One row of the horizontal pass: SIMD inner loop (4 output pixels
 * per iteration) plus a scalar 1..3-column tail.
 *
 * The tail runs in scalar double to match the SIMD-body bit-exactness
 * invariant — scalar C with `double sum = 0.0; sum += img[i] * k[j]`
 * is byte-identical to the `widen-add` SIMD pattern (same FP
 * rounding). Using a scalar tail instead of a masked NEON tail keeps
 * the TU readable without sacrificing correctness; the tail is ≤ 3
 * columns per row regardless of dst_w.
 */
static void h_row_neon(const float *img_row, float *cache_row, int uc, int kw_even, int dst_w,
                       int dst_w_simd, const float *kh)
{
    for (int x = 0; x < dst_w_simd; x += 4) {
        const int kx = x + uc;
        float64x2_t acc_lo, acc_hi;
        h_tap4_neon(img_row, kx, uc, kw_even, kh, &acc_lo, &acc_hi);
        const float32x4_t out = vcombine_f32(vcvt_f32_f64(acc_lo), vcvt_f32_f64(acc_hi));
        vst1q_f32(cache_row + kx, out);
    }

    for (int x = dst_w_simd; x < dst_w; ++x) {
        const int kx = x + uc;
        double sum = 0.0;
        int k_offset = 0;
        for (int u = -uc; u <= uc - kw_even; ++u, ++k_offset) {
            sum += (double)(img_row[kx + u] * kh[k_offset]);
        }
        cache_row[kx] = (float)sum;
    }
}

/*
 * Horizontal pass: for each covered row `ky ∈ [0, h_covered)`, compute
 * the convolved pixel at `img_cache[ky*w + kx]` for `kx ∈ [uc, uc+dst_w)`.
 *
 * Outer-loop upper bound `dst_h + vc - kh_even` matches the AVX2
 * implementation: stops writing row ky = h on even-tap kernels
 * (OOB by one; the row is never consumed by the v-pass).
 */
static void h_pass_neon(const float *img, int w, int uc, int kw_even, int kh_even, int dst_h,
                        int dst_w, int vc, const float *kh, float *img_cache)
{
    VMAF_ASSERT_DEBUG(uc >= 1);
    VMAF_ASSERT_DEBUG(dst_w >= 1);
    VMAF_ASSERT_DEBUG(vc >= 0);
    VMAF_ASSERT_DEBUG(dst_h >= 1);

    const int y_lo = -vc;
    const int y_hi = dst_h + vc - kh_even;
    const int dst_w_simd = (dst_w / 4) * 4;

    for (int y = y_lo; y < y_hi; ++y) {
        const int ky = y + vc;
        const float *img_row = img + (ptrdiff_t)ky * (ptrdiff_t)w;
        float *cache_row = img_cache + (ptrdiff_t)ky * (ptrdiff_t)w;
        h_row_neon(img_row, cache_row, uc, kw_even, dst_w, dst_w_simd, kh);
    }
}

/*
 * Per-output-chunk vertical tap reduction. Uses the same widen-add
 * pattern as h_tap4_neon.
 */
static inline void v_tap4_neon(const float *img_cache, int w, int ky, int kx, int vc, int kh_even,
                               const float *kv, float64x2_t *acc_lo, float64x2_t *acc_hi)
{
    float64x2_t lo = vdupq_n_f64(0.0);
    float64x2_t hi = vdupq_n_f64(0.0);
    int k_offset = 0;
    for (int v = -vc; v <= vc - kh_even; ++v, ++k_offset) {
        const float *row_v = img_cache + (ptrdiff_t)(ky + v) * (ptrdiff_t)w + kx;
        const float32x4_t f4 = vld1q_f32(row_v);
        const float32x4_t coeff_f = vdupq_n_f32(kv[k_offset]);
        SIMD_WIDEN_ADD_F32_F64_NEON_4L(lo, hi, f4, coeff_f);
    }
    *acc_lo = lo;
    *acc_hi = hi;
}

/*
 * One row of the vertical pass: SIMD inner loop plus scalar tail.
 */
static void v_row_neon(const float *img_cache, float *dst_row, int w, int ky, int uc, int vc,
                       int kh_even, int dst_w, int dst_w_simd, const float *kv)
{
    for (int x = 0; x < dst_w_simd; x += 4) {
        const int kx = x + uc;
        float64x2_t acc_lo, acc_hi;
        v_tap4_neon(img_cache, w, ky, kx, vc, kh_even, kv, &acc_lo, &acc_hi);
        const float32x4_t out = vcombine_f32(vcvt_f32_f64(acc_lo), vcvt_f32_f64(acc_hi));
        vst1q_f32(dst_row + x, out);
    }

    for (int x = dst_w_simd; x < dst_w; ++x) {
        const int kx = x + uc;
        double sum = 0.0;
        int k_offset = 0;
        for (int v = -vc; v <= vc - kh_even; ++v, ++k_offset) {
            const float *row_v = img_cache + (ptrdiff_t)(ky + v) * (ptrdiff_t)w + kx;
            sum += (double)(row_v[0] * kv[k_offset]);
        }
        dst_row[x] = (float)sum;
    }
}

/*
 * Vertical pass: for each output row `y ∈ [0, dst_h)` and output
 * column `x ∈ [0, dst_w)`, compute `dst[y*dst_w + x]` by reducing
 * `img_cache[(y+vc+v)*w + (x+uc)]` over `v ∈ [-vc, vc-kh_even]`.
 */
static void v_pass_neon(const float *img_cache, int w, int vc, int kh_even, int dst_h, int dst_w,
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
        v_row_neon(img_cache, dst_row, w, ky, uc, vc, kh_even, dst_w, dst_w_simd, kv);
    }
}

void iqa_convolve_neon(float *img, int w, int h, const float *kernel_h, const float *kernel_v,
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

    /* Caller-owned workspace eliminates per-call calloc (~1200 pairs
     * per 120-frame run at 1080p). NULL triggers an internal alloc
     * for standalone callers (unit tests); the hot path in iqa_ssim
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

    h_pass_neon(img, w, uc, kw_even, kh_even, dst_h, dst_w, vc, kernel_h, img_cache);
    v_pass_neon(img_cache, w, vc, kh_even, dst_h, dst_w, uc, kernel_v, dst);

    if (owned) {
        free(img_cache);
    }

    if (rw)
        *rw = dst_w;
    if (rh)
        *rh = dst_h;
}
