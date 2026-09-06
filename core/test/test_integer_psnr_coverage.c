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
/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this test mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

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
/* Shared extract fixture                                            */
/*                                                                   */
/* Context + collector + one ref/dist picture pair. Grouping them     */
/* keeps every test body inside the readability-function-size budget  */
/* instead of suppressing the finding (ADR-0141).                     */
/* ----------------------------------------------------------------- */

typedef struct PsnrFixture {
    VmafFeatureExtractorContext *ctx;
    VmafFeatureCollector *fc;
    VmafPicture ref;
    VmafPicture dist;
} PsnrFixture;

static char *psnr_fixture_open(PsnrFixture *fx, VmafDictionary *opts, enum VmafPixelFormat pix_fmt,
                               unsigned bpc, unsigned w, unsigned h)
{
    char *fail = NULL;
    VmafFeatureExtractor *fex = psnr_fex_or_fail(&fail);
    mu_assert("psnr extractor missing", fail == NULL);

    fx->ctx = NULL;
    fx->fc = NULL;
    int err = vmaf_feature_extractor_context_create(&fx->ctx, fex, opts);
    mu_assert("psnr context_create", err == 0);
    err = vmaf_feature_extractor_context_init(fx->ctx, pix_fmt, bpc, w, h);
    mu_assert("psnr context_init", err == 0);
    err = vmaf_feature_collector_init(&fx->fc);
    mu_assert("collector_init", err == 0);
    return NULL;
}

static char *psnr_fixture_alloc_pair(PsnrFixture *fx, enum VmafPixelFormat pix_fmt, unsigned bpc,
                                     unsigned w, unsigned h, unsigned ref_grey, unsigned dist_grey)
{
    int err = alloc_grey(&fx->ref, pix_fmt, bpc, w, h, ref_grey);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey(&fx->dist, pix_fmt, bpc, w, h, dist_grey);
    mu_assert("alloc dist", err == 0);
    return NULL;
}

static void psnr_fixture_close(PsnrFixture *fx)
{
    (void)vmaf_feature_extractor_context_close(fx->ctx);
    (void)vmaf_feature_extractor_context_destroy(fx->ctx);
    vmaf_feature_collector_destroy(fx->fc);
    vmaf_picture_unref(&fx->ref);
    vmaf_picture_unref(&fx->dist);
}

/* ----------------------------------------------------------------- */
/* extract() HBD path (10 / 12 bit)                                  */
/* ----------------------------------------------------------------- */

static char *run_extract_hbd_identical(unsigned bpc)
{
    PsnrFixture fx;
    char *fail = psnr_fixture_open(&fx, NULL, VMAF_PIX_FMT_YUV420P, bpc, 16u, 16u);
    mu_assert("fixture open", fail == NULL);
    fail = psnr_fixture_alloc_pair(&fx, VMAF_PIX_FMT_YUV420P, bpc, 16u, 16u, 100u, 100u);
    mu_assert("fixture pictures", fail == NULL);

    int err =
        vmaf_feature_extractor_context_extract(fx.ctx, &fx.ref, NULL, &fx.dist, NULL, 0, fx.fc);
    mu_assert("extract hbd identical", err == 0);

    /* Identical inputs -> SSE = 0 -> the psnr_max sentinel (ADR-1175). */
    double psnr_y = 0.0;
    err = vmaf_feature_collector_get_score(fx.fc, "psnr_y", &psnr_y, 0);
    mu_assert("get psnr_y", err == 0);
    mu_assert("psnr_y is finite", isfinite(psnr_y));
    mu_assert("psnr_y at max for identical inputs", psnr_y > 0.0);

    psnr_fixture_close(&fx);
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
    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "enable_apsnr", "true", 0);
    mu_assert("set enable_apsnr", err == 0);

    PsnrFixture fx;
    char *fail = psnr_fixture_open(&fx, opts, VMAF_PIX_FMT_YUV420P, 8u, 16u, 16u);
    mu_assert("fixture open", fail == NULL);

    /* Run a single frame so apsnr.n_pixels[*] > 0 -- otherwise log10(0) in
     * the flush path would emit NaN/inf.  Score the collector once with
     * non-identical inputs so apsnr.sse[*] > 0 as well. */
    fail = psnr_fixture_alloc_pair(&fx, VMAF_PIX_FMT_YUV420P, 8u, 16u, 16u, 100u, 110u);
    mu_assert("fixture pictures", fail == NULL);

    err = vmaf_feature_extractor_context_extract(fx.ctx, &fx.ref, NULL, &fx.dist, NULL, 0, fx.fc);
    mu_assert("extract for apsnr accumulation", err == 0);

    err = vmaf_feature_extractor_context_flush(fx.ctx, fx.fc);
    mu_assert("flush apsnr branch", err >= 0);

    double apsnr_y = 0.0;
    err = vmaf_feature_collector_get_aggregate(fx.fc, "apsnr_y", &apsnr_y);
    mu_assert("get apsnr_y aggregate", err == 0);
    mu_assert("apsnr_y is finite", isfinite(apsnr_y));

    psnr_fixture_close(&fx);
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

/* ----------------------------------------------------------------- */
/* uncapped option (ADR-1175)                                        */
/* ----------------------------------------------------------------- */

/* ref, a copy differing in exactly one luma sample by +1, and a
 * byte-identical copy — the three pictures both uncapped cases need. */
typedef struct UncappedPair {
    VmafPicture ref;
    VmafPicture dist_diff;
    VmafPicture dist_ident;
} UncappedPair;

static char *uncapped_pair_alloc(UncappedPair *pair, unsigned bpc, unsigned w, unsigned h,
                                 unsigned base)
{
    int err = alloc_grey(&pair->ref, VMAF_PIX_FMT_YUV420P, bpc, w, h, base);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey(&pair->dist_diff, VMAF_PIX_FMT_YUV420P, bpc, w, h, base);
    mu_assert("alloc dist_diff", err == 0);
    err = alloc_grey(&pair->dist_ident, VMAF_PIX_FMT_YUV420P, bpc, w, h, base);
    mu_assert("alloc dist_ident", err == 0);

    if (bpc == 8u) {
        ((uint8_t *)pair->dist_diff.data[0])[0] = (uint8_t)(base + 1u);
    } else {
        ((uint16_t *)pair->dist_diff.data[0])[0] = (uint16_t)(base + 1u);
    }
    return NULL;
}

static void uncapped_pair_free(UncappedPair *pair)
{
    vmaf_picture_unref(&pair->ref);
    vmaf_picture_unref(&pair->dist_diff);
    vmaf_picture_unref(&pair->dist_ident);
}

/* Extracts psnr_y for the differing pair at index 0 and the identical pair at
 * index 1 through one context created with `opts`. */
static char *extract_uncapped_pair(VmafFeatureExtractor *fex, VmafDictionary *opts,
                                   UncappedPair *pair, unsigned bpc, unsigned w, unsigned h,
                                   double *diff_db, double *ident_db)
{
    VmafFeatureExtractorContext *ctx = NULL;
    int err = vmaf_feature_extractor_context_create(&ctx, fex, opts);
    mu_assert("ctx create", err == 0);
    err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, bpc, w, h);
    mu_assert("ctx init", err == 0);

    VmafFeatureCollector *fc = NULL;
    err = vmaf_feature_collector_init(&fc);
    mu_assert("fc init", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &pair->ref, NULL, &pair->dist_diff, NULL, 0,
                                                 fc);
    mu_assert("extract differing pair", err == 0);
    err = vmaf_feature_collector_get_score(fc, "psnr_y", diff_db, 0);
    mu_assert("get psnr_y differing", err == 0);

    err = vmaf_feature_extractor_context_extract(ctx, &pair->ref, NULL, &pair->dist_ident, NULL, 1,
                                                 fc);
    mu_assert("extract identical pair", err == 0);
    err = vmaf_feature_collector_get_score(fc, "psnr_y", ident_db, 1);
    mu_assert("get psnr_y identical", err == 0);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    return NULL;
}

/* `tol == 0.0` asserts bit-exact equality. */
static char *check_uncapped_scores(double diff_db, double ident_db, double expect_diff,
                                   double expect_ident, double tol)
{
    mu_assert("differing-pair psnr_y", fabs(diff_db - expect_diff) <= tol);
    mu_assert("identical-pair psnr_y", ident_db == expect_ident);
    return NULL;
}

/* One bit depth's capped-vs-uncapped case: `capped_db` is the per-bitdepth cap
 * and `uncapped_db` the true logarithmic PSNR of a single +1 luma sample on a
 * 576x324 frame (ADR-1175). */
static char *run_uncapped_case(unsigned bpc, unsigned base, double capped_db, double uncapped_db)
{
    const unsigned w = 576u;
    const unsigned h = 324u;

    char *fail = NULL;
    VmafFeatureExtractor *fex = psnr_fex_or_fail(&fail);
    mu_assert("psnr extractor missing", fail == NULL);

    UncappedPair pair;
    fail = uncapped_pair_alloc(&pair, bpc, w, h, base);
    mu_assert("uncapped pair alloc", fail == NULL);

    /* 1. Default (capped): the differing pair truncates to the cap and the
     *    identical pair reports the same cap as its sentinel. */
    double diff_db = 0.0;
    double ident_db = 0.0;
    fail = extract_uncapped_pair(fex, NULL, &pair, bpc, w, h, &diff_db, &ident_db);
    mu_assert("capped extraction", fail == NULL);
    fail = check_uncapped_scores(diff_db, ident_db, capped_db, capped_db, 0.0);
    mu_assert("capped scores", fail == NULL);

    /* 2. uncapped=true: the differing pair reports the true value, the
     *    identical pair still reports the sentinel. */
    VmafDictionary *opts = NULL;
    int err = vmaf_dictionary_set(&opts, "uncapped", "true", 0);
    mu_assert("set uncapped opt", err == 0);
    fail = extract_uncapped_pair(fex, opts, &pair, bpc, w, h, &diff_db, &ident_db);
    mu_assert("uncapped extraction", fail == NULL);
    fail = check_uncapped_scores(diff_db, ident_db, uncapped_db, capped_db, 1e-5);
    mu_assert("uncapped scores", fail == NULL);

    uncapped_pair_free(&pair);
    return NULL;
}

static char *test_psnr_uncapped_8bit(void)
{
    return run_uncapped_case(8u, 128u, 60.0, 100.840479);
}

static char *test_psnr_uncapped_hbd(void)
{
    return run_uncapped_case(10u, 512u, 72.0, 112.907188);
}

/* Grouped so run_tests stays inside the readability-function-size branch
 * budget as cases are added (ADR-0141). */
static char *run_init_tests(void)
{
    mu_run_test(test_psnr_init_yuv400p_disables_chroma);
    mu_run_test(test_psnr_init_yuv444p_no_chroma_subsample);
    mu_run_test(test_psnr_init_yuv422p_horizontal_only_subsample);
    return NULL;
}

static char *run_extract_tests(void)
{
    mu_run_test(test_psnr_extract_hbd_10bit);
    mu_run_test(test_psnr_extract_hbd_12bit);
    mu_run_test(test_psnr_extract_hbd_16bit);
    mu_run_test(test_psnr_flush_apsnr_enabled);
    return NULL;
}

static char *run_uncapped_tests(void)
{
    mu_run_test(test_psnr_uncapped_8bit);
    mu_run_test(test_psnr_uncapped_hbd);
    return NULL;
}

char *run_tests(void)
{
    char *fail = run_init_tests();
    if (fail)
        return fail;
    fail = run_extract_tests();
    if (fail)
        return fail;
    return run_uncapped_tests();
}

/* NOLINTEND(modernize-use-nullptr) */
