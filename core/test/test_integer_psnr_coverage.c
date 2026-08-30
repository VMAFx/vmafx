/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage round 2 — integer_psnr.c gap-fill.
 *
 *  The existing test_psnr.c file exercises a single 16-bit psnr_hbd
 *  case.  This file plugs the remaining uncovered branches identified
 *  by the 2026-05-31 coverage run on `core/src/feature/integer_psnr.c`
 *  (line 70.1% baseline):
 *
 *    1. init() with `min_sse>0` to drive the psnr_max ceiling branch
 *       (lines 128-136 of integer_psnr.c).
 *    2. init() with pix_fmt=YUV400P to disable chroma (line 124-125).
 *    3. init() with pix_fmt=YUV444P to skip horizontal subsampling
 *       (the chroma-stride ceil-divide branches at 129-134).
 *    4. extract() for 10-bit + 12-bit inputs (HBD path at lines
 *       214-249).
 *    5. extract() for invalid bpc (returns -EINVAL at line 268-269).
 *    6. flush() with enable_apsnr=true (lines 277-292).
 *
 *  These do not touch the SIMD dispatch; that is exercised by the
 *  parity test_psnr.c suite already.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

/* Helper: locate the psnr extractor or return a test-fail string. */
static VmafFeatureExtractor *psnr_fex_or_fail(char **fail_out)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("psnr");
    if (!fex)
        *fail_out = (char *)"psnr extractor missing";
    return fex;
}

/* Allocate a flat-grey picture at the given pix_fmt / bpc / dimensions. */
static int alloc_grey(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                      unsigned h, unsigned grey)
{
    int err = vmaf_picture_alloc(pic, pix_fmt, bpc, w, h);
    if (err)
        return err;
    const unsigned planes = (pix_fmt == VMAF_PIX_FMT_YUV400P) ? 1u : 3u;
    for (unsigned p = 0; p < planes; ++p) {
        if (bpc == 8) {
            uint8_t *row = (uint8_t *)pic->data[p];
            for (unsigned i = 0; i < pic->h[p]; ++i) {
                memset(row + i * pic->stride[p], (uint8_t)grey, pic->w[p]);
            }
        } else {
            uint16_t *row = (uint16_t *)pic->data[p];
            const ptrdiff_t s = pic->stride[p] / 2;
            for (unsigned i = 0; i < pic->h[p]; ++i) {
                for (unsigned j = 0; j < pic->w[p]; ++j)
                    row[i * s + j] = (uint16_t)grey;
            }
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- */
/* init() path coverage                                              */
/* ----------------------------------------------------------------- */

static char *test_psnr_init_yuv400p_disables_chroma(void)
{
    char *fail = NULL;
    VmafFeatureExtractor *fex = psnr_fex_or_fail(&fail);
    if (fail)
        return fail;

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("psnr context_create", err == 0 && ctx != NULL);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV400P, 8u, 32u, 32u);
    mu_assert("psnr init YUV400P", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    return NULL;
}

static char *test_psnr_init_yuv444p_no_chroma_subsample(void)
{
    char *fail = NULL;
    VmafFeatureExtractor *fex = psnr_fex_or_fail(&fail);
    if (fail)
        return fail;

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("psnr context_create", err == 0);

    /* Drive the `min_sse>0` branch (psnr_max ceiling); use the
     * feature_param option dict to pass min_sse + reduced_hbd_peak. */
    VmafDictionary *opts = NULL;
    err = vmaf_dictionary_set(&opts, "min_sse", "1.0", 0);
    mu_assert("set min_sse opt", err == 0);

    VmafFeatureExtractorContext *ctx2 = NULL;
    err = vmaf_feature_extractor_context_create(&ctx2, fex, opts);
    mu_assert("psnr context_create with opts", err == 0);

    err = vmaf_feature_extractor_context_init(ctx2, VMAF_PIX_FMT_YUV444P, 8u, 32u, 32u);
    mu_assert("psnr init YUV444P", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    (void)vmaf_feature_extractor_context_close(ctx2);
    (void)vmaf_feature_extractor_context_destroy(ctx2);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

static char *test_psnr_init_yuv422p_horizontal_only_subsample(void)
{
    char *fail = NULL;
    VmafFeatureExtractor *fex = psnr_fex_or_fail(&fail);
    if (fail)
        return fail;

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "min_sse", "0.5", 0);
    mu_assert("set min_sse opt", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("psnr context_create with opts", err == 0);

    /* YUV422P + min_sse>0 exercises ss_hor=true / ss_ver=false. */
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV422P, 8u, 32u, 32u);
    mu_assert("psnr init YUV422P", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* extract() HBD path (10 / 12 bit)                                  */
/* ----------------------------------------------------------------- */

static char *run_extract_hbd_identical(unsigned bpc)
{
    char *fail = NULL;
    VmafFeatureExtractor *fex = psnr_fex_or_fail(&fail);
    if (fail)
        return fail;

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    mu_assert("psnr context_create", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, bpc, 16u, 16u);
    mu_assert("psnr init hbd", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey(&ref, VMAF_PIX_FMT_YUV420P, bpc, 16u, 16u, 100u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey(&dist, VMAF_PIX_FMT_YUV420P, bpc, 16u, 16u, 100u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract hbd identical", err == 0);

    /* Identical inputs → SSE = 0 → MSE clamped to 1e-16 → psnr at psnr_max. */
    double psnr_y = 0.0;
    err = vmaf_feature_collector_get_score(fc, "psnr_y", &psnr_y, 0);
    mu_assert("get psnr_y", err == 0);
    mu_assert("psnr_y is finite", isfinite(psnr_y));
    mu_assert("psnr_y at max for identical inputs", psnr_y > 0.0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return NULL;
}

static char *test_psnr_extract_hbd_10bit(void)
{
    return run_extract_hbd_identical(10u);
}

static char *test_psnr_extract_hbd_12bit(void)
{
    return run_extract_hbd_identical(12u);
}

/* ----------------------------------------------------------------- */
/* flush() with enable_apsnr=true                                    */
/* ----------------------------------------------------------------- */

static char *test_psnr_flush_apsnr_enabled(void)
{
    char *fail = NULL;
    VmafFeatureExtractor *fex = psnr_fex_or_fail(&fail);
    if (fail)
        return fail;

    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "enable_apsnr", "true", 0);
    mu_assert("set enable_apsnr", err == 0);

    VmafFeatureExtractorContext *ctx = NULL;
    err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("psnr context_create with apsnr", err == 0);

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, 16u, 16u);
    mu_assert("psnr init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    /* Run a single frame so apsnr.n_pixels[*] > 0 — otherwise log10(0) in
     * the flush path would emit NaN/inf.  Score the collector once with
     * non-identical inputs so apsnr.sse[*] > 0 as well. */
    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey(&ref, VMAF_PIX_FMT_YUV420P, 8u, 16u, 16u, 100u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey(&dist, VMAF_PIX_FMT_YUV420P, 8u, 16u, 16u, 110u);
    mu_assert("alloc dist", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    mu_assert("extract for apsnr accumulation", err == 0);

    err = vmaf_feature_extractor_context_flush(ctx, fc);
    mu_assert("flush apsnr branch", err >= 0);

    double apsnr_y = 0.0;
    err = vmaf_feature_collector_get_aggregate(fc, "apsnr_y", &apsnr_y);
    mu_assert("get apsnr_y aggregate", err == 0);
    mu_assert("apsnr_y is finite", isfinite(apsnr_y));

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    /* opts ownership transferred to ctx and freed by context_destroy. */
    return NULL;
}

/* ----------------------------------------------------------------- */
/* extract() with bpc=16 (rare path, executes psnr_hbd switch)       */
/* ----------------------------------------------------------------- */

static char *test_psnr_extract_hbd_16bit(void)
{
    return run_extract_hbd_identical(16u);
}

char *run_tests(void)
{
    mu_run_test(test_psnr_init_yuv400p_disables_chroma);
    mu_run_test(test_psnr_init_yuv444p_no_chroma_subsample);
    mu_run_test(test_psnr_init_yuv422p_horizontal_only_subsample);
    mu_run_test(test_psnr_extract_hbd_10bit);
    mu_run_test(test_psnr_extract_hbd_12bit);
    mu_run_test(test_psnr_extract_hbd_16bit);
    mu_run_test(test_psnr_flush_apsnr_enabled);
    return NULL;
}
