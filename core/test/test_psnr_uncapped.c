/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Regression test for T-UPSTREAM-1109 (Netflix/vmaf#1109) — the
 *  `psnr_max` ceiling used to serve two incompatible roles at once:
 *
 *    (a) the finite stand-in reported when the two planes are
 *        byte-identical and the true PSNR is +inf, and
 *    (b) a hard truncation of every genuinely computed value above it.
 *
 *  Role (b) meant an 8-bit 576x324 pair differing by a single luma step
 *  (sse == 1 over 186624 samples, true PSNR 100.840479 dB) was reported
 *  as 60.000000 dB. ADR-1193 adds an opt-in `uncapped` option that drops
 *  role (b) and keeps role (a).
 *
 *  What each case pins:
 *    1. default (uncapped absent)  -> psnr_y == 60.0        (unchanged)
 *    2. uncapped=true              -> psnr_y == 100.840479  (the fix)
 *    3. uncapped=true, identical   -> psnr_y == 60.0        (sentinel kept)
 *    4/5/6. the same three for the `float_psnr` extractor.
 *    7. uncapped=true on 10-bit    -> above the 72 dB ceiling.
 *
 *  Case 2 and case 5 are the ones that fail against the pre-ADR-1193
 *  tree (they report 60.0); cases 1, 3, 4, 6 are the no-change guards
 *  that would catch a fix which moved the default scores.
 *
 *  This is a fork-added file. The Netflix golden assertions in
 *  python/test/ pin the 60 / 84 / 108 dB values for byte-identical
 *  (sse == 0) pairs and are untouched by this change — case 3 and case 6
 *  assert exactly that invariant from the C side.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#include "dict.h"
#include "feature/feature_collector.h"
#include "feature/feature_extractor.h"
#include "libvmaf/picture.h"

/* The ledger's reproduction geometry: 576x324 luma = 186624 samples, so a
 * single +1 luma step gives sse == 1 and
 * 10 * log10(255^2 * 186624) = 100.840479... dB. */
#define UNCAPPED_W 576u
#define UNCAPPED_H 324u
#define UNCAPPED_TRUE_PSNR_Y 100.84047854497734
/* Default 8-bit ceiling: (6 * bpc) + 12. */
#define UNCAPPED_PSNR_MAX_8BPC 60.0

/* Fill one plane with a constant sample value. */
static void fill_plane(VmafPicture *pic, unsigned p, unsigned bpc, unsigned grey)
{
    if (bpc == 8u) {
        uint8_t *base = (uint8_t *)pic->data[p];
        for (unsigned i = 0; i < pic->h[p]; ++i) {
            memset(base + (size_t)i * pic->stride[p], (uint8_t)grey, pic->w[p]);
        }
        return;
    }
    uint16_t *base = (uint16_t *)pic->data[p];
    const size_t s = (size_t)pic->stride[p] / 2u;
    for (unsigned i = 0; i < pic->h[p]; ++i) {
        for (unsigned j = 0; j < pic->w[p]; ++j) {
            base[(i * s) + j] = (uint16_t)grey;
        }
    }
}

/* Allocate a flat-grey YUV420P picture at the fixture geometry. */
static int alloc_grey(VmafPicture *pic, unsigned bpc, unsigned grey)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, bpc, UNCAPPED_W, UNCAPPED_H);
    if (err) {
        return err;
    }
    for (unsigned p = 0; p < 3u; ++p) {
        fill_plane(pic, p, bpc, grey);
    }
    return 0;
}

/* Bump the first luma sample by one step so sse == 1 over the luma plane
 * while both chroma planes stay byte-identical (sse == 0). */
static void flip_one_luma_step(VmafPicture *pic, unsigned bpc)
{
    if (bpc == 8u) {
        uint8_t *luma = (uint8_t *)pic->data[0];
        luma[0] = (uint8_t)(luma[0] + 1u);
    } else {
        uint16_t *luma = (uint16_t *)pic->data[0];
        luma[0] = (uint16_t)(luma[0] + 1u);
    }
}

/*
 * Score one frame through `fex_name` with the given option dict and hand
 * back the value of `feature`. `identical` selects the sse == 0 pair.
 * Returns a mu_assert failure string, or nullptr with *out set.
 */
static char *make_pair(VmafPicture *ref, VmafPicture *dist, unsigned bpc, int identical)
{
    int err = alloc_grey(ref, bpc, 100u);
    mu_assert("alloc ref", err == 0);
    err = alloc_grey(dist, bpc, 100u);
    mu_assert("alloc dist", err == 0);
    if (!identical) {
        flip_one_luma_step(dist, bpc);
    }
    return nullptr;
}

/* Create and init one extractor context with the given option dict. */
static char *make_ctx(VmafFeatureExtractorContext **ctx, const char *fex_name, VmafDictionary *opts,
                      unsigned bpc)
{
    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(fex_name);
    mu_assert("feature extractor missing", fex != nullptr);

    int err = vmaf_feature_extractor_context_create(ctx, fex, opts);
    mu_assert("context_create", err == 0 && *ctx != nullptr);

    err = vmaf_feature_extractor_context_init(*ctx, VMAF_PIX_FMT_YUV420P, bpc, UNCAPPED_W,
                                              UNCAPPED_H);
    mu_assert("context_init", err == 0);
    return nullptr;
}

/*
 * Score one frame through `fex_name` with the given option dict and hand
 * back the value of `feature`. `identical` selects the sse == 0 pair.
 * Returns a mu_assert failure string, or nullptr with *out set.
 */
static char *score_one(const char *fex_name, const char *feature, VmafDictionary *opts,
                       unsigned bpc, int identical, double *out)
{
    VmafFeatureExtractorContext *ctx = nullptr;
    char *fail = make_ctx(&ctx, fex_name, opts, bpc);
    if (fail) {
        return fail;
    }

    VmafFeatureCollector *fc = nullptr;
    int err = vmaf_feature_collector_init(&fc);
    mu_assert("collector_init", err == 0);

    VmafPicture ref;
    VmafPicture dist;
    fail = make_pair(&ref, &dist, bpc, identical);
    if (fail) {
        return fail;
    }

    err = vmaf_feature_extractor_context_extract(ctx, &ref, nullptr, &dist, nullptr, 0, fc);
    mu_assert("extract", err == 0);

    err = vmaf_feature_collector_get_score(fc, feature, out, 0);
    mu_assert("get_score", err == 0);
    mu_assert("score is finite", isfinite(*out));

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    /* `opts` ownership transferred to ctx and freed by context_destroy. */
    return nullptr;
}

static char *make_uncapped_opts(VmafDictionary **opts)
{
    const int err = vmaf_dictionary_set(opts, "uncapped", "true", 0);
    mu_assert("set uncapped opt", err == 0);
    return nullptr;
}

/* ----------------------------------------------------------------- */
/* integer `psnr`                                                     */
/* ----------------------------------------------------------------- */

/* No-change guard: the default must keep reporting the truncated value. */
static char *test_psnr_default_still_truncates(void)
{
    double score = 0.0;
    char *fail = score_one("psnr", "psnr_y", nullptr, 8u, 0, &score);
    if (fail) {
        return fail;
    }
    mu_assert("default psnr_y must stay at the 60 dB ceiling",
              fabs(score - UNCAPPED_PSNR_MAX_8BPC) < 1e-9);
    return nullptr;
}

/* The fix: with `uncapped` the true 100.840479 dB is reported. */
static char *test_psnr_uncapped_reports_true_value(void)
{
    VmafDictionary *opts = nullptr;
    char *fail = make_uncapped_opts(&opts);
    if (fail) {
        return fail;
    }

    double score = 0.0;
    fail = score_one("psnr", "psnr_y", opts, 8u, 0, &score);
    if (fail) {
        return fail;
    }
    mu_assert("uncapped psnr_y must report the true 100.840479 dB",
              fabs(score - UNCAPPED_TRUE_PSNR_Y) < 1e-9);
    return nullptr;
}

/* The sentinel survives: an sse == 0 pair still reports psnr_max, which
 * is what the Netflix golden 60 / 84 / 108 dB assertions pin. */
static char *test_psnr_uncapped_keeps_zero_sse_sentinel(void)
{
    VmafDictionary *opts = nullptr;
    char *fail = make_uncapped_opts(&opts);
    if (fail) {
        return fail;
    }

    double score = 0.0;
    fail = score_one("psnr", "psnr_y", opts, 8u, 1, &score);
    if (fail) {
        return fail;
    }
    mu_assert("uncapped psnr_y must keep psnr_max as the sse==0 sentinel",
              fabs(score - UNCAPPED_PSNR_MAX_8BPC) < 1e-9);
    return nullptr;
}

/* The chroma planes of the flipped pair are byte-identical, so they must
 * report the sentinel in uncapped mode too — i.e. `uncapped` does not
 * inflate the sentinel the way `min_sse` does. */
static char *test_psnr_uncapped_chroma_sentinel_not_inflated(void)
{
    VmafDictionary *opts = nullptr;
    char *fail = make_uncapped_opts(&opts);
    if (fail) {
        return fail;
    }

    double score = 0.0;
    fail = score_one("psnr", "psnr_cb", opts, 8u, 0, &score);
    if (fail) {
        return fail;
    }
    mu_assert("uncapped psnr_cb must stay at psnr_max for identical chroma",
              fabs(score - UNCAPPED_PSNR_MAX_8BPC) < 1e-9);
    return nullptr;
}

/* HBD: the 10-bit ceiling is 72 dB and one 10-bit luma step over the same
 * geometry is well above it. */
static char *test_psnr_uncapped_hbd_exceeds_ceiling(void)
{
    VmafDictionary *opts = nullptr;
    char *fail = make_uncapped_opts(&opts);
    if (fail) {
        return fail;
    }

    double score = 0.0;
    fail = score_one("psnr", "psnr_y", opts, 10u, 0, &score);
    if (fail) {
        return fail;
    }
    /* 10 * log10(1023^2 * 186624) = 112.90... dB, above the 72 dB cap. */
    mu_assert("uncapped 10-bit psnr_y must exceed the 72 dB ceiling", score > 72.0);
    mu_assert("uncapped 10-bit psnr_y must match 10*log10(peak^2 * n)",
              fabs(score - 10.0 * log10(1023.0 * 1023.0 * (double)(UNCAPPED_W * UNCAPPED_H))) <
                  1e-9);
    return nullptr;
}

/* ----------------------------------------------------------------- */
/* `float_psnr`                                                       */
/* ----------------------------------------------------------------- */

static char *test_float_psnr_default_still_truncates(void)
{
    double score = 0.0;
    char *fail = score_one("float_psnr", "float_psnr", nullptr, 8u, 0, &score);
    if (fail) {
        return fail;
    }
    mu_assert("default float_psnr must stay at the 60 dB ceiling",
              fabs(score - UNCAPPED_PSNR_MAX_8BPC) < 1e-9);
    return nullptr;
}

static char *test_float_psnr_uncapped_reports_true_value(void)
{
    VmafDictionary *opts = nullptr;
    char *fail = make_uncapped_opts(&opts);
    if (fail) {
        return fail;
    }

    double score = 0.0;
    fail = score_one("float_psnr", "float_psnr", opts, 8u, 0, &score);
    if (fail) {
        return fail;
    }
    mu_assert("uncapped float_psnr must report the true 100.840479 dB",
              fabs(score - UNCAPPED_TRUE_PSNR_Y) < 1e-6);
    return nullptr;
}

static char *test_float_psnr_uncapped_keeps_zero_noise_sentinel(void)
{
    VmafDictionary *opts = nullptr;
    char *fail = make_uncapped_opts(&opts);
    if (fail) {
        return fail;
    }

    double score = 0.0;
    fail = score_one("float_psnr", "float_psnr", opts, 8u, 1, &score);
    if (fail) {
        return fail;
    }
    mu_assert("uncapped float_psnr must keep psnr_max as the zero-noise sentinel",
              fabs(score - UNCAPPED_PSNR_MAX_8BPC) < 1e-9);
    return nullptr;
}

static char *run_integer_psnr_tests(void)
{
    mu_run_test(test_psnr_default_still_truncates);
    mu_run_test(test_psnr_uncapped_reports_true_value);
    mu_run_test(test_psnr_uncapped_keeps_zero_sse_sentinel);
    mu_run_test(test_psnr_uncapped_chroma_sentinel_not_inflated);
    mu_run_test(test_psnr_uncapped_hbd_exceeds_ceiling);
    return nullptr;
}

static char *run_float_psnr_tests(void)
{
    mu_run_test(test_float_psnr_default_still_truncates);
    mu_run_test(test_float_psnr_uncapped_reports_true_value);
    mu_run_test(test_float_psnr_uncapped_keeps_zero_noise_sentinel);
    return nullptr;
}

char *run_tests(void)
{
    char *fail = run_integer_psnr_tests();
    if (fail) {
        return fail;
    }
    return run_float_psnr_tests();
}
