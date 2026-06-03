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

#include <errno.h>
#include <immintrin.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#include "libvmaf/vmaf_assert.h"
#include "ms_ssim_decimate_avx2.h"

/*
 * 9-tap 9/7 biorthogonal wavelet LPF, separable form.
 *
 * REBASE-SENSITIVE INVARIANT: these coefficients MUST match
 * `ms_ssim_lpf_{h,v}` in libvmaf/src/feature/ms_ssim_decimate.c.
 * Byte-identical bit-exactness vs. the scalar reference depends on
 * identical float32 bit patterns here.
 * See docs/adr/0125-ms-ssim-decimate-simd.md and
 *     libvmaf/src/feature/AGENTS.md.
 */
#define MS_SSIM_DECIMATE_LPF_LEN 9
#define MS_SSIM_DECIMATE_LPF_HALF 4

static const float ms_ssim_lpf_h[MS_SSIM_DECIMATE_LPF_LEN] = {
    0.026727f, -0.016828f, -0.078201f, 0.266846f, 0.602914f,
    0.266846f, -0.078201f, -0.016828f, 0.026727f,
};

static const float ms_ssim_lpf_v[MS_SSIM_DECIMATE_LPF_LEN] = {
    0.026727f, -0.016828f, -0.078201f, 0.266846f, 0.602914f,
    0.266846f, -0.078201f, -0.016828f, 0.026727f,
};

/*
 * KBND_SYMMETRIC mirror — byte-identical to the scalar reference's
 * `ms_ssim_decimate_mirror`. Period-based reflection (period = 2*n)
 * handles the sub-kernel-radius regime.
 */
static inline int ms_ssim_decimate_mirror(int idx, int n)
{
    const int period = 2 * n;
    int r = idx % period;
    if (r < 0) {
        r += period;
    }
    if (r >= n) {
        r = period - r - 1;
    }
    return r;
}

/* Scalar 9-tap horizontal pass for border / fallback columns. */
static inline float h_pass_scalar(const float *src_row, int x_out, int w)
{
    float acc = 0.0f;
    const int x_src = x_out * 2;
    for (int k = 0; k < MS_SSIM_DECIMATE_LPF_LEN; ++k) {
        const int xi = ms_ssim_decimate_mirror(x_src + k - MS_SSIM_DECIMATE_LPF_HALF, w);
        acc = fmaf(src_row[xi], ms_ssim_lpf_h[k], acc);
    }
    return acc;
}

/* Scalar 9-tap vertical pass for border / fallback rows and columns. */
static inline float v_pass_scalar(const float *tmp, int y_out, int x_out, int w_out, int h)
{
    float acc = 0.0f;
    const int y_src = y_out * 2;
    for (int k = 0; k < MS_SSIM_DECIMATE_LPF_LEN; ++k) {
        const int yi = ms_ssim_decimate_mirror(y_src + k - MS_SSIM_DECIMATE_LPF_HALF, h);
        acc = fmaf(tmp[(size_t)yi * (size_t)w_out + (size_t)x_out], ms_ssim_lpf_v[k], acc);
    }
    return acc;
}

/*
 * AVX2 horizontal pass: 8 output columns per iteration.
 *
 * For each output column x_out in [x_out_base, x_out_base + 8) we
 * compute the 9-tap sum in stride-2 source reads (x_src = 2*x_out +
 * k - 4). Lanes are independent and each lane's accumulation order is
 * fixed by the k-loop, so per-lane bit-identity vs. the scalar
 * `fmaf` chain holds (AVX2 FMA == IEEE-754 fused-multiply-add with
 * single rounding, same as `fmaf`).
 *
 * Stride-2 deinterleave recipe: for each k, load 16 contiguous source
 * floats starting at 2*x_out_base + k - 4 (two 8-wide loads) and
 * extract the even-indexed elements via shuffle+permute4x64.
 */
static inline __m256 h_pass_avx2_8(const float *src_row, int x_out_base)
{
    __m256 acc = _mm256_setzero_ps();
    for (int k = 0; k < MS_SSIM_DECIMATE_LPF_LEN; ++k) {
        const float *base = &src_row[x_out_base * 2 + k - MS_SSIM_DECIMATE_LPF_HALF];
        __m256 a = _mm256_loadu_ps(base);
        __m256 b = _mm256_loadu_ps(base + 8);
        /* Shuffle a,b to get [a0,a2,b0,b2,a4,a6,b4,b6]. */
        __m256 s = _mm256_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0));
        /* Permute 64-bit lanes to get [a0,a2,a4,a6,b0,b2,b4,b6]. */
        __m256 evens =
            _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(s), _MM_SHUFFLE(3, 1, 2, 0)));
        acc = _mm256_fmadd_ps(evens, _mm256_set1_ps(ms_ssim_lpf_h[k]), acc);
    }
    return acc;
}

/*
 * AVX2 vertical pass: 8 output columns per iteration.
 *
 * Precondition: y_out*2 - 4 >= 0 AND y_out*2 + 4 < h (checked by the
 * caller; out-of-range rows go through the scalar fallback).
 */
static inline __m256 v_pass_avx2_8(const float *tmp, int y_out, int x_out, int w_out)
{
    __m256 acc = _mm256_setzero_ps();
    const int y_src = y_out * 2;
    for (int k = 0; k < MS_SSIM_DECIMATE_LPF_LEN; ++k) {
        const int yi = y_src + k - MS_SSIM_DECIMATE_LPF_HALF;
        __m256 v = _mm256_loadu_ps(&tmp[(size_t)yi * (size_t)w_out + (size_t)x_out]);
        acc = _mm256_fmadd_ps(v, _mm256_set1_ps(ms_ssim_lpf_v[k]), acc);
    }
    return acc;
}

/*
 * Horizontal-pass inner SIMD range (output-column space).
 *
 * An 8-wide SIMD iteration at x_out_base = c needs:
 *   - 2c + k - 4 >= 0 for all k=0..8   =>  c >= 2
 *   - Load range [2c + k - 4 .. 2c + k - 4 + 15] in-bounds for
 *     k=0..8, i.e. 2c + 8 - 4 + 15 < w  =>  c <= (w - 20) / 2.
 * Anything outside this range falls back to h_pass_scalar (same
 * bit-pattern as the scalar reference).
 */
static inline void h_simd_range(int w, int w_out, int *x_start, int *x_end)
{
    const int start = 2;
    const int cmax = (w - 20) / 2;
    int end = start;
    if (cmax >= start + 7) {
        const int n_batches = (cmax - start - 7) / 8 + 1;
        end = start + 8 * n_batches;
    }
    if (end > w_out) {
        end = w_out;
    }
    *x_start = (start < w_out) ? start : w_out;
    *x_end = end;
}

static void h_pass_row(const float *src_row, float *tmp_row, int w, int w_out, int x_simd_start,
                       int x_simd_end)
{
    int x_out = 0;
    for (; x_out < x_simd_start && x_out < w_out; ++x_out) {
        tmp_row[x_out] = h_pass_scalar(src_row, x_out, w);
    }
    for (; x_out + 8 <= x_simd_end; x_out += 8) {
        const __m256 acc = h_pass_avx2_8(src_row, x_out);
        _mm256_storeu_ps(&tmp_row[x_out], acc);
    }
    for (; x_out < w_out; ++x_out) {
        tmp_row[x_out] = h_pass_scalar(src_row, x_out, w);
    }
}

static void v_pass_row(const float *tmp, float *dst_row, int y_out, int w_out, int h,
                       int y_simd_start, int y_simd_end)
{
    const int simd_row = (y_out >= y_simd_start) && (y_out < y_simd_end);
    if (!simd_row) {
        for (int x_out = 0; x_out < w_out; ++x_out) {
            dst_row[x_out] = v_pass_scalar(tmp, y_out, x_out, w_out, h);
        }
        return;
    }
    int x_out = 0;
    for (; x_out + 8 <= w_out; x_out += 8) {
        const __m256 acc = v_pass_avx2_8(tmp, y_out, x_out, w_out);
        _mm256_storeu_ps(&dst_row[x_out], acc);
    }
    for (; x_out < w_out; ++x_out) {
        dst_row[x_out] = v_pass_scalar(tmp, y_out, x_out, w_out, h);
    }
}

int ms_ssim_decimate_avx2(const float *src, int w, int h, float *dst, int *rw, int *rh)
{
    VMAF_ASSERT_DEBUG(src != NULL);
    VMAF_ASSERT_DEBUG(dst != NULL);
    VMAF_ASSERT_DEBUG(src != dst);
    VMAF_ASSERT_DEBUG(w > 0);
    VMAF_ASSERT_DEBUG(h > 0);

    const int w_out = (w / 2) + (w & 1);
    const int h_out = (h / 2) + (h & 1);

    float *tmp = (float *)malloc((size_t)w_out * (size_t)h * sizeof(float));
    if (tmp == NULL) {
        return -ENOMEM;
    }

    int x_simd_start = 0;
    int x_simd_end = 0;
    h_simd_range(w, w_out, &x_simd_start, &x_simd_end);

    for (int y = 0; y < h; ++y) {
        const float *src_row = &src[(size_t)y * (size_t)w];
        float *tmp_row = &tmp[(size_t)y * (size_t)w_out];
        h_pass_row(src_row, tmp_row, w, w_out, x_simd_start, x_simd_end);
    }

    /*
     * Vertical pass SIMD inner range:
     *   - 2*y_out - 4 >= 0   =>  y_out >= 2
     *   - 2*y_out + 4 < h    =>  y_out <= (h - 5) / 2
     */
    const int y_simd_start = 2;
    const int y_simd_end = (h - 5) / 2 + 1;

    for (int y_out = 0; y_out < h_out; ++y_out) {
        float *dst_row = &dst[(size_t)y_out * (size_t)w_out];
        v_pass_row(tmp, dst_row, y_out, w_out, h, y_simd_start, y_simd_end);
    }

    free(tmp);

    if (rw != NULL) {
        *rw = w_out;
    }
    if (rh != NULL) {
        *rh = h_out;
    }
    return 0;
}
