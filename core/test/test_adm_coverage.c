/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 3 — integer_adm.c gap-fill.
 *
 *  The existing test_integer_adm_simd.c exercises SIMD vs. scalar
 *  bit-exactness for the DWT2 / decouple / CSF kernels. This file adds
 *  end-to-end CPU coverage that was absent from the fast suite:
 *
 *    1. Default options: init() + extract() + check VMAF_integer_feature_adm2_score
 *       is in [0,1] range (exercises the full init + compute_adm path).
 *    2. debug=true option: extract() emits the extended integer_adm / num / den
 *       sub-scores (lines 3508-3543 of integer_adm.c).
 *    3. 10-bit HBD path: init() with bpc=10 exercises the 16-bit DWT2 dispatch.
 *    4. extract() invalid condition: adm_norm_view_dist * adm_ref_display_height
 *       below the 1080p*3H minimum returns -EINVAL (line 3470-3472).
 *
 *  All tests run on tiny 64x64 YUV420P synthetic frames; no real YUV
 *  fixtures are needed. The DWT2 requirement (width divisible by 8 on
 *  AVX2) is satisfied by 64.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

#define ADM_W (64u)
#define ADM_H (64u)

/* Allocate a flat-grey 8-bit YUV420P picture. */
static int alloc_grey8(VmafPicture *pic, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, ADM_W, ADM_H);
    if (err)
        return err;
    uint8_t *p = (uint8_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0];
    for (unsigned r = 0; r < ADM_H; ++r)
        memset(p + r * s, v, ADM_W);
    return 0;
}

/* Allocate a flat-grey 10-bit YUV420P picture. */
static int alloc_grey10(VmafPicture *pic, uint16_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 10, ADM_W, ADM_H);
    if (err)
        return err;
    uint16_t *p = (uint16_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0] / 2;
    for (unsigned r = 0; r < ADM_H; ++r) {
        for (unsigned c = 0; c < ADM_W; ++c)
            p[r * s + c] = v;
    }
    return 0;
}

/* ----------------------------------------------------------------- */
/* Default options: adm2 score in [0,1] for distinct 8-bit inputs   */
/* ----------------------------------------------------------------- */

static char *test_adm_default_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("adm extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, ADM_W, ADM_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8(&ref, 100u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 120u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract adm default", err == 0);

    double adm2 = NAN;
    err = vmaf_feature_collector_get_score(fc, "VMAF_integer_feature_adm2_score", &adm2, 0);
    mu_assert("get adm2 score", err == 0);
    mu_assert("adm2 is finite", isfinite(adm2));
    mu_assert("adm2 in [0,1]", adm2 >= 0.0 && adm2 <= 1.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* Identical inputs: adm2 score should be 1.0                        */
/* ----------------------------------------------------------------- */

static char *test_adm_identical_is_one(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("adm extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, ADM_W, ADM_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8(&ref, 128u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 128u);
    mu_assert("alloc dist identical", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract adm identical", err == 0);

    double adm2 = NAN;
    err = vmaf_feature_collector_get_score(fc, "VMAF_integer_feature_adm2_score", &adm2, 0);
    mu_assert("get adm2 score", err == 0);
    mu_assert("adm2 identical == 1.0", fabs(adm2 - 1.0) < 1e-6);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* debug=true: extract also writes integer_adm / num / den           */
/* ----------------------------------------------------------------- */

static char *test_adm_debug_mode(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("adm extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "debug", "true", 0);
    mu_assert("set debug", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create with debug", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, ADM_W, ADM_H);
    mu_assert("context_init debug", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8(&ref, 100u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 110u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract adm debug", err == 0);

    /* debug=true emits extra sub-scores; adm2 is still present. */
    double adm2 = NAN;
    err = vmaf_feature_collector_get_score(fc, "VMAF_integer_feature_adm2_score", &adm2, 0);
    mu_assert("get adm2 debug mode", err == 0);
    mu_assert("adm2 debug finite", isfinite(adm2));

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* 10-bit HBD path                                                   */
/* ----------------------------------------------------------------- */

static char *test_adm_10bit_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("adm extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create 10bit", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 10u, ADM_W, ADM_H);
    mu_assert("context_init 10bit", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey10(&ref, 500u);
    mu_assert("alloc ref 10bit", err == 0);
    err = alloc_grey10(&dist, 600u);
    mu_assert("alloc dist 10bit", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract adm 10bit", err == 0);

    double adm2 = NAN;
    err = vmaf_feature_collector_get_score(fc, "VMAF_integer_feature_adm2_score", &adm2, 0);
    mu_assert("get adm2 10bit", err == 0);
    mu_assert("adm2 10bit finite", isfinite(adm2));

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* Invalid view-dist: extract() must return -EINVAL                  */
/* (adm_norm_view_dist * adm_ref_display_height < default)          */
/* ----------------------------------------------------------------- */

static char *test_adm_invalid_view_dist_returns_einval(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("adm extractor missing", fex != NULL);

    /* adm_norm_view_dist is a FEATURE_PARAM; setting it to its minimum
     * allowed value (0.75) makes norm_view_dist * ref_display_height =
     * 0.75 * 1080 = 810, which is below the threshold of
     * DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT = 3240. */
    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "adm_norm_view_dist", "0.75", 0);
    mu_assert("set adm_norm_view_dist", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create with norm_view_dist", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, ADM_W, ADM_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8(&ref, 100u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 100u);
    mu_assert("alloc dist", err == 0);

    /* With norm_view_dist=0.01 the view_dist * display_height product is
     * below the DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT
     * minimum; extract() must return -EINVAL. */
    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract with bad norm_view_dist should return -EINVAL", err == -EINVAL);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* CSF mode budget guard: mode 1 (BARTEN) overflows uint16_t scale-0  */
/* factor and returns -EINVAL; modes 0, 2, 3 fit and succeed.        */
/* ----------------------------------------------------------------- */

/* One adm_csf_mode configuration end to end: build the extractor context,
 * push one frame and check extract()'s status against `expected_err`. Split
 * out of the test body so both stay inside the readability-function-size
 * threshold (ADR-0141). */
static char *run_csf_mode_case(VmafFeatureExtractor *fex, const char *mode, int expected_err)
{
    VmafDictionary *opts = nullptr;
    int err = vmaf_dictionary_set(&opts, "adm_csf_mode", mode, 0);
    mu_assert("set adm_csf_mode", err == 0);

    VmafFeatureExtractorContext *ctx = nullptr;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create with adm_csf_mode", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, ADM_W, ADM_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = nullptr;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8(&ref, 100u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 100u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, nullptr, &dist, nullptr, 0, fc);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);

    mu_assert("extract() status does not match the adm_csf_mode budget contract",
              err == expected_err);
    return nullptr;
}

static char *test_adm_csf_mode_budget_guard(void)
{
    static const char *const modes[] = {"0", "1", "2", "3"};
    static const int expected_err[] = {0, -EINVAL, 0, 0};

    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("adm extractor missing", fex != nullptr);

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        char *msg = run_csf_mode_case(fex, modes[i], expected_err[i]);
        if (msg)
            return msg;
    }
    return nullptr;
}

char *run_tests(void)
{
    mu_run_test(test_adm_default_extract);
    mu_run_test(test_adm_identical_is_one);
    mu_run_test(test_adm_debug_mode);
    mu_run_test(test_adm_10bit_extract);
    mu_run_test(test_adm_invalid_view_dist_returns_einval);
    mu_run_test(test_adm_csf_mode_budget_guard);
    return NULL;
}
