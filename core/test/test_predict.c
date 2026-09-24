/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include <stdint.h>

#include "feature/feature_collector.h"
#include "metadata_handler.h"
#include "test.h"
#include "predict.h"
#include "predict.c"

#include <libvmaf/model.h>
#include <math.h>

typedef struct {
    VmafDictionary **metadata;
    int flags;
} MetaStruct;

/* Append the same score for every model feature, which is what drives both the
 * prediction path and the registered metadata callback. */
static char *append_all_features(VmafFeatureCollector *feature_collector, const VmafModel *model)
{
    for (unsigned i = 0; i < model->n_features; i++) {
        const int err =
            vmaf_feature_collector_append(feature_collector, model->feature[i].name, 60., 0);
        mu_assert("problem during vmaf_feature_collector_append", !err);
    }
    return NULL;
}

static char *test_predict_score_at_index(void)
{
    int err;

    VmafFeatureCollector *feature_collector;
    err = vmaf_feature_collector_init(&feature_collector);
    mu_assert("problem during vmaf_feature_collector_init", !err);

    VmafModel *model;
    VmafModelConfig cfg = {
        .name = "vmaf",
        .flags = VMAF_MODEL_FLAGS_DEFAULT,
    };
    err = vmaf_model_load(&model, &cfg, "vmaf_v0.6.1");
    mu_assert("problem during vmaf_model_load", !err);

    mu_assert_msg(append_all_features(feature_collector, model));

    double vmaf_score = 0.;
    err = vmaf_predict_score_at_index(model, feature_collector, 0, &vmaf_score, true, false, 0);
    mu_assert("problem during vmaf_predict_score_at_index", !err);

    vmaf_model_destroy(model);
    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

static void set_meta(void *data, VmafMetadata *metadata)
{
    if (!data)
        return;
    MetaStruct *meta = data;
    char key[128];
    char value[128];
    (void)snprintf(key, sizeof(key), "%s_%u", metadata->feature_name, metadata->picture_index);
    (void)snprintf(value, sizeof(value), "%f", metadata->score);
    vmaf_dictionary_set(meta->metadata, key, value, meta->flags);
}

/* A metadata registration whose `data` pointer is NULL must still be accepted
 * and driven; one whose `callback` is NULL must be rejected. `m` is taken by
 * value because each case mutates its own copy. */
static char *check_metadata_registration_edges(VmafMetadataConfiguration m, const VmafModel *model)
{
    VmafFeatureCollector *feature_collector;

    m.data = NULL;
    int err = vmaf_feature_collector_init(&feature_collector);
    mu_assert("problem during vmaf_feature_collector_init", !err);

    err = vmaf_feature_collector_register_metadata(feature_collector, m);
    mu_assert("problem during vmaf_feature_collector_register_metadata_1", !err);

    mu_assert_msg(append_all_features(feature_collector, model));

    vmaf_feature_collector_destroy(feature_collector);

    m.callback = NULL;
    err = vmaf_feature_collector_init(&feature_collector);
    mu_assert("problem during vmaf_feature_collector_init", !err);

    err = vmaf_feature_collector_register_metadata(feature_collector, m);
    mu_assert("problem during vmaf_feature_collector_register_metadata_2", err);

    vmaf_feature_collector_destroy(feature_collector);
    return NULL;
}

/* The metadata callback wrote one entry per scored feature; the vmaf score of
 * frame 0 has to be there under its own key with its own formatted value. */
static char *check_propagated_entry(VmafDictionary **dict)
{
    VmafDictionaryEntry *e = vmaf_dictionary_get(dict, "vmaf_0", 0);
    mu_assert("error on propagaton metadata: propagated key not found!", e);
    mu_assert("error on propagaton metadata: propagated key wrong!", !strcmp(e->key, "vmaf_0"));
    mu_assert("error on propagaton metadata: propagated data wrong!",
              !strcmp(e->val, "100.000000"));
    return NULL;
}

static char *test_propagate_metadata(void)
{
    int err;

    VmafDictionary *dict = NULL;
    MetaStruct meta_data = {
        .metadata = &dict,
        .flags = 0,
    };

    VmafMetadataConfiguration m = {
        .feature_name = "vmaf",
        .callback = set_meta,
        .data = &meta_data,
    };

    VmafFeatureCollector *feature_collector;
    err = vmaf_feature_collector_init(&feature_collector);
    mu_assert("problem during vmaf_feature_collector_init", !err);

    err = vmaf_feature_collector_register_metadata(feature_collector, m);
    mu_assert("problem during vmaf_feature_collector_register_metadata_0", !err);

    VmafModel *model;
    VmafModelConfig cfg = {
        .name = "vmaf",
        .flags = VMAF_MODEL_FLAGS_DEFAULT,
    };
    err = vmaf_model_load(&model, &cfg, "vmaf_v0.6.1");
    mu_assert("problem during vmaf_model_load", !err);
    err = vmaf_feature_collector_mount_model(feature_collector, model);
    mu_assert("problem during vmaf_mount_model", !err);

    mu_assert_msg(append_all_features(feature_collector, model));

    mu_assert_msg(check_propagated_entry(&dict));

    vmaf_feature_collector_destroy(feature_collector);

    mu_assert_msg(check_metadata_registration_edges(m, model));

    /*
     * The metadata-dispatch path strdup'd key+val into `dict` via
     * `vmaf_dictionary_set` -> `dict_append_new_entry` (dict.c:121,
     * 124). The owning test has to free that dict; otherwise ASan
     * flags the strdup'd entries as leaked. SAN-PREDICT-METADATA-LEAK.
     */
    vmaf_dictionary_free(&dict);

    vmaf_model_destroy(model);
    return NULL;
}

/* One (first, second) pair the solver must reject, with the message that
 * rejection has to carry. */
typedef struct {
    VmafPoint first;
    VmafPoint second;
    char *msg;
} LinearRejectCase;

/* One pair the solver must accept, with the slope and intercept it has to
 * produce. `tol` of 0 means the values must match exactly. */
typedef struct {
    VmafPoint first;
    VmafPoint second;
    double want_a;
    double want_b;
    double tol;
} LinearSolveCase;

/*
 * Both halves of the solver contract, carried as data so the branch count does
 * not grow with the case count: degenerate pairs (a second point that is not
 * strictly beyond the first, or a pair on a horizontal or vertical line) must
 * be rejected, and the accepted pairs must produce the listed slope and
 * intercept.
 */
static char *test_find_linear_function_parameters(void)
{
    const LinearRejectCase rejected[] = {
        {{.x = 1, .y = 1},
         {.x = 0, .y = 0},
         "first_point coordinates need to be smaller or equal to second_point coordinates"},
        {{.x = 0, .y = 1},
         {.x = 0, .y = 0},
         "first_point coordinates need to be smaller or equal to second_point coordinates"},
        {{.x = 1, .y = 0},
         {.x = 0, .y = 0},
         "first_point coordinates need to be smaller or equal to second_point coordinates"},
        {{.x = 50, .y = 30},
         {.x = 50, .y = 100},
         "first_point and second_point cannot lie on a horizontal or vertical line"},
        {{.x = 50, .y = 30},
         {.x = 100, .y = 30},
         "first_point and second_point cannot lie on a horizontal or vertical line"},
    };
    const LinearSolveCase solved[] = {
        {{.x = 50, .y = 20}, {.x = 110, .y = 110}, 1.5, -55.0, 0.0},
        {{.x = 50, .y = 30}, {.x = 110, .y = 110}, 1.333333333333333, -36.666666666666664, 1e-8},
        {{.x = 50, .y = 30}, {.x = 50, .y = 30}, 1.0, 0.0, 0.0},
        {{.x = 10, .y = 10}, {.x = 50, .y = 110}, 2.5, -15.0, 0.0},
    };

    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        double a;
        double b;
        const int err =
            find_linear_function_parameters(rejected[i].first, rejected[i].second, &a, &b);
        mu_assert(rejected[i].msg, err);
    }

    for (size_t i = 0; i < sizeof(solved) / sizeof(solved[0]); i++) {
        double a;
        double b;
        const int err = find_linear_function_parameters(solved[i].first, solved[i].second, &a, &b);
        mu_assert("error code should be 0", !err);
        mu_assert("returned a does not match", fabs(a - solved[i].want_a) <= solved[i].tol);
        mu_assert("returned b does not match", fabs(b - solved[i].want_b) <= solved[i].tol);
    }
    return NULL;
}

/* One sampled segment of a piecewise-linear mapping: every x in
 * [first, last) tenths must land on `slope * x + intercept`, or on x itself
 * when `identity` is set. */
typedef struct {
    VmafPoint *knots;
    double slope;
    double intercept;
    char *msg;
    unsigned knot_cnt;
    int first;
    int last;
    int identity;
} PiecewiseCase;

static char *check_piecewise_cases(const PiecewiseCase *cases, size_t cnt)
{
    for (size_t c = 0; c < cnt; c++) {
        for (int i = cases[c].first; i < cases[c].last; ++i) {
            const double x = i * 0.1;
            const double want = cases[c].identity ? x : cases[c].slope * x + cases[c].intercept;
            /* Seeded so a mapping that refuses the knots fails the comparison
             * deterministically instead of reading an indeterminate value. */
            double y = 0.;
            piecewise_linear_mapping(x, cases[c].knots, cases[c].knot_cnt, &y);
            mu_assert(cases[c].msg, fabs(y - want) < 1e-8);
        }
    }
    return NULL;
}

/* The 2160p and 1080p reference knot sets: each has a first linear segment, a
 * steeper transition segment and a final identity segment. */
static char *check_piecewise_reference_knots(void)
{
    static VmafPoint knots2160p[] = {{.x = 0.0, .y = -55.0},
                                     {.x = 95.0, .y = 87.5},
                                     {.x = 105.0, .y = 105.0},
                                     {.x = 110.0, .y = 110.0}};
    static VmafPoint knots1080p[] = {{.x = 0.0, .y = -36.66},
                                     {.x = 90.0, .y = 83.04},
                                     {.x = 95.0, .y = 95.0},
                                     {.x = 100.0, .y = 100.0}};
    const PiecewiseCase cases[] = {
        {knots2160p, 1.5, -55.0, "returned y0 does not match y0_true", 4, 0, 950, 0},
        {knots1080p, 1.33, -36.66, "returned y1 does not match y1_true", 4, 0, 900, 0},
        {knots2160p, 1.75, -78.75, "returned y0 does not match y0_true", 4, 950, 1050, 0},
        {knots1080p, 2.392, -132.24, "returned y1 does not match y1_true", 4, 900, 950, 0},
        {knots2160p, 0.0, 0.0, "returned y0 does not match y0_true", 4, 1050, 1100, 1},
        {knots1080p, 0.0, 0.0, "returned y1 does not match x1", 4, 950, 1000, 1},
    };
    return check_piecewise_cases(cases, sizeof(cases) / sizeof(cases[0]));
}

static char *test_piecewise_linear_mapping(void)
{
    int err;

    double y;

    VmafPoint knots1[] = {{.x = 0, .y = 1}, {.x = 1, .y = 2}, {.x = 1, .y = 3}};
    err = piecewise_linear_mapping(0, knots1, 3, &y);
    mu_assert(
        "The x-coordinate of each point need to be greater that the x-coordinate of the previous point, the y-coordinate needs to be greater or equal",
        err);

    VmafPoint knots2[] = {{.x = 0, .y = 2}, {.x = 1, .y = 1}};
    err = piecewise_linear_mapping(0, knots2, 2, &y);
    mu_assert(
        "The x-coordinate of each point need to be greater that the x-coordinate of the previous point, the y-coordinate needs to be greater or equal",
        err);

    mu_assert_msg(check_piecewise_reference_knots());

    static VmafPoint knots_single[] = {{.x = 10.0, .y = 10.0}, {.x = 50.0, .y = 60.0}};
    const PiecewiseCase single[] = {
        {knots_single, 1.25, -2.5, "returned y0 does not match y0_true", 2, 0, 1100, 0},
    };
    mu_assert_msg(check_piecewise_cases(single, sizeof(single) / sizeof(single[0])));

    return NULL;
}

/* Regression for fix/core-lifecycle-memory-audit:
 * piecewise_linear_mapping / piecewise_segment_apply previously returned
 * bare positive `EINVAL` instead of the negated convention used everywhere
 * else in libvmaf.  Local callers only check truthiness so the bug was
 * silent, but any caller that propagates the value upward inverts the sign.
 * Lock the contract down by asserting the negative value explicitly. */
static char *test_piecewise_linear_mapping_returns_neg_einval(void)
{
    double y = 0.0;

    /* n_knots <= 1 → -EINVAL */
    VmafPoint single[] = {{.x = 0, .y = 1}};
    int err = piecewise_linear_mapping(0, single, 1, &y);
    mu_assert("n_knots<=1 must return -EINVAL (not +EINVAL)", err == -EINVAL);

    /* horizontal-segment knots (x equal, y differ) → -EINVAL via segment guard */
    VmafPoint vertical[] = {{.x = 0, .y = 1}, {.x = 0, .y = 2}};
    err = piecewise_linear_mapping(0, vertical, 2, &y);
    mu_assert("vertical segment must return -EINVAL (not +EINVAL)", err == -EINVAL);

    /* decreasing y → -EINVAL */
    VmafPoint decreasing[] = {{.x = 0, .y = 2}, {.x = 1, .y = 1}};
    err = piecewise_linear_mapping(0, decreasing, 2, &y);
    mu_assert("decreasing y must return -EINVAL (not +EINVAL)", err == -EINVAL);

    return NULL;
}

static char *test_guided_feature_sentinel_semantics(void)
{
    typedef struct {
        char *message;
        double lhs;
        double rhs;
        bool expected;
    } EqualityCase;

    /* Sentinel contract: chroma correction occurs only when the guided feature
     * equals the sentinel value (0.0). Any non-zero value, NaN, or Inf must
     * compare not-equal to the sentinel; NaN is never equal even to NaN. */
    const EqualityCase cases[] = {
        {"0.0 matches sentinel 0.0", 0.0, 0.0, true},
        {"-0.0 matches sentinel 0.0", -0.0, 0.0, true},
        {"0.0 matches sentinel -0.0", 0.0, -0.0, true},
        {"1e-12 does not match sentinel 0.0", 1e-12, 0.0, false},
        {"NAN does not match sentinel 0.0", NAN, 0.0, false},
        {"NAN does not match NAN", NAN, NAN, false},
        {"INFINITY does not match sentinel 0.0", INFINITY, 0.0, false},
        {"INFINITY matches INFINITY", INFINITY, INFINITY, true},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const bool actual = float_values_equal(cases[i].lhs, cases[i].rhs);
        mu_assert(cases[i].message, actual == cases[i].expected);
    }
    return nullptr;
}

char *run_tests(void)
{
    mu_run_test(test_predict_score_at_index);
    mu_run_test(test_find_linear_function_parameters);
    mu_run_test(test_piecewise_linear_mapping);
    mu_run_test(test_piecewise_linear_mapping_returns_neg_einval);
    mu_run_test(test_guided_feature_sentinel_semantics);
    mu_run_test(test_propagate_metadata);
    return NULL;
}
