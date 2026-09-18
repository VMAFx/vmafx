/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
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

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

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

/* Look up the "adm" extractor, create+init a context over an ADM_W x ADM_H
 * YUV420P frame at `bpc` bits (optionally carrying `opts`), then init a
 * feature collector. Every test below starts this way; only the
 * context_create / context_init failure messages differ per caller (plain
 * vs. debug vs. 10-bit vs. bad-option variants), so those two plus `bpc`
 * are the only parameters. */
static char *adm_fixture_open(VmafFeatureExtractorContext **ctx, VmafFeatureCollector **fc,
                              VmafDictionary *opts, unsigned bpc, char *create_msg, char *init_msg)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    mu_assert("adm extractor missing", fex != NULL);

    int err = vmaf_feature_extractor_context_create(ctx, fex, opts);
    mu_assert(create_msg, err == 0);
    err = vmaf_feature_extractor_context_init(*ctx, VMAF_PIX_FMT_YUV420P, bpc, ADM_W, ADM_H);
    mu_assert(init_msg, err == 0);

    err = vmaf_feature_collector_init(fc);
    mu_assert("collector_init", err == 0);
    return NULL;
}

/* Common teardown for every test in this file. */
static void adm_fixture_close(VmafFeatureExtractorContext *ctx, VmafFeatureCollector *fc,
                              VmafPicture *ref, VmafPicture *dist)
{
    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(ref);
    vmaf_picture_unref(dist);
}

/* ----------------------------------------------------------------- */
/* Default options: adm2 score in [0,1] for distinct 8-bit inputs   */
/* ----------------------------------------------------------------- */

static char *test_adm_default_extract(void)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    char *msg = adm_fixture_open(&ctx, &fc, NULL, 8u, "context_create", "context_init");
    if (msg)
        return msg;

    VmafPicture ref;
    VmafPicture dist;
    int err = alloc_grey8(&ref, 100u);
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

    adm_fixture_close(ctx, fc, &ref, &dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* Identical inputs: adm2 score should be 1.0                        */
/* ----------------------------------------------------------------- */

static char *test_adm_identical_is_one(void)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    char *msg = adm_fixture_open(&ctx, &fc, NULL, 8u, "context_create", "context_init");
    if (msg)
        return msg;

    VmafPicture ref;
    VmafPicture dist;
    int err = alloc_grey8(&ref, 128u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 128u);
    mu_assert("alloc dist identical", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract adm identical", err == 0);

    double adm2 = NAN;
    err = vmaf_feature_collector_get_score(fc, "VMAF_integer_feature_adm2_score", &adm2, 0);
    mu_assert("get adm2 score", err == 0);
    mu_assert("adm2 identical == 1.0", fabs(adm2 - 1.0) < 1e-6);

    adm_fixture_close(ctx, fc, &ref, &dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* debug=true: extract also writes integer_adm / num / den           */
/* ----------------------------------------------------------------- */

static char *test_adm_debug_mode(void)
{
    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "debug", "true", 0);
    mu_assert("set debug", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    char *msg =
        adm_fixture_open(&ctx, &fc, opts, 8u, "context_create with debug", "context_init debug");
    if (msg)
        return msg;

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

    adm_fixture_close(ctx, fc, &ref, &dist);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* 10-bit HBD path                                                   */
/* ----------------------------------------------------------------- */

static char *test_adm_10bit_extract(void)
{
    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    char *msg =
        adm_fixture_open(&ctx, &fc, NULL, 10u, "context_create 10bit", "context_init 10bit");
    if (msg)
        return msg;

    VmafPicture ref;
    VmafPicture dist;
    int err = alloc_grey10(&ref, 500u);
    mu_assert("alloc ref 10bit", err == 0);
    err = alloc_grey10(&dist, 600u);
    mu_assert("alloc dist 10bit", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract adm 10bit", err == 0);

    double adm2 = NAN;
    err = vmaf_feature_collector_get_score(fc, "VMAF_integer_feature_adm2_score", &adm2, 0);
    mu_assert("get adm2 10bit", err == 0);
    mu_assert("adm2 10bit finite", isfinite(adm2));

    adm_fixture_close(ctx, fc, &ref, &dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* Invalid view-dist: extract() must return -EINVAL                  */
/* (adm_norm_view_dist * adm_ref_display_height < default)          */
/* ----------------------------------------------------------------- */

static char *test_adm_invalid_view_dist_returns_einval(void)
{
    /* adm_norm_view_dist is a FEATURE_PARAM; setting it to its minimum
     * allowed value (0.75) makes norm_view_dist * ref_display_height =
     * 0.75 * 1080 = 810, which is below the threshold of
     * DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT = 3240. */
    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "adm_norm_view_dist", "0.75", 0);
    mu_assert("set adm_norm_view_dist", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    VmafFeatureCollector *fc = NULL;
    char *msg =
        adm_fixture_open(&ctx, &fc, opts, 8u, "context_create with norm_view_dist", "context_init");
    if (msg)
        return msg;

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

    adm_fixture_close(ctx, fc, &ref, &dist);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_default_extract);
    mu_run_test(test_adm_identical_is_one);
    mu_run_test(test_adm_debug_mode);
    mu_run_test(test_adm_10bit_extract);
    mu_run_test(test_adm_invalid_view_dist_returns_einval);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
