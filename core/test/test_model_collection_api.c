/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Coverage for model-collection public API entry points that had zero
 *  C-unit-test coverage per audit-test-coverage-2026-05-16.md §2, and
 *  two further entry points identified on 2026-05-17:
 *
 *    - vmaf_model_collection_load()
 *    - vmaf_use_features_from_model_collection()
 *    - vmaf_score_at_index_model_collection()
 *    - vmaf_score_pooled_model_collection()
 *    - vmaf_model_collection_load_from_path()   (zero coverage until this PR)
 *    - vmaf_model_collection_feature_overload() (zero coverage until this PR)
 *
 *  Strategy: pre-import the six feature scores required by vmaf_b_v0.6.3
 *  (same features as vmaf_v0.6.1) via vmaf_import_feature_score() so no
 *  YUV frames are needed. Then exercise the scoring path through the public
 *  model-collection entry points and verify NULL-pointer guards.
 *
 *  JSON_MODEL_PATH is injected by meson as a c_args define pointing to the
 *  repo's model/ directory (same pattern as test_model.c).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "libvmaf/feature.h"
#include "libvmaf/libvmaf.h"
#include "libvmaf/model.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* vmaf_b_v0.6.3 uses the same six integer features as vmaf_v0.6.1. */
static const char *const FEAT_NAMES[] = {
    "VMAF_integer_feature_adm2_score",       "VMAF_integer_feature_motion2_score",
    "VMAF_integer_feature_vif_scale0_score", "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score", "VMAF_integer_feature_vif_scale3_score",
};
#define N_FEATS (sizeof(FEAT_NAMES) / sizeof(FEAT_NAMES[0]))

/* Number of synthetic frames to pre-import. */
#define N_FRAMES 4u

/*
 * Seed the feature collector with constant mid-quality scores so that
 * vmaf_predict_score_at_index can run without real YUV extraction.
 * adm2 ≈ 0.9, motion2 ≈ 0.5, vif_scale{0-3} ≈ 0.6 produce a VMAF
 * score well inside [0, 100], which is all we need to assert here.
 */
static const double FEAT_VALS[N_FEATS] = {
    0.9, /* adm2       */
    0.5, /* motion2    */
    0.6, /* vif_scale0 */
    0.7, /* vif_scale1 */
    0.7, /* vif_scale2 */
    0.7, /* vif_scale3 */
};

static int seed_ctx(VmafContext *vmaf)
{
    int err = 0;

    for (unsigned i = 0; i < N_FRAMES && !err; i++) {
        for (unsigned f = 0; f < N_FEATS && !err; f++) {
            err = vmaf_import_feature_score(vmaf, FEAT_NAMES[f], FEAT_VALS[f], i);
        }
    }
    return err;
}

/* ---------------------------------------------------------------------- */

static char *test_model_collection_load_valid(void)
{
    VmafModel *model = NULL;
    VmafModelCollection *mc = NULL;
    VmafModelConfig cfg = {0};

    int err = vmaf_model_collection_load(&model, &mc, &cfg, "vmaf_b_v0.6.3");
    mu_assert("vmaf_model_collection_load failed", err == 0);
    mu_assert("vmaf_model_collection_load returned NULL model", model != NULL);
    mu_assert("vmaf_model_collection_load returned NULL collection", mc != NULL);

    vmaf_model_destroy(model);
    vmaf_model_collection_destroy(mc);
    return NULL;
}

static char *test_model_collection_load_bad_version(void)
{
    VmafModel *model = NULL;
    VmafModelCollection *mc = NULL;
    VmafModelConfig cfg = {0};

    int err = vmaf_model_collection_load(&model, &mc, &cfg, "nonexistent_version_xyz");
    mu_assert("bad version must not return 0", err != 0);
    return NULL;
}

/* ---------------------------------------------------------------------- */

static char *test_use_features_from_model_collection_null_ctx(void)
{
    VmafModel *model = NULL;
    VmafModelCollection *mc = NULL;
    VmafModelConfig cfg = {0};

    int err = vmaf_model_collection_load(&model, &mc, &cfg, "vmaf_b_v0.6.3");
    mu_assert("load failed", err == 0);

    err = vmaf_use_features_from_model_collection(NULL, mc);
    mu_assert("NULL vmaf ctx must return error", err != 0);

    vmaf_model_destroy(model);
    vmaf_model_collection_destroy(mc);
    return NULL;
}

static char *test_use_features_from_model_collection_null_mc(void)
{
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, cfg) == 0);

    int err = vmaf_use_features_from_model_collection(vmaf, NULL);
    mu_assert("NULL collection must return error", err != 0);

    vmaf_close(vmaf);
    return NULL;
}

/* ---------------------------------------------------------------------- */

static char *test_score_at_index_model_collection(void)
{
    VmafConfiguration vcfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, vcfg) == 0);

    VmafModel *model = NULL;
    VmafModelCollection *mc = NULL;
    VmafModelConfig mcfg = {0};
    int err = vmaf_model_collection_load(&model, &mc, &mcfg, "vmaf_b_v0.6.3");
    mu_assert("collection load failed", err == 0);

    /* Seed scores without extractors — avoids requiring YUV frames. */
    err = seed_ctx(vmaf);
    mu_assert("seed_ctx failed", err == 0);

    /* NULL-guard checks. */
    VmafModelCollectionScore score = {0};
    mu_assert("NULL vmaf -> error",
              vmaf_score_at_index_model_collection(NULL, mc, &score, 0u) != 0);
    mu_assert("NULL mc -> error",
              vmaf_score_at_index_model_collection(vmaf, NULL, &score, 0u) != 0);
    mu_assert("NULL score -> error", vmaf_score_at_index_model_collection(vmaf, mc, NULL, 0u) != 0);

    /* Happy path: a real score for a seeded frame. */
    err = vmaf_score_at_index_model_collection(vmaf, mc, &score, 0u);
    mu_assert("score_at_index_model_collection failed", err == 0);
    mu_assert("bagging score not finite", isfinite(score.bootstrap.bagging_score) &&
                                              score.bootstrap.bagging_score > 0.0 &&
                                              score.bootstrap.bagging_score < 100.0);

    vmaf_close(vmaf);
    vmaf_model_destroy(model);
    vmaf_model_collection_destroy(mc);
    return NULL;
}

/* ---------------------------------------------------------------------- */

static char *test_score_pooled_model_collection(void)
{
    VmafConfiguration vcfg = {.log_level = VMAF_LOG_LEVEL_NONE};
    VmafContext *vmaf = NULL;
    mu_assert("vmaf_init failed", vmaf_init(&vmaf, vcfg) == 0);

    VmafModel *model = NULL;
    VmafModelCollection *mc = NULL;
    VmafModelConfig mcfg = {0};
    int err = vmaf_model_collection_load(&model, &mc, &mcfg, "vmaf_b_v0.6.3");
    mu_assert("collection load failed", err == 0);

    err = seed_ctx(vmaf);
    mu_assert("seed_ctx failed", err == 0);

    /* NULL-guard checks. */
    VmafModelCollectionScore score = {0};
    mu_assert("NULL vmaf -> error",
              vmaf_score_pooled_model_collection(NULL, mc, VMAF_POOL_METHOD_MEAN, &score, 0u,
                                                 N_FRAMES - 1u) != 0);
    mu_assert("NULL mc -> error",
              vmaf_score_pooled_model_collection(vmaf, NULL, VMAF_POOL_METHOD_MEAN, &score, 0u,
                                                 N_FRAMES - 1u) != 0);
    mu_assert("NULL score -> error",
              vmaf_score_pooled_model_collection(vmaf, mc, VMAF_POOL_METHOD_MEAN, NULL, 0u,
                                                 N_FRAMES - 1u) != 0);

    /* Happy path: pool all seeded frames. */
    err = vmaf_score_pooled_model_collection(vmaf, mc, VMAF_POOL_METHOD_MEAN, &score, 0u,
                                             N_FRAMES - 1u);
    mu_assert("score_pooled_model_collection failed", err == 0);
    mu_assert("pooled bagging score not finite", isfinite(score.bootstrap.bagging_score) &&
                                                     score.bootstrap.bagging_score > 0.0 &&
                                                     score.bootstrap.bagging_score < 100.0);

    vmaf_close(vmaf);
    vmaf_model_destroy(model);
    vmaf_model_collection_destroy(mc);
    return NULL;
}

/* ---------------------------------------------------------------------- */

/*
 * vmaf_model_collection_load_from_path — previously had ZERO test coverage.
 *
 * Happy path: load vmaf_b_v0.6.3.json directly from the filesystem path
 * (JSON_MODEL_PATH is the meson c_args macro pointing at model/).
 * Error path: non-existent path must return non-zero.
 */
static char *test_model_collection_load_from_path_valid(void)
{
    VmafModel *model = NULL;
    VmafModelCollection *mc = NULL;
    VmafModelConfig cfg = {0};
    const char *path = JSON_MODEL_PATH "vmaf_b_v0.6.3.json";

    int err = vmaf_model_collection_load_from_path(&model, &mc, &cfg, path);
    mu_assert("vmaf_model_collection_load_from_path failed", err == 0);
    mu_assert("vmaf_model_collection_load_from_path returned NULL model", model != NULL);
    mu_assert("vmaf_model_collection_load_from_path returned NULL collection", mc != NULL);

    vmaf_model_destroy(model);
    vmaf_model_collection_destroy(mc);
    return NULL;
}

static char *test_model_collection_load_from_path_bad_path(void)
{
    VmafModel *model = NULL;
    VmafModelCollection *mc = NULL;
    VmafModelConfig cfg = {0};

    int err =
        vmaf_model_collection_load_from_path(&model, &mc, &cfg, "/nonexistent/path/to/model.json");
    mu_assert("bad path must return non-zero", err != 0);
    return NULL;
}

/* ---------------------------------------------------------------------- */

/*
 * vmaf_model_collection_feature_overload — previously had ZERO test coverage.
 *
 * NULL-guard path: a NULL model_collection pointer must return non-zero
 * without crashing, exercising the first guard in the implementation.
 */
static char *test_model_collection_feature_overload_null_guard(void)
{
    VmafModel *model = NULL;
    VmafModelCollection *mc = NULL;
    VmafModelConfig cfg = {0};
    int err = vmaf_model_collection_load(&model, &mc, &cfg, "vmaf_b_v0.6.3");
    mu_assert("load failed", err == 0);

    VmafFeatureDictionary *opts = NULL;
    (void)vmaf_feature_dictionary_set(&opts, "enable_temporal", "1");

    /* NULL model_collection pointer -> -EINVAL per the implementation guard. */
    err = vmaf_model_collection_feature_overload(model, NULL, "VMAF_feature_adm2", opts);
    mu_assert("NULL mc pointer must return error", err != 0);

    (void)vmaf_feature_dictionary_free(&opts);
    vmaf_model_destroy(model);
    vmaf_model_collection_destroy(mc);
    return NULL;
}

/* ---------------------------------------------------------------------- */

/*
 * Internal entry point (extern "C" in read_json_model.h). Declared locally so
 * this test does not need the internal core/src include directory on its
 * compile line; the symbol lives in the linked libvmaf.
 */
int vmaf_read_json_model_collection_from_buffer(VmafModel **model,
                                                VmafModelCollection **model_collection,
                                                VmafModelConfig *cfg, const char *data,
                                                const int data_len);

/*
 * Partial multi-model collection build must not leak on a later sub-model
 * failure.
 *
 * Regression for the leak where model_collection_read_one() at i >= 1
 * destroyed only the just-failed sub-model and returned the error, while
 * model_collection_parse_loop() propagated it directly — leaving *model
 * (sub-model 0) and *model_collection (sub-models 1..i-1) populated and
 * leaked. The fix tears both down and NULLs them out on that path.
 *
 * Construction: sub-models "0" and "1" are the real, valid vmaf_v0.6.1
 * single-model JSON (so *model is set and *model_collection holds one
 * appended sub-model); sub-model "2" is an object with no "model_dict" key,
 * so model_parse() returns -EINVAL after both earlier sub-models were
 * accepted. The error path must (a) return non-zero, and (b) leave both
 * out-params NULL — proving sub-model 0 (*model) and the partial collection
 * (*model_collection) were torn down. Under ASan/LSan the same path also
 * proves the heap is clean.
 */
static char *read_whole_file(const char *path, char **out, long *out_len)
{
    FILE *in = fopen(path, "rb");
    mu_assert("could not open source model json", in != NULL);
    mu_assert("seek end failed", fseek(in, 0L, SEEK_END) == 0);
    long len = ftell(in);
    mu_assert("ftell failed", len > 0);
    mu_assert("seek start failed", fseek(in, 0L, SEEK_SET) == 0);
    char *buf = malloc((size_t)len + 1);
    mu_assert("oom reading model json", buf != NULL);
    size_t got = fread(buf, 1, (size_t)len, in);
    (void)fclose(in);
    mu_assert("short read of model json", got == (size_t)len);
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return NULL;
}

static char *test_model_collection_partial_failure_no_leak(void)
{
    /* A valid single model reused verbatim as sub-models "0" and "1". */
    char *valid = NULL;
    long valid_len = 0;
    char *msg = read_whole_file(JSON_MODEL_PATH "vmaf_v0.6.1.json", &valid, &valid_len);
    if (msg) {
        free(valid);
        return msg;
    }

    /* {"0": <valid>, "1": <valid>, "2": {"unused": 0}} — sub-model 2 has no
     * "model_dict", so model_parse() fails with -EINVAL only after 0 and 1
     * were accepted (0 stored in *model, 1 appended to *model_collection). */
    const char *prefix = "{\"0\": ";
    const char *mid = ", \"1\": ";
    const char *suffix = ", \"2\": {\"unused\": 0}}";
    size_t cap =
        strlen(prefix) + (size_t)valid_len + strlen(mid) + (size_t)valid_len + strlen(suffix) + 1;
    char *json = malloc(cap);
    if (!json) {
        free(valid);
        mu_assert("oom building collection json", json != NULL);
    }
    int n = snprintf(json, cap, "%s%s%s%s%s", prefix, valid, mid, valid, suffix);
    free(valid);
    mu_assert("snprintf truncated collection json", n > 0 && (size_t)n < cap);

    VmafModel *model = (VmafModel *)0x1;                  /* poison: must be NULLed */
    VmafModelCollection *mc = (VmafModelCollection *)0x1; /* poison: must be NULLed */
    VmafModelConfig cfg = {0};
    int err = vmaf_read_json_model_collection_from_buffer(&model, &mc, &cfg, json, n);
    free(json);

    mu_assert("partial collection load must fail", err != 0);
    mu_assert("leaked sub-model 0: *model not NULL after failure", model == NULL);
    mu_assert("leaked partial collection: *model_collection not NULL after failure", mc == NULL);
    return NULL;
}

/* ---------------------------------------------------------------------- */

char *run_tests(void)
{
    mu_run_test(test_model_collection_load_valid);
    mu_run_test(test_model_collection_load_bad_version);
    mu_run_test(test_use_features_from_model_collection_null_ctx);
    mu_run_test(test_use_features_from_model_collection_null_mc);
    mu_run_test(test_score_at_index_model_collection);
    mu_run_test(test_score_pooled_model_collection);
    mu_run_test(test_model_collection_load_from_path_valid);
    mu_run_test(test_model_collection_load_from_path_bad_path);
    mu_run_test(test_model_collection_feature_overload_null_guard);
    mu_run_test(test_model_collection_partial_failure_no_leak);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
