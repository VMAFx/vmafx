/* SPDX-License-Identifier: EUPL-1.2 */
/* Copyright 2026 Lusoris */

/**
 * @file test_predict_rust_ops.c
 * @brief predict.c reaches the Rust predictor only through the installed table (ADR-1713).
 *
 * Runs with VMAF_FEATURE_IMPL=rust (Meson `env`) in every build, with or
 * without Rust, against a counting stand-in for the table that
 * core/src/rust/shim/rust_predict.c installs: with no table the C predictor
 * runs; an installed table gets the raw scores and its result is the score;
 * -ENOTSUP from create() keeps the C predictor; other errors propagate; and
 * vmaf_model_destroy() frees the handle through the table.
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "feature/feature_collector.h"
#include "float_bits.h"
#include "model.h"
#include "predict.h"
#include "test.h"

#include <libvmaf/model.h>

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit, ADR-1138. */

#define FAKE_SCORE 1234.5
#define N_FRAMES 2u

typedef struct FakeCalls {
    int create_status;
    int predict_status;
    unsigned creates;
    unsigned predicts;
    unsigned destroys;
    const void *destroyed;
    double first_raw;
    unsigned index;
    enum VmafModelFlags flags;
    const struct svm_node *nodes;
} FakeCalls;

static FakeCalls fake;
static int fake_handle; /* the address create() hands out */

static int fake_create(const VmafModel *model, void **handle)
{
    (void)model;
    fake.creates++;
    if (fake.create_status)
        return fake.create_status;
    *handle = &fake_handle;
    return 0;
}

static int fake_predict(const VmafModel *model, void *handle, const double *scores,
                        enum VmafModelFlags flags, struct svm_node *nodes, unsigned index,
                        double *prediction)
{
    (void)model;
    fake.predicts++;
    fake.first_raw = scores[0];
    fake.index = index;
    fake.flags = flags;
    fake.nodes = nodes;
    if (handle != &fake_handle)
        return -EFAULT;
    if (fake.predict_status)
        return fake.predict_status;
    *prediction = FAKE_SCORE;
    return 0;
}

static void fake_destroy(void *handle)
{
    fake.destroys++;
    fake.destroyed = handle;
}

static const struct VmafRustPredictOps fake_ops = {
    .create = fake_create,
    .predict = fake_predict,
    .destroy = fake_destroy,
};

typedef struct Fixture {
    VmafModel *model;
    VmafFeatureCollector *fc;
} Fixture;

static double raw_score(unsigned feature, unsigned frame)
{
    return 0.25 + (double)feature + 0.5 * (double)frame;
}

/* A model and a collector holding N_FRAMES frames of its features. */
static char *fixture_open(Fixture *fx)
{
    memset(fx, 0, sizeof(*fx));
    VmafModelConfig cfg = {.name = "vmaf", .flags = VMAF_MODEL_FLAGS_DEFAULT};
    mu_assert("collector initialises", !vmaf_feature_collector_init(&fx->fc));
    mu_assert("model loads", !vmaf_model_load(&fx->model, &cfg, "vmaf_v0.6.1"));
    for (unsigned f = 0; f < N_FRAMES; f++) {
        for (unsigned i = 0; i < fx->model->n_features; i++) {
            const int err = vmaf_feature_collector_append(fx->fc, fx->model->feature[i].name,
                                                          raw_score(i, f), f);
            mu_assert("score appends", !err);
        }
    }
    return NULL;
}

static void fixture_close(Fixture *fx)
{
    vmaf_model_destroy(fx->model);
    vmaf_feature_collector_destroy(fx->fc);
}

/* The table to run with; the counters start from zero. */
static void use_table(const struct VmafRustPredictOps *ops)
{
    memset(&fake, 0, sizeof(fake));
    vmaf_predict_install_rust_ops(ops);
}

static int predict_at(Fixture *fx, unsigned index, double *score)
{
    return vmaf_predict_score_at_index(fx->model, fx->fc, index, score, false, false,
                                       VMAF_MODEL_FLAG_DISABLE_CLIP);
}

/* The C predictor's score of frame 0, from a model decided without a table. */
static char *c_score(double *score)
{
    Fixture fx;
    use_table(NULL);
    mu_assert_msg(fixture_open(&fx));
    const int err = predict_at(&fx, 0, score);
    const int state = fx.model->rust_predict_state;
    const bool handle = fx.model->rust_predict != NULL;
    fixture_close(&fx);
    mu_assert("the C predictor scores", err == 0);
    mu_assert("without a table the model stays on the C predictor", state == 2);
    mu_assert("without a table no Rust handle exists", !handle);
    mu_assert("the C predictor's score is its own", *score != FAKE_SCORE);
    return NULL;
}

static char *test_no_table_runs_the_c_predictor(void)
{
    double score = 0.0;
    return c_score(&score);
}

/* What the table was handed for frame 1. */
static char *check_routed_arguments(const Fixture *fx)
{
    mu_assert("the table gets the raw score",
              vmaf_test_identical_f64(fake.first_raw, raw_score(0, 1)));
    mu_assert("the table gets the frame index", fake.index == 1);
    mu_assert("the table gets the flags", fake.flags == VMAF_MODEL_FLAG_DISABLE_CLIP);
    mu_assert("the table gets the node scratch", fake.nodes == fx->model->predict_nodes);
    return NULL;
}

static char *check_routed_call(const Fixture *fx, int err, double score)
{
    double published = 0.0;
    const int get_err = vmaf_feature_collector_get_score(fx->fc, "vmaf", &published, 1);
    mu_assert("the table scores", err == 0);
    mu_assert("the table's result is the score", score == FAKE_SCORE);
    mu_assert("the table's result is published", get_err == 0 && published == FAKE_SCORE);
    mu_assert("the model runs the Rust predictor", fx->model->rust_predict_state == 1);
    return check_routed_arguments(fx);
}

static char *test_table_routes_the_prediction(void)
{
    Fixture fx;
    use_table(&fake_ops);
    mu_assert_msg(fixture_open(&fx));
    double score = 0.0;
    const int err = vmaf_predict_score_at_index(fx.model, fx.fc, 1, &score, true, false,
                                                VMAF_MODEL_FLAG_DISABLE_CLIP);
    char *msg = check_routed_call(&fx, err, score);
    const int again = predict_at(&fx, 0, &score);
    fixture_close(&fx);
    vmaf_predict_install_rust_ops(NULL);
    mu_assert_msg(msg);
    mu_assert("a second frame scores", again == 0);
    mu_assert("the handle is built once per model", fake.creates == 1 && fake.predicts == 2);
    mu_assert("destroy frees the handle through the table",
              fake.destroys == 1 && fake.destroyed == &fake_handle);
    return NULL;
}

static char *test_unsupported_model_runs_the_c_predictor(void)
{
    double expected = 0.0;
    mu_assert_msg(c_score(&expected));
    Fixture fx;
    use_table(&fake_ops);
    fake.create_status = -ENOTSUP;
    mu_assert_msg(fixture_open(&fx));
    double score = 0.0;
    const int err = predict_at(&fx, 0, &score);
    const int again = predict_at(&fx, 0, &score);
    const int state = fx.model->rust_predict_state;
    fixture_close(&fx);
    vmaf_predict_install_rust_ops(NULL);
    mu_assert("an unsupported model scores", err == 0 && again == 0);
    mu_assert("an unsupported model runs the C predictor",
              state == 2 && vmaf_test_identical_f64(score, expected));
    mu_assert("the decision is made once", fake.creates == 1 && fake.predicts == 0);
    mu_assert("no handle to free", fake.destroys == 0);
    return NULL;
}

static char *test_create_error_propagates(void)
{
    Fixture fx;
    use_table(&fake_ops);
    fake.create_status = -ENOMEM;
    mu_assert_msg(fixture_open(&fx));
    double score = 0.0;
    const int err = predict_at(&fx, 0, &score);
    const int state = fx.model->rust_predict_state;
    fake.create_status = 0;
    const int retry = predict_at(&fx, 0, &score);
    fixture_close(&fx);
    vmaf_predict_install_rust_ops(NULL);
    mu_assert("a create error is the prediction's error", err == -ENOMEM && state == 0);
    mu_assert("the next frame tries again", retry == 0 && score == FAKE_SCORE);
    mu_assert("one handle, freed", fake.creates == 2 && fake.destroys == 1);
    return NULL;
}

static char *test_predict_error_propagates(void)
{
    Fixture fx;
    use_table(&fake_ops);
    fake.predict_status = -EINVAL;
    mu_assert_msg(fixture_open(&fx));
    double score = 42.0;
    const int err = predict_at(&fx, 0, &score);
    fixture_close(&fx);
    vmaf_predict_install_rust_ops(NULL);
    mu_assert("a predict error is the prediction's error", err == -EINVAL);
    mu_assert("a failed prediction leaves the output untouched", score == 42.0);
    mu_assert("the handle is freed", fake.destroys == 1);
    return NULL;
}

static char *test_removed_table_fails_a_rust_model(void)
{
    Fixture fx;
    use_table(&fake_ops);
    mu_assert_msg(fixture_open(&fx));
    double score = 0.0;
    const int first = predict_at(&fx, 0, &score);
    vmaf_predict_install_rust_ops(NULL);
    double second_score = 42.0;
    const int second = predict_at(&fx, 0, &second_score);
    fixture_close(&fx);
    mu_assert("the first frame runs the table", first == 0 && score == FAKE_SCORE);
    mu_assert("without its table a Rust model fails", second == -EINVAL && second_score == 42.0);
    mu_assert("without a table nothing is freed through it", fake.destroys == 0);
    return NULL;
}

char *run_tests(void)
{
    mu_run_test(test_no_table_runs_the_c_predictor);
    mu_run_test(test_table_routes_the_prediction);
    mu_run_test(test_unsupported_model_runs_the_c_predictor);
    mu_run_test(test_create_error_propagates);
    mu_run_test(test_predict_error_propagates);
    mu_run_test(test_removed_table_fails_a_rust_model);
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
