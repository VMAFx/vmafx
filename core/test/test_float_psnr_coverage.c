/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CPU-only coverage for core/src/feature/float_psnr.c.
 *
 *  All existing float_psnr tests are GPU-gated (CUDA/SYCL parity).
 *  This file adds end-to-end CPU path coverage for the fast suite:
 *
 *    1. init() + extract() + close() for 8-bit identical inputs —
 *       exercises the scalar noise_line path and feature collector append.
 *    2. init() with bpc=10 — validates the 10-bit peak/psnr_max branch.
 *    3. init() with bpc=12 — validates the 12-bit peak/psnr_max branch.
 *    4. init() with bpc=16 — validates the 16-bit peak/psnr_max branch.
 *    5. init() with an invalid bpc (e.g. 7) — must return an error.
 *    6. extract() with distinct 8-bit inputs — score < psnr_max.
 *    7. close() cleanup (aligned_free on partially-constructed state).
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
 * required Windows build compiles this TU with cl.exe, and this test mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#define FP_W (16u)
#define FP_H (16u)

static int alloc_grey(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned grey)
{
    int err = vmaf_picture_alloc(pic, pix_fmt, bpc, FP_W, FP_H);
    if (err)
        return err;

    for (unsigned p = 0; p < 3u; ++p) {
        if (bpc == 8) {
            uint8_t *row = (uint8_t *)pic->data[p];
            for (unsigned i = 0; i < pic->h[p]; ++i)
                memset(row + i * pic->stride[p], (uint8_t)grey, pic->w[p]);
        } else {
            uint16_t *row = (uint16_t *)pic->data[p];
            ptrdiff_t s = (ptrdiff_t)(pic->stride[p] / 2u);
            for (unsigned i = 0; i < pic->h[p]; ++i) {
                for (unsigned j = 0; j < pic->w[p]; ++j)
                    row[i * s + (ptrdiff_t)j] = (uint16_t)grey;
            }
        }
    }
    return 0;
}

/* Helper: run a full init→extract→close cycle and return the float_psnr score. */
static int run_float_psnr(unsigned bpc, unsigned ref_val, unsigned dist_val, double *score_out)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_psnr");
    if (!fex)
        return -ENOENT;

    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, NULL);
    if (err)
        return err;

    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, bpc, FP_W, FP_H);
    if (err) {
        (void)vmaf_feature_extractor_context_destroy(ctx);
        return err;
    }

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    if (err) {
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
        return err;
    }

    VmafPicture ref;
    VmafPicture dist;
    err = alloc_grey(&ref, VMAF_PIX_FMT_YUV420P, bpc, ref_val);
    if (err)
        goto cleanup;
    err = alloc_grey(&dist, VMAF_PIX_FMT_YUV420P, bpc, dist_val);
    if (err) {
        vmaf_picture_unref(&ref);
        goto cleanup;
    }

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    if (err) {
        vmaf_picture_unref(&ref);
        vmaf_picture_unref(&dist);
        goto cleanup;
    }

    if (score_out)
        err = vmaf_feature_collector_get_score(fc, "float_psnr", score_out, 0);

    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);

cleanup:
    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    return err;
}

/* ----------------------------------------------------------------- */

static char *test_float_psnr_8bit_identical(void)
{
    double score = 0.0;
    int err = run_float_psnr(8u, 128u, 128u, &score);
    mu_assert("float_psnr 8-bit identical: run ok", err == 0);
    mu_assert("float_psnr 8-bit identical: finite score", isfinite(score));
    /* Identical inputs → noise = 0 → clamped to psnr_max = 60 dB for 8-bit. */
    mu_assert("float_psnr 8-bit identical: score at psnr_max (60)", score >= 59.9 && score <= 60.1);
    return NULL;
}

static char *test_float_psnr_8bit_distinct(void)
{
    double score = 0.0;
    int err = run_float_psnr(8u, 100u, 200u, &score);
    mu_assert("float_psnr 8-bit distinct: run ok", err == 0);
    mu_assert("float_psnr 8-bit distinct: finite score", isfinite(score));
    mu_assert("float_psnr 8-bit distinct: score < psnr_max", score < 60.0);
    mu_assert("float_psnr 8-bit distinct: score > 0", score > 0.0);
    return NULL;
}

static char *test_float_psnr_10bit_identical(void)
{
    double score = 0.0;
    /* 10-bit: use mid-range 512 as identical inputs. */
    int err = run_float_psnr(10u, 512u, 512u, &score);
    mu_assert("float_psnr 10-bit identical: run ok", err == 0);
    mu_assert("float_psnr 10-bit identical: finite score", isfinite(score));
    /* psnr_max for 10-bit is 72 dB. */
    mu_assert("float_psnr 10-bit identical: score at psnr_max (72)",
              score >= 71.9 && score <= 72.1);
    return NULL;
}

static char *test_float_psnr_12bit_identical(void)
{
    double score = 0.0;
    /* 12-bit: use mid-range 2048 as identical inputs. */
    int err = run_float_psnr(12u, 2048u, 2048u, &score);
    mu_assert("float_psnr 12-bit identical: run ok", err == 0);
    mu_assert("float_psnr 12-bit identical: finite score", isfinite(score));
    /* psnr_max for 12-bit is 84 dB. */
    mu_assert("float_psnr 12-bit identical: score at psnr_max (84)",
              score >= 83.9 && score <= 84.1);
    return NULL;
}

static char *test_float_psnr_16bit_identical(void)
{
    double score = 0.0;
    int err = run_float_psnr(16u, 32768u, 32768u, &score);
    mu_assert("float_psnr 16-bit identical: run ok", err == 0);
    mu_assert("float_psnr 16-bit identical: finite score", isfinite(score));
    /* psnr_max for 16-bit is 108 dB. */
    mu_assert("float_psnr 16-bit identical: score at psnr_max (108)",
              score >= 107.9 && score <= 108.1);
    return NULL;
}

static char *test_float_psnr_invalid_bpc(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_psnr");
    mu_assert("float_psnr extractor present", fex != NULL);

    /* Allocate priv manually and call init() with bpc=7 (invalid). */
    void *priv = calloc(1u, fex->priv_size);
    mu_assert("priv alloc", priv != NULL);
    fex->priv = priv;
    int err = fex->init(fex, VMAF_PIX_FMT_YUV420P, 7u, FP_W, FP_H);
    mu_assert("float_psnr init bpc=7 must fail", err != 0);
    if (fex->close)
        (void)fex->close(fex);
    free(priv);
    fex->priv = NULL;
    return NULL;
}

/* The uncapped case needs the same three pictures in both modes; grouping them
 * keeps every helper below the readability-function-size budget (ADR-0141). */
typedef struct FloatPsnrPair {
    VmafPicture ref;
    VmafPicture dist_diff;  /* one luma sample differs from ref by +1 */
    VmafPicture dist_ident; /* byte-identical to ref */
} FloatPsnrPair;

static char *float_psnr_pair_alloc(FloatPsnrPair *pair)
{
    int err = alloc_grey(&pair->ref, VMAF_PIX_FMT_YUV420P, 8u, 128u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey(&pair->dist_diff, VMAF_PIX_FMT_YUV420P, 8u, 128u);
    mu_assert("alloc dist_diff", err == 0);
    err = alloc_grey(&pair->dist_ident, VMAF_PIX_FMT_YUV420P, 8u, 128u);
    mu_assert("alloc dist_ident", err == 0);

    ((uint8_t *)pair->dist_diff.data[0])[0] = 129u;
    return NULL;
}

static void float_psnr_pair_free(FloatPsnrPair *pair)
{
    vmaf_picture_unref(&pair->ref);
    vmaf_picture_unref(&pair->dist_diff);
    vmaf_picture_unref(&pair->dist_ident);
}

/* Runs one extraction pass with `opts`: the differing pair at index 0 and the
 * identical pair at index 1, reporting both float_psnr scores. */
static char *extract_float_psnr_pair(VmafFeatureExtractor *fex, VmafDictionary *opts,
                                     FloatPsnrPair *pair, double *diff_score, double *ident_score)
{
    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("ctx create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, FP_W, FP_H);
    mu_assert("ctx init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("fc init", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &pair->ref, NULL, &pair->dist_diff, NULL, 0,
                                                 fc);
    mu_assert("extract diff", err == 0);
    err = vmaf_feature_collector_get_score(fc, "float_psnr", diff_score, 0);
    mu_assert("get score diff", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &pair->ref, NULL, &pair->dist_ident, NULL, 1,
                                                 fc);
    mu_assert("extract ident", err == 0);
    err = vmaf_feature_collector_get_score(fc, "float_psnr", ident_score, 1);
    mu_assert("get score ident", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    return NULL;
}

/* `tol == 0.0` asserts bit-exact equality. */
static char *check_float_psnr_pair(VmafFeatureExtractor *fex, VmafDictionary *opts,
                                   FloatPsnrPair *pair, double expect_diff, double expect_ident,
                                   double tol)
{
    double diff_score = 0.0;
    double ident_score = 0.0;
    char *fail = extract_float_psnr_pair(fex, opts, pair, &diff_score, &ident_score);
    mu_assert("extraction pass", fail == NULL);
    mu_assert("differing-pair float_psnr", fabs(diff_score - expect_diff) <= tol);
    mu_assert("identical-pair float_psnr", ident_score == expect_ident);
    return NULL;
}

static char *test_float_psnr_uncapped(void)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("float_psnr");
    mu_assert("float_psnr extractor present", fex != NULL);

    FloatPsnrPair pair;
    char *fail = float_psnr_pair_alloc(&pair);
    mu_assert("pair alloc", fail == NULL);

    /* 1. Default (capped): differing -> 60.0, identical -> 60.0 */
    fail = check_float_psnr_pair(fex, NULL, &pair, 60.0, 60.0, 0.0);
    mu_assert("capped mode", fail == NULL);

    /* 2. Uncapped: differing -> 72.213264 dB (16x16, sse == 1), identical -> 60.0 */
    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "uncapped", "true", 0);
    mu_assert("set uncapped opt", err == 0);
    fail = check_float_psnr_pair(fex, opts, &pair, 72.213264, 60.0, 1e-4);
    mu_assert("uncapped mode", fail == NULL);

    float_psnr_pair_free(&pair);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_float_psnr_8bit_identical);
    mu_run_test(test_float_psnr_8bit_distinct);
    mu_run_test(test_float_psnr_10bit_identical);
    mu_run_test(test_float_psnr_12bit_identical);
    mu_run_test(test_float_psnr_16bit_identical);
    mu_run_test(test_float_psnr_invalid_bpc);
    mu_run_test(test_float_psnr_uncapped);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
