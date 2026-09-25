/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Regression coverage for
 *  T-UPSTREAM-1494-ADM-CSF-MODE-IRFACTOR-OVERFLOW-2026-09-03 / ADR-1191 and
 *  T-ADM-CSF-MODE-1-BARTEN-DEGENERATE-2026-09-05 / ADR-1325.
 *
 *  The integer ADM pipeline stores its contrast-sensitivity weights as
 *  fixed-point integers whose budgets were sized for the Watson97 CSF
 *  (weights around 1e-2). `adm_csf_mode=1` (Barten) at the default
 *  `adm_csf_scale` produces weights around 1.2 at scale 0 and 27 at scale 3,
 *  which wrapped the `uint16_t` / `uint32_t` storage silently and turned the
 *  emitted scores into noise (measured: `integer_adm2_csf_1` 0.000614 against
 *  a float reference of 0.9396). The stage-2 fix uses one shared power-of-two
 *  exponent per scale and restores it after contrast-masking reduction.
 *  Likewise, the blended-CSF tables in
 *  barten_csf_tools.h return `-EINVAL` **as a float** for viewing geometries
 *  they do not tabulate, and converting a negative float to an unsigned
 *  integer type is undefined behaviour.
 *
 *  `init()` now validates the configured weights, while `extract()` returns
 *  -EINVAL only for invalid table output -- the same place, and same status,
 *  as the pre-existing viewing-geometry guard that
 *  core/test/test_adm_coverage.c pins. So:
 *
 *    1. the default configuration still scores (the guard must not regress
 *       the Netflix golden path);
 *    2. `adm_csf_mode=1` with the default scale coefficients scores instead
 *       of rejecting a documented configuration or producing wrapped weights;
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
#include <math.h>
#include <stddef.h>
#include <stdint.h>

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

/* Allocate deterministic textured luma. A flat field makes every detail band
 * zero and would let a broken CSF conversion pass this regression. */
static int alloc_pattern8(VmafPicture *pic, bool distorted)
{
    const int err = vmaf_picture_alloc(pic, VMAF_PIX_FMT_YUV420P, 8, ADM_W, ADM_H);
    if (err)
        return err;
    uint8_t *p = (uint8_t *)pic->data[0];
    const ptrdiff_t stride = pic->stride[0];
    for (unsigned r = 0; r < ADM_H; ++r) {
        for (unsigned c = 0; c < ADM_W; ++c) {
            const unsigned base = (17u * c + 29u * r + (c * r) % 31u) & 255u;
            const int delta = distorted ? (((c ^ r) & 1u) ? 24 : -24) : 0;
            const int sample = (int)base + delta;
            p[(ptrdiff_t)r * stride + c] = (uint8_t)(sample < 0 ? 0 : sample > 255 ? 255 : sample);
        }
    }
    return 0;
}

static int collect_scores(VmafFeatureCollector *fc, const char *const score_keys[3],
                          double scores[3])
{
    const char *const default_keys[3] = {"integer_adm2_csf_1", "integer_aim_csf_1",
                                         "integer_adm3_csf_1"};
    const char *const *keys = score_keys ? score_keys : default_keys;
    for (unsigned i = 0; i < 3u; ++i) {
        if (vmaf_feature_collector_get_score(fc, keys[i], &scores[i], 0))
            return 1;
    }
    return 0;
}

/* Score one synthetic frame pair with the `adm` extractor under `opts` and
 * return extract()'s status. `opts` may be NULL for the default
 * configuration. `err_msg` is set (and the return value is meaningless) only
 * when the harness itself fails. */
static int adm_extract_status(const AdmOpt *opts, const char *const score_keys[3], double scores[3],
                              char **err_msg)
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
    if (alloc_pattern8(&ref, false) || alloc_pattern8(&dist, true)) {
        *err_msg = "picture alloc failed";
        vmaf_feature_collector_destroy(fc);
        (void)vmaf_feature_extractor_context_close(ctx);
        (void)vmaf_feature_extractor_context_destroy(ctx);
        return 1;
    }

    err = vmaf_feature_extractor_context_extract(ctx, &ref, NULL, &dist, NULL, 0, fc);
    if (!err && scores && collect_scores(fc, score_keys, scores))
        *err_msg = "mode-1 score missing";

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
    const int err = adm_extract_status(NULL, NULL, NULL, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("default adm config must still score", err == 0);
    return NULL;
}

/* ----------------------------------------------------------------- */
/* csf_mode=1 at the default scale coefficients: finite scores       */
/* ----------------------------------------------------------------- */

static char *test_adm_csf_mode_barten_default_scale_scores(void)
{
    /* barten_csf(0, 3.0, 1080, 100.0, 1.0) is 1.21049666, so the scale-0
     * horizontal/vertical weight is 1.21049666 * 2^21 = 2538595 and the
     * diagonal weight 1.21049666 * 2^23 = 10154382 -- 38x and 155x past the
     * 65535 the uint16_t storage holds. Before ADR-1191 they wrapped to
     * 48227 and 61838 and the extractor scored on. */
    const AdmOpt opts[] = {{"adm_csf_mode", "1"}, {NULL, NULL}};
    double scores[3] = {NAN, NAN, NAN};
    char *msg = NULL;
    const int err = adm_extract_status(opts, NULL, scores, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("adm_csf_mode=1 at default adm_csf_scale must score", err == 0);
    mu_assert("adm_csf_mode=1 outputs must be finite",
              isfinite(scores[0]) && isfinite(scores[1]) && isfinite(scores[2]));
    mu_assert("adm_csf_mode=1 adm2 must remain non-degenerate", scores[0] > 0.9 && scores[0] < 1.1);
    mu_assert("adm_csf_mode=1 aim must exercise the anomaly path",
              scores[1] > 0.001 && scores[1] < 0.01);
    mu_assert("adm_csf_mode=1 adm3 must remain non-degenerate", scores[2] > 0.9 && scores[2] < 1.1);
    return NULL;
}

static char *test_adm_csf_mode_barten_max_scale_scores(void)
{
    const AdmOpt opts[] = {
        {"adm_csf_mode", "1"}, {"adm_csf_scale", "50"}, {"adm_csf_diag_scale", "50"}, {NULL, NULL}};
    const char *const keys[3] = {"integer_adm2_scfd_50_csf_1_scf_50",
                                 "integer_aim_scfd_50_csf_1_scf_50",
                                 "integer_adm3_scfd_50_csf_1_scf_50"};
    double scores[3] = {NAN, NAN, NAN};
    char *msg = NULL;
    const int err = adm_extract_status(opts, keys, scores, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("maximum Barten scale must score", err == 0);
    mu_assert("maximum Barten scale adm2 must remain non-degenerate",
              isfinite(scores[0]) && scores[0] > 0.9 && scores[0] < 1.1);
    mu_assert("maximum Barten scale aim must remain non-degenerate",
              isfinite(scores[1]) && scores[1] > 0.001 && scores[1] < 0.1);
    mu_assert("maximum Barten scale adm3 must remain non-degenerate",
              isfinite(scores[2]) && scores[2] > 0.9 && scores[2] < 1.1);
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
    const int err = adm_extract_status(opts, NULL, NULL, &msg);
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
    const int err = adm_extract_status(opts, NULL, NULL, &msg);
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
    int err = adm_extract_status(blend, NULL, NULL, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("adm_csf_mode=2 at 1080@3H must still score", err == 0);

    const AdmOpt blend_mae[] = {{"adm_csf_mode", "3"}, {NULL, NULL}};
    err = adm_extract_status(blend_mae, NULL, NULL, &msg);
    mu_assert("harness failure", msg == NULL);
    mu_assert("adm_csf_mode=3 at 1080@3H must still score", err == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_adm_default_config_still_scores);
    mu_run_test(test_adm_csf_mode_barten_default_scale_scores);
    mu_run_test(test_adm_csf_mode_barten_max_scale_scores);
    mu_run_test(test_adm_csf_mode_barten_small_scale_accepted);
    mu_run_test(test_adm_csf_mode_blend_untabulated_geometry_rejected);
    mu_run_test(test_adm_csf_mode_blend_default_geometry_accepted);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
