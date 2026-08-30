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

#include "mem.h"
#include "iqa/math_utils.h"
#include "iqa/decimate.h"
#include "iqa/ssim_tools.h"
#include "ms_ssim_decimate.h"

/*
 * MS-SSIM 9-tap 9/7 biorthogonal wavelet LPF coefficients moved to
 * libvmaf/src/feature/ms_ssim_decimate.c (separable form). The 2-D
 * `g_lpf` array in upstream Netflix/vmaf ms_ssim.c is no longer used
 * on this fork because the decimate path switched from
 * `iqa_decimate(..., 2, &lpf_2d, ...)` to the separable scalar-FMA
 * path `ms_ssim_decimate`. See ADR-0125.
 *
 * REBASE-SENSITIVE INVARIANT: if Netflix upstream modifies `g_lpf`,
 * `g_lpf_h`, or `g_lpf_v` in this file during a sync, mirror the
 * change to `ms_ssim_lpf_h` / `ms_ssim_lpf_v` in ms_ssim_decimate.c.
 * See `libvmaf/src/feature/AGENTS.md` and docs/rebase-notes.md.
 */
#define LPF_LEN 9

/* Alpha, beta, and gamma values for each scale */
static float g_alphas[] = {0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.1333f};
static float g_betas[] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};
static float g_gammas[] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};

struct ms_ssim_context {
    double l; /* Luminance */
    double c; /* Contrast */
    double s; /* Structure */
    float alpha;
    float beta;
    float gamma;
};

/* Called for each pixel */
static int ms_ssim_map_fn(const struct iqa_ssim_int *si, void *ctx)
{
    struct ms_ssim_context *ms_ctx = (struct ms_ssim_context *)ctx;
    ms_ctx->l += si->l;
    ms_ctx->c += si->c;
    ms_ctx->s += si->s;
    return 0;
}

/* Called to calculate the final result */
static float ms_ssim_reduce_fn(int w, int h, void *ctx)
{
    /* Cast before multiply: w*h overflows int32 for widths > 46340.  Cast
     * each operand so the multiplication is done in double precision.       */
    double size = (double)w * (double)h;
    struct ms_ssim_context *ms_ctx = (struct ms_ssim_context *)ctx;
    ms_ctx->l = pow(ms_ctx->l / size, (double)ms_ctx->alpha);
    ms_ctx->c = pow(ms_ctx->c / size, (double)ms_ctx->beta);
    ms_ctx->s = pow(fabs(ms_ctx->s / size), (double)ms_ctx->gamma);
    return (float)(ms_ctx->l * ms_ctx->c * ms_ctx->s);
}

/* Releases the scaled buffers */
static void ms_ssim_free_buffers(float **buf, int scales)
{
    int idx;
    for (idx = 0; idx < scales; ++idx)
        free(buf[idx]);
}

/* Allocates the scaled buffers. If error, all buffers are free'd */
static int ms_ssim_alloc_buffers(float **buf, int w, int h, int scales)
{
    int idx;
    int cur_w = w;
    int cur_h = h;
    for (idx = 0; idx < scales; ++idx) {
        buf[idx] = (float *)malloc((size_t)cur_w * (size_t)cur_h * sizeof(float));
        if (!buf[idx]) {
            ms_ssim_free_buffers(buf, idx);
            return 1;
        }
        cur_w = cur_w / 2 + (cur_w & 1);
        cur_h = cur_h / 2 + (cur_h & 1);
    }
    return 0;
}

/* Verify that no scale drops below the kernel footprint. */
static int ms_ssim_check_scale_ok(int w, int h, int scales, int gauss)
{
    int cur_w = w;
    int cur_h = h;
    for (int idx = 0; idx < scales; ++idx) {
        if (gauss ? cur_w < GAUSSIAN_LEN || cur_h < GAUSSIAN_LEN :
                    cur_w < LPF_LEN || cur_h < LPF_LEN) {
            (void)printf("error: scale below 1x1!\n");
            return 1;
        }
        cur_w /= 2;
        cur_h /= 2;
    }
    return 0;
}

/* Populate the SSIM window struct with the selected (square or gaussian) kernel. */
static void ms_ssim_init_window(struct iqa_kernel *window, int gauss)
{
    window->kernel = (float *)g_square_window;
    window->kernel_h = (float *)g_square_window_h; /* zli-nflx */
    window->kernel_v = (float *)g_square_window_v; /* zli-nflx */
    window->w = window->h = SQUARE_LEN;
    window->normalized = 1;
    window->bnd_opt = KBND_SYMMETRIC;
    if (gauss) {
        window->kernel = (float *)g_gaussian_window;
        window->kernel_h = (float *)g_gaussian_window_h; /* zli-nflx */
        window->kernel_v = (float *)g_gaussian_window_v; /* zli-nflx */
        window->w = window->h = GAUSSIAN_LEN;
    }
}

/* Allocate the per-scale image pointer arrays and their backing storage.
 * Returns 0 on success; frees everything and returns 1 on any failure. */
static int ms_ssim_alloc_pyramids(float ***ref_imgs_out, float ***cmp_imgs_out, int w, int h,
                                  int scales)
{
    float **ref_imgs = (float **)malloc((size_t)scales * sizeof(float *));
    float **cmp_imgs = (float **)malloc((size_t)scales * sizeof(float *));
    if (!ref_imgs || !cmp_imgs) {
        free((void *)ref_imgs);
        free((void *)cmp_imgs);
        (void)printf("error: unable to malloc ref_imgs or cmp_imgs.\n");
        (void)fflush(stdout);
        return 1;
    }
    if (ms_ssim_alloc_buffers(ref_imgs, w, h, scales)) {
        free((void *)ref_imgs);
        free((void *)cmp_imgs);
        (void)printf("error: unable to ms_ssim_alloc_buffers on ref_imgs.\n");
        (void)fflush(stdout);
        return 1;
    }
    if (ms_ssim_alloc_buffers(cmp_imgs, w, h, scales)) {
        ms_ssim_free_buffers(ref_imgs, scales);
        free((void *)ref_imgs);
        free((void *)cmp_imgs);
        (void)printf("error: unable to ms_ssim_alloc_buffers on cmp_imgs.\n");
        (void)fflush(stdout);
        return 1;
    }
    *ref_imgs_out = ref_imgs;
    *cmp_imgs_out = cmp_imgs;
    return 0;
}

/* Release both pyramids and their pointer arrays. */
static void ms_ssim_free_pyramids(float **ref_imgs, float **cmp_imgs, int scales)
{
    ms_ssim_free_buffers(ref_imgs, scales);
    ms_ssim_free_buffers(cmp_imgs, scales);
    free((void *)ref_imgs);
    free((void *)cmp_imgs);
}

/* Copy the input planes into the first level of each pyramid, forcing stride = w. */
static void ms_ssim_seed_pyramid(const float *ref, const float *cmp, int w, int h, int stride,
                                 float *ref_dst, float *cmp_dst)
{
    for (int y = 0; y < h; ++y) {
        int src_offset = y * stride;
        int offset = y * w;
        for (int x = 0; x < w; ++x, ++offset, ++src_offset) {
            ref_dst[offset] = (float)ref[src_offset];
            cmp_dst[offset] = (float)cmp[src_offset];
        }
    }
}

/* Build the decimation pyramid via the separable path (ADR-0125). */
static int ms_ssim_build_pyramids(float **ref_imgs, float **cmp_imgs, int w, int h, int scales)
{
    int cur_w = w;
    int cur_h = h;
    for (int idx = 1; idx < scales; ++idx) {
        if (ms_ssim_decimate(ref_imgs[idx - 1], cur_w, cur_h, ref_imgs[idx], 0, 0) ||
            ms_ssim_decimate(cmp_imgs[idx - 1], cur_w, cur_h, cmp_imgs[idx], &cur_w, &cur_h)) {
            (void)printf("error: decimation fails on ref_imgs or cmp_imgs.\n");
            (void)fflush(stdout);
            return 1;
        }
    }
    return 0;
}

/* Run a single scale of the SSIM reduction, selecting the Wang vs Rouse/Hemami
 * variant as upstream does. Fills the per-component scores (l, c, s). */
static void ms_ssim_run_scale(float *ref_img, float *cmp_img, int cur_w, int cur_h,
                              struct iqa_kernel *window, struct iqa_map_reduce *mr,
                              struct ms_ssim_context *ms_ctx, int wang, float *l, float *c,
                              float *s)
{
    if (!wang) {
        /* MS-SSIM* (Rouse/Hemami) */
        struct iqa_ssim_args s_args;
        s_args.alpha = 1.0f;
        s_args.beta = 1.0f;
        s_args.gamma = 1.0f;
        s_args.K1 = 0.0f; /* Force stabilization constants to 0 */
        s_args.K2 = 0.0f;
        s_args.L = 255;
        s_args.f = 1; /* Don't resize */
        mr->context = ms_ctx;
        iqa_ssim(ref_img, cmp_img, cur_w, cur_h, window, mr, &s_args, l, c, s);
    } else {
        /* MS-SSIM (Wang) — default parameters (args=NULL) per upstream. */
        iqa_ssim(ref_img, cmp_img, cur_w, cur_h, window, NULL, NULL, l, c, s);
    }
}

/* Iterate scales and produce the product-of-scales MS-SSIM score plus the
 * per-scale l/c/s components. Returns 0 on success; returns 1 (and leaves
 * msssim_out untouched) if the product reaches INFINITY mid-loop. */
static int ms_ssim_score_scales(float **ref_imgs, float **cmp_imgs, int w, int h, int scales,
                                int wang, const float *alphas, const float *betas,
                                const float *gammas, struct iqa_kernel *window,
                                struct iqa_map_reduce *mr, double *l_scores, double *c_scores,
                                double *s_scores, double *msssim_out)
{
    int cur_w = w;
    int cur_h = h;
    double msssim = 1.0;
    for (int idx = 0; idx < scales; ++idx) {
        struct ms_ssim_context ms_ctx;
        ms_ctx.l = 0;
        ms_ctx.c = 0;
        ms_ctx.s = 0;
        ms_ctx.alpha = alphas[idx];
        ms_ctx.beta = betas[idx];
        ms_ctx.gamma = gammas[idx];

        float l;
        float c;
        float s;
        ms_ssim_run_scale(ref_imgs[idx], cmp_imgs[idx], cur_w, cur_h, window, mr, &ms_ctx, wang, &l,
                          &c, &s);

        /* Guard pow(negative, fractional_exponent) which yields NaN for l or
         * c < 0 (can occur on heavily distorted synthetic frames).  fabs()
         * mirrors the same guard already present in ms_ssim_reduce_fn for s
         * (see Rouse path at line ~80).                                    */
        msssim *= pow(fabs((double)l), (double)alphas[idx]) *
                  pow(fabs((double)c), (double)betas[idx]) *
                  pow(fabs((double)s), (double)gammas[idx]);
        l_scores[idx] = l;
        c_scores[idx] = c;
        s_scores[idx] = s;

        if (msssim == INFINITY) {
            (void)printf("error: ms_ssim is INFINITY.\n");
            (void)fflush(stdout);
            return 1;
        }
        cur_w = cur_w / 2 + (cur_w & 1);
        cur_h = cur_h / 2 + (cur_h & 1);
    }
    *msssim_out = msssim;
    return 0;
}

/* Cross-TU: declared in ms_ssim.h, called from float_ms_ssim.c.
 * clang-tidy misc-use-internal-linkage runs per-TU and can't see the
 * header bridge. Suppression is a rebase-invariant per ADR-0278. */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
int compute_ms_ssim(const float *ref, const float *cmp, int w, int h, int ref_stride,
                    int cmp_stride, double *score, double *l_scores, double *c_scores,
                    double *s_scores)
{
    const int wang = 1; /* set default to wang's ms_ssim */
    const int scales = SCALES;
    const int gauss = 1;

    /* check stride */
    int stride = ref_stride; /* stride in bytes */
    if (stride != cmp_stride) {
        (void)printf("error: for ms_ssim, ref_stride (%d) != dis_stride (%d) bytes.\n", ref_stride,
                     cmp_stride);
        (void)fflush(stdout);
        return 1;
    }
    stride /= (int)sizeof(float); /* stride_ in pixels */

    if (ms_ssim_check_scale_ok(w, h, scales, gauss))
        return 1;

    struct iqa_kernel window;
    ms_ssim_init_window(&window, gauss);

    struct iqa_map_reduce mr;
    mr.map = ms_ssim_map_fn;
    mr.reduce = ms_ssim_reduce_fn;

    float **ref_imgs = NULL;
    float **cmp_imgs = NULL;
    if (ms_ssim_alloc_pyramids(&ref_imgs, &cmp_imgs, w, h, scales))
        return 1;

    ms_ssim_seed_pyramid(ref, cmp, w, h, stride, ref_imgs[0], cmp_imgs[0]);

    if (ms_ssim_build_pyramids(ref_imgs, cmp_imgs, w, h, scales)) {
        ms_ssim_free_pyramids(ref_imgs, cmp_imgs, scales);
        return 1;
    }

    double msssim = 1.0;
    if (ms_ssim_score_scales(ref_imgs, cmp_imgs, w, h, scales, wang, g_alphas, g_betas, g_gammas,
                             &window, &mr, l_scores, c_scores, s_scores, &msssim)) {
        ms_ssim_free_pyramids(ref_imgs, cmp_imgs, scales);
        return 1;
    }

    ms_ssim_free_pyramids(ref_imgs, cmp_imgs, scales);
    *score = msssim;
    return 0;
}
