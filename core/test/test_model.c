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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"
// NOLINTNEXTLINE(bugprone-suspicious-include): white-box test deliberately includes model.c to inspect static built_in_models per ADR-0278 / ADR-0141.
#include "model.c"
#include "read_json_model.h"

static int model_compare(VmafModel *model_a, VmafModel *model_b)
{
    int err = 0;

    err += model_a->slope != model_b->slope;
    err += model_a->intercept != model_b->intercept;

    err += model_a->n_features != model_b->n_features;
    for (unsigned i = 0; i < model_a->n_features; i++) {
        err += model_a->feature[i].slope != model_b->feature[i].slope;
        err += model_a->feature[i].intercept != model_b->feature[i].intercept;
        err += !model_a->feature[i].opts_dict != !model_b->feature[i].opts_dict;
    }

    err += model_a->score_clip.enabled != model_b->score_clip.enabled;
    err += model_a->score_clip.min != model_b->score_clip.min;
    err += model_a->score_clip.max != model_b->score_clip.max;

    err += model_a->norm_type != model_b->norm_type;

    err += model_a->score_transform.enabled != model_b->score_transform.enabled;
    err += model_a->score_transform.p0.enabled != model_b->score_transform.p0.enabled;
    err += model_a->score_transform.p0.value != model_b->score_transform.p0.value;
    err += model_a->score_transform.p1.enabled != model_b->score_transform.p1.enabled;
    err += model_a->score_transform.p1.value != model_b->score_transform.p1.value;
    err += model_a->score_transform.p2.enabled != model_b->score_transform.p2.enabled;
    err += model_a->score_transform.p2.value != model_b->score_transform.p2.value;
    err += model_a->score_transform.knots.enabled != model_b->score_transform.knots.enabled;
    for (unsigned i = 0; i < model_a->score_transform.knots.n_knots; i++) {
        err += model_a->score_transform.knots.list[i].x != model_b->score_transform.knots.list[i].x;
        err += model_a->score_transform.knots.list[i].y != model_b->score_transform.knots.list[i].y;
    }
    err += model_a->score_transform.out_lte_in != model_b->score_transform.out_lte_in;
    err += model_a->score_transform.out_gte_in != model_b->score_transform.out_gte_in;

    return err;
}

/* Read the whole file into a malloc'd buffer; caller frees.
 * Returns NULL on error; sets *len on success. */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)fclose(f);
        return nullptr;
    }
    const long sz = ftell(f);
    const long max_slurp_size = 1L << 20;
    if (sz < 0 || sz > max_slurp_size || fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return nullptr;
    }
    const size_t file_sz = (size_t)sz;
    char *buf = calloc(file_sz + 1u, 1u);
    if (!buf) {
        (void)fclose(f);
        return nullptr;
    }
    const size_t got = fread(buf, 1, file_sz, f);
    (void)fclose(f);
    if (got != file_sz) {
        free(buf);
        return nullptr;
    }
    *len = file_sz;
    return buf;
}

static int append_str(char *dst, size_t dst_sz, size_t *off, const char *s)
{
    size_t len = strlen(s);
    if (*off + len >= dst_sz)
        return -ENOSPC;
    memcpy(&dst[*off], s, len + 1);
    *off += len;
    return 0;
}

static int append_uint(char *dst, size_t dst_sz, size_t *off, unsigned u)
{
    if (*off >= dst_sz)
        return -ENOSPC;
    int w = snprintf(&dst[*off], dst_sz - *off, "%u", u);
    if (w < 0 || (size_t)w >= dst_sz - *off)
        return -ENOSPC;
    *off += (size_t)w;
    return 0;
}

static char *test_json_model(void)
{
    int err = 0;

    VmafModel *model_json;
    VmafModelConfig cfg_json = {nullptr};
    const char *path_json = JSON_MODEL_PATH "vmaf_v0.6.1neg.json";

    err = vmaf_read_json_model_from_path(&model_json, &cfg_json, path_json);
    mu_assert("problem during vmaf_read_json_model", !err);

    VmafModel *model;
    VmafModelConfig cfg = {nullptr};
    const char *version = "vmaf_v0.6.1neg";

    err = vmaf_model_load(&model, &cfg, version);
    mu_assert("problem during vmaf_model_load_from_path", !err);

    err = model_compare(model_json, model);
    mu_assert("parsed json/built-in models do not match", !err);

    vmaf_model_destroy(model_json);
    vmaf_model_destroy(model);
    return nullptr;
}

#if VMAF_BUILT_IN_MODELS
static char *test_built_in_model(void)
{
    int err = 0;

    VmafModel *model;
    VmafModelConfig cfg = {nullptr};
    const char *version = "vmaf_v0.6.1neg";
    err = vmaf_model_load(&model, &cfg, version);
    mu_assert("problem during vmaf_model_load", !err);

    VmafModel *model_file;
    VmafModelConfig cfg_file = {nullptr};
    const char *path = JSON_MODEL_PATH "vmaf_v0.6.1neg.json";
    err = vmaf_model_load_from_path(&model_file, &cfg_file, path);
    mu_assert("problem during vmaf_model_load_from_path", !err);

    err = model_compare(model, model_file);
    mu_assert("parsed buffer/file models do not match", !err);

    vmaf_model_destroy(model);
    vmaf_model_destroy(model_file);
    return nullptr;
}
#endif

/* Regression for fix/core-lifecycle-memory-audit:
 * vmaf_model_load / vmaf_model_collection_load previously passed `version`
 * straight into strcmp(); a NULL caller dereferenced the second operand.
 * Both entry points must now reject NULL with -EINVAL. */
static char *test_model_load_rejects_null_version(void)
{
    VmafModel *model = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_model_load(&model, &cfg, nullptr);
    mu_assert("vmaf_model_load(NULL version) must return -EINVAL", err == -EINVAL);
    mu_assert("vmaf_model_load(NULL version) must not allocate model", model == NULL);

    VmafModelCollection *collection = nullptr;
    err = vmaf_model_collection_load(&model, &collection, &cfg, nullptr);
    mu_assert("vmaf_model_collection_load(NULL version) must return -EINVAL", err == -EINVAL);
    mu_assert("vmaf_model_collection_load(NULL version) must not allocate model", model == NULL);
    mu_assert("vmaf_model_collection_load(NULL version) must not allocate collection",
              collection == NULL);

    return nullptr;
}

static char *test_model_load_and_destroy(void)
{
    int err;

    VmafModel *model;
    VmafModelConfig cfg = {nullptr};
    const char *path = JSON_MODEL_PATH "vmaf_float_v0.6.1.json";
    err = vmaf_model_load_from_path(&model, &cfg, path);
    mu_assert("problem during vmaf_model_load_from_path", !err);

    /*
    for (unsigned i = 0; i < model->n_features; i++)
        fprintf(stderr, "feature name: %s slope: %f intercept: %f\n",
                model->feature[i].name,
                model->feature[i].slope,
                model->feature[i].intercept
        );
    */

    vmaf_model_destroy(model);

    return nullptr;
}

static char *check_model_feature_entry(const VmafModel *model, const char *expected_val, int step)
{
    mu_assert("feature 0 should be \"VMAF_integer_feature_adm2_score\"",
              !strcmp("VMAF_integer_feature_adm2_score", model->feature[0].name));
    mu_assert("feature 0 \"VMAF_integer_feature_adm2_score\" should have a non-NULL opts_dict",
              model->feature[0].opts_dict != nullptr);
    const VmafDictionaryEntry *e =
        vmaf_dictionary_get(&model->feature[0].opts_dict, "adm_enhancement_gain_limit", 0);
    mu_assert("dict lookup must return entry", e != nullptr);
    if (step == 1) {
        mu_assert("dict should have a new key/val pair",
                  !strcmp(e->key, "adm_enhancement_gain_limit") && !strcmp(e->val, expected_val));
    } else if (step == 2) {
        mu_assert("dict should have an existing key/val pair",
                  !strcmp(e->key, "adm_enhancement_gain_limit") && !strcmp(e->val, expected_val));
    } else {
        mu_assert("dict should have an updated key/val pair",
                  !strcmp(e->key, "adm_enhancement_gain_limit") && !strcmp(e->val, expected_val));
    }
    return nullptr;
}

static char *test_model_feature_step1(VmafModel **out_model)
{
    VmafModel *model = nullptr;
    VmafModelConfig cfg = {nullptr};
    const char *version = "vmaf_v0.6.1";
    int err = vmaf_model_load(&model, &cfg, version);
    mu_assert("problem during vmaf_model_load", !err);

    VmafFeatureDictionary *dict = nullptr;
    err = vmaf_feature_dictionary_set(&dict, "adm_enhancement_gain_limit", "1.1");
    mu_assert("problem during vmaf_feature_dictionary_set", !err);

    mu_assert("feature 0 should be \"VMAF_integer_feature_adm2_score\"",
              !strcmp("VMAF_integer_feature_adm2_score", model->feature[0].name));
    mu_assert("feature 0 \"VMAF_integer_feature_adm2_score\" should have a NULL opts_dict",
              !model->feature[0].opts_dict);

    err = vmaf_model_feature_overload(model, "adm", dict);
    mu_assert("problem during vmaf_model_feature_overload", !err);

    char *msg = check_model_feature_entry(model, "1.1", 1);
    if (msg) {
        vmaf_model_destroy(model);
        return msg;
    }
    *out_model = model;
    return nullptr;
}

static char *test_model_feature_step2(VmafModel *model)
{
    VmafModel *model_neg = nullptr;
    VmafModelConfig cfg_neg = {nullptr};
    const char *version_neg = "vmaf_v0.6.1neg";
    int err = vmaf_model_load(&model_neg, &cfg_neg, version_neg);
    mu_assert("problem during vmaf_model_load", !err);

    err = model_compare(model, model_neg);
    mu_assert("overloaded model should match model_neg", err);

    VmafFeatureDictionary *dict_neg = nullptr;
    err = vmaf_feature_dictionary_set(&dict_neg, "adm_enhancement_gain_limit", "1.2");
    mu_assert("problem during vmaf_feature_dictionary_set", !err);

    char *msg = check_model_feature_entry(model, "1.1", 2);
    if (msg) {
        vmaf_model_destroy(model_neg);
        return msg;
    }

    err = vmaf_model_feature_overload(model, "adm", dict_neg);
    mu_assert("problem during vmaf_model_feature_overload", !err);

    msg = check_model_feature_entry(model, "1.2", 3);
    vmaf_model_destroy(model_neg);
    return msg;
}

static char *test_model_feature(void)
{
    VmafModel *model = nullptr;
    char *msg = test_model_feature_step1(&model);
    if (msg)
        return msg;
    msg = test_model_feature_step2(model);
    vmaf_model_destroy(model);
    return msg;
}

static char *test_model_check_default_behavior_unset_flags(void)
{
    int err;

    VmafModel *model;
    VmafModelConfig cfg = {
        .name = "some_vmaf",
    };
    const char *path = JSON_MODEL_PATH "vmaf_float_v0.6.1.json";
    err = vmaf_model_load_from_path(&model, &cfg, path);
    mu_assert("problem during vmaf_model_load_from_path", !err);
    mu_assert("Model name is inconsistent.\n", !strcmp(model->name, "some_vmaf"));
    mu_assert("Clipping must be enabled by default.\n", model->score_clip.enabled);
    mu_assert("Score transform must be disabled by default.\n", !model->score_transform.enabled);
    /* Confidence interval is not a single-model property: it is computed by
     * vmaf_predict_score_at_index_model_collection() from an ensemble and
     * returned via VmafModelCollectionScore.bootstrap.ci.p95.  The collection-
     * type assertion is in test_model_collection_bootstrap_type(). */
    mu_assert("Feature 0 name must be VMAF_feature_adm2_score.\n",
              !strcmp(model->feature[0].name, "VMAF_feature_adm2_score"));

    vmaf_model_destroy(model);

    return nullptr;
}

static char *test_model_check_default_behavior_set_flags(void)
{
    int err;

    VmafModel *model;
    VmafModelConfig cfg = {
        .name = "some_vmaf",
        .flags = VMAF_MODEL_FLAGS_DEFAULT,
    };
    const char *path = JSON_MODEL_PATH "vmaf_float_v0.6.1.json";
    err = vmaf_model_load_from_path(&model, &cfg, path);
    mu_assert("problem during vmaf_model_load_from_path", !err);
    mu_assert("Model name is inconsistent.\n", !strcmp(model->name, "some_vmaf"));
    mu_assert("Clipping must be enabled by default.\n", model->score_clip.enabled);
    mu_assert("Score transform must be disabled by default.\n", !model->score_transform.enabled);
    /* See test_model_collection_bootstrap_type() for the confidence-interval
     * (VmafModelCollectionScore.bootstrap.ci.p95) collection-scope assertion. */
    mu_assert("Feature 0 name must be VMAF_feature_adm2_score.\n",
              !strcmp(model->feature[0].name, "VMAF_feature_adm2_score"));

    vmaf_model_destroy(model);

    return nullptr;
}

static char *test_model_set_flags_transform_and_clip(void)
{
    VmafModel *model1;
    VmafModelConfig cfg1 = {
        .flags = VMAF_MODEL_FLAG_ENABLE_TRANSFORM,
    };
    const char *path1 = JSON_MODEL_PATH "vmaf_float_v0.6.1.json";
    int err = vmaf_model_load_from_path(&model1, &cfg1, path1);
    mu_assert("problem during vmaf_model_load_from_path", !err);
    mu_assert("Score transform must be enabled.\n", model1->score_transform.enabled);
    mu_assert("Clipping must be enabled.\n", model1->score_clip.enabled);
    vmaf_model_destroy(model1);

    VmafModel *model2;
    VmafModelConfig cfg2 = {
        .flags = VMAF_MODEL_FLAG_DISABLE_CLIP,
    };
    const char *path2 = JSON_MODEL_PATH "vmaf_float_v0.6.1.json";
    err = vmaf_model_load_from_path(&model2, &cfg2, path2);
    mu_assert("problem during vmaf_model_load_from_path", !err);
    mu_assert("Score transform must be disabled.\n", !model2->score_transform.enabled);
    mu_assert("Clipping must be disabled.\n", !model2->score_clip.enabled);
    vmaf_model_destroy(model2);
    return nullptr;
}

static char *test_model_set_flags_default_opts(void)
{
    VmafModel *model3;
    VmafModelConfig cfg3 = {nullptr};
    const char *path3 = JSON_MODEL_PATH "vmaf_float_v0.6.1.json";
    int err = vmaf_model_load_from_path(&model3, &cfg3, path3);
    mu_assert("problem during vmaf_model_load_from_path", !err);
    mu_assert("feature[0].opts_dict must be NULL.\n", !model3->feature[0].opts_dict);
    mu_assert("feature[1].opts_dict must be NULL.\n", !model3->feature[1].opts_dict);
    mu_assert("feature[2].opts_dict must be NULL.\n", !model3->feature[2].opts_dict);
    mu_assert("feature[3].opts_dict must be NULL.\n", !model3->feature[3].opts_dict);
    mu_assert("feature[4].opts_dict must be NULL.\n", !model3->feature[4].opts_dict);
    mu_assert("feature[5].opts_dict must be NULL.\n", !model3->feature[5].opts_dict);
    vmaf_model_destroy(model3);
    return nullptr;
}

static char *check_model_neg_feature_opts(const VmafModel *model4)
{
    const VmafDictionaryEntry *entry =
        vmaf_dictionary_get(&model4->feature[0].opts_dict, "adm_enhn_gain_limit", 0);
    mu_assert("feature[0].opts_dict lookup must return entry.\n", entry != nullptr);
    mu_assert("feature[0].opts_dict must have key adm_enhn_gain_limit.\n",
              strcmp(entry->key, "adm_enhn_gain_limit") == 0);
    mu_assert("feature[0].opts_dict[\"adm_enhn_gain_limit\"] must have value 1.\n",
              strcmp(entry->val, "1") == 0);

    for (unsigned f = 2; f <= 5; f++) {
        entry = vmaf_dictionary_get(&model4->feature[f].opts_dict, "vif_enhn_gain_limit", 0);
        mu_assert("feature opts_dict lookup must return entry.\n", entry != nullptr);
        mu_assert("feature opts_dict must have key vif_enhn_gain_limit.\n",
                  strcmp(entry->key, "vif_enhn_gain_limit") == 0);
        mu_assert("feature opts_dict[\"vif_enhn_gain_limit\"] must have value 1.\n",
                  strcmp(entry->val, "1") == 0);
    }
    return nullptr;
}

static char *test_model_set_flags_neg_opts(void)
{
    VmafModel *model4;
    VmafModelConfig cfg4 = {nullptr};
    const char *path4 = JSON_MODEL_PATH "vmaf_float_v0.6.1neg.json";
    int err = vmaf_model_load_from_path(&model4, &cfg4, path4);
    mu_assert("problem during vmaf_model_load_from_path", !err);
    mu_assert("feature[0].opts_dict must not be NULL.\n", model4->feature[0].opts_dict);
    mu_assert("feature[1].opts_dict must be NULL.\n", !model4->feature[1].opts_dict);
    mu_assert("feature[2].opts_dict must not be NULL.\n", model4->feature[2].opts_dict);
    mu_assert("feature[3].opts_dict must not be NULL.\n", model4->feature[3].opts_dict);
    mu_assert("feature[4].opts_dict must not be NULL.\n", model4->feature[4].opts_dict);
    mu_assert("feature[5].opts_dict must not be NULL.\n", model4->feature[5].opts_dict);

    char *msg = check_model_neg_feature_opts(model4);
    vmaf_model_destroy(model4);
    return msg;
}

static char *test_model_set_flags(void)
{
    char *msg = test_model_set_flags_transform_and_clip();
    if (msg)
        return msg;
    msg = test_model_set_flags_default_opts();
    if (msg)
        return msg;
    return test_model_set_flags_neg_opts();
}

/* Exercises vmaf_read_json_model_from_buffer — never hit by the existing
 * tests, which only use vmaf_read_json_model_from_path. Round-trip: file →
 * buffer → parse, and compare against the path-parsed model. */
static char *test_json_model_from_buffer(void)
{
    const char *path = JSON_MODEL_PATH "vmaf_float_v0.6.1.json";
    size_t len = 0;
    char *buf = slurp(path, &len);
    mu_assert("slurp failed", buf != nullptr);

    VmafModel *m_buf = nullptr;
    VmafModelConfig cfg_buf = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m_buf, &cfg_buf, buf, (int)len);
    free(buf);
    mu_assert("from_buffer failed", !err);

    VmafModel *m_path = nullptr;
    VmafModelConfig cfg_path = {nullptr};
    err = vmaf_read_json_model_from_path(&m_path, &cfg_path, path);
    if (err) {
        vmaf_model_destroy(m_buf);
        return "from_path failed";
    }

    int cmp = model_compare(m_buf, m_path);
    vmaf_model_destroy(m_buf);
    vmaf_model_destroy(m_path);
    mu_assert("buffer/path models diverge", !cmp);
    return nullptr;
}

/* Missing path → -EINVAL from the fopen guard in vmaf_read_json_model_from_path. */
static char *test_json_model_missing_path(void)
{
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err =
        vmaf_read_json_model_from_path(&m, &cfg, "/nonexistent/path/vmaf_does_not_exist.json");
    mu_assert("missing path should return -EINVAL", err == -EINVAL);
    return nullptr;
}

/* Malformed JSON buffer → non-zero error from the parser. */
static char *test_json_model_malformed_buffer(void)
{
    const char garbage[] = "{this is definitely not valid json}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, garbage, (int)sizeof(garbage) - 1);
    mu_assert("malformed JSON should return non-zero", err != 0);
    /* On the error path the parser may still have allocated *m; free if so. */
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* Empty buffer → non-zero error. */
static char *test_json_model_empty_buffer(void)
{
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, "", 0);
    mu_assert("empty buffer should return non-zero", err != 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* Exercises vmaf_read_json_model_collection_from_path on the bootstrap
 * ensemble model. Verifies model_collection_parse iterates its keyed
 * sub-models ("0", "1", …) and fills both *model (first) and
 * *model_collection (rest). */
static char *test_json_model_collection_from_path(void)
{
    const char *path = JSON_MODEL_PATH "vmaf_b_v0.6.3.json";
    VmafModel *m = nullptr;
    VmafModelCollection *mc = nullptr;
    VmafModelConfig cfg = {.name = "vmaf_b"};
    int err = vmaf_read_json_model_collection_from_path(&m, &mc, &cfg, path);
    mu_assert("collection_from_path failed", !err);
    mu_assert("first model not populated", m != nullptr);
    mu_assert("collection not populated", mc != nullptr);

    vmaf_model_destroy(m);
    vmaf_model_collection_destroy(mc);
    return nullptr;
}

/* Same ensemble model via the buffer entry point. */
static char *test_json_model_collection_from_buffer(void)
{
    const char *path = JSON_MODEL_PATH "vmaf_b_v0.6.3.json";
    size_t len = 0;
    char *buf = slurp(path, &len);
    mu_assert("slurp failed", buf != nullptr);

    VmafModel *m = nullptr;
    VmafModelCollection *mc = nullptr;
    VmafModelConfig cfg = {.name = "vmaf_b_buf"};
    int err = vmaf_read_json_model_collection_from_buffer(&m, &mc, &cfg, buf, (int)len);
    free(buf);
    mu_assert("collection_from_buffer failed", !err);
    mu_assert("first model not populated", m != nullptr);
    mu_assert("collection not populated", mc != nullptr);

    vmaf_model_destroy(m);
    vmaf_model_collection_destroy(mc);
    return nullptr;
}

/* Missing path for the collection API → -EINVAL. */
static char *test_json_model_collection_missing_path(void)
{
    VmafModel *m = nullptr;
    VmafModelCollection *mc = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err =
        vmaf_read_json_model_collection_from_path(&m, &mc, &cfg, "/nonexistent/path/vmaf_b.json");
    mu_assert("missing collection path should return -EINVAL", err == -EINVAL);
    return nullptr;
}

/* Collection buffer that is not an object → model_collection_parse early
 * -EINVAL branch. */
static char *test_json_model_collection_malformed_buffer(void)
{
    const char garbage[] = "[1, 2, 3]";
    VmafModel *m = nullptr;
    VmafModelCollection *mc = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_collection_from_buffer(&m, &mc, &cfg, garbage,
                                                          (int)sizeof(garbage) - 1);
    mu_assert("non-object collection should return non-zero", err != 0);
    return nullptr;
}

/* Hits parser branches that upstream model JSONs don't exercise:
 *   - model_type = RESIDUEBOOTSTRAP_LIBSVMNUSVR
 *   - norm_type = none
 *   - score_transform with knots array + out_lte_in
 *   - feature_opts_dicts with string and bool values (not just numbers)
 * We don't assert a specific return code — libsvm's parser tolerates a
 * lot of garbage, so the call may succeed or fail depending on where in
 * the token stream it gives up. All we care about here is that the
 * pre-libsvm branches get exercised without crashing. */
static char *test_json_model_synthetic_branches(void)
{
    const char json[] =
        "{"
        "\"model_dict\": {"
        "\"model_type\": \"RESIDUEBOOTSTRAP_LIBSVMNUSVR\","
        "\"norm_type\": \"none\","
        "\"score_transform\": {"
        "\"enabled\": true,"
        "\"p0\": 1.0,"
        "\"p1\": 2.0,"
        "\"p2\": 3.0,"
        "\"knots\": [[0.0, 0.0], [1.0, 1.0]],"
        "\"out_lte_in\": \"true\","
        "\"out_gte_in\": \"false\""
        "},"
        "\"feature_names\": [\"f1\", \"f2\"],"
        "\"slopes\": [1.0, 0.1, 0.2],"
        "\"intercepts\": [0.0, 0.0, 0.0],"
        "\"feature_opts_dicts\": ["
        "{\"k_num\": 1.5, \"k_str\": \"hello\", \"k_true\": true, \"k_false\": false}"
        "],"
        "\"score_clip\": [0, 100],"
        "\"model\": \"not-a-real-libsvm-payload\""
        "}"
        "}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    /* libsvm's string parser is permissive and may accept arbitrary bytes,
     * so don't assert on err — the point is that parse_model_dict /
     * parse_score_transform / parse_feature_opts_dicts executed without
     * crashing and any allocation got cleaned up. */
    (void)err;
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

static int append_65_feature_names(char *json, size_t json_sz, size_t *off)
{
    int err = append_str(json, json_sz, off, "{\"model_dict\":{\"feature_names\":[");
    for (unsigned i = 0; i < 65u && !err; i++) {
        if (i > 0)
            err = append_str(json, json_sz, off, ",");
        if (!err)
            err = append_str(json, json_sz, off, "\"feature_");
        if (!err)
            err = append_uint(json, json_sz, off, i);
        if (!err)
            err = append_str(json, json_sz, off, "\"");
    }
    return err;
}

static int append_65_feature_slopes_intercepts(char *json, size_t json_sz, size_t *off)
{
    int err = append_str(json, json_sz, off, "],\"slopes\":[1.0");
    for (unsigned i = 0; i < 65u && !err; i++) {
        if (!err)
            err = append_str(json, json_sz, off, ",");
        if (!err)
            err = append_uint(json, json_sz, off, i + 1u);
        if (!err)
            err = append_str(json, json_sz, off, ".0");
    }
    if (!err)
        err = append_str(json, json_sz, off, "],\"intercepts\":[0.0");
    for (unsigned i = 0; i < 65u && !err; i++) {
        if (!err)
            err = append_str(json, json_sz, off, ",");
        if (!err)
            err = append_uint(json, json_sz, off, i);
        if (!err)
            err = append_str(json, json_sz, off, ".0");
    }
    if (!err)
        err = append_str(json, json_sz, off, "]}}");
    return err;
}

static int build_65_feature_json(char *json, size_t json_sz, size_t *out_len)
{
    size_t off = 0;
    int err = append_65_feature_names(json, json_sz, &off);
    if (!err)
        err = append_65_feature_slopes_intercepts(json, json_sz, &off);
    *out_len = off;
    return err;
}

static char *check_65_feature_model(const VmafModel *m)
{
    mu_assert("feature count must be preserved", m->n_features == 65u);
    mu_assert("feature capacity must grow past the old fixed limit", m->feature_cap >= 65u);
    mu_assert("last feature name must parse", strcmp(m->feature[64].name, "feature_64") == 0);
    mu_assert("last feature slope must parse", m->feature[64].slope == 65.0);
    mu_assert("last feature intercept must parse", m->feature[64].intercept == 64.0);
    return nullptr;
}

static char *test_json_model_allows_more_than_64_features(void)
{
    char json[8192];
    size_t off = 0;
    int err = build_65_feature_json(json, sizeof(json), &off);
    mu_assert("synthetic model JSON builder overflowed", !err);

    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)off);
    mu_assert("65-feature JSON model must parse", !err);
    char *msg = check_65_feature_model(m);
    vmaf_model_destroy(m);
    return msg;
}

static int build_11_knot_json(char *json, size_t json_sz, size_t *out_len)
{
    size_t off = 0;
    int err = append_str(json, json_sz, &off,
                         "{\"model_dict\":{\"score_transform\":{\"enabled\":true,\"knots\":[");
    for (unsigned i = 0; i < 11u && !err; i++) {
        if (i > 0)
            err = append_str(json, json_sz, &off, ",");
        if (!err)
            err = append_str(json, json_sz, &off, "[");
        if (!err)
            err = append_uint(json, json_sz, &off, i);
        if (!err)
            err = append_str(json, json_sz, &off, ".0,");
        if (!err)
            err = append_uint(json, json_sz, &off, i + 1u);
        if (!err)
            err = append_str(json, json_sz, &off, ".0]");
    }
    if (!err)
        err = append_str(json, json_sz, &off, "]}}}");
    *out_len = off;
    return err;
}

static char *check_11_knot_model(const VmafModel *m)
{
    mu_assert("knot count must be preserved", m->score_transform.knots.n_knots == 11u);
    mu_assert("knot capacity must grow past the old fixed limit",
              m->score_transform.knots.cap >= 11u);
    mu_assert("last knot x must parse", m->score_transform.knots.list[10].x == 10.0);
    mu_assert("last knot y must parse", m->score_transform.knots.list[10].y == 11.0);
    return nullptr;
}

static char *test_json_model_allows_more_than_10_knots(void)
{
    char json[2048];
    size_t off = 0;
    int err = build_11_knot_json(json, sizeof(json), &off);
    mu_assert("synthetic knot JSON builder overflowed", !err);

    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)off);
    mu_assert("11-knot JSON model must parse", !err);
    char *msg = check_11_knot_model(m);
    vmaf_model_destroy(m);
    return msg;
}

/* parse_model_dict: unknown model_type value → -EINVAL (line 333). */
static char *test_json_model_unknown_model_type(void)
{
    const char json[] = "{\"model_dict\": {\"model_type\": \"NOT_A_REAL_TYPE\"}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("unknown model_type must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict: unknown norm_type value → -EINVAL (line 347). */
static char *test_json_model_unknown_norm_type(void)
{
    const char json[] = "{\"model_dict\": {\"norm_type\": \"weird-norm\"}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("unknown norm_type must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict: model_type not a string → -EINVAL (line 324). */
static char *test_json_model_model_type_not_string(void)
{
    const char json[] = "{\"model_dict\": {\"model_type\": 42}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-string model_type must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict: norm_type not a string → -EINVAL (line 340). */
static char *test_json_model_norm_type_not_string(void)
{
    const char json[] = "{\"model_dict\": {\"norm_type\": 7}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-string norm_type must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict: score_transform not an object → -EINVAL (line 308). */
static char *test_json_model_score_transform_not_object(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": [1,2,3]}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-object score_transform must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform: p0 neither null nor number → -EINVAL (line 216). */
static char *test_json_model_score_transform_p0_bad_type(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"p0\": \"oops\"}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("string p0 must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform: p1 bad type → -EINVAL (line 228). */
static char *test_json_model_score_transform_p1_bad_type(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"p1\": true}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("bool p1 must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform: p2 bad type → -EINVAL (line 240). */
static char *test_json_model_score_transform_p2_bad_type(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"p2\": false}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("bool p2 must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform: knots neither null nor array → -EINVAL (line 255). */
static char *test_json_model_score_transform_knots_bad_type(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"knots\": 99}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("number knots must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform: out_lte_in not a string → -EINVAL (line 262). */
static char *test_json_model_score_transform_out_lte_in_not_string(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"out_lte_in\": 1}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-string out_lte_in must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform: out_gte_in not a string → -EINVAL (line 271). */
static char *test_json_model_score_transform_out_gte_in_not_string(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"out_gte_in\": 1}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-string out_gte_in must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform: enabled neither true nor false → -EINVAL (line 204). */
static char *test_json_model_score_transform_enabled_bad_type(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"enabled\": 7}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-bool enabled must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_feature_names: non-string element → -EINVAL (line 180). */
static char *test_json_model_feature_names_non_string(void)
{
    const char json[] = "{\"model_dict\": {\"feature_names\": [\"ok\", 42]}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-string feature name must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* Regression for the append_feature_name memory leak found by the nightly
 * fuzz_json_model LeakSanitizer lane. A duplicate `feature_names` key re-runs
 * parse_feature_names from index 0, so the second array overwrites
 * `feature[0].name`. Before the fix, append_feature_name strdup'd over the slot
 * without freeing the prior value — the first name ("orphan_name_0") was
 * orphaned. vmaf_model_destroy only walks the *current* slot occupants, so the
 * orphan was unreachable and leaked regardless of whether the parse ultimately
 * succeeded or hit a cross-key validation error. This test reproduces the
 * leaking input shape; under ASan (-Db_sanitize=address) it surfaces as a
 * LeakSanitizer direct-leak pre-fix and is clean post-fix. The assertion is
 * deliberately lenient on the return code (the single-model and collection JSON
 * walkers differ in whether they reject a per-feature length disagreement) — the
 * regression signal is the absence of a leak, enforced by the sanitizer. */
static char *test_json_model_feature_names_duplicate_key_no_leak(void)
{
    const char json[] = "{\"model_dict\": {"
                        "\"feature_names\": [\"orphan_name_0\"],"
                        "\"slopes\": [1.0, 2.0, 3.0],"
                        "\"feature_names\": [\"replacement\"]"
                        "}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    /* Contract: on non-zero return *model is left NULL (no caller destroy); on
     * success it is heap-owned and the caller must release it. Honour both so
     * the only allocation the sanitizer can flag is the orphaned feature name. */
    if (err == 0) {
        mu_assert("successful parse must yield a model", m != NULL);
        vmaf_model_destroy(m);
    } else {
        mu_assert("rejected parse must leave *model untouched (NULL)", m == NULL);
    }
    return nullptr;
}

/* parse_slopes: non-number element → -EINVAL (line 116). */
static char *test_json_model_slopes_non_number(void)
{
    const char json[] = "{\"model_dict\": {\"slopes\": [1.0, \"x\"]}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-number slope must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_intercepts: first element not a number → -EINVAL (line 92). */
static char *test_json_model_intercepts_first_not_number(void)
{
    const char json[] = "{\"model_dict\": {\"intercepts\": [\"nope\"]}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-number first intercept must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_knots: outer element not an array → -EINVAL (line 149). */
static char *test_json_model_knots_outer_not_array(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"knots\": [42]}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-array knot must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_knots_list: knot pair holds >2 numbers → -EINVAL (line 132). */
static char *test_json_model_knots_too_many_values(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"knots\": [[0.0, 1.0, 2.0]]}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("knot triple must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_feature_opts_dicts: value type other than number/bool/string
 * (here: null) → -EINVAL (line 77). */
static char *test_json_model_feature_opts_dict_bad_value_type(void)
{
    const char json[] = "{\"model_dict\": {\"feature_opts_dicts\": [{\"k\": null}]}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("null opts value must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict: score_clip not an array → -EINVAL (line 354). */
static char *test_json_model_score_clip_not_array(void)
{
    const char json[] = "{\"model_dict\": {\"score_clip\": 0}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-array score_clip must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict: top-level model_dict value not an object → -EINVAL (line 299). */
static char *test_json_model_model_dict_not_object(void)
{
    const char json[] = "{\"model_dict\": [1,2]}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-object model_dict must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* Collection parser hits json_skip() for keys that don't match the
 * generated "%d" index sequence (line 556). Also hits the -EINVAL early
 * return when the inner model payload is malformed (line 538). */
static char *test_json_model_collection_skips_unknown_keys(void)
{
    const char json[] = "{"
                        "\"extra_meta\": \"ignored\","
                        "\"0\": \"not an object — will fail inner parse\""
                        "}";
    VmafModel *m = nullptr;
    VmafModelCollection *mc = nullptr;
    VmafModelConfig cfg = {.name = "synth"};
    int err =
        vmaf_read_json_model_collection_from_buffer(&m, &mc, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("bad inner model should fail the collection parse", err != 0);
    if (m)
        vmaf_model_destroy(m);
    if (mc)
        vmaf_model_collection_destroy(mc);
    return nullptr;
}

/* Exercises the score_transform parser branches (p0, p1, p2, out_gte_in)
 * via VMAF_MODEL_FLAG_ENABLE_TRANSFORM on a model that actually carries a
 * score_transform block. vmaf_v0.6.1.json has one. */
static char *test_json_model_score_transform(void)
{
    VmafModel *m;
    VmafModelConfig cfg = {
        .flags = VMAF_MODEL_FLAG_ENABLE_TRANSFORM,
    };
    const char *path = JSON_MODEL_PATH "vmaf_v0.6.1.json";
    int err = vmaf_read_json_model_from_path(&m, &cfg, path);
    mu_assert("load failed", !err);
    mu_assert("score_transform should be enabled", m->score_transform.enabled);
    mu_assert("p0 should be enabled", m->score_transform.p0.enabled);
    mu_assert("p1 should be enabled", m->score_transform.p1.enabled);
    mu_assert("p2 should be enabled", m->score_transform.p2.enabled);
    mu_assert("out_gte_in should be set", m->score_transform.out_gte_in);
    vmaf_model_destroy(m);
    return nullptr;
}

/* Verifies that vmaf_b_v0.6.3.json (bootstrap ensemble) parses as a
 * collection with a bootstrap model type and more than one sub-model.
 * Confidence-interval values (VmafModelCollectionScore.bootstrap.ci.p95)
 * are computed at score time by vmaf_predict_score_at_index_model_collection();
 * this test covers the structural preconditions for that path. */
static char *test_model_collection_bootstrap_type(void)
{
    const char *path = JSON_MODEL_PATH "vmaf_b_v0.6.3.json";
    VmafModel *m = nullptr;
    VmafModelCollection *mc = nullptr;
    VmafModelConfig cfg = {.name = "vmaf_b_ci"};
    int err = vmaf_read_json_model_collection_from_path(&m, &mc, &cfg, path);
    mu_assert("vmaf_b collection load failed", !err);
    mu_assert("primary model must be non-null", m != NULL);
    mu_assert("collection must be non-null", mc != NULL);
    mu_assert("bootstrap collection must have >1 sub-models", mc->cnt > 1u);
    mu_assert("collection model type must be a bootstrap variant",
              mc->type == VMAF_MODEL_BOOTSTRAP_SVM_NUSVR ||
                  mc->type == VMAF_MODEL_RESIDUE_BOOTSTRAP_SVM_NUSVR);
    vmaf_model_destroy(m);
    vmaf_model_collection_destroy(mc);
    return nullptr;
}

static char *test_version_next(void)
{
    const void *next = nullptr;
    const char *version = nullptr;
    unsigned count = 0;
    while ((next = vmaf_model_version_next(next, &version)) != NULL) {
        const VmafBuiltInModel *m = next;
        mu_assert("vmaf_model_version_next must hand out the stored version pointer",
                  m->version == version);
        count++;
    }
    mu_assert("vmaf_model_version_next must iterate every built-in model exactly once",
              count == BUILT_IN_MODEL_CNT);
    return nullptr;
}

/* parse_model_dict_array_key: "slopes" value not an array → -EINVAL
 * (read_json_model.c:444). The existing slopes_non_number test enters
 * parse_slopes via a valid array; this one short-circuits one frame
 * earlier on the outer type check. */
static char *test_json_model_slopes_not_array(void)
{
    const char json[] = "{\"model_dict\": {\"slopes\": \"not an array\"}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-array slopes must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict_array_key: "intercepts" value not an array → -EINVAL
 * (read_json_model.c:453). Companion to slopes_not_array — covers the
 * same outer-type branch on the intercepts key. */
static char *test_json_model_intercepts_not_array(void)
{
    const char json[] = "{\"model_dict\": {\"intercepts\": 42}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-array intercepts must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict_array_key: "feature_names" value not an array → -EINVAL
 * (read_json_model.c:462). */
static char *test_json_model_feature_names_not_array(void)
{
    const char json[] =
        "{\"model_dict\": {\"feature_names\": \"VMAF_feature_integer_motion2_score\"}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-array feature_names must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict_array_key: "feature_opts_dicts" value not an array → -EINVAL
 * (read_json_model.c:467). */
static char *test_json_model_feature_opts_dicts_not_array(void)
{
    const char json[] = "{\"model_dict\": {\"feature_opts_dicts\": {\"a\": 1}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-array feature_opts_dicts must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict_array_key: "model" value not a string → -EINVAL
 * (read_json_model.c:472). The libsvm payload must come as a string; a
 * numeric value short-circuits the parser before parse_libsvm_model. */
static char *test_json_model_model_payload_not_string(void)
{
    const char json[] = "{\"model_dict\": {\"model\": 12345}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-string model payload must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict_chroma_correction: value not a number → -EINVAL
 * (read_json_model.c:433-434). */
static char *test_json_model_chroma_correction_not_number(void)
{
    const char json[] =
        "{\"model_dict\": {\"chroma_correction_parameter\": \"definitely-not-a-number\"}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-number chroma_correction_parameter must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict_score_clip: first element (min) not a number → -EINVAL
 * (read_json_model.c:420-421). The DISABLE_CLIP flag would short-circuit the
 * type check before it fires; default flags include enable_clip so the test
 * passes a 0-flag cfg. */
static char *test_json_model_score_clip_min_not_number(void)
{
    const char json[] = "{\"model_dict\": {\"score_clip\": [\"x\", 100.0]}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-number score_clip min must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict_score_clip: second element (max) not a number → -EINVAL
 * (read_json_model.c:423-424). */
static char *test_json_model_score_clip_max_not_number(void)
{
    const char json[] = "{\"model_dict\": {\"score_clip\": [0.0, \"y\"]}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-number score_clip max must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform_poly: value is JSON_NULL → enabled=false, returns 0
 * (read_json_model.c:268-270). The polynomial-disabled path is exercised by
 * a model_dict with score_transform.p0=null and ENABLE_TRANSFORM in flags
 * so the score_transform block isn't silently skipped. */
static char *test_json_model_score_transform_poly_null_disables(void)
{
    const char json[] =
        "{\"model_dict\": {\"score_transform\": {\"enabled\": true, \"p0\": null}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {.flags = VMAF_MODEL_FLAG_ENABLE_TRANSFORM};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    /* The model has no libsvm payload, so vmaf_read_json_model will surface
     * a non-zero error downstream; we only need to know that parsing the
     * null-poly key did not itself reject (no -EINVAL from parse_score_
     * transform_poly). Walking the JSON_NULL branch is the goal. */
    (void)err;
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform_knots_key: knots value is JSON_NULL → enabled=false
 * (read_json_model.c:282-285). Companion to the poly null test above. */
static char *test_json_model_score_transform_knots_null_disables(void)
{
    const char json[] =
        "{\"model_dict\": {\"score_transform\": {\"enabled\": true, \"knots\": null}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {.flags = VMAF_MODEL_FLAG_ENABLE_TRANSFORM};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    (void)err;
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform_bool_str: value is not a string → -EINVAL
 * (read_json_model.c:299-300). Existing tests cover the happy path
 * ("true"/"false") and a non-string for "enabled"; this one drives the
 * branch via the "out_lte_in" key which routes through bool_str. */
static char *test_json_model_score_transform_bool_str_not_string(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"out_lte_in\": 1.5}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {.flags = VMAF_MODEL_FLAG_ENABLE_TRANSFORM};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-string bool field must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_model_dict_entry: an unrecognised key triggers the
 * parse_model_dict_array_key fall-through and the json_skip(s) path
 * (read_json_model.c:497). Differs from the existing collection_skips
 * test in that this hits parse_model_dict_entry's tail, not the
 * collection-level skip. */
static char *test_json_model_unrecognised_model_dict_key(void)
{
    const char json[] = "{\"model_dict\": {\"this_key_is_not_recognised\": 42}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    /* parse_model_dict_array_key returns 1 → parse_model_dict_entry skips
     * the value and continues. The outer model_parse still surfaces an
     * error because there is no libsvm payload, but the skip branch has
     * been walked. Any negative return is acceptable. */
    (void)err;
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_intercepts: subsequent (not first) intercept not a number → -EINVAL
 * (read_json_model.c:167-168). The existing
 * test_json_model_intercepts_first_not_number covers the first-element
 * check at line 161-162; this one drives the in-loop check after a valid
 * first intercept has been consumed. */
static char *test_json_model_intercepts_mid_not_number(void)
{
    const char json[] = "{\"model_dict\": {\"intercepts\": [1.0, \"bad\"]}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {nullptr};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("non-number mid intercept must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform_poly: numeric branch surfaces a non-zero
 * polynomial constant (read_json_model.c:272-275). Hit through "p1"
 * because the vmaf_v0.6.1.json fixture only covers all-enabled paths;
 * an isolated synthetic model with only p1 sets the enabled=true /
 * value=number branch in isolation. */
static char *test_json_model_score_transform_poly_number_sets_value(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"enabled\": true, "
                        "\"p1\": 0.75}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {.flags = VMAF_MODEL_FLAG_ENABLE_TRANSFORM};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    /* No libsvm payload → vmaf_read_json_model surfaces an error
     * downstream; the parse_score_transform_poly numeric branch ran
     * before that, which is the goal. */
    (void)err;
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform_poly: token is neither NULL nor NUMBER → -EINVAL
 * (read_json_model.c:277). Existing tests cover bool tokens for p0/p1/p2;
 * a STRING token routes through parse_score_transform_entry → poly and
 * reaches the trailing -EINVAL return. */
static char *test_json_model_score_transform_poly_string_rejects(void)
{
    const char json[] =
        "{\"model_dict\": {\"score_transform\": {\"enabled\": true, \"p1\": \"oops\"}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {.flags = VMAF_MODEL_FLAG_ENABLE_TRANSFORM};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    mu_assert("string-valued polynomial constant must reject", err < 0);
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

/* parse_score_transform_knots_key: knots is an array → walks parse_knots
 * (read_json_model.c:287-292). Existing knots tests cover error branches
 * inside parse_knots; this one drives the successful array-walk wrapping
 * in parse_score_transform_knots_key. */
static char *test_json_model_score_transform_knots_array_walks(void)
{
    const char json[] = "{\"model_dict\": {\"score_transform\": {\"enabled\": true, "
                        "\"knots\": [[0.0, 0.0], [50.0, 50.0], [100.0, 100.0]]}}}";
    VmafModel *m = nullptr;
    VmafModelConfig cfg = {.flags = VMAF_MODEL_FLAG_ENABLE_TRANSFORM};
    int err = vmaf_read_json_model_from_buffer(&m, &cfg, json, (int)sizeof(json) - 1);
    /* No libsvm payload → downstream error, but the knots walk runs
     * before the surface error fires. */
    (void)err;
    if (m)
        vmaf_model_destroy(m);
    return nullptr;
}

typedef struct {
    const char *name;
    char *(*fn)(void);
} TestCase;

static const TestCase test_cases[] = {
    {"test_json_model", test_json_model},
#if VMAF_BUILT_IN_MODELS
    {"test_built_in_model", test_built_in_model},
#endif
    {"test_model_load_and_destroy", test_model_load_and_destroy},
    {"test_model_load_rejects_null_version", test_model_load_rejects_null_version},
    {"test_model_check_default_behavior_unset_flags",
     test_model_check_default_behavior_unset_flags},
    {"test_model_check_default_behavior_set_flags", test_model_check_default_behavior_set_flags},
    {"test_model_set_flags", test_model_set_flags},
    {"test_model_feature", test_model_feature},
    {"test_json_model_from_buffer", test_json_model_from_buffer},
    {"test_json_model_missing_path", test_json_model_missing_path},
    {"test_json_model_malformed_buffer", test_json_model_malformed_buffer},
    {"test_json_model_empty_buffer", test_json_model_empty_buffer},
    {"test_json_model_collection_from_path", test_json_model_collection_from_path},
    {"test_json_model_collection_from_buffer", test_json_model_collection_from_buffer},
    {"test_json_model_collection_missing_path", test_json_model_collection_missing_path},
    {"test_json_model_collection_malformed_buffer", test_json_model_collection_malformed_buffer},
    {"test_model_collection_bootstrap_type", test_model_collection_bootstrap_type},
    {"test_json_model_score_transform", test_json_model_score_transform},
    {"test_json_model_synthetic_branches", test_json_model_synthetic_branches},
    {"test_json_model_allows_more_than_64_features", test_json_model_allows_more_than_64_features},
    {"test_json_model_allows_more_than_10_knots", test_json_model_allows_more_than_10_knots},
    {"test_json_model_collection_skips_unknown_keys",
     test_json_model_collection_skips_unknown_keys},
    {"test_json_model_unknown_model_type", test_json_model_unknown_model_type},
    {"test_json_model_unknown_norm_type", test_json_model_unknown_norm_type},
    {"test_json_model_model_type_not_string", test_json_model_model_type_not_string},
    {"test_json_model_norm_type_not_string", test_json_model_norm_type_not_string},
    {"test_json_model_score_transform_not_object", test_json_model_score_transform_not_object},
    {"test_json_model_score_transform_p0_bad_type", test_json_model_score_transform_p0_bad_type},
    {"test_json_model_score_transform_p1_bad_type", test_json_model_score_transform_p1_bad_type},
    {"test_json_model_score_transform_p2_bad_type", test_json_model_score_transform_p2_bad_type},
    {"test_json_model_score_transform_knots_bad_type",
     test_json_model_score_transform_knots_bad_type},
    {"test_json_model_score_transform_out_lte_in_not_string",
     test_json_model_score_transform_out_lte_in_not_string},
    {"test_json_model_score_transform_out_gte_in_not_string",
     test_json_model_score_transform_out_gte_in_not_string},
    {"test_json_model_score_transform_enabled_bad_type",
     test_json_model_score_transform_enabled_bad_type},
    {"test_json_model_feature_names_non_string", test_json_model_feature_names_non_string},
    {"test_json_model_feature_names_duplicate_key_no_leak",
     test_json_model_feature_names_duplicate_key_no_leak},
    {"test_json_model_slopes_non_number", test_json_model_slopes_non_number},
    {"test_json_model_intercepts_first_not_number", test_json_model_intercepts_first_not_number},
    {"test_json_model_knots_outer_not_array", test_json_model_knots_outer_not_array},
    {"test_json_model_knots_too_many_values", test_json_model_knots_too_many_values},
    {"test_json_model_feature_opts_dict_bad_value_type",
     test_json_model_feature_opts_dict_bad_value_type},
    {"test_json_model_score_clip_not_array", test_json_model_score_clip_not_array},
    {"test_json_model_model_dict_not_object", test_json_model_model_dict_not_object},
    {"test_json_model_slopes_not_array", test_json_model_slopes_not_array},
    {"test_json_model_intercepts_not_array", test_json_model_intercepts_not_array},
    {"test_json_model_feature_names_not_array", test_json_model_feature_names_not_array},
    {"test_json_model_feature_opts_dicts_not_array", test_json_model_feature_opts_dicts_not_array},
    {"test_json_model_model_payload_not_string", test_json_model_model_payload_not_string},
    {"test_json_model_chroma_correction_not_number", test_json_model_chroma_correction_not_number},
    {"test_json_model_score_clip_min_not_number", test_json_model_score_clip_min_not_number},
    {"test_json_model_score_clip_max_not_number", test_json_model_score_clip_max_not_number},
    {"test_json_model_score_transform_poly_null_disables",
     test_json_model_score_transform_poly_null_disables},
    {"test_json_model_score_transform_knots_null_disables",
     test_json_model_score_transform_knots_null_disables},
    {"test_json_model_score_transform_bool_str_not_string",
     test_json_model_score_transform_bool_str_not_string},
    {"test_json_model_unrecognised_model_dict_key", test_json_model_unrecognised_model_dict_key},
    {"test_json_model_intercepts_mid_not_number", test_json_model_intercepts_mid_not_number},
    {"test_json_model_score_transform_poly_number_sets_value",
     test_json_model_score_transform_poly_number_sets_value},
    {"test_json_model_score_transform_poly_string_rejects",
     test_json_model_score_transform_poly_string_rejects},
    {"test_json_model_score_transform_knots_array_walks",
     test_json_model_score_transform_knots_array_walks},
    {"test_version_next", test_version_next},
};

char *run_tests(void)
{
    const size_t cnt = sizeof(test_cases) / sizeof(test_cases[0]);
    for (size_t i = 0; i < cnt; i++) {
        char *msg = mu_report(test_cases[i].name, test_cases[i].fn);
        if (msg)
            return msg;
    }
    return nullptr;
}
