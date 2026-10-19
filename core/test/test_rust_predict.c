/* SPDX-License-Identifier: EUPL-1.2 */
/* Copyright 2026 Lusoris */

/**
 * @file test_rust_predict.c
 * @brief The Rust predictor returns the C predictor's score bit for bit (RC4 lane P, #1723).
 *
 * Runs with VMAF_FEATURE_IMPL=rust (Meson `env`). vmaf_init() installs the
 * Rust predictor's table (core/src/rust/shim/rust_predict.c). Each comparison
 * loads one model twice: the first instance decides its predictor with no
 * table installed and stays on the C predictor, the second with the Rust
 * table. For each `vmaf_v1.0.16*` model and for edited copies of it (score
 * transform with knots and rectification, no normalisation, clip off, flags,
 * the chroma sentinel, non-finite input) the same raw feature scores go
 * through vmaf_predict_score_at_index() on both instances. Scores must be
 * equal as IEEE doubles and both must fail with the same errno.
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dict.h"
#include "feature/feature_collector.h"
#include "model.h"
#include "predict.h"
#include "rust/shim/rust_predict.h"
#include "test.h"

#include <libvmaf/libvmaf.h>
#include <libvmaf/model.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit, ADR-1138. */

#define N_FRAMES 48u
#define MAX_FEATURES 16u

static const char *const model_files[] = {
    "vmaf_v1.0.16/vmaf_v1.0.16_1d5h_2160.json",
    "vmaf_v1.0.16/vmaf_v1.0.16_3d0h_2160.json",
    "vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json",
    "vmaf_v1.0.16/vmaf_v1.0.16_5d0h.json",
    "vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_1d5h_2160.json",
    "vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_3d0h_2160.json",
    "vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_3d0h.json",
    "vmaf_v1.0.16_hfr/vmaf_v1.0.16_hfr_5d0h.json",
};

/* An edit applied to both instances before their first prediction. */
typedef int (*ModelEdit)(VmafModel *model);

static uint64_t lcg_state = 0x9e3779b97f4a7c15ull;

/* Deterministic doubles in [lo, hi). */
static double next_double(double lo, double hi)
{
    lcg_state = lcg_state * 6364136223846793005ull + 1442695040888963407ull;
    const double u = (double)(lcg_state >> 11) / 9007199254740992.0;
    return lo + (hi - lo) * u;
}

static VmafModel *load_model(const char *file)
{
    char path[1024];
    VmafModel *model = NULL;
    VmafModelConfig cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    const int len = snprintf(path, sizeof(path), "%s%s", JSON_MODEL_PATH, file);
    if (len < 0 || (size_t)len >= sizeof(path))
        return NULL;
    if (vmaf_model_load_from_path(&model, &cfg, path))
        return NULL;
    /* Plain feature names: the collector is filled by name below. */
    int err = 0;
    for (unsigned i = 0; i < model->n_features; i++)
        err |= vmaf_dictionary_free(&model->feature[i].opts_dict);
    if (err) {
        vmaf_model_destroy(model);
        return NULL;
    }
    return model;
}

static int is_chroma(const VmafModel *model, unsigned i)
{
    return strstr(model->feature[i].name, "speed_chroma") != NULL;
}

/* Raw scores of every frame; every 5th frame zeroes speed_chroma (the
 * sentinel). Frame @p nan_frame (N_FRAMES = none) carries a NaN. */
static int fill_collector(VmafFeatureCollector *fc, const VmafModel *model, unsigned nan_frame)
{
    for (unsigned f = 0; f < N_FRAMES; f++) {
        for (unsigned i = 0; i < model->n_features; i++) {
            double score = is_chroma(model, i) && f % 5 == 0 ? 0.0 : next_double(-0.5, 130.0);
            if (f == nan_frame && i == 0)
                score = NAN;
            const int err = vmaf_feature_collector_append(fc, model->feature[i].name, score, f);
            if (err)
                return err;
        }
    }
    return 0;
}

static int same_bits(double a, double b)
{
    return memcmp(&a, &b, sizeof(a)) == 0;
}

/* One frame through both instances; returns 0 when they agree. */
static int compare_frame(VmafModel *c_model, VmafModel *r_model, VmafFeatureCollector *fc,
                         unsigned frame, enum VmafModelFlags flags)
{
    double c_score = 0.0;
    double r_score = 0.0;
    const int c_err =
        vmaf_predict_score_at_index(c_model, fc, frame, &c_score, false, false, flags);
    const int r_err =
        vmaf_predict_score_at_index(r_model, fc, frame, &r_score, false, false, flags);
    if (c_err != r_err)
        return -1;
    return c_err || same_bits(c_score, r_score) ? 0 : -1;
}

/* The first prediction decides the predictor: C without a table, Rust with it. */
static char *warm_up(VmafModel *c_model, VmafModel *r_model, VmafFeatureCollector *fc)
{
    double warm = 0.0;
    vmaf_predict_install_rust_ops(NULL);
    mu_assert("the C instance scores the warm-up frame",
              !vmaf_predict_score_at_index(c_model, fc, 1, &warm, false, false, 0));
    vmaf_rust_predict_install();
    mu_assert("the Rust instance scores the warm-up frame",
              !vmaf_predict_score_at_index(r_model, fc, 1, &warm, false, false, 0));
    mu_assert("the first instance runs the C predictor", c_model->rust_predict_state == 2);
    mu_assert("the second instance runs the Rust predictor", r_model->rust_predict_state == 1);
    return NULL;
}

static int compare_all_frames(VmafModel *c_model, VmafModel *r_model, VmafFeatureCollector *fc)
{
    static const enum VmafModelFlags flag_sets[] = {
        VMAF_MODEL_FLAGS_DEFAULT,
        VMAF_MODEL_FLAG_DISABLE_CLIP,
        VMAF_MODEL_FLAG_DISABLE_TRANSFORM,
        (enum VmafModelFlags)(VMAF_MODEL_FLAG_DISABLE_CLIP | VMAF_MODEL_FLAG_DISABLE_TRANSFORM),
    };
    int diff = 0;
    for (unsigned f = 0; f < N_FRAMES; f++) {
        for (unsigned k = 0; k < sizeof(flag_sets) / sizeof(flag_sets[0]); k++)
            diff |= compare_frame(c_model, r_model, fc, f, flag_sets[k]);
    }
    return diff;
}

static char *compare_pair(VmafModel *c_model, VmafModel *r_model, const char *label)
{
    VmafFeatureCollector *fc = NULL;
    mu_assert("feature count fits the test", c_model->n_features <= MAX_FEATURES);
    mu_assert("collector init", !vmaf_feature_collector_init(&fc));
    mu_assert("collector fill", !fill_collector(fc, c_model, N_FRAMES));
    char *msg = warm_up(c_model, r_model, fc);
    const int diff = msg ? 0 : compare_all_frames(c_model, r_model, fc);
    vmaf_feature_collector_destroy(fc);
    if (diff && fprintf(stderr, "Rust and C predictor differ on: %s\n", label) < 0)
        return "the Rust predictor differs from the C predictor (stderr unwritable)";
    mu_assert("the Rust predictor equals the C predictor bit for bit", diff == 0);
    return msg;
}

static char *compare_model(const char *file, ModelEdit edit, const char *label)
{
    VmafModel *c_model = load_model(file);
    VmafModel *r_model = load_model(file);
    char *msg = NULL;
    if (!c_model || !r_model)
        msg = "model loads";
    else if (edit && (edit(c_model) || edit(r_model)))
        msg = "model edit";
    else
        msg = compare_pair(c_model, r_model, label);
    vmaf_model_destroy(c_model);
    vmaf_model_destroy(r_model);
    return msg;
}

static int set_knots(VmafModel *model)
{
    VmafPoint *knots = (VmafPoint *)calloc(3, sizeof(*knots));
    if (!knots)
        return -ENOMEM;
    knots[1] = (VmafPoint){60.0, 55.0};
    knots[2] = (VmafPoint){100.0, 100.0};
    free(model->score_transform.knots.list);
    model->score_transform.knots.list = knots;
    model->score_transform.knots.n_knots = 3;
    model->score_transform.knots.enabled = true;
    return 0;
}

static int edit_knots_lte(VmafModel *model)
{
    model->score_transform.out_lte_in = true;
    model->score_transform.out_gte_in = false;
    return set_knots(model);
}

static int edit_knots_gte(VmafModel *model)
{
    model->score_transform.out_lte_in = false;
    model->score_transform.out_gte_in = true;
    return set_knots(model);
}

static int edit_p1_only(VmafModel *model)
{
    model->score_transform.p0.enabled = false;
    model->score_transform.p2.enabled = false;
    return 0;
}

static int edit_no_transform_no_clip(VmafModel *model)
{
    model->score_transform.enabled = false;
    model->score_clip.enabled = false;
    return 0;
}

static int edit_norm_none(VmafModel *model)
{
    model->norm_type = VMAF_MODEL_NORMALIZATION_TYPE_NONE;
    return 0;
}

static int edit_norm_none_no_chroma(VmafModel *model)
{
    model->chroma_from_luma.enabled = false;
    return edit_norm_none(model);
}

/* Without an explicit install: vmaf_init() puts the Rust table in place. */
static char *test_vmaf_init_installs_the_rust_predictor(void)
{
    VmafContext *vmaf = NULL;
    VmafConfiguration cfg = {.log_level = VMAF_LOG_LEVEL_INFO};
    VmafFeatureCollector *fc = NULL;
    VmafModel *model = load_model(model_files[2]);
    mu_assert("model loads", model != NULL);
    mu_assert("collector init", !vmaf_feature_collector_init(&fc));
    mu_assert("collector fill", !fill_collector(fc, model, N_FRAMES));
    vmaf_predict_install_rust_ops(NULL);
    mu_assert("vmaf_init", !vmaf_init(&vmaf, cfg));
    double score = 0.0;
    const int err = vmaf_predict_score_at_index(model, fc, 1, &score, false, false, 0);
    const int state = model->rust_predict_state;
    const int close_err = vmaf_close(vmaf);
    vmaf_model_destroy(model);
    vmaf_feature_collector_destroy(fc);
    mu_assert("vmaf_close", close_err == 0);
    mu_assert("the model scores", err == 0);
    mu_assert("vmaf_init() installed the Rust predictor", state == 1);
    return NULL;
}

static char *test_v1_models_match_c(void)
{
    for (unsigned m = 0; m < sizeof(model_files) / sizeof(model_files[0]); m++)
        mu_assert_msg(compare_model(model_files[m], NULL, model_files[m]));
    return NULL;
}

static char *test_transform_variants_match_c(void)
{
    mu_assert_msg(compare_model(model_files[2], edit_knots_lte, "knots + out_lte_in"));
    mu_assert_msg(compare_model(model_files[2], edit_knots_gte, "knots + out_gte_in"));
    mu_assert_msg(compare_model(model_files[2], edit_p1_only, "p1 only"));
    mu_assert_msg(
        compare_model(model_files[2], edit_no_transform_no_clip, "no transform, no clip"));
    return NULL;
}

static char *test_norm_none_matches_c(void)
{
    mu_assert_msg(compare_model(model_files[3], edit_norm_none, "norm none"));
    mu_assert_msg(compare_model(model_files[3], edit_norm_none_no_chroma, "norm none, no chroma"));
    return NULL;
}

static char *nonfinite_pair(VmafModel *c_model, VmafModel *r_model)
{
    VmafFeatureCollector *fc = NULL;
    mu_assert("collector init", !vmaf_feature_collector_init(&fc));
    char *msg = fill_collector(fc, c_model, 0) ? "collector fill" : NULL;
    if (!msg)
        msg = warm_up(c_model, r_model, fc);
    double c_score = 42.0;
    double r_score = 42.0;
    const int c_err = vmaf_predict_score_at_index(c_model, fc, 0, &c_score, false, false, 0);
    const int r_err = vmaf_predict_score_at_index(r_model, fc, 0, &r_score, false, false, 0);
    vmaf_feature_collector_destroy(fc);
    mu_assert("NaN input fails on the C predictor with -EINVAL", msg || c_err == -EINVAL);
    mu_assert("NaN input fails on the Rust predictor with -EINVAL", msg || r_err == -EINVAL);
    mu_assert("a failed prediction leaves the output untouched", msg || r_score == 42.0);
    return msg;
}

static char *test_nonfinite_input_fails_like_c(void)
{
    VmafModel *c_model = load_model(model_files[2]);
    VmafModel *r_model = load_model(model_files[2]);
    char *msg = c_model && r_model ? nonfinite_pair(c_model, r_model) : "model loads";
    vmaf_model_destroy(c_model);
    vmaf_model_destroy(r_model);
    return msg;
}

char *run_tests(void)
{
    mu_run_test(test_vmaf_init_installs_the_rust_predictor);
    mu_run_test(test_v1_models_match_c);
    mu_run_test(test_transform_variants_match_c);
    mu_run_test(test_norm_none_matches_c);
    mu_run_test(test_nonfinite_input_fails_like_c);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
