/*
 * Copyright (c) 2011, Tom Distler (http://tdistler.com)
 * All rights reserved.
 *
 * The BSD License
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * - Neither the name of the tdistler.com nor the names of its contributors may
 *   be used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * (06/10/2016) Updated by zli-nflx (zli@netflix.com) to optimize iqa_convolve.
 */

#include <stddef.h>
#include <stdlib.h>
#include "convolve.h"
#include "iqa_options.h"

float KBND_SYMMETRIC(const float *img, int w, int h, int x, int y, float bnd_const)
{
    /*
     * Period-based symmetric extension (period = 2*n). Matches the
     * single-reflect form used previously whenever the offset is ≤ n,
     * and additionally handles the sub-kernel-radius regime where
     * |x| > w or |y| > h — cases that only occur when the input
     * dimension is smaller than the kernel half-width, and which a
     * single reflection leaves out of bounds. See
     * docs/adr/0125-ms-ssim-decimate-simd.md.
     */
    (void)bnd_const;
    const int px = 2 * w;
    int rx = x % px;
    if (rx < 0) {
        rx += px;
    }
    if (rx >= w) {
        rx = px - rx - 1;
    }
    const int py = 2 * h;
    int ry = y % py;
    if (ry < 0) {
        ry += py;
    }
    if (ry >= h) {
        ry = py - ry - 1;
    }
    return img[ry * w + rx];
}

float KBND_REPLICATE(const float *img, int w, int h, int x, int y, float bnd_const)
{
    (void)bnd_const;
    if (x < 0)
        x = 0;
    if (x >= w)
        x = w - 1;
    if (y < 0)
        y = 0;
    if (y >= h)
        y = h - 1;
    return img[y * w + x];
}

float KBND_CONSTANT(const float *img, int w, int h, int x, int y, float bnd_const)
{
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x >= w || y >= h)
        return bnd_const;
    return img[y * w + x];
}

static float iqa_calc_scale(const struct iqa_kernel *k)
{
    if (k->normalized)
        return 1.0f;

    /*
     * Upstream zli-nflx left scale computation for non-normalised 1D separable
     * kernels unimplemented.  All callers in this fork set k->normalized = true
     * (Gaussian blur with pre-normalised coefficients), so this path is
     * unreachable in practice.  Return a safe neutral scale (1.0f) instead of
     * aborting — JPL Rule 14 forbids assert(0) where a recoverable value
     * exists (ADR-0012).  Dead code below removed to prevent compiler warnings.
     */
    return 1.0f;
}

#ifdef IQA_CONVOLVE_1D

/* Horizontal pass of the 1D-separable convolve — fills img_cache rows that
 * the v-pass will later read. Rows beyond `dst_h + vc - kh_even` are never
 * consumed; stopping there avoids an OOB row on even-tap kernels (see
 * docs/rebase-notes.md §0033). */
static void iqa_convolve_horizontal_pass(const float *img, int w, const struct iqa_kernel *k,
                                         float *img_cache, int dst_w, int dst_h, float scale)
{
    const int uc = k->w / 2;
    const int vc = k->h / 2;
    const int kw_even = (k->w & 1) ? 0 : 1;
    const int kh_even = (k->h & 1) ? 0 : 1;
    for (int y = -vc; y < dst_h + vc - kh_even; ++y) {
        for (int x = 0; x < dst_w; ++x) {
            double sum = 0.0;
            int k_offset = 0;
            const int ky = y + vc;
            const int kx = x + uc;
            const ptrdiff_t img_offset = (ptrdiff_t)ky * w + kx;
            for (int u = -uc; u <= uc - kw_even; ++u, ++k_offset) {
                sum += (double)img[img_offset + u] * k->kernel_h[k_offset];
            }
            img_cache[img_offset] = (float)(sum * scale);
        }
    }
}

static void iqa_convolve_vertical_pass(const float *img_cache, int w, const struct iqa_kernel *k,
                                       float *dst, int dst_w, int dst_h, float scale)
{
    const int uc = k->w / 2;
    const int vc = k->h / 2;
    const int kh_even = (k->h & 1) ? 0 : 1;
    for (int x = 0; x < dst_w; ++x) {
        for (int y = 0; y < dst_h; ++y) {
            double sum = 0.0;
            int k_offset = 0;
            const int ky = y + vc;
            const int kx = x + uc;
            const ptrdiff_t img_offset = (ptrdiff_t)ky * w + kx;
            for (int v = -vc; v <= vc - kh_even; ++v, ++k_offset) {
                sum += (double)img_cache[img_offset + (ptrdiff_t)v * w] * k->kernel_v[k_offset];
            }
            dst[y * dst_w + x] = (float)(sum * scale);
        }
    }
}

/* Returns 0 on success, -1 on OOM (JPL-CERT MEM52-CPP: assert(0) on allocation
 * failure is forbidden in production C — ADR-0012). */
static int iqa_convolve_1d_separable(float *img, int w, int h, const struct iqa_kernel *k,
                                     float *result, int dst_w, int dst_h)
{
    const float scale = iqa_calc_scale(k);
    float *img_cache = (float *)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!img_cache)
        return -1;

    float *dst = result ? result : img;
    iqa_convolve_horizontal_pass(img, w, k, img_cache, dst_w, dst_h, scale);
    iqa_convolve_vertical_pass(img_cache, w, k, dst, dst_w, dst_h, scale);
    free(img_cache);
    return 0;
}

#else /* use 2D filter */

static void iqa_convolve_2d(float *img, int w, const struct iqa_kernel *k, float *result, int dst_w,
                            int dst_h)
{
    const int uc = k->w / 2;
    const int vc = k->h / 2;
    const int kw_even = (k->w & 1) ? 0 : 1;
    const int kh_even = (k->h & 1) ? 0 : 1;
    const float scale = iqa_calc_scale(k);
    float *dst = result ? result : img;

    for (int y = 0; y < dst_h; ++y) {
        for (int x = 0; x < dst_w; ++x) {
            float sum = 0.0f;
            int k_offset = 0;
            const int ky = y + vc;
            const int kx = x + uc;
            for (int v = -vc; v <= vc - kh_even; ++v) {
                const int img_offset = (ky + v) * w + kx;
                for (int u = -uc; u <= uc - kw_even; ++u, ++k_offset) {
                    sum += img[img_offset + u] * k->kernel[k_offset];
                }
            }
            dst[y * dst_w + x] = (float)(sum * scale);
        }
    }
}

#endif

void iqa_convolve(float *img, int w, int h, const struct iqa_kernel *k, float *result, int *rw,
                  int *rh)
{
    const int dst_w = w - k->w + 1;
    const int dst_h = h - k->h + 1;

#ifdef IQA_CONVOLVE_1D
    /* Discard OOM return: iqa_convolve is a void API; the caller sees an
     * all-zero destination frame on failure (calloc-initialised cache was
     * never written).  A real fix would return int, but that is an
     * upstream-incompatible ABI change deferred to a separate ADR. */
    (void)iqa_convolve_1d_separable(img, w, h, k, result, dst_w, dst_h);
#else
    (void)h;
    iqa_convolve_2d(img, w, k, result, dst_w, dst_h);
#endif

    if (rw)
        *rw = dst_w;
    if (rh)
        *rh = dst_h;
}

int iqa_img_filter(float *img, int w, int h, const struct iqa_kernel *k, float *result)
{
    int x;
    int y;
    int img_offset;
    float *dst = result;

    if (!k || !k->bnd_opt)
        return 1;

    if (!dst) {
        dst = (float *)malloc((size_t)w * (size_t)h * sizeof(float));
        if (!dst)
            return 2;
    }

    const float scale = iqa_calc_scale(k);

    /* Kernel is applied to all positions where top-left corner is in the image */
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            dst[y * w + x] = iqa_filter_pixel(img, w, h, x, y, k, scale);
        }
    }

    /* If no result buffer given, copy results to image buffer */
    if (!result) {
        for (y = 0; y < h; ++y) {
            img_offset = y * w;
            for (x = 0; x < w; ++x, ++img_offset) {
                img[img_offset] = dst[img_offset];
            }
        }
        free(dst);
    }
    return 0;
}

float iqa_filter_pixel(const float *img, int w, int h, int x, int y, const struct iqa_kernel *k,
                       const float kscale)
{
    if (!k)
        return img[y * w + x];

    const int uc = k->w / 2;
    const int vc = k->h / 2;
    const int kw_even = (k->w & 1) ? 0 : 1;
    const int kh_even = (k->h & 1) ? 0 : 1;
    const int x_edge_left = uc;
    const int x_edge_right = w - uc;
    const int y_edge_top = vc;
    const int y_edge_bottom = h - vc;

    const int edge =
        (x < x_edge_left || y < y_edge_top || x >= x_edge_right || y >= y_edge_bottom) ? 1 : 0;

    double sum = 0.0;
    int k_offset = 0;
    for (int v = -vc; v <= vc - kh_even; ++v) {
        const ptrdiff_t img_offset = (ptrdiff_t)(y + v) * w + x;
        for (int u = -uc; u <= uc - kw_even; ++u, ++k_offset) {
            if (!edge) {
                sum += (double)img[img_offset + u] * k->kernel[k_offset];
            } else {
                sum +=
                    (double)k->bnd_opt(img, w, h, x + u, y + v, k->bnd_const) * k->kernel[k_offset];
            }
        }
    }
    return (float)(sum * kscale);
}
