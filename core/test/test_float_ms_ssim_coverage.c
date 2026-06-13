/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CPU-only coverage for core/src/feature/float_ms_ssim.c.
 *
 *  float_ms_ssim tests exist only as GPU-gated CUDA/SYCL parity tests.
 *  This file adds CPU-path coverage for the fast suite:
 *
 *    1. init() + extract() + close() with 8-bit identical inputs.
 *    2. Identical inputs: float_ms_ssim score should be 1.0.
 *    3. Distinct inputs: score < 1.0.
 *    4. 10-bit init path.
 *
 *  MS-SSIM uses a 5-level 11-tap Gaussian pyramid.  The minimum allowed
 *  dimension is GAUSSIAN_LEN << (SCALES - 1) = 11 << 4 = 176.  Use 176x176.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

#define FMSS_W (176u)
#define FMSS_H (176u)

static int alloc_grey8(VmafPicture *pic, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, FMSS_W, FMSS_H);
    if (err)
        return err;
    for (unsigned p = 0; p < 3u; ++p) {
        uint8_t *plane = (uint8_t *)pic->data[p];
        ptrdiff_t s = pic->stride[p];
        for (unsigned r = 0; r < pic->h[p]; ++r)
            memset(plane + r * s, v, pic->w[p]);
    }
    return 0;
}

static char *test_float_ms_ssim_8bit_identical(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FMSS_W, FMSS_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8(&ref, 128u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 128u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0u, fc);
    mu_assert("extract ok", err == 0);

    double ms_ssim = NAN;
    err = vmaf_feature_collector_get_score(fc, "float_ms_ssim", &ms_ssim, 0);
    mu_assert("get float_ms_ssim", err == 0);
    mu_assert("ms_ssim finite", isfinite(ms_ssim));
    /* Identical inputs → MS-SSIM = 1.0. */
    mu_assert("ms_ssim ~= 1.0 for identical inputs", fabs(ms_ssim - 1.0) < 1e-4);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

static char *test_float_ms_ssim_8bit_distinct(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FMSS_W, FMSS_H);
    mu_assert("context_init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8(&ref, 100u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 200u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0u, fc);
    mu_assert("extract ok", err == 0);

    double ms_ssim = NAN;
    err = vmaf_feature_collector_get_score(fc, "float_ms_ssim", &ms_ssim, 0);
    mu_assert("get float_ms_ssim", err == 0);
    mu_assert("ms_ssim finite", isfinite(ms_ssim));
    mu_assert("ms_ssim < 1.0 for distinct inputs", ms_ssim < 1.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

static char *test_float_ms_ssim_10bit_init(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ms_ssim");
    mu_assert("float_ms_ssim extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 10u, FMSS_W, FMSS_H);
    mu_assert("context_init 10bit", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ms_ssim_8bit_identical);
    mu_run_test(test_float_ms_ssim_8bit_distinct);
    mu_run_test(test_float_ms_ssim_10bit_init);
    return NULL;
}
