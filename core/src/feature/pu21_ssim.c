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
 * PU21-local SSIM at an arbitrary data range L. Self-contained scalar SSIM —
 * deliberately NOT a modification of the shared iqa_ssim() (which hardcodes
 * L = 255 and feeds the Netflix golden assertions). It reuses only the
 * read-only Gaussian-window table (g_gaussian_window*) and the read-only
 * iqa_convolve() separable-blur helper.
 *
 * The algorithm mirrors iqa_ssim()'s default (non-args) Gaussian path
 * step-for-step (11x11 normalised Gaussian window, KBND_SYMMETRIC border,
 * zli-nflx variance clamp, zli-nflx flat-region sigma_both clamp), with the
 * single difference that the SSIM stabilisation constants C1=(0.01*L)^2 and
 * C2=(0.03*L)^2 use the caller-supplied L.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

#include "iqa/convolve.h"
#include "iqa/ssim_tools.h"
#include "pu21_ssim.h"

#define PU21_SSIM_MAX(x, y) (((x) > (y)) ? (x) : (y))

/* Build the read-only 11x11 normalised Gaussian window descriptor. The kernel
 * tables are `static const` in ssim_tools.h; the iqa_kernel pointers are
 * non-const for upstream API compatibility, so cast away const for read-only
 * use (iqa_convolve never writes the kernel). */
static void pu21_ssim_init_window(struct iqa_kernel *window)
{
    window->kernel = (float *)g_gaussian_window;
    window->kernel_h = (float *)g_gaussian_window_h;
    window->kernel_v = (float *)g_gaussian_window_v;
    window->w = window->h = GAUSSIAN_LEN;
    window->normalized = 1;
    window->bnd_opt = KBND_SYMMETRIC;
    window->bnd_const = 0.0f;
}

struct pu21_ssim_workspace {
    float *ref_mu;
    float *cmp_mu;
    float *ref_sigma_sqd;
    float *cmp_sigma_sqd;
    float *sigma_both;
};

static int pu21_ssim_workspace_alloc(struct pu21_ssim_workspace *ws, size_t n)
{
    ws->ref_mu = (float *)malloc(n * sizeof(float));
    ws->cmp_mu = (float *)malloc(n * sizeof(float));
    ws->ref_sigma_sqd = (float *)malloc(n * sizeof(float));
    ws->cmp_sigma_sqd = (float *)malloc(n * sizeof(float));
    ws->sigma_both = (float *)malloc(n * sizeof(float));
    return (ws->ref_mu && ws->cmp_mu && ws->ref_sigma_sqd && ws->cmp_sigma_sqd && ws->sigma_both) ?
               0 :
               -1;
}

static void pu21_ssim_workspace_free(struct pu21_ssim_workspace *ws)
{
    /* free(NULL) is a well-defined no-op (C89 §7.20.3.2). */
    free(ws->ref_mu);
    free(ws->cmp_mu);
    free(ws->ref_sigma_sqd);
    free(ws->cmp_sigma_sqd);
    free(ws->sigma_both);
}

/* Blur mu / squared / cross terms, then convert E[x^2] -> variance (clamped to
 * >= 0, zli-nflx). On return cw/ch hold the valid (interior) region size.
 *
 * iqa_convolve packs its output CONTIGUOUSLY at the reduced stride dst_w
 * (= w-kw+1), not at the source stride w (see iqa_convolve_vertical_pass:
 * dst[y*dst_w + x]). Every blurred buffer therefore occupies the top
 * cw*ch contiguous floats, and the reduction must index with stride cw. */
static void pu21_ssim_compute_stats(struct pu21_ssim_workspace *ws, float *ref_f, float *cmp_f,
                                    int w, int h, const struct iqa_kernel *window, int *cw, int *ch)
{
    const size_t n = (size_t)w * (size_t)h;

    /* mu = blur(img); packed contiguously at the reduced stride. */
    iqa_convolve(ref_f, w, h, window, ws->ref_mu, 0, 0);
    iqa_convolve(cmp_f, w, h, window, ws->cmp_mu, 0, 0);

    /* precompute ref^2, cmp^2, ref*cmp over the full plane. */
    for (size_t i = 0; i < n; i++) {
        ws->ref_sigma_sqd[i] = ref_f[i] * ref_f[i];
        ws->cmp_sigma_sqd[i] = cmp_f[i] * cmp_f[i];
        ws->sigma_both[i] = ref_f[i] * cmp_f[i];
    }

    /* blur the squared/cross terms; the last call updates cw/ch to the valid
     * region returned by iqa_convolve (w-kw+1, h-kh+1). */
    iqa_convolve(ws->ref_sigma_sqd, w, h, window, ws->ref_sigma_sqd, 0, 0);
    iqa_convolve(ws->cmp_sigma_sqd, w, h, window, ws->cmp_sigma_sqd, 0, 0);
    iqa_convolve(ws->sigma_both, w, h, window, ws->sigma_both, cw, ch);
}

/* Accumulate mean l*c*s over the valid region. The blurred buffers are packed
 * contiguously at stride cw (= w-kw+1), so the region is the leading cw*ch
 * floats — iterate it as a single contiguous span. */
static double pu21_ssim_accumulate(const struct pu21_ssim_workspace *ws, int cw, int ch, float C1,
                                   float C2, float C3)
{
    double ssim_sum = 0.0;
    const size_t valid = (size_t)cw * (size_t)ch;
    for (size_t off = 0; off < valid; off++) {
        float ref_var = ws->ref_sigma_sqd[off] - ws->ref_mu[off] * ws->ref_mu[off];
        float cmp_var = ws->cmp_sigma_sqd[off] - ws->cmp_mu[off] * ws->cmp_mu[off];
        ref_var = PU21_SSIM_MAX(0.0f, ref_var);
        cmp_var = PU21_SSIM_MAX(0.0f, cmp_var);
        const float sigma_both = ws->sigma_both[off] - ws->ref_mu[off] * ws->cmp_mu[off];

        const float sigma_ref_sigma_cmp = sqrtf(ref_var * cmp_var);
        const double l =
            (2.0 * ws->ref_mu[off] * ws->cmp_mu[off] + C1) /
            (ws->ref_mu[off] * ws->ref_mu[off] + ws->cmp_mu[off] * ws->cmp_mu[off] + C1);
        const double c =
            (2.0 * sigma_ref_sigma_cmp + C2) / ((double)ref_var + (double)cmp_var + C2);
        /* zli-nflx flat-region clamp: when ref == cmp and the filtered region
         * is flat (zero std), sigma_both can go slightly negative via float
         * rounding while sigma_ref_sigma_cmp is zero; clamp so s stays at 1.0. */
        const float clamped_sigma_both =
            (sigma_both < 0.0f && sigma_ref_sigma_cmp <= 0.0f) ? 0.0f : sigma_both;
        const double s = (clamped_sigma_both + C3) / (sigma_ref_sigma_cmp + C3);
        ssim_sum += l * c * s;
    }
    return ssim_sum;
}

/* Cross-TU: declared in pu21_ssim.h, called from pu21.c. clang-tidy
 * misc-use-internal-linkage runs per-TU and can't see the header bridge. */
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU helper — external linkage required (ADR-0278).
int pu21_compute_ssim(const double *ref, const double *dist, int w, int h, double L, double *score)
{
    if (!ref || !dist || !score)
        return -EINVAL;
    if (w < GAUSSIAN_LEN || h < GAUSSIAN_LEN)
        return -EINVAL;

    struct iqa_kernel window;
    pu21_ssim_init_window(&window);

    const size_t n = (size_t)w * (size_t)h;

    /* Working copies of the input planes converted to float (iqa_convolve
     * operates on float and overwrites/derives in place via the result
     * buffers). SSIM is a ratio near 1.0, not a difference of large
     * nearly-equal values, so the float blur is numerically adequate (matches
     * the fork's float SSIM kernel). */
    float *ref_f = (float *)malloc(n * sizeof(float));
    float *cmp_f = (float *)malloc(n * sizeof(float));
    struct pu21_ssim_workspace ws = {0};
    int ret = -ENOMEM;

    if (!ref_f || !cmp_f)
        goto cleanup;
    if (pu21_ssim_workspace_alloc(&ws, n) != 0)
        goto cleanup;

    for (size_t i = 0; i < n; i++) {
        ref_f[i] = (float)ref[i];
        cmp_f[i] = (float)dist[i];
    }

    int cw = w;
    int ch = h;
    pu21_ssim_compute_stats(&ws, ref_f, cmp_f, w, h, &window, &cw, &ch);

    /* SSIM stabilisation constants use the caller-supplied data range L
     * (C1=(0.01*L)^2, C2=(0.03*L)^2); pu21 passes L=256. */
    const float C1 = (float)((0.01 * L) * (0.01 * L));
    const float C2 = (float)((0.03 * L) * (0.03 * L));
    const float C3 = C2 / 2.0f;

    const double ssim_sum = pu21_ssim_accumulate(&ws, cw, ch, C1, C2, C3);

    *score = ssim_sum / ((double)cw * (double)ch);
    ret = 0;

cleanup:
    pu21_ssim_workspace_free(&ws);
    free(ref_f);
    free(cmp_f);
    return ret;
}
