/* SPDX-License-Identifier: BSD-2-Clause-Patent */
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <libvmaf/model.h>

#include "compat/path_utf8.h"
#include "config.h"
#include "feature/feature_extractor.h"
#include "log.h"
#include "model.h"
#include "read_json_model.h"
#include "svm.h"
#include "vmafx/sha256.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is an
 * upstream-mirror file whose Netflix source spells the null pointer constant
 * `NULL` (every upstream sync would re-conflict against a keyword rewrite) and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

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

unsigned vmaf_built_in_model_count_for_test(void)
{
    return BUILT_IN_MODEL_CNT;
}

const char *vmaf_built_in_model_version_for_test(const void *built_in_model)
{
    if (!built_in_model)
        return NULL;
    const VmafBuiltInModel *model = built_in_model;
    return model->version;
}

int vmaf_model_builtin_data(const char *version, const char **data, size_t *len)
{
    if (!version || !data || !len)
        return -EINVAL;
    for (unsigned i = 0; i < BUILT_IN_MODEL_CNT; i++) {
        if (!strcmp(version, built_in_models[i].version)) {
            *data = built_in_models[i].data;
            *len = (size_t)*built_in_models[i].data_len;
            return 0;
        }
    }
    return -ENOENT;
}

/* Bound of a model file hashed by vmaf_model_stamp() (HISS-02): far above any
 * model the fork ships (the largest is a few MB). */
#define MODEL_STAMP_MAX_BYTES (1ull << 30)
#define MODEL_STAMP_CHUNK 65536u

/* SHA-256 of the file at `path` into `hex`: 0 or a negative errno. */
static int model_hash_file(const char *path, char hex[VMAFX_SHA256_HEX_CHARS])
{
    FILE *const file = vmaf_fopen_utf8(path, "rb");
    if (!file)
        return -EIO;
    unsigned char *const chunk = malloc(MODEL_STAMP_CHUNK);
    if (!chunk) {
        (void)fclose(file);
        return -ENOMEM;
    }
    VmafxSha256 sha;
    vmafx_sha256_init(&sha);
    unsigned long long total = 0;
    size_t got = 0;
    do {
        got = fread(chunk, 1, MODEL_STAMP_CHUNK, file);
        vmafx_sha256_update(&sha, chunk, got);
        total += got;
    } while (got == MODEL_STAMP_CHUNK && total < MODEL_STAMP_MAX_BYTES);
    const int failed = ferror(file) || total >= MODEL_STAMP_MAX_BYTES;
    free(chunk);
    const int closed = fclose(file);
    if (failed || closed != 0)
        return -EIO;
    vmafx_sha256_final_hex(&sha, hex);
    return 0;
}

int vmaf_model_stamp(VmafModel *model, const char *source, const void *data, size_t len,
                     uint64_t flags)
{
    if (!model || !source)
        return -EINVAL;
    char hex[VMAFX_SHA256_HEX_CHARS];
    if (data) {
        vmafx_sha256_hex(data, len, hex);
    } else {
        const int err = model_hash_file(source, hex);
        if (err)
            return err;
    }
    const size_t source_len = strlen(source);
    char *const copy = malloc(source_len + 1);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, source, source_len + 1);
    free(model->source);
    model->source = copy;
    memcpy(model->sha256, hex, sizeof(model->sha256));
    model->load_flags = flags;
    return 0;
}

int vmaf_model_stamp_loaded(int err, VmafModel **model, VmafModelCollection **collection,
                            const VmafModelConfig *cfg, const char *source, const void *data,
                            size_t len)
{
    if (err)
        return err;
    const uint64_t flags = cfg ? (uint64_t)cfg->flags : 0u;
    err = vmaf_model_stamp(*model, source, data, len, flags);
    const unsigned members = collection && *collection ? (*collection)->cnt : 0u;
    for (unsigned i = 0; !err && i < members; i++) {
        VmafModel *const member = (*collection)->model[i];
        err = member == *model ? 0 : vmaf_model_stamp(member, source, data, len, flags);
    }
    if (!err)
        return 0;
    /* The lead model is not a member of the collection (read_json_model.cpp,
     * model_collection_read_one()): release both. */
    if (collection && *collection) {
        vmaf_model_collection_destroy(*collection);
        *collection = NULL;
    }
    vmaf_model_destroy(*model);
    *model = NULL;
    return err;
}

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

    const int err = vmaf_read_json_model_from_buffer(model, cfg, built_in_model->data,
                                                     *built_in_model->data_len);
    return vmaf_model_stamp_loaded(err, model, NULL, cfg, version, built_in_model->data,
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
    err = vmaf_model_stamp_loaded(err, model, NULL, cfg, path, NULL, 0);
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

/* RC4 WP5: append `<feature_name>.<key>=<value>` of every entry of `dict` to
 * model->overrides (':'-joined, in the order applied). */
static int model_record_overrides(VmafModel *model, const char *feature_name,
                                  const VmafDictionary *dict)
{
    size_t len = model->overrides ? strlen(model->overrides) : 0u;
    size_t add = 0;
    for (unsigned i = 0; i < dict->cnt; i++)
        add += strlen(feature_name) + strlen(dict->entry[i].key) + strlen(dict->entry[i].val) + 3;
    const size_t cap = len + add + 1;
    char *const text = realloc(model->overrides, cap);
    if (!text)
        return -ENOMEM;
    text[len] = '\0';
    for (unsigned i = 0; i < dict->cnt && len < cap; i++) {
        const int n = snprintf(text + len, cap - len, "%s%s.%s=%s", len ? ":" : "", feature_name,
                               dict->entry[i].key, dict->entry[i].val);
        len += n > 0 ? (size_t)n : 0u;
    }
    model->overrides = text;
    return 0;
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
        const VmafFeatureExtractor *fex =
            vmaf_get_feature_extractor_by_feature_name(model->feature[i].name, 0);
        if (!fex)
            continue;
        if (strcmp(feature_name, fex->name) != 0)
            continue;
        VmafDictionary *d = vmaf_dictionary_merge((VmafDictionary **)&model->feature[i].opts_dict,
                                                  (VmafDictionary **)&opts_dict, 0);
        if (!d) {
            /* Netflix/vmaf#1242: this used to `return -ENOMEM` and skip the
             * unconditional free below, leaking the caller's dictionary on
             * every allocation failure.  The contract documented in
             * <libvmaf/model.h> is that the dictionary is consumed once the
             * argument guards above have passed, so break out and let the
             * common exit release it.  core/src/model.cpp (the unbuilt C++
             * twin) already had this shape. */
            err = -ENOMEM;
            break;
        }
        err = vmaf_dictionary_free(&model->feature[i].opts_dict);
        if (err)
            break;
        model->feature[i].opts_dict = d;
    }

    if (!err)
        err = model_record_overrides(model, feature_name, (const VmafDictionary *)opts_dict);
    err |= vmaf_dictionary_free((VmafDictionary **)&opts_dict);
    return err;
}

unsigned vmaf_model_feature_count(const VmafModel *model)
{
    return model ? model->n_features : 0;
}

const char *vmaf_model_feature_name(const VmafModel *model, unsigned index)
{
    if (!model || index >= model->n_features) {
        return NULL;
    }
    return model->feature[index].name;
}

/* Build a fresh, empty collection sized for the first model.
 *
 * Every failure path frees whatever this function itself allocated and returns
 * -ENOMEM with *out untouched, so the caller never sees a half-built
 * collection. Keeping the unwind local to each early return is what lets
 * vmaf_model_collection_append() drop its cascading unwind labels (HISS-01);
 * the ordering of the frees is unchanged — mc->model before mc. */
static int model_collection_new(VmafModelCollection **out, const VmafModel *model)
{
    /* Checked before anything is allocated: every strlen() below would
     * dereference a null name, and the caller reaches here straight from a
     * parsed model dictionary where the key can legitimately be absent. */
    if (!model->name)
        return -EINVAL;

    VmafModelCollection *mc = malloc(sizeof(*mc));
    if (!mc)
        return -ENOMEM;
    memset(mc, 0, sizeof(*mc));

    const size_t initial_sz = 8 * sizeof(*mc->model);
    mc->model = (VmafModel **)malloc(initial_sz);
    if (!mc->model) {
        free(mc);
        return -ENOMEM;
    }
    memset((void *)mc->model, 0, initial_sz);
    mc->size = 8;
    mc->type = model->type;

    /* Guard against size_t underflow when name is shorter than the
     * ".json" suffix we strip (5 chars).  An empty or very short
     * model name is a caller error; reject it cleanly.  mc->model was
     * already allocated above and must be freed before mc itself. */
    if (strlen(model->name) < 5) {
        free((void *)mc->model);
        free(mc);
        return -ENOMEM;
    }
    const size_t name_sz = strlen(model->name) - 5 + 1;
    mc->name = malloc(name_sz);
    if (!mc->name) {
        free((void *)mc->model);
        free(mc);
        return -ENOMEM;
    }
    memset((char *)mc->name, 0, name_sz);
    strncpy((char *)mc->name, model->name, name_sz - 1);

    *out = mc;
    return 0;
}

int vmaf_model_collection_append(VmafModelCollection **model_collection, VmafModel *model)
{
    if (!model_collection)
        return -EINVAL;
    if (!model)
        return -EINVAL;

    VmafModelCollection *mc = *model_collection;

    if (!mc) {
        const int err = model_collection_new(&mc, model);
        if (err) {
            /* Match the historical contract: a failed first append leaves the
             * caller's handle NULL rather than dangling. */
            *model_collection = NULL;
            return err;
        }
        *model_collection = mc;
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

    const int err = vmaf_read_json_model_collection_from_buffer(
        model, model_collection, cfg, built_in_model->data, *built_in_model->data_len);
    return vmaf_model_stamp_loaded(err, model, model_collection, cfg, version, built_in_model->data,
                                   *built_in_model->data_len);
}

int vmaf_model_collection_load_from_path(VmafModel **model, VmafModelCollection **model_collection,
                                         VmafModelConfig *cfg, const char *path)
{
    int err = vmaf_read_json_model_collection_from_path(model, model_collection, cfg, path);
    err = vmaf_model_stamp_loaded(err, model, model_collection, cfg, path, NULL, 0);
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
    /* Argument-validation guards consume nothing: the caller still owns
     * `opts_dict` when any of them fires.  See the ownership contract in
     * <libvmaf/model.h> and <libvmaf/feature.h>. */
    if (!model_collection || !*model_collection)
        return -EINVAL;
    if (!model || !feature_name || !opts_dict)
        return -EINVAL;
    VmafModelCollection *mc = *model_collection;

    int err = 0;
    for (unsigned i = 0; i < mc->cnt; i++) {
        VmafFeatureDictionary *d = NULL;
        /* Netflix/vmaf#1242: the copy's return value used to be discarded and
         * the partially-built copy leaked on failure, while the function could
         * still report success from the lead-model call below.  Free the
         * partial copy, fold the error into `err`, and stop iterating. */
        const int copy_err =
            vmaf_dictionary_copy((VmafDictionary **)&opts_dict, (VmafDictionary **)&d);
        if (copy_err) {
            err |= vmaf_dictionary_free((VmafDictionary **)&d);
            err |= copy_err;
            break;
        }
        err |= vmaf_model_feature_overload(mc->model[i], feature_name, d);
    }

    /* Always run the lead-model overload so `opts_dict` is consumed on every
     * path, matching the ownership contract in <libvmaf/model.h>. */
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

const char *vmaf_default_model_version(void)
{
    return VMAF_DEFAULT_MODEL_VERSION;
}

/* NOLINTEND(modernize-use-nullptr) */
