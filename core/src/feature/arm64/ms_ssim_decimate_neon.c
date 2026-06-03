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

#include <arm_neon.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#include "libvmaf/vmaf_assert.h"
#include "ms_ssim_decimate_neon.h"

/*
 * 9-tap 9/7 biorthogonal wavelet LPF, separable form.
 *
 * REBASE-SENSITIVE INVARIANT: these coefficients MUST match
 * `ms_ssim_lpf_{h,v}` in libvmaf/src/feature/ms_ssim_decimate.c and in
 * the x86 AVX2/AVX-512 variants. Byte-identical bit-exactness depends
 * on identical float32 bit patterns across all four TUs.
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
 * period-based form. Single-reflection reads out of bounds when the
 * kernel half-radius exceeds n (e.g. 9-tap kernel on a 1x1 input);
 * the period = 2*n formulation bounces correctly for any offset.
 * See docs/development/known-upstream-bugs.md and
 *     docs/adr/0125-ms-ssim-decimate-simd.md.
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
 * NEON horizontal pass: 4 output columns per iteration.
 *
 * Stride-2 deinterleave via `vld2q_f32`: loads 8 contiguous source
 * floats and returns .val[0] = [p0, p2, p4, p6] (evens — what we need)
 * and .val[1] = [p1, p3, p5, p7] (odds — discarded).
 */
static inline float32x4_t h_pass_neon_4(const float *src_row, int x_out_base)
{
    float32x4_t acc = vdupq_n_f32(0.0f);
    for (int k = 0; k < MS_SSIM_DECIMATE_LPF_LEN; ++k) {
        const float *base = &src_row[x_out_base * 2 + k - MS_SSIM_DECIMATE_LPF_HALF];
        const float32x4x2_t pair = vld2q_f32(base);
        acc = vfmaq_n_f32(acc, pair.val[0], ms_ssim_lpf_h[k]);
    }
    return acc;
}

/* NEON vertical pass: 4 output columns per iteration, contiguous loads. */
static inline float32x4_t v_pass_neon_4(const float *tmp, int y_out, int x_out, int w_out)
{
    float32x4_t acc = vdupq_n_f32(0.0f);
    const int y_src = y_out * 2;
    for (int k = 0; k < MS_SSIM_DECIMATE_LPF_LEN; ++k) {
        const int yi = y_src + k - MS_SSIM_DECIMATE_LPF_HALF;
        const float32x4_t v = vld1q_f32(&tmp[(size_t)yi * (size_t)w_out + (size_t)x_out]);
        acc = vfmaq_n_f32(acc, v, ms_ssim_lpf_v[k]);
    }
    return acc;
}

/*
 * Horizontal-pass inner SIMD range (output-column space).
 *
 * A 4-wide NEON iteration at x_out_base = c needs:
 *   - 2c + k - 4 >= 0 for all k=0..8   =>  c >= 2
 *   - Load range [2c + k - 4 .. 2c + k - 4 + 7] in-bounds for k=0..8,
 *     i.e. 2c + 8 - 4 + 7 < w  =>  c <= (w - 12) / 2.
 */
static inline void h_simd_range(int w, int w_out, int *x_start, int *x_end)
{
    const int start = 2;
    const int cmax = (w - 12) / 2;
    int end = start;
    if (cmax >= start + 3) {
        const int n_batches = (cmax - start - 3) / 4 + 1;
        end = start + 4 * n_batches;
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
    for (; x_out + 4 <= x_simd_end; x_out += 4) {
        const float32x4_t acc = h_pass_neon_4(src_row, x_out);
        vst1q_f32(&tmp_row[x_out], acc);
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
    for (; x_out + 4 <= w_out; x_out += 4) {
        const float32x4_t acc = v_pass_neon_4(tmp, y_out, x_out, w_out);
        vst1q_f32(&dst_row[x_out], acc);
    }
    for (; x_out < w_out; ++x_out) {
        dst_row[x_out] = v_pass_scalar(tmp, y_out, x_out, w_out, h);
    }
}

int ms_ssim_decimate_neon(const float *src, int w, int h, float *dst, int *rw, int *rh)
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
