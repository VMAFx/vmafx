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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <libvmaf/model.h>

#include "config.h"
#include "feature/feature_extractor.h"
#include "log.h"
#include "model.h"
#include "read_json_model.h"
#include "svm.h"

typedef struct VmafBuiltInModel {
    const char *version;
    const char *data;
    const int *data_len;
} VmafBuiltInModel;

#if VMAF_BUILT_IN_MODELS
#if VMAF_FLOAT_FEATURES
extern const char src_vmaf_float_v0_6_1neg_json[];
extern const int src_vmaf_float_v0_6_1neg_json_len;
extern const char src_vmaf_float_v0_6_1_json[];
extern const int src_vmaf_float_v0_6_1_json_len;
extern const char src_vmaf_float_b_v0_6_3_json[];
extern const int src_vmaf_float_b_v0_6_3_json_len;
extern const char src_vmaf_float_4k_v0_6_1_json[];
extern const int src_vmaf_float_4k_v0_6_1_json_len;
#endif
extern const char src_vmaf_v0_6_1_json[];
extern const int src_vmaf_v0_6_1_json_len;
extern const char src_vmaf_b_v0_6_3_json[];
extern const int src_vmaf_b_v0_6_3_json_len;
extern const char src_vmaf_v0_6_1neg_json[];
extern const int src_vmaf_v0_6_1neg_json_len;
extern const char src_vmaf_4k_v0_6_1_json[];
extern const int src_vmaf_4k_v0_6_1_json_len;
extern const char src_vmaf_4k_v0_6_1neg_json[];
extern const int src_vmaf_4k_v0_6_1neg_json_len;
/* VMAF v1.0.16 SDR models (ported from Netflix upstream 4718b4f5f). */
extern const char src_vmaf_v1_0_16_3d0h_json[];
extern const int src_vmaf_v1_0_16_3d0h_json_len;
extern const char src_vmaf_v1_0_16_3d0h_2160_json[];
extern const int src_vmaf_v1_0_16_3d0h_2160_json_len;
extern const char src_vmaf_v1_0_16_5d0h_json[];
extern const int src_vmaf_v1_0_16_5d0h_json_len;
extern const char src_vmaf_v1_0_16_1d5h_2160_json[];
extern const int src_vmaf_v1_0_16_1d5h_2160_json_len;
extern const char src_vmaf_v1_0_16_hfr_3d0h_json[];
extern const int src_vmaf_v1_0_16_hfr_3d0h_json_len;
extern const char src_vmaf_v1_0_16_hfr_3d0h_2160_json[];
extern const int src_vmaf_v1_0_16_hfr_3d0h_2160_json_len;
extern const char src_vmaf_v1_0_16_hfr_5d0h_json[];
extern const int src_vmaf_v1_0_16_hfr_5d0h_json_len;
extern const char src_vmaf_v1_0_16_hfr_1d5h_2160_json[];
extern const int src_vmaf_v1_0_16_hfr_1d5h_2160_json_len;
#endif

static const VmafBuiltInModel built_in_models[] = {
#if VMAF_BUILT_IN_MODELS
#if VMAF_FLOAT_FEATURES
    {
        .version = "vmaf_float_v0.6.1",
        .data = src_vmaf_float_v0_6_1_json,
        .data_len = &src_vmaf_float_v0_6_1_json_len,
    },
    {
        .version = "vmaf_float_b_v0.6.3",
        .data = src_vmaf_float_b_v0_6_3_json,
        .data_len = &src_vmaf_float_b_v0_6_3_json_len,
    },
    {
        .version = "vmaf_float_v0.6.1neg",
        .data = src_vmaf_float_v0_6_1neg_json,
        .data_len = &src_vmaf_float_v0_6_1neg_json_len,
    },
    {
        .version = "vmaf_float_4k_v0.6.1",
        .data = src_vmaf_float_4k_v0_6_1_json,
        .data_len = &src_vmaf_float_4k_v0_6_1_json_len,
    },
#endif
    {
        .version = "vmaf_v0.6.1",
        .data = src_vmaf_v0_6_1_json,
        .data_len = &src_vmaf_v0_6_1_json_len,
    },
    {
        .version = "vmaf_b_v0.6.3",
        .data = src_vmaf_b_v0_6_3_json,
        .data_len = &src_vmaf_b_v0_6_3_json_len,
    },
    {
        .version = "vmaf_v0.6.1neg",
        .data = src_vmaf_v0_6_1neg_json,
        .data_len = &src_vmaf_v0_6_1neg_json_len,
    },
    {
        .version = "vmaf_4k_v0.6.1",
        .data = src_vmaf_4k_v0_6_1_json,
        .data_len = &src_vmaf_4k_v0_6_1_json_len,
    },
    {
        .version = "vmaf_4k_v0.6.1neg",
        .data = src_vmaf_4k_v0_6_1neg_json,
        .data_len = &src_vmaf_4k_v0_6_1neg_json_len,
    },
    /* VMAF v1.0.16 SDR models (ported from Netflix upstream 4718b4f5f). */
    {
        .version = "vmaf_v1.0.16_3d0h",
        .data = src_vmaf_v1_0_16_3d0h_json,
        .data_len = &src_vmaf_v1_0_16_3d0h_json_len,
    },
    {
        .version = "vmaf_v1.0.16_3d0h_2160",
        .data = src_vmaf_v1_0_16_3d0h_2160_json,
        .data_len = &src_vmaf_v1_0_16_3d0h_2160_json_len,
    },
    {
        .version = "vmaf_v1.0.16_5d0h",
        .data = src_vmaf_v1_0_16_5d0h_json,
        .data_len = &src_vmaf_v1_0_16_5d0h_json_len,
    },
    {
        .version = "vmaf_v1.0.16_1d5h_2160",
        .data = src_vmaf_v1_0_16_1d5h_2160_json,
        .data_len = &src_vmaf_v1_0_16_1d5h_2160_json_len,
    },
    {
        .version = "vmaf_v1.0.16_hfr_3d0h",
        .data = src_vmaf_v1_0_16_hfr_3d0h_json,
        .data_len = &src_vmaf_v1_0_16_hfr_3d0h_json_len,
    },
    {
        .version = "vmaf_v1.0.16_hfr_3d0h_2160",
        .data = src_vmaf_v1_0_16_hfr_3d0h_2160_json,
        .data_len = &src_vmaf_v1_0_16_hfr_3d0h_2160_json_len,
    },
    {
        .version = "vmaf_v1.0.16_hfr_5d0h",
        .data = src_vmaf_v1_0_16_hfr_5d0h_json,
        .data_len = &src_vmaf_v1_0_16_hfr_5d0h_json_len,
    },
    {
        .version = "vmaf_v1.0.16_hfr_1d5h_2160",
        .data = src_vmaf_v1_0_16_hfr_1d5h_2160_json,
        .data_len = &src_vmaf_v1_0_16_hfr_1d5h_2160_json_len,
    },
#endif
    {0}};

#define BUILT_IN_MODEL_CNT (((sizeof(built_in_models)) / (sizeof(built_in_models[0]))) - 1)

int vmaf_model_load(VmafModel **model, VmafModelConfig *cfg, const char *version)
{
    /* `version` reaches strcmp unprotected; a NULL caller would dereference
     * the second operand.  Reject up-front instead of crashing.  Adversarial
     * audit 2026-05-31, fix/core-lifecycle-memory-audit. */
    if (!version)
        return -EINVAL;

    const VmafBuiltInModel *built_in_model = NULL;

    for (unsigned i = 0; i < BUILT_IN_MODEL_CNT; i++) {
        if (!strcmp(version, built_in_models[i].version)) {
            built_in_model = &built_in_models[i];
            break;
        }
    }

    if (!built_in_model) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING, "no such built-in model: \"%s\"\n", version);
        return -EINVAL;
    }

    return vmaf_read_json_model_from_buffer(model, cfg, built_in_model->data,
                                            *built_in_model->data_len);
}

char *vmaf_model_generate_name(VmafModelConfig *cfg)
{
    const char *default_name = "vmaf";
    const size_t name_sz = cfg->name ? strlen(cfg->name) + 1 : strlen(default_name) + 1;

    char *name = malloc(name_sz);
    if (!name)
        return NULL;

    const char *src = cfg->name ? cfg->name : default_name;
    memcpy(name, src, name_sz);

    return name;
}

int vmaf_model_load_from_path(VmafModel **model, VmafModelConfig *cfg, const char *path)
{
    int err = vmaf_read_json_model_from_path(model, cfg, path);
    if (err) {
        /* Demote to WARNING: the CLI falls back to vmaf_model_collection_load_from_path
         * when this call fails, so a bootstrap/collection JSON (e.g. vmaf_b_v0.6.3.json)
         * is not an error — it simply has a different top-level structure. The caller
         * emits a fatal error if the collection fallback also fails. A .pkl-specific
         * follow-up stays at ERROR because pkl is permanently unsupported. */
        vmaf_log(VMAF_LOG_LEVEL_WARNING, "could not read model from path: \"%s\"\n", path);
        const char *ext = strrchr(path, '.');
        if (ext && !strcmp(ext, ".pkl")) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "support for pkl model files has been removed, use json\n");
        }
    }
    return err;
}

int vmaf_model_feature_overload(VmafModel *model, const char *feature_name,
                                VmafFeatureDictionary *opts_dict)
{
    if (!model)
        return -EINVAL;
    if (!feature_name)
        return -EINVAL;
    if (!opts_dict)
        return -EINVAL;

    int err = 0;

    for (unsigned i = 0; i < model->n_features; i++) {
        VmafFeatureExtractor *fex =
            vmaf_get_feature_extractor_by_feature_name(model->feature[i].name, 0);
        if (!fex)
            continue;
        if (strcmp(feature_name, fex->name) != 0)
            continue;
        VmafDictionary *d = vmaf_dictionary_merge((VmafDictionary **)&model->feature[i].opts_dict,
                                                  (VmafDictionary **)&opts_dict, 0);
        if (!d)
            return -ENOMEM;
        err = vmaf_dictionary_free(&model->feature[i].opts_dict);
        if (err)
            goto exit;
        model->feature[i].opts_dict = d;
    }

exit:
    err |= vmaf_dictionary_free((VmafDictionary **)&opts_dict);
    return err;
}

void vmaf_model_destroy(VmafModel *model)
{
    if (!model)
        return;
    free(model->path);
    free(model->name);
    svm_free_and_destroy_model(&(model->svm));
    /* Walk the full feature_cap, not min(feature_cap, n_features).
     *
     * feature_cap IS the allocated element count, so this cannot read past the
     * buffer — it preserves the overflow safety the previous min() was written
     * for, while also freeing slots n_features does not cover.
     *
     * That gap was a real leak. n_features is only incremented by
     * parse_feature_names, but ensure_feature_capacity() is also called by
     * parse_feature_opts_dicts / parse_slopes / parse_intercepts, and
     * parse_feature_opts_dicts stores an owned VmafDictionary in the slot. A
     * model carrying `feature_opts_dicts` with no (or fewer) `feature_names`
     * therefore left dictionaries above n_features that nothing could free —
     * a 16-byte-per-entry leak found by the fuzz_json_model LeakSanitizer lane.
     * Inflating n_features instead would be wrong: it is the semantic count of
     * model features and feeds prediction, not a memory-management counter.
     *
     * Walking the tail is safe because ensure_feature_capacity() memsets every
     * newly grown slot to zero, so an untouched slot holds NULL and both
     * free(NULL) and vmaf_dictionary_free(&NULL) are no-ops. */
    for (unsigned i = 0; i < model->feature_cap; i++) {
        free(model->feature[i].name);
        vmaf_dictionary_free(&model->feature[i].opts_dict);
    }
    free(model->feature);
    free(model->score_transform.knots.list);
    free(model->predict_nodes);
    if (model->predict_feature_names) {
        for (unsigned i = 0; i < model->n_features; i++) {
            free(model->predict_feature_names[i]);
        }
        free((void *)model->predict_feature_names);
    }
    free((void *)model->predict_feature_vectors);
    /* Round-5 race fix (finding #3): destroy the predict-cache mutex that was
     * initialized in vmaf_read_json_model(). */
    pthread_mutex_destroy(&model->predict_cache_lock);
    free(model);
}

int vmaf_model_collection_append(VmafModelCollection **model_collection, VmafModel *model)
{
    if (!model_collection)
        return -EINVAL;
    if (!model)
        return -EINVAL;

    VmafModelCollection *mc = *model_collection;

    if (!mc) {
        mc = *model_collection = malloc(sizeof(*mc));
        if (!mc)
            goto fail;
        memset(mc, 0, sizeof(*mc));
        const size_t initial_sz = 8 * sizeof(*mc->model);
        mc->model = (VmafModel **)malloc(initial_sz);
        if (!mc->model)
            goto fail_mc;
        memset((void *)mc->model, 0, initial_sz);
        mc->size = 8;
        mc->type = model->type;
        /* Guard against size_t underflow when name is shorter than the
         * ".json" suffix we strip (5 chars).  An empty or very short
         * model name is a caller error; reject it cleanly.  Use fail_model
         * (not fail_mc): mc->model was already allocated above and must be
         * freed before mc itself. */
        if (strlen(model->name) < 5)
            goto fail_model;
        const size_t name_sz = strlen(model->name) - 5 + 1;
        mc->name = malloc(name_sz);
        if (!mc->name)
            goto fail_model;
        memset((char *)mc->name, 0, name_sz);
        strncpy((char *)mc->name, model->name, name_sz - 1);
    }

    if (mc->type != model->type)
        return -EINVAL;

    if (mc->cnt == mc->size) {
        const size_t sz = mc->size * sizeof(*mc->model) * 2;
        VmafModel **m = (VmafModel **)realloc((void *)mc->model, sz);
        /* Grow failure on an EXISTING collection: realloc keeps the old buffer
         * valid, so do NOT take the fail label (it would null the caller's
         * out-param and free a still-usable mc — a leak + lost handle). */
        if (!m)
            return -ENOMEM;
        mc->model = m;
        mc->size *= 2;
    }

    mc->model[mc->cnt++] = model;
    return 0;

fail_model:
    free((void *)mc->model);
fail_mc:
    free(mc);
fail:
    *model_collection = NULL;
    return -ENOMEM;
}

void vmaf_model_collection_destroy(VmafModelCollection *model_collection)
{
    if (!model_collection)
        return;
    for (unsigned i = 0; i < model_collection->cnt; i++) {
        vmaf_model_destroy(model_collection->model[i]);
    }
    free((void *)model_collection->model);
    free((char *)model_collection->name);
    free(model_collection);
}

int vmaf_model_collection_load(VmafModel **model, VmafModelCollection **model_collection,
                               VmafModelConfig *cfg, const char *version)
{
    /* Mirror vmaf_model_load NULL guard — strcmp on a NULL operand is UB. */
    if (!version)
        return -EINVAL;

    const VmafBuiltInModel *built_in_model = NULL;

    for (unsigned i = 0; i < BUILT_IN_MODEL_CNT; i++) {
        if (!strcmp(version, built_in_models[i].version)) {
            built_in_model = &built_in_models[i];
            break;
        }
    }

    if (!built_in_model) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING, "no such built-in model collection: \"%s\"\n", version);
        return -EINVAL;
    }

    return vmaf_read_json_model_collection_from_buffer(
        model, model_collection, cfg, built_in_model->data, *built_in_model->data_len);
}

int vmaf_model_collection_load_from_path(VmafModel **model, VmafModelCollection **model_collection,
                                         VmafModelConfig *cfg, const char *path)
{
    int err = vmaf_read_json_model_collection_from_path(model, model_collection, cfg, path);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not read model collection from path: \"%s\"\n", path);
        const char *ext = strrchr(path, '.');
        if (ext && !strcmp(ext, ".pkl")) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "support for pkl model files has been removed, use json\n");
        }
    }

    return err;
}

int vmaf_model_collection_feature_overload(VmafModel *model, VmafModelCollection **model_collection,
                                           const char *feature_name,
                                           VmafFeatureDictionary *opts_dict)
{
    if (!model_collection)
        return -EINVAL;
    VmafModelCollection *mc = *model_collection;

    int err = 0;
    for (unsigned i = 0; i < mc->cnt; i++) {
        VmafFeatureDictionary *d = NULL;
        if (vmaf_dictionary_copy((VmafDictionary **)&opts_dict, (VmafDictionary **)&d))
            goto exit;
        err |= vmaf_model_feature_overload(mc->model[i], feature_name, d);
    }

exit:
    err |= vmaf_model_feature_overload(model, feature_name, opts_dict);
    return err;
}

const void *vmaf_model_version_next(const void *prev, const char **version)
{
    if (BUILT_IN_MODEL_CNT == 0)
        return NULL;

    const VmafBuiltInModel *prev_model = prev;
    const VmafBuiltInModel *out_model = NULL;

    if (!prev_model) {
        out_model = &built_in_models[0];
    } else {
        const size_t idx = (size_t)(prev_model - built_in_models);
        if (idx + 1 < BUILT_IN_MODEL_CNT)
            out_model = prev_model + 1;
    }

    if (version && out_model)
        *version = out_model->version;
    return out_model;
}
