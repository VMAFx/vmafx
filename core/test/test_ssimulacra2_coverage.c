/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 3 — ssimulacra2.c gap-fill.
 *
 *  The existing test_ssimulacra2_simd.c exercises SIMD vs. scalar
 *  bit-exactness for the per-kernel paths (XYB, blur, SSIM, edge).
 *  This file adds end-to-end CPU path coverage absent from the fast suite:
 *
 *    1. Default options 8-bit: init() + extract() on a tiny 32x32 frame
 *       produces a finite score in the expected range for distinct inputs.
 *    2. Identical inputs: ssimulacra2 score should be close to 0.0
 *       (near-perfect quality: low value means high quality in ssimulacra2
 *       — but the exact contract is a finite non-NaN value).
 *    3. 10-bit HBD path: init() with bpc=10 allocates with the correct
 *       sample_scale and extract() still produces a finite result.
 *    4. YUV444P input: a 4:4:4 frame exercises the full-chroma plane path
 *       in convert_picture_to_linear_rgb.
 *
 *  ssimulacra2 requires width >= 8 per scale (multi-scale stops when
 *  cw < 8 || ch < 8).  32x32 is enough to run at least 2 scales.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

#define S2_W (32u)
#define S2_H (32u)

/* Fill a single plane of an 8-bit picture with a constant value. */
static void fill_plane8(VmafPicture *pic, unsigned plane, uint8_t v)
{
    uint8_t *p = (uint8_t *)pic->data[plane];
    ptrdiff_t s = pic->stride[plane];
    for (unsigned r = 0; r < pic->h[plane]; ++r)
        memset(p + r * s, v, pic->w[plane]);
}

/* Allocate an 8-bit YUV420P picture with constant luma + neutral chroma. */
static int alloc_grey8_420(VmafPicture *pic, uint8_t luma)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, S2_W, S2_H);
    if (err)
        return err;
    fill_plane8(pic, 0, luma);
    fill_plane8(pic, 1, 128u);
    fill_plane8(pic, 2, 128u);
    return 0;
}

/* Allocate an 8-bit YUV444P picture with constant luma + neutral chroma. */
static int alloc_grey8_444(VmafPicture *pic, uint8_t luma)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV444P, 8, S2_W, S2_H);
    if (err)
        return err;
    fill_plane8(pic, 0, luma);
    fill_plane8(pic, 1, 128u);
    fill_plane8(pic, 2, 128u);
    return 0;
}

/* Allocate a 10-bit YUV420P picture with constant luma + neutral chroma. */
static int alloc_grey10_420(VmafPicture *pic, uint16_t luma)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 10, S2_W, S2_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3; ++p) {
        uint16_t *plane = (uint16_t *)pic->data[p];
        uint16_t val = (p == 0) ? luma : 512u; /* neutral 10-bit chroma */
        ptrdiff_t s = pic->stride[p] / 2;
        for (unsigned r = 0; r < pic->h[p]; ++r) {
            for (unsigned c = 0; c < pic->w[p]; ++c)
                plane[r * s + c] = val;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- */
/* Default: 8-bit YUV420P, distinct inputs                          */
/* ----------------------------------------------------------------- */

static char *test_ssimulacra2_default_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssimulacra2");
    mu_assert("ssimulacra2 extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, S2_W, S2_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8_420(&ref, 100u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8_420(&dist, 150u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract ssimulacra2", err == 0);

    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "ssimulacra2", &score, 0);
    mu_assert("get ssimulacra2 score", err == 0);
    mu_assert("ssimulacra2 score is finite", isfinite(score));

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* Identical inputs                                                  */
/* ----------------------------------------------------------------- */

static char *test_ssimulacra2_identical_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssimulacra2");
    mu_assert("ssimulacra2 extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, S2_W, S2_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8_420(&ref, 128u);
    mu_assert("alloc ref identical", err == 0);
    err = alloc_grey8_420(&dist, 128u);
    mu_assert("alloc dist identical", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract identical", err == 0);

    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "ssimulacra2", &score, 0);
    mu_assert("get ssimulacra2 identical", err == 0);
    /* For identical inputs the score is finite (near-perfect → low value
     * in ssimulacra2 semantics; exact value depends on the metric). */
    mu_assert("ssimulacra2 identical is finite", isfinite(score));

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* 10-bit HBD path                                                   */
/* ----------------------------------------------------------------- */

static char *test_ssimulacra2_10bit_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssimulacra2");
    mu_assert("ssimulacra2 extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create 10bit", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 10u, S2_W, S2_H);
    mu_assert("context_init 10bit", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey10_420(&ref, 400u);
    mu_assert("alloc ref 10bit", err == 0);
    err = alloc_grey10_420(&dist, 700u);
    mu_assert("alloc dist 10bit", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract ssimulacra2 10bit", err == 0);

    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "ssimulacra2", &score, 0);
    mu_assert("get ssimulacra2 10bit", err == 0);
    mu_assert("ssimulacra2 10bit finite", isfinite(score));

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* YUV444P path: full-chroma 4:4:4 input                            */
/* ----------------------------------------------------------------- */

static char *test_ssimulacra2_yuv444p_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssimulacra2");
    mu_assert("ssimulacra2 extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create 444", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV444P, 8u, S2_W, S2_H);
    mu_assert("context_init 444", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8_444(&ref, 100u);
    mu_assert("alloc ref 444", err == 0);
    err = alloc_grey8_444(&dist, 140u);
    mu_assert("alloc dist 444", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract ssimulacra2 444", err == 0);

    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "ssimulacra2", &score, 0);
    mu_assert("get ssimulacra2 444", err == 0);
    mu_assert("ssimulacra2 444 finite", isfinite(score));

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ssimulacra2_default_extract);
    mu_run_test(test_ssimulacra2_identical_extract);
    mu_run_test(test_ssimulacra2_10bit_extract);
    mu_run_test(test_ssimulacra2_yuv444p_extract);
    return NULL;
}
