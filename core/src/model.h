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
#else
#include <limits.h>
#include <stdbool.h>
#endif
#include <pthread.h>

#include "dict.h"
#include "libvmaf/model.h"

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
    struct svm_model *svm;
    // Pre-allocated prediction state (populated lazily, reused per frame)
    struct svm_node *predict_nodes; // n_features + 1 entries
    char **predict_feature_names;   // n_features cached name strings
    void **predict_feature_vectors; // cached FeatureVector* pointers (opaque)
    /* Round-5 race fix (finding #3): guards the three lazy-init blocks in
     * predict_ensure_caches().  Initialized in vmaf_read_json_model(),
     * destroyed in vmaf_model_destroy(). */
    pthread_mutex_t predict_cache_lock;
};

struct VmafModelCollection {
    VmafModel **model;
    unsigned cnt, size;
    enum VmafModelType type;
    const char *name;
};

char *vmaf_model_generate_name(VmafModelConfig *cfg);

int vmaf_model_collection_append(VmafModelCollection **model_collection, VmafModel *model);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VMAF_SRC_MODEL_H_ */
