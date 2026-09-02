/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Y-FUNQUE+ atoms-only feature extractor — oracle + robustness tests.
 *
 *  Oracle source: a faithful Python reference (pywt 'periodization' Haar +
 *  OpenCV INTER_CUBIC bicubic + the verified dossier constants) computed the
 *  expected atom values; see ADR-1114 and .workingdir2/rc/metrics/
 *  y-funque-plus.md. The deterministic input patterns below are pure
 *  closed-form integer formulas so the same bytes flow through both the
 *  Python reference and this C test. The C implementation matches the
 *  reference to better than 1e-10 in practice; the committed tolerance is the
 *  fork's places=4 atom gate (5e-5) to leave headroom for cross-host libm.
 *
 *  Coverage:
 *    1. 8x8 identical (ref==dist), frame 0 — analytic oracles:
 *         ms_ssim == 0, dlm == 1, mad == 0.
 *    2. 64x64 deterministic ref != dist, frame 0 — Python oracle, places=4.
 *    3. 2-frame sequence — exercises MAD-Ref != 0; Python oracle, places=4.
 *    4. 65x33 ODD dimensions, identical — regression for odd-dim buffer
 *       sizing; analytic oracles (ms==0, dlm==1, mad==0).
 *    5. 100x100 identical — exercises the downscale-then-crop path
 *       (ds 50x50 -> crop 48x48); analytic oracles guard the crop copy.
 *    6. 6x6 too-small frame — init() rejection (bounded-input guard).
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

/* Tolerances. The analytic identical-input oracles are tight; the
 * non-trivial oracles are at the fork's places=4 atom gate. */
#define YF_TOL_EXACT (1e-9)
#define YF_TOL_PLACES4 (5e-5)

typedef double (*gen_fn)(unsigned x, unsigned y);

static double gen_const_gradient(unsigned x, unsigned y)
{
    return (double)((y * 8u + x) * 3u + 10u);
}

static double gen_det_ref(unsigned x, unsigned y)
{
    return (double)((37u * x + 53u * y + 11u * x * y) % 200u + 16u);
}

static double gen_det_ref2(unsigned x, unsigned y)
{
    return (double)((37u * x + 53u * y + 11u * x * y + 5u * (x + y)) % 200u + 16u);
}

static double gen_det_dist(unsigned x, unsigned y)
{
    const double rv = gen_det_ref(x, y);
    double dv = rv + (double)((int)((x + 2u * y) % 7u) - 3);
    if (dv < 0.0)
        dv = 0.0;
    if (dv > 255.0)
        dv = 255.0;
    return dv;
}

static double gen_odd_pattern(unsigned x, unsigned y)
{
    return (double)((71u * x + 29u * y + 7u * x * y) % 230u + 12u);
}

static double gen_crop_pattern(unsigned x, unsigned y)
{
    return (double)((41u * x + 59u * y + 13u * x * y) % 210u + 20u);
}

static int alloc_luma8(VmafPicture *pic, unsigned w, unsigned h, gen_fn gen)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, w, h);
    if (err)
        return err;
    uint8_t *p = pic->data[0];
    const ptrdiff_t s = pic->stride[0];
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            double v = gen(x, y);
            if (v < 0.0)
                v = 0.0;
            if (v > 255.0)
                v = 255.0;
            p[(size_t)y * s + x] = (uint8_t)(v + 0.5);
        }
    }
    for (unsigned plane = 1; plane < 3; plane++) {
        uint8_t *c = pic->data[plane];
        const ptrdiff_t cs = pic->stride[plane];
        for (unsigned y = 0; y < pic->h[plane]; y++)
            memset(c + (size_t)y * cs, 128, pic->w[plane]);
    }
    return 0;
}

/* Create + init a live context and a fresh collector for an `w`x`h` 8-bit
 * pipeline. Returns 0 on success and writes the handles; non-zero otherwise. */
static int make_ctx(VmafFeatureExtractorContext **ctx, VmafFeatureCollector **fc, unsigned w,
                    unsigned h)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("y_funque_plus");
    if (!fex)
        return -1;
    int err = vmaf_feature_extractor_context_create(ctx, fex, NULL);
    if (err)
        return err;
    err = vmaf_feature_extractor_context_init(*ctx, VMAF_PIX_FMT_YUV420P, 8u, w, h);
    if (err)
        return err;
    return vmaf_feature_collector_init(fc);
}

static void destroy_ctx(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc)
{
    if (ctx) {
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
    }
    if (fc)
        vmaf_feature_collector_destroy(fc);
}

/* Run one (ref, dist) pair at `index` and read the three atoms back. */
static int run_one(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc, unsigned w,
                   unsigned h, gen_fn ref_gen, gen_fn dist_gen, unsigned index, double *ms,
                   double *dlm, double *mad)
{
    VmafPicture ref;
    VmafPicture dist;
    int err = alloc_luma8(&ref, w, h, ref_gen);
    if (err)
        return err;
    err = alloc_luma8(&dist, w, h, dist_gen);
    if (err) {
        vmaf_picture_unref(&ref);
        return err;
    }
    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, index, fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    if (err)
        return err;
    err |= vmaf_feature_collector_get_score(fc, "y_funque_plus_ms_ssim", ms, index);
    err |= vmaf_feature_collector_get_score(fc, "y_funque_plus_dlm", dlm, index);
    err |= vmaf_feature_collector_get_score(fc, "y_funque_plus_mad", mad, index);
    return err;
}

/* 1. 8x8 identical inputs, frame 0 — analytic oracles. */
static char *test_yf_identical_8x8(void)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    mu_assert("make_ctx 8x8", make_ctx(&ctx, &fc, 8u, 8u) == 0);

    double ms = NAN;
    double dlm = NAN;
    double mad = NAN;
    int err = run_one(ctx, fc, 8u, 8u, gen_const_gradient, gen_const_gradient, 0u, &ms, &dlm, &mad);
    mu_assert("run 8x8 identical", err == 0);
    mu_assert("8x8 ident ms_ssim == 0", fabs(ms) < YF_TOL_EXACT);
    mu_assert("8x8 ident dlm == 1", fabs(dlm - 1.0) < YF_TOL_EXACT);
    mu_assert("8x8 ident mad == 0", fabs(mad) < YF_TOL_EXACT);

    destroy_ctx(ctx, fc);
    return NULL;
}

/* 2. 64x64 deterministic ref != dist, frame 0 — Python oracle. */
static char *test_yf_nontrivial_64x64(void)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    mu_assert("make_ctx 64x64", make_ctx(&ctx, &fc, 64u, 64u) == 0);

    double ms = NAN;
    double dlm = NAN;
    double mad = NAN;
    int err = run_one(ctx, fc, 64u, 64u, gen_det_ref, gen_det_dist, 0u, &ms, &dlm, &mad);
    mu_assert("run 64x64 f0", err == 0);
    /* Oracle (Python reference, places=4): see ADR-1114 / dossier. */
    mu_assert("64x64 f0 ms_ssim", fabs(ms - 0.073307224372635232) < YF_TOL_PLACES4);
    mu_assert("64x64 f0 dlm", fabs(dlm - 0.99725644809672631) < YF_TOL_PLACES4);
    mu_assert("64x64 f0 mad == 0", fabs(mad) < YF_TOL_EXACT);

    destroy_ctx(ctx, fc);
    return NULL;
}

/* 3. 2-frame sequence — exercises MAD-Ref != 0; Python oracle. */
static char *test_yf_temporal_mad(void)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    mu_assert("make_ctx temporal", make_ctx(&ctx, &fc, 64u, 64u) == 0);

    double ms = NAN;
    double dlm = NAN;
    double mad = NAN;

    int err = run_one(ctx, fc, 64u, 64u, gen_det_ref, gen_det_dist, 0u, &ms, &dlm, &mad);
    mu_assert("run f0", err == 0);
    mu_assert("f0 mad == 0", fabs(mad) < YF_TOL_EXACT);

    err = run_one(ctx, fc, 64u, 64u, gen_det_ref2, gen_det_dist, 1u, &ms, &dlm, &mad);
    mu_assert("run f1", err == 0);
    /* Oracle (Python reference, places=4). */
    mu_assert("f1 ms_ssim", fabs(ms - 1.2604363511195453) < YF_TOL_PLACES4);
    mu_assert("f1 dlm", fabs(dlm - 0.51557907268591441) < YF_TOL_PLACES4);
    mu_assert("f1 mad", fabs(mad - 0.11999870749080883) < YF_TOL_PLACES4);

    destroy_ctx(ctx, fc);
    return NULL;
}

/* 4. 65x33 ODD dimensions, identical — odd-dim buffer regression. */
static char *test_yf_odd_dims(void)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    mu_assert("make_ctx odd", make_ctx(&ctx, &fc, 65u, 33u) == 0);

    double ms = NAN;
    double dlm = NAN;
    double mad = NAN;
    int err = run_one(ctx, fc, 65u, 33u, gen_odd_pattern, gen_odd_pattern, 0u, &ms, &dlm, &mad);
    mu_assert("run 65x33 identical", err == 0);
    /* Identical inputs at odd dims: analytic oracles hold and no OOB access
     * (ASan under `make test` covers the latter). */
    mu_assert("65x33 ident ms_ssim == 0", fabs(ms) < YF_TOL_EXACT);
    mu_assert("65x33 ident dlm == 1", fabs(dlm - 1.0) < YF_TOL_EXACT);
    mu_assert("65x33 ident mad == 0", fabs(mad) < YF_TOL_EXACT);

    destroy_ctx(ctx, fc);
    return NULL;
}

/* 5. 100x100 identical — exercises the downscale-then-crop path. ds is
 * 50x50 but the crop-to-2^levels target is 48x48, so the in-place row
 * crop in yf_prepare_plane runs (ds != crop). Identical inputs keep the
 * analytic oracles, so this is a robustness regression for the crop copy
 * (a stride bug there would corrupt the pyramid and break ms==0/dlm==1). */
static char *test_yf_crop_path(void)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    mu_assert("make_ctx 100x100", make_ctx(&ctx, &fc, 100u, 100u) == 0);

    double ms = NAN;
    double dlm = NAN;
    double mad = NAN;
    int err = run_one(ctx, fc, 100u, 100u, gen_crop_pattern, gen_crop_pattern, 0u, &ms, &dlm, &mad);
    mu_assert("run 100x100 identical", err == 0);
    mu_assert("100x100 ident ms_ssim == 0", fabs(ms) < YF_TOL_EXACT);
    mu_assert("100x100 ident dlm == 1", fabs(dlm - 1.0) < YF_TOL_EXACT);
    mu_assert("100x100 ident mad == 0", fabs(mad) < YF_TOL_EXACT);

    destroy_ctx(ctx, fc);
    return NULL;
}

/* 6. Too-small frame is rejected by init() (bounded-input guard). */
static char *test_yf_reject_tiny(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("y_funque_plus");
    mu_assert("y_funque_plus extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create tiny", err == 0);
    /* 6x6 -> downscale 3x3 -> crop (6>>3)<<2 = 0 -> below the Haar minimum. */
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, 6u, 6u);
    mu_assert("init rejects 6x6", err != 0);

    (void)vmaf_feature_extractor_context_destroy(ctx);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_yf_identical_8x8);
    mu_run_test(test_yf_nontrivial_64x64);
    mu_run_test(test_yf_temporal_mad);
    mu_run_test(test_yf_odd_dims);
    mu_run_test(test_yf_crop_path);
    mu_run_test(test_yf_reject_tiny);
    return NULL;
}
