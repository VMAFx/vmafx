/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 3 — integer_ssim.c gap-fill.
 *
 *  The existing test_integer_ssim_simd.c exercises the AVX2 accumulate_row
 *  path vs. scalar for bit-exactness.  This file plugs the remaining
 *  uncovered branches identified in core/src/feature/integer_ssim.c:
 *
 *    1. init() with clip_db=true computes max_db (lines 337-340).
 *    2. extract() with enable_db=true converts ssim to decibel units
 *       (line 378-379 of integer_ssim.c).
 *    3. extract() with enable_db + clip_db clips at max_db for identical
 *       inputs (perfect ssim → clipped score).
 *    4. extract() in 10-bit mode (bpc=10) exercises the HBD scalar path.
 *
 *  No SIMD-vs-scalar parity is asserted here; that lives in
 *  test_integer_ssim_simd.c.
 */

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

#define SSIM_W (32u)
#define SSIM_H (32u)

/* Allocate a flat-grey 8-bit YUV420P picture. */
static int alloc_grey8(VmafPicture *pic, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, SSIM_W, SSIM_H);
    if (err)
        return err;
    uint8_t *p = (uint8_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0];
    for (unsigned r = 0; r < SSIM_H; ++r)
        memset(p + r * s, v, SSIM_W);
    return 0;
}

/* Allocate a flat-grey 10-bit YUV420P picture. */
static int alloc_grey10(VmafPicture *pic, uint16_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 10, SSIM_W, SSIM_H);
    if (err)
        return err;
    uint16_t *p = (uint16_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0] / 2;
    for (unsigned r = 0; r < SSIM_H; ++r) {
        for (unsigned c = 0; c < SSIM_W; ++c)
            p[r * s + c] = v;
    }
    return 0;
}

/* ----------------------------------------------------------------- */
/* Default path: no options, 8-bit, distinct inputs                  */
/* ----------------------------------------------------------------- */

static char *test_ssim_default_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssim");
    mu_assert("ssim extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, SSIM_W, SSIM_H);
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
    mu_assert("extract default", err == 0);

    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "ssim", &score, 0);
    mu_assert("get ssim score", err == 0);
    mu_assert("ssim in [0,1]", isfinite(score) && score >= 0.0 && score <= 1.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* enable_db=true: score is reported in decibels                     */
/* ----------------------------------------------------------------- */

static char *test_ssim_enable_db_branch(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssim");
    mu_assert("ssim extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "enable_db", "true", 0);
    mu_assert("set enable_db", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create with enable_db", err == 0);

    /* clip_db is left at default (false) so max_db = INFINITY — the clip
     * branch in init() is *not* taken. */
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, SSIM_W, SSIM_H);
    mu_assert("context_init enable_db", err == 0);

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
    mu_assert("extract enable_db", err == 0);

    double score = NAN;
    /* enable_db does not have VMAF_OPT_FLAG_FEATURE_PARAM; the feature is
     * still stored under the plain "ssim" key. */
    err = vmaf_feature_collector_get_score(fc, "ssim", &score, 0);
    mu_assert("get ssim score (db key)", err == 0);
    /* In dB mode the score should be non-negative and finite. */
    mu_assert("ssim db score finite and non-negative", isfinite(score) && score >= 0.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* clip_db=true: init() computes max_db from frame dimensions        */
/* ----------------------------------------------------------------- */

static char *test_ssim_clip_db_branch(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssim");
    mu_assert("ssim extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    /* enable_db must also be true for the clip to take effect in extract(). */
    int err = vmaf_dictionary_set(&opts, "enable_db", "true", 0);
    mu_assert("set enable_db", err == 0);
    err = vmaf_dictionary_set(&opts, "clip_db", "true", 0);
    mu_assert("set clip_db", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create with clip_db", err == 0);

    /* clip_db=true triggers the max_db computation in init() (lines 337-340). */
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, SSIM_W, SSIM_H);
    mu_assert("context_init clip_db", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    /* Identical inputs → SSIM = 1.0 → dB = −10*log10(0) = INFINITY → clipped
     * to max_db. */
    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8(&ref, 128u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 128u);
    mu_assert("alloc dist identical", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract identical clip_db", err == 0);

    double score = NAN;
    /* clip_db / enable_db lack VMAF_OPT_FLAG_FEATURE_PARAM, so the score
     * is still stored under the plain "ssim" key. */
    err = vmaf_feature_collector_get_score(fc, "ssim", &score, 0);
    mu_assert("get ssim clipped score", err == 0);
    mu_assert("ssim clip_db score is finite", isfinite(score));
    mu_assert("ssim clip_db score is positive", score > 0.0);

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

static char *test_ssim_10bit_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssim");
    mu_assert("ssim extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create 10bit", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 10u, SSIM_W, SSIM_H);
    mu_assert("context_init 10bit", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey10(&ref, 400u);
    mu_assert("alloc ref 10bit", err == 0);
    err = alloc_grey10(&dist, 600u);
    mu_assert("alloc dist 10bit", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract 10bit", err == 0);

    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "ssim", &score, 0);
    mu_assert("get ssim 10bit score", err == 0);
    mu_assert("ssim 10bit in [0,1]", isfinite(score) && score >= 0.0 && score <= 1.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* Allocate a 16-bit YUV420P picture from a per-pixel callback. */
typedef uint16_t (*px16_fn)(unsigned r, unsigned c);
static int alloc_px16(VmafPicture *pic, px16_fn f)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 16, SSIM_W, SSIM_H);
    if (err)
        return err;
    for (unsigned plane = 0; plane < 3; ++plane) {
        uint16_t *p = (uint16_t *)pic->data[plane];
        ptrdiff_t s = pic->stride[plane] / 2;
        unsigned w = pic->w[plane];
        unsigned h = pic->h[plane];
        for (unsigned r = 0; r < h; ++r)
            for (unsigned c = 0; c < w; ++c)
                p[r * s + c] = f(r, c);
    }
    return 0;
}

/* Small-amplitude, locally anti-correlated 16-bpc content: ref ramps up
 * where dist ramps down across the same window. With the correct (large)
 * 16-bpc c2 the structure term is stabilised toward 1; with the overflowed
 * (≈0, negative) c2 it collapses to the raw negative covariance ratio,
 * driving the SSIM score below 0. */
static uint16_t ramp_ref16(unsigned r, unsigned c)
{
    return (uint16_t)(32u + ((r + c) & 7u));
}
static uint16_t ramp_dist16(unsigned r, unsigned c)
{
    return (uint16_t)(32u + (7u - ((r + c) & 7u)));
}

/* ----------------------------------------------------------------- */
/* 16-bit HBD path: distorted pair must yield SSIM in [0,1].         */
/*                                                                   */
/* Regression guard for round-2 bug-hunt R2-1: integer_ssim computed */
/* `c1/c2 = samplemax * samplemax * K * w_d²` with `int samplemax`.   */
/* For 16-bpc, samplemax = 65535 and 65535*65535 overflows int       */
/* (signed-overflow UB, wraps to −131071), so the SSIM stability     */
/* constants go negative. With small-magnitude 16-bit content the    */
/* (correct, large-positive) c2 should dominate and pull the contrast*/
/* /structure terms toward a sane [0,1] score; the negative buggy c2 */
/* drives the score out of range / non-finite. Flat content is immune*/
/* (c1/c2 cancel when variance is 0), hence the small textured ramp. */
/* ----------------------------------------------------------------- */

static char *test_ssim_16bit_distorted_in_range(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssim");
    mu_assert("ssim extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create 16bit", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 16u, SSIM_W, SSIM_H);
    mu_assert("context_init 16bit", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_px16(&ref, ramp_ref16);
    mu_assert("alloc ref 16bit", err == 0);
    err = alloc_px16(&dist, ramp_dist16);
    mu_assert("alloc dist 16bit", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract 16bit", err == 0);

    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "ssim", &score, 0);
    mu_assert("get ssim 16bit score", err == 0);
    /* SSIM is mathematically bounded in [-1,1]; a correct 16-bpc c1/c2
     * yields a value in [0,1] here. The int-overflow bug pushes it out
     * of [0,1] or to a non-finite value. */
    mu_assert("ssim 16bit finite", isfinite(score));
    mu_assert("ssim 16bit in [0,1]", score >= 0.0 && score <= 1.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* Identical inputs: SSIM should be 1.0 on the default (no-db) path */
/* ----------------------------------------------------------------- */

static char *test_ssim_identical_is_one(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("ssim");
    mu_assert("ssim extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, SSIM_W, SSIM_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8(&ref, 64u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 64u);
    mu_assert("alloc dist identical", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract identical", err == 0);

    double score = NAN;
    err = vmaf_feature_collector_get_score(fc, "ssim", &score, 0);
    mu_assert("get ssim score", err == 0);
    /* Identical inputs must yield SSIM = 1.0 (within floating-point tolerance). */
    mu_assert("ssim identical == 1.0", fabs(score - 1.0) < 1e-6);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_ssim_default_extract);
    mu_run_test(test_ssim_enable_db_branch);
    mu_run_test(test_ssim_clip_db_branch);
    mu_run_test(test_ssim_10bit_extract);
    mu_run_test(test_ssim_16bit_distorted_in_range);
    mu_run_test(test_ssim_identical_is_one);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
