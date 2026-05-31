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

#include "libvmaf/model.h"
#include "log.h"
#include "model.h"
#include "pdjson.h"
#include "read_json_model.h"
#include "svm.h"
#include "thread_locale.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MODEL_FEATURE_INITIAL_CAP 8u
#define MODEL_KNOT_INITIAL_CAP 4u

static int grow_count(unsigned current, unsigned needed, unsigned *out)
{
    unsigned next = current ? current : 1u;
    while (next < needed) {
        if (next > (UINT_MAX / 2u))
            return -EOVERFLOW;
        next *= 2u;
    }
    *out = next;
    return 0;
}

static int ensure_feature_capacity(VmafModel *model, unsigned needed)
{
    if (needed <= model->feature_cap)
        return 0;

    unsigned next = 0;
    const unsigned current = model->feature_cap ? model->feature_cap : MODEL_FEATURE_INITIAL_CAP;
    int err = grow_count(current, needed, &next);
    if (err)
        return err;

    VmafModelFeature *feature = realloc(model->feature, sizeof(*feature) * next);
    if (!feature)
        return -ENOMEM;

    const size_t tail_sz = sizeof(*feature) * (next - model->feature_cap);
    memset(&feature[model->feature_cap], 0, tail_sz);
    model->feature = feature;
    model->feature_cap = next;
    return 0;
}

static int ensure_knot_capacity(VmafModel *model, unsigned needed)
{
    if (needed <= model->score_transform.knots.cap)
        return 0;

    unsigned next = 0;
    const unsigned current = model->score_transform.knots.cap ? model->score_transform.knots.cap :
                                                                MODEL_KNOT_INITIAL_CAP;
    int err = grow_count(current, needed, &next);
    if (err)
        return err;

    VmafPoint *list = realloc(model->score_transform.knots.list, sizeof(*list) * next);
    if (!list)
        return -ENOMEM;

    const size_t tail_sz = sizeof(*list) * (next - model->score_transform.knots.cap);
    memset(&list[model->score_transform.knots.cap], 0, tail_sz);
    model->score_transform.knots.list = list;
    model->score_transform.knots.cap = next;
    return 0;
}

static int parse_feature_opts_entry(json_stream *s, VmafModel *model, unsigned i, char *key)
{
    if (json_peek(s) == JSON_NUMBER) {
        const char *val = json_get_string(s, NULL);
        const uint64_t flags = VMAF_DICT_DO_NOT_OVERWRITE | VMAF_DICT_NORMALIZE_NUMERICAL_VALUES;
        return vmaf_dictionary_set(&(model->feature[i].opts_dict), key, val, flags);
    }
    if (json_peek(s) == JSON_TRUE || json_peek(s) == JSON_FALSE) {
        const uint64_t flags = VMAF_DICT_DO_NOT_OVERWRITE;
        const char *val = (json_peek(s) == JSON_TRUE) ? "true" : "false";
        return vmaf_dictionary_set(&(model->feature[i].opts_dict), key, val, flags);
    }
    if (json_peek(s) == JSON_STRING) {
        const char *val = json_get_string(s, NULL);
        const uint64_t flags = VMAF_DICT_DO_NOT_OVERWRITE;
        return vmaf_dictionary_set(&(model->feature[i].opts_dict), key, val, flags);
    }
    /* Unknown JSON value type for a feature option key — not string, number,
     * or boolean.  Log before returning so operators can diagnose which model
     * file triggered the error. */
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "read_json_model: unsupported JSON value type for feature option "
             "key \"%s\" (expected string, number, or boolean)\n",
             key);
    return -EINVAL;
}

static int parse_feature_opts_object(json_stream *s, VmafModel *model, unsigned i)
{
    while (json_peek(s) != JSON_OBJECT_END && !json_get_error(s)) {
        if (json_next(s) != JSON_STRING)
            return -EINVAL;
        char *key = strdup(json_get_string(s, NULL));
        if (!key)
            return -ENOMEM;
        int err = parse_feature_opts_entry(s, model, i, key);
        free(key);
        if (err)
            return err;
        json_skip(s);
    }
    return 0;
}

static int parse_feature_opts_dicts(json_stream *s, VmafModel *model)
{
    unsigned i = 0;
    while (json_peek(s) != JSON_ARRAY_END && !json_get_error(s)) {
        if (json_next(s) != JSON_OBJECT)
            return -EINVAL;
        int err = ensure_feature_capacity(model, i + 1u);
        if (err)
            return err;

        err = parse_feature_opts_object(s, model, i);
        if (err)
            return err;
        i++;
        json_skip_until(s, JSON_OBJECT_END);
    }
    json_skip_until(s, JSON_ARRAY_END);

    return 0;
}

static int parse_intercepts(json_stream *s, VmafModel *model)
{
    if (json_next(s) != JSON_NUMBER)
        return -EINVAL;
    model->intercept = json_get_number(s);

    unsigned i = 0;
    while (json_peek(s) != JSON_ARRAY_END && !json_get_error(s)) {
        if (json_next(s) != JSON_NUMBER)
            return -EINVAL;
        int err = ensure_feature_capacity(model, i + 1u);
        if (err)
            return err;
        model->feature[i++].intercept = json_get_number(s);
    }

    return 0;
}

static int parse_slopes(json_stream *s, VmafModel *model)
{
    if (json_next(s) != JSON_NUMBER)
        return -EINVAL;
    model->slope = json_get_number(s);

    unsigned i = 0;
    while (json_peek(s) != JSON_ARRAY_END && !json_get_error(s)) {
        if (json_next(s) != JSON_NUMBER)
            return -EINVAL;
        int err = ensure_feature_capacity(model, i + 1u);
        if (err)
            return err;
        model->feature[i++].slope = json_get_number(s);
    }

    return 0;
}

static int parse_knots_list(struct json_stream *s, struct VmafModel *model, unsigned idx)
{
    unsigned i = 0;
    while (json_peek(s) != JSON_ARRAY_END && !json_get_error(s)) {
        if (json_next(s) != JSON_NUMBER)
            return -EINVAL;
        if (i >= 2)
            return -EINVAL;
        if (i == 0) {
            model->score_transform.knots.list[idx].x = json_get_number(s);
        } else {
            model->score_transform.knots.list[idx].y = json_get_number(s);
        }
        i++;
    }

    return 0;
}

static int parse_knots(json_stream *s, struct VmafModel *model)
{
    unsigned i = 0;
    while (json_peek(s) != JSON_ARRAY_END && !json_get_error(s)) {
        if (json_next(s) != JSON_ARRAY)
            return -EINVAL;
        int err = ensure_knot_capacity(model, i + 1u);
        if (err)
            return err;
        err = parse_knots_list(s, model, i);
        if (err)
            return err;
        json_skip_until(s, JSON_ARRAY_END);
        i++;
    }
    model->score_transform.knots.n_knots = i;
    model->score_transform.knots.enabled = true;
    return 0;
}

static int append_feature_name(VmafModel *model, const char *name, unsigned index)
{
    int err = ensure_feature_capacity(model, index + 1u);
    if (err)
        return err;
    model->feature[index].name = strdup(name);
    if (!model->feature[index].name)
        return -ENOMEM;
    return 0;
}

static int parse_feature_names(json_stream *s, VmafModel *model)
{
    int err = 0;

    unsigned i = 0;
    while (json_peek(s) != JSON_ARRAY_END && !json_get_error(s)) {
        if (json_next(s) != JSON_STRING)
            return -EINVAL;
        const char *name = json_get_string(s, NULL);
        err = append_feature_name(model, name, i++);
        if (err)
            return err;
        model->n_features++;
    }

    json_skip_until(s, JSON_ARRAY_END);
    return 0;
}

static int parse_score_transform_poly(json_stream *s, bool *enabled, double *value)
{
    if (json_peek(s) == JSON_NULL) {
        *enabled = false;
        return 0;
    }
    if (json_next(s) == JSON_NUMBER) {
        *enabled = true;
        *value = json_get_number(s);
        return 0;
    }
    return -EINVAL;
}

static int parse_score_transform_knots_key(json_stream *s, VmafModel *model)
{
    if (json_peek(s) == JSON_NULL) {
        model->score_transform.knots.enabled = false;
        model->score_transform.knots.n_knots = 0;
        return 0;
    }
    if (json_next(s) == JSON_ARRAY) {
        int err = parse_knots(s, model);
        if (err)
            return err;
        json_skip_until(s, JSON_ARRAY_END);
        return 0;
    }
    return -EINVAL;
}

static int parse_score_transform_bool_str(json_stream *s, bool *out)
{
    if (json_next(s) != JSON_STRING)
        return -EINVAL;
    const char *val = json_get_string(s, NULL);
    if (!strcmp(val, "true"))
        *out = true;
    return 0;
}

static int parse_score_transform_entry(json_stream *s, VmafModel *model, const char *key)
{
    if (!strcmp(key, "enabled")) {
        if (json_peek(s) != JSON_TRUE && json_peek(s) != JSON_FALSE)
            return -EINVAL;
        model->score_transform.enabled = (json_next(s) == JSON_TRUE);
        return 0;
    }
    if (!strcmp(key, "p0")) {
        return parse_score_transform_poly(s, &model->score_transform.p0.enabled,
                                          &model->score_transform.p0.value);
    }
    if (!strcmp(key, "p1")) {
        return parse_score_transform_poly(s, &model->score_transform.p1.enabled,
                                          &model->score_transform.p1.value);
    }
    if (!strcmp(key, "p2")) {
        return parse_score_transform_poly(s, &model->score_transform.p2.enabled,
                                          &model->score_transform.p2.value);
    }
    if (!strcmp(key, "knots"))
        return parse_score_transform_knots_key(s, model);
    if (!strcmp(key, "out_lte_in"))
        return parse_score_transform_bool_str(s, &model->score_transform.out_lte_in);
    if (!strcmp(key, "out_gte_in"))
        return parse_score_transform_bool_str(s, &model->score_transform.out_gte_in);

    json_skip(s);
    return 0;
}

static int parse_score_transform(json_stream *s, VmafModel *model)
{
    model->score_transform.enabled = false;
    while (json_peek(s) != JSON_OBJECT_END && !json_get_error(s)) {
        if (json_next(s) != JSON_STRING)
            return -EINVAL;

        const char *key = json_get_string(s, NULL);
        int err = parse_score_transform_entry(s, model, key);
        if (err)
            return err;
    }

    return 0;
}

static int parse_libsvm_model(json_stream *s, VmafModel *model)
{
    size_t sz;
    const char *libsvm_model = json_get_string(s, &sz);
    model->svm = svm_parse_model_from_buffer(libsvm_model, sz);
    if (!model->svm)
        return -ENOMEM;

    return 0;
}

static int parse_model_dict_score_transform(json_stream *s, VmafModel *model,
                                            enum VmafModelFlags flags)
{
    if (json_next(s) != JSON_OBJECT)
        return -EINVAL;

    int err = parse_score_transform(s, model);
    if (err)
        return err;

    if (!model->score_transform.enabled && (flags & VMAF_MODEL_FLAG_ENABLE_TRANSFORM)) {
        model->score_transform.enabled = true;
    }
    json_skip_until(s, JSON_OBJECT_END);
    return 0;
}

static int parse_model_dict_model_type(json_stream *s, VmafModel *model)
{
    if (json_next(s) != JSON_STRING)
        return -EINVAL;
    const char *model_type = json_get_string(s, NULL);
    if (!strcmp(model_type, "RESIDUEBOOTSTRAP_LIBSVMNUSVR")) {
        model->type = VMAF_MODEL_RESIDUE_BOOTSTRAP_SVM_NUSVR;
    } else if (!strcmp(model_type, "BOOTSTRAP_LIBSVMNUSVR")) {
        model->type = VMAF_MODEL_BOOTSTRAP_SVM_NUSVR;
    } else if (!strcmp(model_type, "LIBSVMNUSVR")) {
        model->type = VMAF_MODEL_TYPE_SVM_NUSVR;
    } else {
        return -EINVAL;
    }
    return 0;
}

static int parse_model_dict_norm_type(json_stream *s, VmafModel *model)
{
    if (json_next(s) != JSON_STRING)
        return -EINVAL;
    const char *norm_type = json_get_string(s, NULL);
    if (!strcmp(norm_type, "linear_rescale")) {
        model->norm_type = VMAF_MODEL_NORMALIZATION_TYPE_LINEAR_RESCALE;
    } else if (!strcmp(norm_type, "none")) {
        model->norm_type = VMAF_MODEL_NORMALIZATION_TYPE_NONE;
    } else {
        return -EINVAL;
    }
    return 0;
}

static int parse_model_dict_score_clip(json_stream *s, VmafModel *model, enum VmafModelFlags flags)
{
    if (json_next(s) != JSON_ARRAY)
        return -EINVAL;
    if (!(flags & VMAF_MODEL_FLAG_DISABLE_CLIP)) {
        model->score_clip.enabled = true;
        if (json_next(s) != JSON_NUMBER)
            return -EINVAL;
        model->score_clip.min = json_get_number(s);
        if (json_next(s) != JSON_NUMBER)
            return -EINVAL;
        model->score_clip.max = json_get_number(s);
    }
    json_skip_until(s, JSON_ARRAY_END);
    return 0;
}

static int parse_model_dict_chroma_correction(json_stream *s, VmafModel *model)
{
    if (json_next(s) != JSON_NUMBER)
        return -EINVAL;
    model->chroma_from_luma.enabled = true;
    model->chroma_from_luma.chroma_correction_parameter = json_get_number(s);
    return 0;
}

static int parse_model_dict_array_key(json_stream *s, VmafModel *model, const char *key)
{
    if (!strcmp(key, "slopes")) {
        if (json_next(s) != JSON_ARRAY)
            return -EINVAL;
        int err = parse_slopes(s, model);
        if (err)
            return err;
        json_skip_until(s, JSON_ARRAY_END);
        return 0;
    }
    if (!strcmp(key, "intercepts")) {
        if (json_next(s) != JSON_ARRAY)
            return -EINVAL;
        int err = parse_intercepts(s, model);
        if (err)
            return err;
        json_skip_until(s, JSON_ARRAY_END);
        return 0;
    }
    if (!strcmp(key, "feature_names")) {
        if (json_next(s) != JSON_ARRAY)
            return -EINVAL;
        return parse_feature_names(s, model);
    }
    if (!strcmp(key, "feature_opts_dicts")) {
        if (json_next(s) != JSON_ARRAY)
            return -EINVAL;
        return parse_feature_opts_dicts(s, model);
    }
    if (!strcmp(key, "model")) {
        if (json_next(s) != JSON_STRING)
            return -EINVAL;
        return parse_libsvm_model(s, model);
    }
    /* Unrecognised key: caller skips. */
    return 1;
}

static int parse_model_dict_entry(json_stream *s, VmafModel *model, enum VmafModelFlags flags,
                                  const char *key)
{
    if (!strcmp(key, "score_transform"))
        return parse_model_dict_score_transform(s, model, flags);
    if (!strcmp(key, "model_type"))
        return parse_model_dict_model_type(s, model);
    if (!strcmp(key, "norm_type"))
        return parse_model_dict_norm_type(s, model);
    if (!strcmp(key, "score_clip"))
        return parse_model_dict_score_clip(s, model, flags);
    if (!strcmp(key, "chroma_correction_parameter"))
        return parse_model_dict_chroma_correction(s, model);

    int r = parse_model_dict_array_key(s, model, key);
    if (r <= 0)
        return r;

    json_skip(s);
    return 0;
}

static int parse_model_dict(json_stream *s, VmafModel *model, enum VmafModelFlags flags)
{
    if (json_next(s) != JSON_OBJECT)
        return -EINVAL;

    while (json_peek(s) != JSON_OBJECT_END && !json_get_error(s)) {
        if (json_next(s) != JSON_STRING)
            return -EINVAL;
        const char *key = json_get_string(s, NULL);
        int err = parse_model_dict_entry(s, model, flags, key);
        if (err)
            return err;
    }

    json_skip_until(s, JSON_OBJECT_END);
    return 0;
}

static int model_parse(json_stream *s, VmafModel *model, enum VmafModelFlags flags)
{
    int err = -EINVAL;

    if (json_next(s) != JSON_OBJECT)
        return -EINVAL;

    while (json_peek(s) != JSON_OBJECT_END && !json_get_error(s)) {
        if (json_next(s) != JSON_STRING)
            return -EINVAL;
        const char *key = json_get_string(s, NULL);

        if (!strcmp(key, "model_dict")) {
            err = parse_model_dict(s, model, flags);
            if (err)
                return err;
            continue;
        }

        json_skip(s);
    }

    json_skip_until(s, JSON_OBJECT_END);
    return err;
}

static int vmaf_read_json_model(VmafModel **model, VmafModelConfig *cfg, json_stream *s)
{
    int err = -EINVAL;
    VmafModel *const m = *model = malloc(sizeof(*m));
    if (!m)
        return -ENOMEM;
    memset(m, 0, sizeof(*m));

    m->name = vmaf_model_generate_name(cfg);
    if (!m->name) {
        err = -ENOMEM;
        goto fail;
    }

    VmafThreadLocaleState *locale_state = vmaf_thread_locale_push_c();

    err = model_parse(s, m, cfg->flags);

    vmaf_thread_locale_pop(locale_state);

    if (err)
        goto fail;

    return 0;

fail:
    /* Leak-free teardown on parse failure. `vmaf_model_destroy`
     * walks the partially-populated feature[] array (including any
     * dict + strdup'd feature_name allocations from model_parse) +
     * frees knots.list + name + the struct itself. Reset *model to
     * NULL so naive callers (`if (m) vmaf_model_destroy(m)`) don't
     * double-free. */
    vmaf_model_destroy(m);
    *model = NULL;
    return err;
}

int vmaf_read_json_model_from_buffer(VmafModel **model, VmafModelConfig *cfg, const char *data,
                                     const int data_len)
{
    int err = 0;
    json_stream s;
    json_open_buffer(&s, data, data_len);
    err = vmaf_read_json_model(model, cfg, &s);
    json_close(&s);
    return err;
}

int vmaf_read_json_model_from_path(VmafModel **model, VmafModelConfig *cfg, const char *path)
{
    int err = 0;
    FILE *in = fopen(path, "r");
    if (!in)
        return -EINVAL;
    json_stream s;
    json_open_stream(&s, in);
    err = vmaf_read_json_model(model, cfg, &s);
    json_close(&s);
    if (fclose(in) != 0 && err == 0)
        err = -EIO;
    return err;
}

static int model_collection_read_one(json_stream *s, VmafModel **model,
                                     VmafModelCollection **model_collection, VmafModelConfig *c,
                                     unsigned i)
{
    /* When i==0, ownership of m is transferred to *model below; when
     * i>=1, ownership is transferred to *model_collection via append.
     * The analyzer loses track of this cross-parameter ownership. */
    // NOLINTBEGIN(clang-analyzer-unix.Malloc)
    VmafModel *m;
    int err = vmaf_read_json_model(&m, c, s);
    if (err)
        return err;

    if (i == 0) {
        *model = m;
    } else {
        err = vmaf_model_collection_append(model_collection, m);
        if (err) {
            vmaf_model_destroy(m);
            return err;
        }
    }
    // NOLINTEND(clang-analyzer-unix.Malloc)
    return 0;
}

static int model_collection_parse_loop(json_stream *s, VmafModel **model,
                                       VmafModelCollection **model_collection, VmafModelConfig *c,
                                       const char *name, char *cfg_name, size_t cfg_name_sz)
{
    /* `generated_key_sz` = 4 + 1 = 5 is a true compile-time constant, but
     * `const size_t …` is not a constant-expression in C, so declaring a
     * plain fixed-size array (not a VLA) avoids MSVC C2057. Covers up to
     * four decimal digits of `i` (9999 + NUL). */
    char generated_key[5];
    unsigned i = 0;
    int err = -EINVAL;

    while (json_peek(s) != JSON_OBJECT_END && !json_get_error(s)) {
        if (json_next(s) != JSON_STRING)
            return -EINVAL;

        const char *key = json_get_string(s, NULL);
        (void)snprintf(generated_key, sizeof(generated_key), "%d", i);

        if (strcmp(key, generated_key) != 0) {
            json_skip(s);
            continue;
        }

        err = model_collection_read_one(s, model, model_collection, c, i);
        if (err)
            return err;

        if (i == 0)
            c->name = cfg_name;
        (void)snprintf(cfg_name, cfg_name_sz, "%s_%04d", name, ++i);
    }

    if (!(*model_collection))
        err = -EINVAL;
    return err;
}

static int model_collection_parse(json_stream *s, VmafModel **model,
                                  VmafModelCollection **model_collection, VmafModelConfig *cfg)
{
    *model_collection = NULL;

    if (json_next(s) != JSON_OBJECT)
        return -EINVAL;

    VmafModelConfig c = *cfg;
    const char *name = c.name = vmaf_model_generate_name(cfg);
    if (!c.name)
        return -ENOMEM;

    const size_t cfg_name_sz = strlen(name) + 5 + 1;
    /* Heap-allocated for MSVC portability (no VLAs). `cfg_name` survives
     * across the while-loop iterations because `c.name` points into it
     * after the first successful sub-model read. */
    char *cfg_name = (char *)malloc(cfg_name_sz);
    if (!cfg_name) {
        free((char *)name);
        return -ENOMEM;
    }

    int err =
        model_collection_parse_loop(s, model, model_collection, &c, name, cfg_name, cfg_name_sz);

    free(cfg_name);
    free((char *)name);
    return err;
}

int vmaf_read_json_model_collection_from_path(VmafModel **model,
                                              VmafModelCollection **model_collection,
                                              VmafModelConfig *cfg, const char *path)
{
    int err = 0;
    FILE *in = fopen(path, "r");
    if (!in)
        return -EINVAL;
    json_stream s;
    json_open_stream(&s, in);
    err = model_collection_parse(&s, model, model_collection, cfg);
    json_close(&s);
    if (fclose(in) != 0 && err == 0)
        err = -EIO;
    return err;
}

int vmaf_read_json_model_collection_from_buffer(VmafModel **model,
                                                VmafModelCollection **model_collection,
                                                VmafModelConfig *cfg, const char *data,
                                                const int data_len)
{
    int err = 0;
    json_stream s;
    json_open_buffer(&s, data, data_len);
    err = model_collection_parse(&s, model, model_collection, cfg);
    json_close(&s);
    return err;
}
