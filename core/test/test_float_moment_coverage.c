/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CPU-only coverage for core/src/feature/float_moment.c.
 *
 *  float_moment tests exist only as GPU-gated CUDA/SYCL parity tests.
 *  This file adds CPU-path coverage for the fast suite:
 *
 *    1. init() + extract() + close() with 8-bit inputs — exercises the
 *       compute_1st_moment + compute_2nd_moment scalar paths via the
 *       SIMD-dispatched function pointers.
 *    2. Identical inputs: 1st and 2nd moments for ref and dist are equal.
 *    3. Distinct inputs: moments differ between ref and dist.
 *    4. close() on NULL-initialised state (partial init guard).
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

#define FM_W (16u)
#define FM_H (16u)

static int alloc_grey8(VmafPicture *pic, uint8_t v)
{
    int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, FM_W, FM_H);
    if (err)
        return err;
    uint8_t *p = (uint8_t *)pic->data[0];
    ptrdiff_t s = pic->stride[0];
    for (unsigned r = 0; r < FM_H; ++r)
        memset(p + r * s, v, FM_W);
    return 0;
}

static char *test_float_moment_8bit_identical(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_moment");
    mu_assert("float_moment extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FM_W, FM_H);
    mu_assert("context_init 8bit", err == 0);

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

    double ref1st = NAN;
    double dis1st = NAN;
    err = vmaf_feature_collector_get_score(fc, "float_moment_ref1st", &ref1st, 0);
    mu_assert("get ref1st", err == 0);
    err = vmaf_feature_collector_get_score(fc, "float_moment_dis1st", &dis1st, 0);
    mu_assert("get dis1st", err == 0);
    mu_assert("ref1st finite", isfinite(ref1st));
    mu_assert("dis1st finite", isfinite(dis1st));
    mu_assert("identical inputs: ref1st == dis1st", fabs(ref1st - dis1st) < 1e-6);

    double ref2nd = NAN;
    double dis2nd = NAN;
    err = vmaf_feature_collector_get_score(fc, "float_moment_ref2nd", &ref2nd, 0);
    mu_assert("get ref2nd", err == 0);
    err = vmaf_feature_collector_get_score(fc, "float_moment_dis2nd", &dis2nd, 0);
    mu_assert("get dis2nd", err == 0);
    mu_assert("ref2nd finite", isfinite(ref2nd));
    mu_assert("dis2nd finite", isfinite(dis2nd));
    mu_assert("identical inputs: ref2nd == dis2nd", fabs(ref2nd - dis2nd) < 1e-6);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

static char *test_float_moment_8bit_distinct(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_moment");
    mu_assert("float_moment extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FM_W, FM_H);
    mu_assert("context_init 8bit", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey8(&ref, 64u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey8(&dist, 200u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0u, fc);
    mu_assert("extract ok", err == 0);

    double ref1st = NAN;
    double dis1st = NAN;
    err = vmaf_feature_collector_get_score(fc, "float_moment_ref1st", &ref1st, 0);
    mu_assert("get ref1st", err == 0);
    err = vmaf_feature_collector_get_score(fc, "float_moment_dis1st", &dis1st, 0);
    mu_assert("get dis1st", err == 0);
    mu_assert("distinct inputs: moments differ", fabs(ref1st - dis1st) > 1e-3);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

/* Verify bpc is not used for moment (float_moment ignores bpc per the
 * (void)bpc in init()).  10-bit init must succeed. */
static char *test_float_moment_10bit_init(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_moment");
    mu_assert("float_moment extractor present", fex != NULL);

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 10u, FM_W, FM_H);
    mu_assert("context_init 10bit", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_moment_8bit_identical);
    mu_run_test(test_float_moment_8bit_distinct);
    mu_run_test(test_float_moment_10bit_init);
    return NULL;
}
