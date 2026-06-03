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
#include "ms_ssim_decimate_avx512.h"

/*
 * 9-tap 9/7 biorthogonal wavelet LPF, separable form.
 *
 * REBASE-SENSITIVE INVARIANT: these coefficients MUST match
 * `ms_ssim_lpf_{h,v}` in libvmaf/src/feature/ms_ssim_decimate.c and in
 * libvmaf/src/feature/x86/ms_ssim_decimate_avx2.c. Byte-identical
 * bit-exactness depends on identical float32 bit patterns across all
 * three TUs.
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
 * KBND_SYMMETRIC mirror — byte-identical to the scalar reference.
 * Period-based reflection (period = 2*n) handles the sub-kernel-radius
 * regime.
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
 * AVX-512 horizontal pass: 16 output columns per iteration.
 *
 * Stride-2 deinterleave recipe: load 32 contiguous source floats (two
 * 16-wide loads) and use `_mm512_permutex2var_ps` with the precomputed
 * "even indices" permutation to extract [p0, p2, p4, ..., p30]. One
 * FMA per k; 9 k-iterations total.
 */
static inline __m512 h_pass_avx512_16(const float *src_row, int x_out_base, __m512i even_idx)
{
    __m512 acc = _mm512_setzero_ps();
    for (int k = 0; k < MS_SSIM_DECIMATE_LPF_LEN; ++k) {
        const float *base = &src_row[x_out_base * 2 + k - MS_SSIM_DECIMATE_LPF_HALF];
        __m512 a = _mm512_loadu_ps(base);
        __m512 b = _mm512_loadu_ps(base + 16);
        __m512 evens = _mm512_permutex2var_ps(a, even_idx, b);
        acc = _mm512_fmadd_ps(evens, _mm512_set1_ps(ms_ssim_lpf_h[k]), acc);
    }
    return acc;
}

/* AVX-512 vertical pass: 16 output columns per iteration, contiguous loads. */
static inline __m512 v_pass_avx512_16(const float *tmp, int y_out, int x_out, int w_out)
{
    __m512 acc = _mm512_setzero_ps();
    const int y_src = y_out * 2;
    for (int k = 0; k < MS_SSIM_DECIMATE_LPF_LEN; ++k) {
        const int yi = y_src + k - MS_SSIM_DECIMATE_LPF_HALF;
        __m512 v = _mm512_loadu_ps(&tmp[(size_t)yi * (size_t)w_out + (size_t)x_out]);
        acc = _mm512_fmadd_ps(v, _mm512_set1_ps(ms_ssim_lpf_v[k]), acc);
    }
    return acc;
}

/*
 * Horizontal-pass inner SIMD range (output-column space).
 *
 * A 16-wide SIMD iteration at x_out_base = c needs:
 *   - 2c + k - 4 >= 0 for all k=0..8   =>  c >= 2
 *   - Load range [2c + k - 4 .. 2c + k - 4 + 31] in-bounds for k=0..8,
 *     i.e. 2c + 8 - 4 + 31 < w  =>  c <= (w - 36) / 2.
 */
static inline void h_simd_range(int w, int w_out, int *x_start, int *x_end)
{
    const int start = 2;
    const int cmax = (w - 36) / 2;
    int end = start;
    if (cmax >= start + 15) {
        const int n_batches = (cmax - start - 15) / 16 + 1;
        end = start + 16 * n_batches;
    }
    if (end > w_out) {
        end = w_out;
    }
    *x_start = (start < w_out) ? start : w_out;
    *x_end = end;
}

static void h_pass_row(const float *src_row, float *tmp_row, int w, int w_out, int x_simd_start,
                       int x_simd_end, __m512i even_idx)
{
    int x_out = 0;
    for (; x_out < x_simd_start && x_out < w_out; ++x_out) {
        tmp_row[x_out] = h_pass_scalar(src_row, x_out, w);
    }
    for (; x_out + 16 <= x_simd_end; x_out += 16) {
        const __m512 acc = h_pass_avx512_16(src_row, x_out, even_idx);
        _mm512_storeu_ps(&tmp_row[x_out], acc);
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
    for (; x_out + 16 <= w_out; x_out += 16) {
        const __m512 acc = v_pass_avx512_16(tmp, y_out, x_out, w_out);
        _mm512_storeu_ps(&dst_row[x_out], acc);
    }
    for (; x_out < w_out; ++x_out) {
        dst_row[x_out] = v_pass_scalar(tmp, y_out, x_out, w_out, h);
    }
}

int ms_ssim_decimate_avx512(const float *src, int w, int h, float *dst, int *rw, int *rh)
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

    /*
     * Even-index permutation for stride-2 deinterleave of 32 source
     * floats into 16 destination lanes: [0, 2, 4, ..., 30].
     * `_mm512_permutex2var_ps(a, idx, b)`: idx[i] < 16 picks a[idx[i]],
     * idx[i] >= 16 picks b[idx[i] - 16].
     */
    const __m512i even_idx =
        _mm512_setr_epi32(0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30);

    int x_simd_start = 0;
    int x_simd_end = 0;
    h_simd_range(w, w_out, &x_simd_start, &x_simd_end);

    for (int y = 0; y < h; ++y) {
        const float *src_row = &src[(size_t)y * (size_t)w];
        float *tmp_row = &tmp[(size_t)y * (size_t)w_out];
        h_pass_row(src_row, tmp_row, w, w_out, x_simd_start, x_simd_end, even_idx);
    }

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
