/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CPU-only coverage for core/src/feature/float_ssim.c.
 *
 *  float_ssim tests exist only as GPU-gated CUDA/SYCL parity tests.
 *  This file adds CPU-path coverage for the fast suite:
 *
 *    1. init() + extract() + close() with 8-bit identical inputs.
 *    2. Identical inputs: SSIM should be 1.0 (no distortion).
 *    3. Distinct inputs: SSIM < 1.0.
 *    4. 10-bit init path.
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

/* float_ssim needs convolution workspace which uses the image dimensions.
 * Use a large enough frame so convolution has multiple full windows. */
#define FSSIM_W (32u)
#define FSSIM_H (32u)

static int alloc_grey8(VmafPicture *pic, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, FSSIM_W, FSSIM_H);
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

static char *test_float_ssim_8bit_identical(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim");
    mu_assert("float_ssim extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FSSIM_W, FSSIM_H);
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

    double ssim = NAN;
    err = vmaf_feature_collector_get_score(fc, "float_ssim", &ssim, 0);
    mu_assert("get float_ssim", err == 0);
    mu_assert("ssim finite", isfinite(ssim));
    /* Identical inputs: SSIM = 1.0 */
    mu_assert("ssim ~= 1.0 for identical inputs", fabs(ssim - 1.0) < 1e-4);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

static char *test_float_ssim_8bit_distinct(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim");
    mu_assert("float_ssim extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FSSIM_W, FSSIM_H);
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

    double ssim = NAN;
    err = vmaf_feature_collector_get_score(fc, "float_ssim", &ssim, 0);
    mu_assert("get float_ssim", err == 0);
    mu_assert("ssim finite", isfinite(ssim));
    mu_assert("ssim < 1.0 for distinct inputs", ssim < 1.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

static char *test_float_ssim_10bit_init(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_ssim");
    mu_assert("float_ssim extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 10u, FSSIM_W, FSSIM_H);
    mu_assert("context_init 10bit", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_ssim_8bit_identical);
    mu_run_test(test_float_ssim_8bit_distinct);
    mu_run_test(test_float_ssim_10bit_init);
    return NULL;
}
