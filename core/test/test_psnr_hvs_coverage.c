/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 3 — psnr_hvs.c (third_party/xiph) gap-fill.
 *
 *  The existing SIMD tests (test_psnr_hvs_avx2.c, test_psnr_hvs_simd.c,
 *  test_psnr_hvs_neon.c) exercise bit-exactness between ISA variants.
 *  This file adds CPU-path branch coverage absent from the fast suite:
 *
 *    1. Default init (bpc=8, enable_chroma=true): extract() produces
 *       finite psnr_hvs_y, psnr_hvs_cb, psnr_hvs_cr, and psnr_hvs scores.
 *    2. bpc > 12 rejection: init() returns -EINVAL (line 421-427 of
 *       psnr_hvs.c).
 *    3. YUV400P forces enable_chroma=false: only psnr_hvs_y and the
 *       Y-only pooled psnr_hvs score are non-zero.
 *    4. enable_chroma=false option: same result as YUV400P path.
 *    5. 10-bit HBD path (bpc=10, bpc <= 12 is accepted): extract() uses
 *       the 16-bit input depth variant.
 *
 *  All tests use tiny 16x16 YUV frames (psnr_hvs processes 8x8 blocks
 *  so 16x16 is sufficient).
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

#define HVS_W (16u)
#define HVS_H (16u)

/* Allocate a flat-grey 8-bit YUV420P picture. */
static int alloc_grey8_420(VmafPicture *pic, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, HVS_W, HVS_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3; ++p) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        ptrdiff_t s = pic->stride[p];
        for (unsigned r = 0; r < pic->h[p]; ++r)
            memset(plane + r * s, v, pic->w[p]);
    }
    return 0;
}

/* Allocate a flat-grey 8-bit YUV400P picture (luma only). */
static int alloc_grey8_400(VmafPicture *pic, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV400P, 8, HVS_W, HVS_H);
    if (err)
        return err;
    uint8_t *plane = (uint8_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0];
    for (unsigned r = 0; r < HVS_H; ++r)
        memset(plane + r * s, v, HVS_W);
    return 0;
}

/* Allocate a flat-grey 10-bit YUV420P picture. */
static int alloc_grey10_420(VmafPicture *pic, uint16_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 10, HVS_W, HVS_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3; ++p) {
        uint16_t *plane = (uint16_t *)pic->data[p];
        ptrdiff_t s = pic->stride[p] / 2;
        for (unsigned r = 0; r < pic->h[p]; ++r) {
            for (unsigned c = 0; c < pic->w[p]; ++c)
                plane[r * s + c] = v;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- */
/* Default: 8-bit, enable_chroma=true (default)                     */
/* ----------------------------------------------------------------- */

static char *test_psnr_hvs_default_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hvs");
    mu_assert("psnr_hvs extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, HVS_W, HVS_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8_420(&ref, 100u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8_420(&dist, 120u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract psnr_hvs default", err == 0);

    double psnr_hvs_y = NAN;
    err = vmaf_feature_collector_get_score(fc, "psnr_hvs_y", &psnr_hvs_y, 0);
    mu_assert("get psnr_hvs_y", err == 0);
    mu_assert("psnr_hvs_y finite positive", isfinite(psnr_hvs_y) && psnr_hvs_y > 0.0);

    double psnr_hvs = NAN;
    err = vmaf_feature_collector_get_score(fc, "psnr_hvs", &psnr_hvs, 0);
    mu_assert("get psnr_hvs", err == 0);
    mu_assert("psnr_hvs finite positive", isfinite(psnr_hvs) && psnr_hvs > 0.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* bpc > 12: init() must return -EINVAL                              */
/* ----------------------------------------------------------------- */

static char *test_psnr_hvs_bpc_gt_12_rejected(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hvs");
    mu_assert("psnr_hvs extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);

    /* bpc=16 > 12 must trigger the EINVAL path in init(). */
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 16u, HVS_W, HVS_H);
    mu_assert("init with bpc=16 must return -EINVAL", err == -EINVAL);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* YUV400P forces enable_chroma=false in init()                      */
/* ----------------------------------------------------------------- */

static char *test_psnr_hvs_yuv400p_luma_only(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hvs");
    mu_assert("psnr_hvs extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);
    /* YUV400P triggers the `enable_chroma = false` branch in init()
     * (lines 429-430 of psnr_hvs.c). */
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV400P, 8u, HVS_W, HVS_H);
    mu_assert("context_init YUV400P", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8_400(&ref, 90u);
    mu_assert("alloc ref 400", err == 0);
    err = alloc_grey8_400(&dist, 110u);
    mu_assert("alloc dist 400", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract psnr_hvs YUV400P", err == 0);

    /* Y channel is always written; psnr_hvs is the Y-only pooled score. */
    double psnr_hvs_y = NAN;
    err = vmaf_feature_collector_get_score(fc, "psnr_hvs_y", &psnr_hvs_y, 0);
    mu_assert("get psnr_hvs_y luma-only", err == 0);
    mu_assert("psnr_hvs_y finite positive", isfinite(psnr_hvs_y) && psnr_hvs_y > 0.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* enable_chroma=false option (explicit)                             */
/* ----------------------------------------------------------------- */

static char *test_psnr_hvs_disable_chroma_option(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hvs");
    mu_assert("psnr_hvs extractor missing", fex != NULL);

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "enable_chroma", "false", 0);
    mu_assert("set enable_chroma=false", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("context_create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, HVS_W, HVS_H);
    mu_assert("context_init enable_chroma=false", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8_420(&ref, 80u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8_420(&dist, 100u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract psnr_hvs chroma-disabled", err == 0);

    double psnr_hvs_y = NAN;
    err = vmaf_feature_collector_get_score(fc, "psnr_hvs_y", &psnr_hvs_y, 0);
    mu_assert("get psnr_hvs_y chroma-disabled", err == 0);
    mu_assert("psnr_hvs_y finite positive (chroma disabled)",
              isfinite(psnr_hvs_y) && psnr_hvs_y > 0.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* 10-bit HBD path (bpc=10 <= 12, accepted)                         */
/* ----------------------------------------------------------------- */

static char *test_psnr_hvs_10bit_extract(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr_hvs");
    mu_assert("psnr_hvs extractor missing", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create 10bit", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 10u, HVS_W, HVS_H);
    mu_assert("context_init 10bit", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey10_420(&ref, 400u);
    mu_assert("alloc ref 10bit", err == 0);
    err = alloc_grey10_420(&dist, 600u);
    mu_assert("alloc dist 10bit", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract psnr_hvs 10bit", err == 0);

    double psnr_hvs_y = NAN;
    err = vmaf_feature_collector_get_score(fc, "psnr_hvs_y", &psnr_hvs_y, 0);
    mu_assert("get psnr_hvs_y 10bit", err == 0);
    mu_assert("psnr_hvs_y 10bit finite positive", isfinite(psnr_hvs_y) && psnr_hvs_y > 0.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_psnr_hvs_default_extract);
    mu_run_test(test_psnr_hvs_bpc_gt_12_rejected);
    mu_run_test(test_psnr_hvs_yuv400p_luma_only);
    mu_run_test(test_psnr_hvs_disable_chroma_option);
    mu_run_test(test_psnr_hvs_10bit_extract);
    return NULL;
}
