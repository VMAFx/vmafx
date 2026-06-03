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
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#include "cpu.h"
#include "libvmaf/vmaf_assert.h"
#include "ms_ssim_decimate.h"

#if ARCH_X86
#include "x86/ms_ssim_decimate_avx2.h"
#if HAVE_AVX512
#include "x86/ms_ssim_decimate_avx512.h"
#endif
#endif

#if ARCH_AARCH64
#include "arm64/ms_ssim_decimate_neon.h"
#endif

/*
 * 9-tap 9/7 biorthogonal wavelet LPF, separable form.
 *
 * REBASE-SENSITIVE INVARIANT: these coefficients MUST match
 * `g_lpf_h` / `g_lpf_v` in libvmaf/src/feature/ms_ssim.c. If Netflix
 * upstream changes the coefficients there, mirror the change here.
 * See docs/adr/0125-ms-ssim-decimate-simd.md.
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
 * KBND_SYMMETRIC mirror: indices outside [0, n) reflect around the
 * nearest border. Period-based formulation (period = 2*n) handles any
 * offset, including the sub-kernel-radius regime where |idx - 0| > n
 * or |idx - (n-1)| > n — cases where a single reflection (as in
 * upstream iqa/KBND_SYMMETRIC) still leaves the index out of bounds.
 * For idx ∈ [-n, 2*n-1] this matches the classic single-reflect form
 * exactly, so the vendored 2-D iqa path and this separable path agree
 * on every MS-SSIM pyramid scale that upstream actually exercises.
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

/* Horizontal pass: src[h x w] -> tmp[h x w_out] via ms_ssim_lpf_h. */
static void h_pass_scalar(const float *src, int w, int h, float *tmp, int w_out)
{
    const int half = MS_SSIM_DECIMATE_LPF_HALF;
    for (int y = 0; y < h; ++y) {
        const float *src_row = &src[(size_t)y * (size_t)w];
        float *tmp_row = &tmp[(size_t)y * (size_t)w_out];
        for (int x_out = 0; x_out < w_out; ++x_out) {
            const int x_src = x_out * 2;
            float acc = 0.0f;
            for (int k = 0; k < MS_SSIM_DECIMATE_LPF_LEN; ++k) {
                const int xi = ms_ssim_decimate_mirror(x_src + k - half, w);
                acc = fmaf(src_row[xi], ms_ssim_lpf_h[k], acc);
            }
            tmp_row[x_out] = acc;
        }
    }
}

/* Vertical pass: tmp[h x w_out] -> dst[h_out x w_out] via ms_ssim_lpf_v. */
static void v_pass_scalar(const float *tmp, int h, float *dst, int w_out, int h_out)
{
    const int half = MS_SSIM_DECIMATE_LPF_HALF;
    for (int y_out = 0; y_out < h_out; ++y_out) {
        const int y_src = y_out * 2;
        float *dst_row = &dst[(size_t)y_out * (size_t)w_out];
        for (int x_out = 0; x_out < w_out; ++x_out) {
            float acc = 0.0f;
            for (int k = 0; k < MS_SSIM_DECIMATE_LPF_LEN; ++k) {
                const int yi = ms_ssim_decimate_mirror(y_src + k - half, h);
                acc = fmaf(tmp[(size_t)yi * (size_t)w_out + (size_t)x_out], ms_ssim_lpf_v[k], acc);
            }
            dst_row[x_out] = acc;
        }
    }
}

int ms_ssim_decimate_scalar(const float *src, int w, int h, float *dst, int *rw, int *rh)
{
    VMAF_ASSERT_DEBUG(src != NULL);
    VMAF_ASSERT_DEBUG(dst != NULL);
    VMAF_ASSERT_DEBUG(src != dst);
    VMAF_ASSERT_DEBUG(w > 0);
    VMAF_ASSERT_DEBUG(h > 0);

    const int w_out = (w / 2) + (w & 1);
    const int h_out = (h / 2) + (h & 1);

    /*
     * Horizontal pass scratch: w_out columns per row, all h rows
     * (factor-2 subsampling is applied in the horizontal pass; the
     * vertical pass still needs all source rows because the 9-tap
     * vertical kernel at y_out*2 reads rows y_out*2-4..y_out*2+4 which
     * spans both even and odd source indices).
     */
    float *tmp = (float *)malloc((size_t)w_out * (size_t)h * sizeof(float));
    if (tmp == NULL) {
        return -ENOMEM;
    }

    h_pass_scalar(src, w, h, tmp, w_out);
    v_pass_scalar(tmp, h, dst, w_out, h_out);

    free(tmp);

    if (rw != NULL) {
        *rw = w_out;
    }
    if (rh != NULL) {
        *rh = h_out;
    }
    return 0;
}

/*
 * Runtime dispatch: pick the fastest bit-identical implementation
 * available on the host CPU. AVX2 / AVX-512 kernels produce
 * byte-identical output to the scalar reference (per-lane sequential
 * FMA, same coefficients, same mirror). See
 * libvmaf/test/test_ms_ssim_decimate.c.
 */
int ms_ssim_decimate(const float *src, int w, int h, float *dst, int *rw, int *rh)
{
    VMAF_ASSERT_DEBUG(src != NULL);
    VMAF_ASSERT_DEBUG(dst != NULL);
    VMAF_ASSERT_DEBUG(src != dst);
    VMAF_ASSERT_DEBUG(w > 0);
    VMAF_ASSERT_DEBUG(h > 0);
#if ARCH_X86 || ARCH_AARCH64
    const unsigned flags = vmaf_get_cpu_flags();
#if ARCH_X86 && HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        return ms_ssim_decimate_avx512(src, w, h, dst, rw, rh);
    }
#endif
#if ARCH_X86
    if (flags & VMAF_X86_CPU_FLAG_AVX2) {
        return ms_ssim_decimate_avx2(src, w, h, dst, rw, rh);
    }
#endif
#if ARCH_AARCH64
    if (flags & VMAF_ARM_CPU_FLAG_NEON) {
        return ms_ssim_decimate_neon(src, w, h, dst, rw, rh);
    }
#endif
#endif
    return ms_ssim_decimate_scalar(src, w, h, dst, rw, rh);
}
