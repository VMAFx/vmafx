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

#ifndef VMAF_SRC_MODEL_H_
#define VMAF_SRC_MODEL_H_

#ifdef __cplusplus
#include <climits>
#include <cstddef>
#else
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#endif
#include <pthread.h>

#include "dict.h"
#include "libvmaf/model.h"
#include "libvmaf/picture.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
enum VmafModelType : unsigned int {
#else
enum VmafModelType {
#endif
    VMAF_MODEL_TYPE_UNKNOWN = 0,
    VMAF_MODEL_TYPE_SVM_NUSVR = 1,
    VMAF_MODEL_BOOTSTRAP_SVM_NUSVR = 2,
    VMAF_MODEL_RESIDUE_BOOTSTRAP_SVM_NUSVR = 3,
    VMAF_MODEL_TYPE_ABI_UINT_MAX = UINT_MAX,
};

/* Narrow test accessors for the opaque built-in-model iterator. */
unsigned vmaf_built_in_model_count_for_test(void);
const char *vmaf_built_in_model_version_for_test(const void *built_in_model);

#ifdef __cplusplus
enum VmafModelNormalizationType : unsigned int {
#else
enum VmafModelNormalizationType {
#endif
    VMAF_MODEL_NORMALIZATION_TYPE_UNKNOWN = 0,
    VMAF_MODEL_NORMALIZATION_TYPE_NONE = 1,
    VMAF_MODEL_NORMALIZATION_TYPE_LINEAR_RESCALE = 2,
    VMAF_MODEL_NORMALIZATION_TYPE_ABI_UINT_MAX = UINT_MAX,
};

struct VmafModelFeature {
    char *name;
    double slope, intercept;
    VmafDictionary *opts_dict;
};

struct VmafPoint {
    double x;
    double y;
};

#ifndef __cplusplus
typedef struct VmafModelFeature VmafModelFeature;
typedef struct VmafPoint VmafPoint;
#endif

struct VmafModel {
    char *path;
    char *name;
    enum VmafModelType type;
    double slope, intercept;
    VmafModelFeature *feature;
    unsigned n_features, feature_cap;
    struct {
        bool enabled;
        double min, max;
    } score_clip;
    struct {
        bool enabled;
        double chroma_correction_parameter;
    } chroma_from_luma;
    enum VmafModelNormalizationType norm_type;
    struct {
        bool enabled;
        struct {
            bool enabled;
            double value;
        } p0, p1, p2;
        struct {
            bool enabled;
            VmafPoint *list;
            unsigned n_knots, cap;
        } knots;
        bool out_lte_in, out_gte_in;
    } score_transform;
    /* Optional `conversion_target` block of the model file (Netflix/vmaf
     * 1ddf81607). pix_fmt UNKNOWN / bpc 0 mean "keep the source picture's". */
    struct {
        bool enabled;
        VmafColor color;
        enum VmafPixelFormat pix_fmt;
        unsigned bpc;
    } conversion_target;
    struct svm_model *svm;
    // Pre-allocated prediction state (populated lazily, reused per frame)
    struct svm_node *predict_nodes; // n_features + 1 entries
    char **predict_feature_names;   // n_features cached name strings
    void **predict_feature_vectors; // cached FeatureVector* pointers (opaque)
    /* Round-5 race fix (finding #3): guards the three lazy-init blocks in
     * predict_ensure_caches().  Initialized in vmaf_read_json_model(),
     * destroyed in vmaf_model_destroy(). */
    pthread_mutex_t predict_cache_lock;
    /* ADR-1755: owner count shared by the caller and every feature collector the
     * model is mounted on. NULL only on a model nothing loaded (it then has the
     * one owner, the caller). vmaf_model_destroy() drops one owner and frees the
     * model with the last. */
    struct VmafRef *owners;
    /* RC4 WP5 (#2142): where the model came from, for the provenance record.
     * Set by the loaders (vmaf_model_load*(), vmaf_model_collection_load*(),
     * the VMAFx loaders); NULL / empty on a model built otherwise. */
    char *source;        /* built-in version, or the path of the file */
    char sha256[65];     /* hex SHA-256 of the bytes as loaded */
    uint64_t load_flags; /* VmafModelConfig.flags at load */
    char *overrides;     /* `<extractor>.<key>=<value>` joined by ':' */
};

struct VmafModelCollection {
    VmafModel **model;
    unsigned cnt, size;
    enum VmafModelType type;
    const char *name;
};

char *vmaf_model_generate_name(VmafModelConfig *cfg);

/* Take one more owner of `model` (ADR-1755). Pair with vmaf_model_destroy().
 * Returns 0, or -EINVAL for a NULL model or one that carries no owner count. */
int vmaf_model_ref(VmafModel *model);

int vmaf_model_collection_append(VmafModelCollection **model_collection, VmafModel *model);

/* The embedded JSON of built-in model (or model set) `version` and its length,
 * the bytes vmaf_model_load() parses (ADR-1852: the VMAFx API hashes them).
 * Returns 0, -ENOENT for an unknown version, or -EINVAL. */
int vmaf_model_builtin_data(const char *version, const char **data, size_t *len);

/* RC4 WP5: record `source` (copied), the SHA-256 of `data[0..len)` (NULL
 * data: of the file at `source`) and `flags` on `model`. 0, -ENOMEM, or the
 * negative errno of reading the file. */
int vmaf_model_stamp(VmafModel *model, const char *source, const void *data, size_t len,
                     uint64_t flags);

/* After a load that returned `err`: stamp `*model` and every member of
 * `*collection` (NULL for a single model) with vmaf_model_stamp() and the
 * load's flags. A stamp failure releases what the load returned, NULLs the
 * out-parameters and is returned; `err` is returned unchanged. */
int vmaf_model_stamp_loaded(int err, VmafModel **model, VmafModelCollection **collection,
                            const VmafModelConfig *cfg, const char *source, const void *data,
                            size_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_SRC_MODEL_H_ */
