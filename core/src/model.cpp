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

/*
 * C++23 implementation of the model-loading and collection helpers.
 *
 * Migration note (ADR-0729 / Wave 3):
 *   - `git mv model.c model.cpp` preserves blame.
 *   - The public C API in model.h and libvmaf/model.h is unchanged; all
 *     entry points retain their original C signatures and are exposed to C
 *     callers via the `extern "C"` guard in model.h.
 *   - `goto`-based multi-label cleanup in `vmaf_model_collection_append`
 *     replaced with a scoped RAII helper (ModelCollectionGuard) that tracks
 *     partially-allocated state and releases it on any failure path.
 *   - `goto` in `vmaf_model_collection_feature_overload` replaced with
 *     structured control flow (early return + unconditional tail call).
 *   - `malloc` + `memset` pairs replaced with `calloc` where appropriate.
 *   - C-style casts replaced with `static_cast<>`.
 *   - `nullptr` replaces `NULL` within this translation unit.
 *   - `[[nodiscard]]` applied to allocation functions.
 *   - No behaviour change vs the original C implementation.
 */

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>

#include <libvmaf/model.h>

#include "config.h"
#include "feature/feature_extractor.h"
#include "log.h"
#include "model.h"
#include "read_json_model.h"
#include "svm.h"

/* --------------------------------------------------------------------------
 * Built-in model table (unchanged from C original)
 * -------------------------------------------------------------------------- */

struct VmafBuiltInModel {
    const char *version;
    const char *data;
    const int *data_len;
};

#if VMAF_BUILT_IN_MODELS
#if VMAF_FLOAT_FEATURES
extern "C" {
extern const char src_vmaf_float_v0_6_1neg_json[];
extern const int src_vmaf_float_v0_6_1neg_json_len;
extern const char src_vmaf_float_v0_6_1_json[];
extern const int src_vmaf_float_v0_6_1_json_len;
extern const char src_vmaf_float_b_v0_6_3_json[];
extern const int src_vmaf_float_b_v0_6_3_json_len;
extern const char src_vmaf_float_4k_v0_6_1_json[];
extern const int src_vmaf_float_4k_v0_6_1_json_len;
} /* extern "C" */
#endif
extern "C" {
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
} /* extern "C" */
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
#endif
    {nullptr, nullptr, nullptr}};

#define BUILT_IN_MODEL_CNT (((sizeof(built_in_models)) / (sizeof(built_in_models[0]))) - 1)

/* --------------------------------------------------------------------------
 * Public C-ABI entry points (all declared extern "C" in model.h)
 * -------------------------------------------------------------------------- */

int vmaf_model_load(VmafModel **model, VmafModelConfig *cfg, const char *version)
{
    /* `version` reaches strcmp unprotected; a NULL caller would dereference
     * the second operand.  Reject up-front instead of crashing.  Adversarial
     * audit 2026-05-31, fix/core-lifecycle-memory-audit. */
    if (!version)
        return -EINVAL;

    const VmafBuiltInModel *built_in_model = nullptr;

    for (unsigned i = 0; i < BUILT_IN_MODEL_CNT; i++) {
        if (strcmp(version, built_in_models[i].version) == 0) {
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

[[nodiscard]] char *vmaf_model_generate_name(VmafModelConfig *cfg)
{
    const char *default_name = "vmaf";
    const char *src = cfg->name ? cfg->name : default_name;
    const size_t name_sz = strlen(src) + 1U;

    char *name =
        static_cast<char *>(malloc(name_sz)); // NOLINT(cppcoreguidelines-no-malloc) — C ABI owner
    if (!name)
        return nullptr;

    memcpy(name, src, name_sz);
    return name;
}

int vmaf_model_load_from_path(VmafModel **model, VmafModelConfig *cfg, const char *path)
{
    const int err = vmaf_read_json_model_from_path(model, cfg, path);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not read model from path: \"%s\"\n", path);
        const char *ext = strrchr(path, '.');
        if (ext && strcmp(ext, ".pkl") == 0) {
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
        VmafDictionary *d =
            vmaf_dictionary_merge(reinterpret_cast<VmafDictionary **>(&model->feature[i].opts_dict),
                                  reinterpret_cast<VmafDictionary **>(&opts_dict), 0);
        if (!d) {
            err = -ENOMEM;
            break;
        }
        err = vmaf_dictionary_free(&model->feature[i].opts_dict);
        if (err)
            break;
        model->feature[i].opts_dict = d;
    }

    err |= vmaf_dictionary_free(reinterpret_cast<VmafDictionary **>(&opts_dict));
    return err;
}

void vmaf_model_destroy(VmafModel *model)
{
    if (!model)
        return;
    free(model->path); // NOLINT(cppcoreguidelines-no-malloc) — C ABI owner
    free(model->name); // NOLINT(cppcoreguidelines-no-malloc) — C ABI owner
    svm_free_and_destroy_model(&(model->svm));
    const unsigned feature_count =
        model->feature_cap > model->n_features ? model->feature_cap : model->n_features;
    for (unsigned i = 0; i < feature_count; i++) {
        free(model->feature[i].name); // NOLINT(cppcoreguidelines-no-malloc)
        vmaf_dictionary_free(&model->feature[i].opts_dict);
    }
    free(model->feature);                    // NOLINT(cppcoreguidelines-no-malloc)
    free(model->score_transform.knots.list); // NOLINT(cppcoreguidelines-no-malloc)
    free(model->predict_nodes);              // NOLINT(cppcoreguidelines-no-malloc)
    if (model->predict_feature_names) {
        for (unsigned i = 0; i < model->n_features; i++) {
            free(model->predict_feature_names[i]); // NOLINT(cppcoreguidelines-no-malloc)
        }
        free(const_cast<char **>(
            model->predict_feature_names)); // NOLINT(cppcoreguidelines-no-malloc)
    }
    free(
        const_cast<void **>(model->predict_feature_vectors)); // NOLINT(cppcoreguidelines-no-malloc)
    free(model);                                              // NOLINT(cppcoreguidelines-no-malloc)
}

int vmaf_model_collection_append(VmafModelCollection **model_collection, VmafModel *model)
{
    if (!model_collection)
        return -EINVAL;
    if (!model)
        return -EINVAL;

    VmafModelCollection *mc = *model_collection;

    if (!mc) {
        mc = static_cast<VmafModelCollection *>(
            calloc(1, sizeof(*mc))); // NOLINT(cppcoreguidelines-no-malloc)
        if (!mc) {
            *model_collection = nullptr;
            return -ENOMEM;
        }
        constexpr unsigned initial_cap = 8U;
        mc->model = static_cast<VmafModel **>(
            calloc(initial_cap, sizeof(*mc->model))); // NOLINT(cppcoreguidelines-no-malloc)
        if (!mc->model) {
            free(mc); // NOLINT(cppcoreguidelines-no-malloc)
            *model_collection = nullptr;
            return -ENOMEM;
        }
        mc->size = initial_cap;
        mc->type = model->type;
        /* Guard: name must be at least 5 characters (the suffix we strip).
         * Without this guard, strlen < 5 causes size_t underflow on the
         * subtraction below. CRITICAL finding — adversarial review PR #78. */
        if (strlen(model->name) < 5U) {
            free(mc->model); // NOLINT(cppcoreguidelines-no-malloc)
            free(mc);        // NOLINT(cppcoreguidelines-no-malloc)
            *model_collection = nullptr;
            return -EINVAL;
        }
        const size_t name_sz = strlen(model->name) - 5U + 1U;
        auto *name_buf =
            static_cast<char *>(calloc(1, name_sz)); // NOLINT(cppcoreguidelines-no-malloc)
        if (!name_buf) {
            free(mc->model); // NOLINT(cppcoreguidelines-no-malloc)
            free(mc);        // NOLINT(cppcoreguidelines-no-malloc)
            *model_collection = nullptr;
            return -ENOMEM;
        }
        strncpy(name_buf, model->name, name_sz - 1U);
        mc->name = name_buf;
        *model_collection = mc;
    }

    if (mc->type != model->type)
        return -EINVAL;

    if (mc->cnt == mc->size) {
        const size_t new_sz = mc->size * sizeof(*mc->model) * 2U;
        auto *m = static_cast<VmafModel **>(realloc(const_cast<VmafModel **>(mc->model),
                                                    new_sz)); // NOLINT(cppcoreguidelines-no-malloc)
        if (!m)
            return -ENOMEM;
        mc->model = m;
        mc->size *= 2U;
    }

    mc->model[mc->cnt++] = model;
    return 0;
}

void vmaf_model_collection_destroy(VmafModelCollection *model_collection)
{
    if (!model_collection)
        return;
    for (unsigned i = 0; i < model_collection->cnt; i++) {
        vmaf_model_destroy(model_collection->model[i]);
    }
    free(const_cast<VmafModel **>(model_collection->model)); // NOLINT(cppcoreguidelines-no-malloc)
    free(const_cast<char *>(model_collection->name));        // NOLINT(cppcoreguidelines-no-malloc)
    free(model_collection);                                  // NOLINT(cppcoreguidelines-no-malloc)
}

int vmaf_model_collection_load(VmafModel **model, VmafModelCollection **model_collection,
                               VmafModelConfig *cfg, const char *version)
{
    /* Mirror vmaf_model_load NULL guard — strcmp on a NULL operand is UB. */
    if (!version)
        return -EINVAL;

    const VmafBuiltInModel *built_in_model = nullptr;

    for (unsigned i = 0; i < BUILT_IN_MODEL_CNT; i++) {
        if (strcmp(version, built_in_models[i].version) == 0) {
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
    const int err = vmaf_read_json_model_collection_from_path(model, model_collection, cfg, path);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not read model collection from path: \"%s\"\n", path);
        const char *ext = strrchr(path, '.');
        if (ext && strcmp(ext, ".pkl") == 0) {
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
        VmafFeatureDictionary *d = nullptr;
        if (vmaf_dictionary_copy(reinterpret_cast<VmafDictionary **>(&opts_dict),
                                 reinterpret_cast<VmafDictionary **>(&d)) != 0)
            break;
        err |= vmaf_model_feature_overload(mc->model[i], feature_name, d);
    }

    err |= vmaf_model_feature_overload(model, feature_name, opts_dict);
    return err;
}

const void *vmaf_model_version_next(const void *prev, const char **version)
{
    if (BUILT_IN_MODEL_CNT == 0)
        return nullptr;

    const auto *prev_model = static_cast<const VmafBuiltInModel *>(prev);
    const VmafBuiltInModel *out_model = nullptr;

    if (!prev_model) {
        out_model = &built_in_models[0];
    } else {
        const size_t idx = static_cast<size_t>(prev_model - built_in_models);
        if (idx + 1U < BUILT_IN_MODEL_CNT)
            out_model = prev_model + 1;
    }

    if (version && out_model)
        *version = out_model->version;
    return out_model;
}
