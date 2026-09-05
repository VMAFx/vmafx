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
 * (06/10/2016) Updated by zli-nflx (zli@netflix.com) to output mean luminence,
 * contrast and structure.
 */

#include <stdlib.h>
#include <math.h>
#include <stddef.h>
/* assert.h intentionally omitted: the assert(!args) guard was removed in
 * favour of a runtime INFINITY return in iqa_ssim (see ADR-1033). */
#include <pthread.h>

#include "iqa.h"
#include "convolve.h"
#include "ssim_tools.h"
#include "ssim_simd.h"

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))

/* SIMD dispatch function pointers (set via iqa_ssim_set_dispatch) */
static ssim_precompute_fn g_ssim_precompute = NULL;
static ssim_variance_fn g_ssim_variance = NULL;
static ssim_accumulate_fn g_ssim_accumulate = NULL;

void iqa_ssim_set_dispatch(ssim_precompute_fn precompute, ssim_variance_fn variance,
                           ssim_accumulate_fn accumulate)
{
    g_ssim_precompute = precompute;
    g_ssim_variance = variance;
    g_ssim_accumulate = accumulate;
}

/* SIMD dispatch for iqa_convolve (see ADR-0138) */
static iqa_convolve_fn g_iqa_convolve = NULL;

void iqa_convolve_set_dispatch(iqa_convolve_fn convolve)
{
    g_iqa_convolve = convolve;
}

/* One-shot guard for SIMD dispatch installation. The pthread_once
 * primitive guarantees the installer callback runs exactly once
 * across the process and emits a full memory barrier before any
 * thread is allowed past the call site. Subsequent threads observe
 * the four global function pointers
 * (g_ssim_precompute, g_ssim_variance, g_ssim_accumulate,
 *  g_iqa_convolve) as fully constructed, never torn.
 *
 * The guard is owned by this translation unit (not by the caller)
 * so that float_ssim.c and float_ms_ssim.c — both of which install
 * the same idempotent ISA-best dispatch — share a single fire.
 * Without that sharing, two per-TU guards would each be allowed to
 * fire once, racing on the same globals (TSan race audit
 * 2026-05-30).
 *
 * The installer callback pointer is published via an atomic store
 * with release-ordering paired against pthread_once's acquire-style
 * barrier inside the trampoline. The pthread_once specified
 * semantics imply that exactly one thread executes the trampoline;
 * losing threads block until that completes, then read the four
 * dispatch globals through pthread_once's full barrier. */
#include <stdatomic.h>
static pthread_once_t g_ssim_dispatch_once = PTHREAD_ONCE_INIT;
/* ATOMIC_VAR_INIT was deprecated in C17 and is absent from MSVC's <stdatomic.h>;
 * plain NULL initialisation is semantically identical per C11 §7.17.2.1 p3 and
 * is accepted by GCC, Clang, and MSVC. */
static _Atomic(void (*)(void)) g_ssim_dispatch_installer = NULL;

static void iqa_ssim_dispatch_trampoline(void)
{
    void (*installer)(void) =
        atomic_load_explicit(&g_ssim_dispatch_installer, memory_order_acquire);
    if (installer)
        installer();
}

/* NOLINTNEXTLINE(readability-non-const-parameter) — POSIX pthread_once mutates *guard (ADR-0141 / ADR-0278). */
void iqa_ssim_install_dispatch_once(pthread_once_t *guard, void (*installer)(void))
{
    /* `guard` parameter is retained for API symmetry with the
     * per-TU header signature; the actual guard is the shared
     * `g_ssim_dispatch_once` so float_ssim.c and float_ms_ssim.c
     * cooperate. The first caller wins; subsequent callers are
     * short-circuited by pthread_once and never re-publish the
     * installer pointer. */
    (void)guard;
    if (!installer)
        return;
    /* Idempotent publish: every caller installs the same ISA-detect
     * routine. We use a relaxed store guarded by a CAS so only the
     * first publisher writes — eliminating the TSan-visible data
     * race on the installer pointer itself. */
    void (*expected)(void) = NULL;
    (void)atomic_compare_exchange_strong_explicit(&g_ssim_dispatch_installer, &expected, installer,
                                                  memory_order_release, memory_order_relaxed);
    (void)pthread_once(&g_ssim_dispatch_once, iqa_ssim_dispatch_trampoline);
}

/* Adapter: feeds the `struct iqa_kernel *` call site into the
 * primitive-args SIMD function pointer; falls back to the scalar
 * `iqa_convolve` when no dispatch is installed. `workspace` is the
 * caller-owned w*h scratch buffer reused across every dispatch site
 * in a single iqa_ssim invocation (see ADR-0138 §Decision). */
static inline void iqa_convolve_dispatch(float *img, int w, int h, const struct iqa_kernel *k,
                                         float *workspace, float *result, int *rw, int *rh)
{
    if (g_iqa_convolve) {
        g_iqa_convolve(img, w, h, k->kernel_h, k->kernel_v, k->w, k->h, k->normalized, workspace,
                       result, rw, rh);
    } else {
        (void)workspace;
        iqa_convolve(img, w, h, k, result, rw, rh);
    }
}

/* calc_luminance */
IQA_INLINE static double calc_luminance(float mu1, float mu2, float C1, float alpha)
{
    double result;
    float sign;
    /* For MS-SSIM* */
    if (C1 == 0 && mu1 * mu1 == 0 && mu2 * mu2 == 0)
        return 1.0;
    result = (2.0 * mu1 * mu2 + C1) / (mu1 * mu1 + mu2 * mu2 + C1);
    if (alpha == 1.0f)
        return result;
    sign = result < 0.0 ? -1.0f : 1.0f;
    return sign * pow(fabs(result), (double)alpha);
}

/* calc_contrast */
IQA_INLINE static double calc_contrast(double sigma_comb_12, float sigma1_sqd, float sigma2_sqd,
                                       float C2, float beta)
{
    double result;
    float sign;
    /* For MS-SSIM* */
    if (C2 == 0 && sigma1_sqd + sigma2_sqd == 0)
        return 1.0;
    result = (2.0 * sigma_comb_12 + C2) / (sigma1_sqd + sigma2_sqd + C2);
    if (beta == 1.0f)
        return result;
    sign = result < 0.0 ? -1.0f : 1.0f;
    return sign * pow(fabs(result), (double)beta);
}

/* calc_structure */
IQA_INLINE static double calc_structure(float sigma_12, double sigma_comb_12, float sigma1,
                                        float sigma2, float C3, float gamma)
{
    double result;
    float sign;
    /* For MS-SSIM* */
    if (C3 == 0 && sigma_comb_12 == 0) {
        if (sigma1 == 0 && sigma2 == 0) {
            return 1.0;
        } else if (sigma1 == 0 || sigma2 == 0) {
            return 0.0;
        }
    }
    result = (sigma_12 + C3) / (sigma_comb_12 + C3);
    if (gamma == 1.0f) {
        return result;
    }
    sign = result < 0.0 ? -1.0f : 1.0f;
    return sign * pow(fabs(result), (double)gamma);
}

/* Scalar fallback for the precompute stage (ref*ref, cmp*cmp, ref*cmp). */
static void ssim_precompute_scalar(const float *ref, const float *cmp, float *ref_sq, float *cmp_sq,
                                   float *ref_cmp, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        int offset = y * w;
        for (int x = 0; x < w; ++x, ++offset) {
            ref_sq[offset] = ref[offset] * ref[offset];
            cmp_sq[offset] = cmp[offset] * cmp[offset];
            ref_cmp[offset] = ref[offset] * cmp[offset];
        }
    }
}

/* Scalar fallback for the variance stage: subtract mu^2 and clamp to 0. */
static void ssim_variance_scalar(float *ref_sigma_sqd, float *cmp_sigma_sqd, float *sigma_both,
                                 const float *ref_mu, const float *cmp_mu, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        int offset = y * w;
        for (int x = 0; x < w; ++x, ++offset) {
            ref_sigma_sqd[offset] -= ref_mu[offset] * ref_mu[offset];
            cmp_sigma_sqd[offset] -= cmp_mu[offset] * cmp_mu[offset];
            /* zli-nflx: clamp to zero after subtraction */
            ref_sigma_sqd[offset] = MAX(0.0, ref_sigma_sqd[offset]);
            cmp_sigma_sqd[offset] = MAX(0.0, cmp_sigma_sqd[offset]);
            sigma_both[offset] -= ref_mu[offset] * cmp_mu[offset];
        }
    }
}

/* Scalar fallback for the default-case accumulate (l*c*s with the zli-nflx
 * flat-region clamp on sigma_both). */
static void ssim_accumulate_default_scalar(const float *ref_mu, const float *cmp_mu,
                                           const float *ref_sigma_sqd, const float *cmp_sigma_sqd,
                                           const float *sigma_both, int w, int h, float C1,
                                           float C2, float C3, double *ssim_sum, double *l_sum,
                                           double *c_sum, double *s_sum)
{
    for (int y = 0; y < h; ++y) {
        int offset = y * w;
        for (int x = 0; x < w; ++x, ++offset) {
            const float sigma_ref_sigma_cmp = sqrtf(ref_sigma_sqd[offset] * cmp_sigma_sqd[offset]);
            const double l =
                (2.0 * ref_mu[offset] * cmp_mu[offset] + C1) /
                (ref_mu[offset] * ref_mu[offset] + cmp_mu[offset] * cmp_mu[offset] + C1);
            const double c = (2.0 * sigma_ref_sigma_cmp + C2) /
                             (ref_sigma_sqd[offset] + cmp_sigma_sqd[offset] + C2);
            /* zli-nflx: when ref == cmp and the filtered region is flat (zero std),
             * sigma_both can go slightly negative via float rounding while
             * sigma_ref_sigma_cmp is zero; clamp so s stays at 1.0. */
            const float clamped_sigma_both =
                (sigma_both[offset] < 0.0f && sigma_ref_sigma_cmp <= 0.0f) ? 0.0f :
                                                                             sigma_both[offset];
            const double s = (clamped_sigma_both + C3) / (sigma_ref_sigma_cmp + C3);
            *ssim_sum += l * c * s;
            *l_sum += l;
            *c_sum += c;
            *s_sum += s;
        }
    }
}

/* Scalar path for the user-tweaked (alpha/beta/gamma) branch. Reached only
 * when the caller passes non-NULL args. Returns INFINITY if mr->map signals
 * abort, otherwise the reduced score. */
static float ssim_accumulate_user_args_scalar(float *ref_sigma_sqd, float *cmp_sigma_sqd,
                                              float *sigma_both, const float *ref_mu,
                                              const float *cmp_mu, int w, int h, float C1, float C2,
                                              float C3, float alpha, float beta, float gamma,
                                              const struct iqa_map_reduce *mr)
{
    struct iqa_ssim_int sint;
    for (int y = 0; y < h; ++y) {
        int offset = y * w;
        for (int x = 0; x < w; ++x, ++offset) {
            /* passing a negative number to sqrt() causes a domain error */
            if (ref_sigma_sqd[offset] < 0.0f) {
                ref_sigma_sqd[offset] = 0.0f;
            }
            if (cmp_sigma_sqd[offset] < 0.0f) {
                cmp_sigma_sqd[offset] = 0.0f;
            }
            const double sigma_root = sqrtf(ref_sigma_sqd[offset] * cmp_sigma_sqd[offset]);
            sint.l = calc_luminance(ref_mu[offset], cmp_mu[offset], C1, alpha);
            sint.c =
                calc_contrast(sigma_root, ref_sigma_sqd[offset], cmp_sigma_sqd[offset], C2, beta);
            sint.s = calc_structure(sigma_both[offset], sigma_root, ref_sigma_sqd[offset],
                                    cmp_sigma_sqd[offset], C3, gamma);
            if (mr->map(&sint, mr->context)) {
                return INFINITY;
            }
        }
    }
    return mr->reduce(w, h, mr->context);
}

struct ssim_workspace {
    float *ref_mu;
    float *cmp_mu;
    float *ref_sigma_sqd;
    float *cmp_sigma_sqd;
    float *sigma_both;
    /* Shared workspace for the SIMD convolve's horizontal-pass cache.
     * Allocated once and threaded through all 5 dispatch sites below,
     * replacing ~1200 per-call calloc/free pairs on a 120-frame 1080p
     * run. Scalar `iqa_convolve` ignores it. See ADR-0138. */
    float *conv_workspace;
};

static int ssim_workspace_alloc(struct ssim_workspace *ws, size_t n_elems)
{
    ws->ref_mu = (float *)malloc(n_elems * sizeof(float));
    ws->cmp_mu = (float *)malloc(n_elems * sizeof(float));
    ws->ref_sigma_sqd = (float *)malloc(n_elems * sizeof(float));
    ws->cmp_sigma_sqd = (float *)malloc(n_elems * sizeof(float));
    ws->sigma_both = (float *)malloc(n_elems * sizeof(float));
    ws->conv_workspace = (float *)malloc(n_elems * sizeof(float));
    return (ws->ref_mu && ws->cmp_mu && ws->ref_sigma_sqd && ws->cmp_sigma_sqd && ws->sigma_both &&
            ws->conv_workspace) ?
               0 :
               -1;
}

static void ssim_workspace_free(struct ssim_workspace *ws)
{
    /* free(NULL) is a well-defined no-op (C89 §7.20.3.2) */
    free(ws->ref_mu);
    free(ws->cmp_mu);
    free(ws->ref_sigma_sqd);
    free(ws->cmp_sigma_sqd);
    free(ws->sigma_both);
    free(ws->conv_workspace);
}

static void ssim_compute_stats(float *ref, float *cmp, int *w, int *h, const struct iqa_kernel *k,
                               struct ssim_workspace *ws)
{
    iqa_convolve_dispatch(ref, *w, *h, k, ws->conv_workspace, ws->ref_mu, 0, 0);
    iqa_convolve_dispatch(cmp, *w, *h, k, ws->conv_workspace, ws->cmp_mu, 0, 0);

    if (g_ssim_precompute) {
        g_ssim_precompute(ref, cmp, ws->ref_sigma_sqd, ws->cmp_sigma_sqd, ws->sigma_both,
                          (*w) * (*h));
    } else {
        ssim_precompute_scalar(ref, cmp, ws->ref_sigma_sqd, ws->cmp_sigma_sqd, ws->sigma_both, *w,
                               *h);
    }

    iqa_convolve_dispatch(ws->ref_sigma_sqd, *w, *h, k, ws->conv_workspace, 0, 0, 0);
    iqa_convolve_dispatch(ws->cmp_sigma_sqd, *w, *h, k, ws->conv_workspace, 0, 0, 0);
    iqa_convolve_dispatch(ws->sigma_both, *w, *h, k, ws->conv_workspace, 0, w, h); /* Update w/h */

    if (g_ssim_variance) {
        g_ssim_variance(ws->ref_sigma_sqd, ws->cmp_sigma_sqd, ws->sigma_both, ws->ref_mu,
                        ws->cmp_mu, (*w) * (*h));
    } else {
        ssim_variance_scalar(ws->ref_sigma_sqd, ws->cmp_sigma_sqd, ws->sigma_both, ws->ref_mu,
                             ws->cmp_mu, *w, *h);
    }
}

static void ssim_init_args(const struct iqa_ssim_args *args, float *alpha, float *beta,
                           float *gamma, int *L, float *K1, float *K2)
{
    if (!args)
        return;
    *alpha = args->alpha;
    *beta = args->beta;
    *gamma = args->gamma;
    *L = args->L;
    *K1 = args->K1;
    *K2 = args->K2;
}

float iqa_ssim(float *ref, float *cmp, int w, int h, const struct iqa_kernel *k,
               const struct iqa_map_reduce *mr, const struct iqa_ssim_args *args, float *l_mean,
               float *c_mean, float *s_mean /* zli-nflx */
)
{
    float alpha = 1.0f;
    float beta = 1.0f;
    float gamma = 1.0f;
    int L = 255;
    float K1 = 0.01f;
    float K2 = 0.03f;

    /* The assert(!args) that was here fired when iqa_ssim was called with a
     * non-NULL args pointer (the Rouse MS-SSIM path passes its own struct).
     * Replace with a runtime guard: non-default args require a map-reduce
     * context (mr), otherwise the reduction step has no place to store
     * per-pixel contributions.  No abort() in production paths.            */
    if (args && !mr)
        return INFINITY;
    ssim_init_args(args, &alpha, &beta, &gamma, &L, &K1, &K2);

    const float C1 = (K1 * L) * (K1 * L);
    const float C2 = (K2 * L) * (K2 * L);
    const float C3 = C2 / 2.0f;

    struct ssim_workspace ws = {0};
    if (ssim_workspace_alloc(&ws, (size_t)w * (size_t)h) != 0) {
        ssim_workspace_free(&ws);
        return INFINITY;
    }

    ssim_compute_stats(ref, cmp, &w, &h, k, &ws);

    double ssim_sum = 0.0;
    double l_sum = 0.0;
    double c_sum = 0.0;
    double s_sum = 0.0;
    float user_args_result = 0.0f;
    if (!args && g_ssim_accumulate) {
        g_ssim_accumulate(ws.ref_mu, ws.cmp_mu, ws.ref_sigma_sqd, ws.cmp_sigma_sqd, ws.sigma_both,
                          w * h, C1, C2, C3, &ssim_sum, &l_sum, &c_sum, &s_sum);
    } else if (!args) {
        ssim_accumulate_default_scalar(ws.ref_mu, ws.cmp_mu, ws.ref_sigma_sqd, ws.cmp_sigma_sqd,
                                       ws.sigma_both, w, h, C1, C2, C3, &ssim_sum, &l_sum, &c_sum,
                                       &s_sum);
    } else {
        user_args_result = ssim_accumulate_user_args_scalar(ws.ref_sigma_sqd, ws.cmp_sigma_sqd,
                                                            ws.sigma_both, ws.ref_mu, ws.cmp_mu, w,
                                                            h, C1, C2, C3, alpha, beta, gamma, mr);
    }

    ssim_workspace_free(&ws);

    if (!args) {
        *l_mean = (float)(l_sum / (double)(w * h)); /* zli-nflx */
        *c_mean = (float)(c_sum / (double)(w * h)); /* zli-nflx */
        *s_mean = (float)(s_sum / (double)(w * h)); /* zli-nflx */
        return (float)(ssim_sum / (double)(w * h));
    }
    return user_args_result;
}
