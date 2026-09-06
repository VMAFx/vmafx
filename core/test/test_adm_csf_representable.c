/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Regression coverage for
 *  T-UPSTREAM-1494-ADM-CSF-MODE-IRFACTOR-OVERFLOW-2026-09-03 / ADR-1191.
 *
 *  The integer ADM pipeline stores its contrast-sensitivity weights as
 *  fixed-point integers whose budgets were sized for the Watson97 CSF
 *  (weights around 1e-2). `adm_csf_mode=1` (Barten) at the default
 *  `adm_csf_scale` produces weights around 1.2 at scale 0 and 27 at scale 3,
 *  which wrapped the `uint16_t` / `uint32_t` storage silently and turned the
 *  emitted scores into noise (measured: `integer_adm2_csf_1` 0.000614 against
 *  a float reference of 0.9396). Likewise, the blended-CSF tables in
 *  barten_csf_tools.h return `-EINVAL` **as a float** for viewing geometries
 *  they do not tabulate, and converting a negative float to an unsigned
 *  integer type is undefined behaviour.
 *
 *  `init()` now evaluates whether the configured weights fit and `extract()`
 *  returns -EINVAL when they do not -- the same place, and the same status,
 *  as the pre-existing viewing-geometry guard that
 *  core/test/test_adm_coverage.c pins. So:
 *
 *    1. the default configuration still scores (the guard must not regress
 *       the Netflix golden path);
 *    2. `adm_csf_mode=1` with the default scale coefficients is rejected
 *       with -EINVAL instead of producing wrapped weights;
 *    3. `adm_csf_mode=1` with the small scale coefficients the fork's own
 *       golden tests use (adm_csf_scale=0.002893 /
 *       adm_csf_diag_scale=0.001586) still scores -- those weights fit, and
 *       python/test/feature_extractor_test.py pins their values;
 *    4. `adm_csf_mode=2` at a viewing geometry the blend table does not
 *       carry is rejected rather than converting -22.0f to an unsigned type;
 *    5. `adm_csf_mode=2` / `=3` at the tabulated default geometry still
 *       score (the default model `vmaf_v1.0.16_3d0h` requests mode 2).
 */

#include <errno.h>
#include <stddef.h>
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

#define ADM_W (64u)
#define ADM_H (64u)

/* One (option, value) pair for adm_extract_status(); a NULL key terminates. */
typedef struct AdmOpt {
    const char *key;
    const char *val;
} AdmOpt;

/* Allocate a flat-grey 8-bit YUV420P picture. */
static int alloc_grey8(VmafPicture *pic, uint8_t v)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, ADM_W, ADM_H);
    if (err)
        return err;
    uint8_t *p = (uint8_t *)pic->data[0];
    const ptrdiff_t stride = pic->stride[0];
    for (unsigned r = 0; r < ADM_H; ++r)
        memset(p + r * stride, v, ADM_W);
    return 0;
}

/* Score one synthetic frame pair with the `adm` extractor under `opts` and
 * return extract()'s status. `opts` may be NULL for the default
 * configuration. `err_msg` is set (and the return value is meaningless) only
 * when the harness itself fails. */
static int adm_extract_status(const AdmOpt *opts, char **err_msg)
{
    *err_msg = NULL;

    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name("adm");
    if (!fex) {
        *err_msg = "adm extractor missing";
        return 1;
    }

    VmafDictionary *dict = NULL;
    for (const AdmOpt *o = opts; o && o->key; ++o) {
        if (vmaf_dictionary_set(&dict, o->key, o->val, 0)) {
            *err_msg = "vmaf_dictionary_set failed";
            (void)vmaf_dictionary_free(&dict);
            return 1;
        }
    }

    VmafFeatureExtractorContext *ctx = NULL;
    /* create() takes ownership of `dict`, and frees it on failure. */
    if (vmaf_feature_extractor_context_create(&ctx, fex, dict)) {
        *err_msg = "context_create failed";
        return 1;
    }

    int err = vmaf_feature_extractor_context_init(ctx, VMAF_PIX_FMT_YUV420P, 8u, ADM_W, ADM_H);
    if (err) {
        *err_msg = "context_init failed";
        (void)vmaf_feature_extractor_context_destroy(ctx);
        return 1;
    }

    VmafFeatureCollector *fc = NULL;
    VmafPicture ref;
    VmafPicture dist;
    if (vmaf_feature_collector_init(&fc)) {
        *err_msg = "collector_init failed";
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
        return 1;
    }
    if (alloc_grey8(&ref, 100u) || alloc_grey8(&dist, 120u)) {
        *err_msg = "picture alloc failed";
        vmaf_feature_collector_destroy(fc);
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
        return 1;
    }

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);

    (void)vmaf_feature_extractor_context_close(ctx);
    (void)vmaf_feature_extractor_context_destroy(ctx);
    vmaf_feature_collector_destroy(fc);
    vmaf_picture_unref(&ref);
    vmaf_picture_unref(&dist);
    return err;
}

/* ----------------------------------------------------------------- */
/* The guard must not regress the default (Netflix golden) path      */
/* ----------------------------------------------------------------- */

static char *test_adm_default_config_still_scores(void)
{
    char *msg = NULL;
    const int err = adm_extract_status(NULL, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("default adm config must still score", err == 0);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* csf_mode=1 at the default scale coefficients: -EINVAL             */
/* ----------------------------------------------------------------- */

static char *test_adm_csf_mode_barten_default_scale_rejected(void)
{
    /* barten_csf(0, 3.0, 1080, 100.0, 1.0) is 1.21049666, so the scale-0
     * horizontal/vertical weight is 1.21049666 * 2^21 = 2538595 and the
     * diagonal weight 1.21049666 * 2^23 = 10154382 -- 38x and 155x past the
     * 65535 the uint16_t storage holds. Before ADR-1191 they wrapped to
     * 48227 and 61838 and the extractor scored on. */
    const AdmOpt opts[] = {{"adm_csf_mode", "1"}, {NULL, NULL}};
    char *msg = NULL;
    const int err = adm_extract_status(opts, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("adm_csf_mode=1 at default adm_csf_scale must return -EINVAL", err == -EINVAL);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* csf_mode=1 with the fork's golden scale coefficients: accepted    */
/* ----------------------------------------------------------------- */

static char *test_adm_csf_mode_barten_small_scale_accepted(void)
{
    /* python/test/feature_extractor_test.py
     * ::test_run_vmaf_integer_fextractor_with_feature_overloads pins the
     * scores of exactly this configuration; the scale-0 weights are 7344 and
     * 16104, comfortably inside uint16_t. The guard must not touch it. */
    const AdmOpt opts[] = {{"adm_csf_mode", "1"},
                           {"adm_csf_scale", "0.002893"},
                           {"adm_csf_diag_scale", "0.001586"},
                           {NULL, NULL}};
    char *msg = NULL;
    const int err = adm_extract_status(opts, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("adm_csf_mode=1 with small scale coefficients must still score", err == 0);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* csf_mode=2 outside the blend table: -EINVAL, not a negative cast  */
/* ----------------------------------------------------------------- */

static char *test_adm_csf_mode_blend_untabulated_geometry_rejected(void)
{
    /* barten_watson_blend_csf() tabulates (1080, 2160, 720, 480) x (3H, 5H)
     * only and returns -EINVAL -- as a float, i.e. -22.0f -- for anything
     * else. adm_ref_display_height=1200 at the default 3H clears the
     * pre-existing `nvd * rdh >= 3240` guard (3600), so before ADR-1191 the
     * -22.0f reached `(uint16_t)(-22.0f * 2097152.0)`, which is undefined
     * behaviour. */
    const AdmOpt opts[] = {{"adm_csf_mode", "2"}, {"adm_ref_display_height", "1200"}, {NULL, NULL}};
    char *msg = NULL;
    const int err = adm_extract_status(opts, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("adm_csf_mode=2 at an untabulated geometry must return -EINVAL", err == -EINVAL);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* csf_mode=2 / =3 at the tabulated default geometry: accepted       */
/* ----------------------------------------------------------------- */

static char *test_adm_csf_mode_blend_default_geometry_accepted(void)
{
    /* Mode 2 is what the fork's default model vmaf_v1.0.16_3d0h requests. */
    const AdmOpt blend[] = {{"adm_csf_mode", "2"}, {NULL, NULL}};
    char *msg = NULL;
    int err = adm_extract_status(blend, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("adm_csf_mode=2 at 1080@3H must still score", err == 0);

    const AdmOpt blend_mae[] = {{"adm_csf_mode", "3"}, {NULL, NULL}};
    err = adm_extract_status(blend_mae, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("adm_csf_mode=3 at 1080@3H must still score", err == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_default_config_still_scores);
    mu_run_test(test_adm_csf_mode_barten_default_scale_rejected);
    mu_run_test(test_adm_csf_mode_barten_small_scale_accepted);
    mu_run_test(test_adm_csf_mode_blend_untabulated_geometry_rejected);
    mu_run_test(test_adm_csf_mode_blend_default_geometry_accepted);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
