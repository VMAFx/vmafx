/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "iqa/math_utils.h"
#include "iqa/decimate.h"
#include "iqa/ssim_tools.h"

static void ssim_init_window(struct iqa_kernel *window, int gaussian)
{
    window->kernel = (float *)g_square_window;
    window->kernel_h = (float *)g_square_window_h;
    window->kernel_v = (float *)g_square_window_v;
    window->w = window->h = SQUARE_LEN;
    window->normalized = 1;
    window->bnd_opt = KBND_SYMMETRIC;
    if (gaussian) {
        window->kernel = (float *)g_gaussian_window;
        window->kernel_h = (float *)g_gaussian_window_h;
        window->kernel_v = (float *)g_gaussian_window_v;
        window->w = window->h = GAUSSIAN_LEN;
    }
}

static void ssim_convert_input(const float *ref, const float *cmp, int w, int h, int stride,
                               float *ref_f, float *cmp_f)
{
    for (int y = 0; y < h; ++y) {
        int src_offset = y * stride;
        int offset = y * w;
        for (int x = 0; x < w; ++x, ++offset, ++src_offset) {
            ref_f[offset] = (float)ref[src_offset];
            cmp_f[offset] = (float)cmp[src_offset];
        }
    }
}

static int ssim_low_pass_alloc(struct iqa_kernel *low_pass, int scale)
{
    low_pass->kernel = (float *)malloc((size_t)scale * (size_t)scale * sizeof(float));
    low_pass->kernel_h = (float *)malloc((size_t)scale * sizeof(float));
    low_pass->kernel_v = (float *)malloc((size_t)scale * sizeof(float));
    if (!(low_pass->kernel && low_pass->kernel_h && low_pass->kernel_v)) {
        free(low_pass->kernel);
        free(low_pass->kernel_h);
        free(low_pass->kernel_v);
        return -1;
    }
    low_pass->w = low_pass->h = scale;
    low_pass->normalized = 0;
    low_pass->bnd_opt = KBND_SYMMETRIC;
    const float inv = 1.0f / (float)scale;
    const float inv2 = 1.0f / (float)(scale * scale);
    for (int i = 0; i < scale * scale; ++i)
        low_pass->kernel[i] = inv2;
    for (int i = 0; i < scale; ++i) {
        low_pass->kernel_h[i] = inv;
        low_pass->kernel_v[i] = inv;
    }
    return 0;
}

static void ssim_low_pass_free(struct iqa_kernel *low_pass)
{
    free(low_pass->kernel);
    free(low_pass->kernel_h);
    free(low_pass->kernel_v);
}

static int ssim_decimate_pair(float *ref_f, float *cmp_f, int *w, int *h, int scale,
                              struct iqa_kernel *low_pass)
{
    if (ssim_low_pass_alloc(low_pass, scale) != 0)
        return -1;
    int err = iqa_decimate(ref_f, *w, *h, scale, low_pass, 0, 0, 0) ||
              iqa_decimate(cmp_f, *w, *h, scale, low_pass, 0, w, h);
    ssim_low_pass_free(low_pass);
    return err ? -1 : 0;
}

/* Cross-TU: declared in ssim.h, called from float_ssim.c. clang-tidy
 * misc-use-internal-linkage runs per-TU and can't see the header bridge. */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
int compute_ssim(const float *ref, const float *cmp, int w, int h, int ref_stride, int cmp_stride,
                 double *score, double *l_score, double *c_score, double *s_score,
                 int scale_override)
{
    int ret = 1;
    float result = INFINITY;
    float l;
    float c;
    float s;

    if (ref_stride != cmp_stride) {
        printf("error: for ssim, ref_stride (%d) != dis_stride (%d) bytes.\n", ref_stride,
               cmp_stride);
        (void)fflush(stdout);
        return ret;
    }
    const int stride = ref_stride / (int)sizeof(float); /* in pixels */

    /* args is hardcoded NULL (default Gaussian SSIM); the args branch
     * is preserved for upstream-parity readability. */
    const struct iqa_ssim_args *args = 0;
    const int gaussian = 1;
    int scale = (scale_override > 0) ? scale_override : _max(1, _round((float)_min(w, h) / 256.0f));

    struct iqa_kernel window;
    ssim_init_window(&window, gaussian);

    float *ref_f = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    float *cmp_f = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
    if (!ref_f || !cmp_f) {
        free(ref_f);
        free(cmp_f);
        printf("error: unable to malloc ref_f or cmp_f.\n");
        (void)fflush(stdout);
        return ret;
    }
    ssim_convert_input(ref, cmp, w, h, stride, ref_f, cmp_f);

    if (scale > 1) {
        struct iqa_kernel low_pass;
        if (ssim_decimate_pair(ref_f, cmp_f, &w, &h, scale, &low_pass) != 0) {
            free(ref_f);
            free(cmp_f);
            printf("error: decimation fails on ref_f or cmp_f.\n");
            (void)fflush(stdout);
            return ret;
        }
    }

    result = iqa_ssim(ref_f, cmp_f, w, h, &window, NULL, args, &l, &c, &s);

    free(ref_f);
    free(cmp_f);

    *score = (double)result;
    *l_score = (double)l;
    *c_score = (double)c;
    *s_score = (double)s;

    return 0;
}
