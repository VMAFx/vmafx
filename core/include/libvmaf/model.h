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

#ifndef LIBVMAF_MODEL_H
#define LIBVMAF_MODEL_H

#include <stdint.h>

#include "feature.h"
#include "libvmaf/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmafModel VmafModel;

/**
 * Discriminates which runtime owns a loaded model.
 *
 *   - SVM        — upstream libsvm-backed model (default, json/pkl).
 *   - DNN_FR     — feature-vector → MOS (tiny FR regressor, .onnx + sidecar).
 *   - DNN_NR     — distorted frame → MOS, no reference needed (.onnx + sidecar).
 *   - DNN_FILTER — degraded → clean residual filter (.onnx + sidecar). Consumed
 *                  by the ffmpeg `vmaf_pre` filter; NOT loaded by libvmaf's
 *                  scoring path. Tracked in the tiny registry for trust-root
 *                  hygiene (sha256-pinned, signed) but never reaches
 *                  `vmaf_score_*`.
 *
 * Auto-detected by vmaf_model_load_from_path() from the file extension:
 * `.json`/`.pkl` → SVM, `.onnx` → DNN_FR (unless the matching sidecar JSON
 * sets `"kind": "nr"` or `"kind": "filter"`, in which case DNN_NR /
 * DNN_FILTER).
 */
typedef enum VmafModelKind {
    VMAF_MODEL_KIND_SVM = 0,
    VMAF_MODEL_KIND_DNN_FR = 1,
    VMAF_MODEL_KIND_DNN_NR = 2,
    VMAF_MODEL_KIND_DNN_FILTER = 3,
} VmafModelKind;

enum VmafModelFlags {
    VMAF_MODEL_FLAGS_DEFAULT = 0,
    VMAF_MODEL_FLAG_DISABLE_CLIP = (1 << 0),
    VMAF_MODEL_FLAG_ENABLE_TRANSFORM = (1 << 1),
    VMAF_MODEL_FLAG_DISABLE_TRANSFORM = (1 << 2),
};

/**
 * VMAF model configuration. Passed to `vmaf_model_load` /
 * `vmaf_model_load_from_path`.
 */
typedef struct VmafModelConfig {
    const char *name; /**< Caller-supplied tag exposed in scoring output. */
    uint64_t flags;   /**< Bitwise OR of `VmafModelFlags` values. */
} VmafModelConfig;

VMAF_EXPORT int vmaf_model_load(VmafModel **model, VmafModelConfig *cfg, const char *version);

VMAF_EXPORT int vmaf_model_load_from_path(VmafModel **model, VmafModelConfig *cfg,
                                          const char *path);

VMAF_EXPORT int vmaf_model_feature_overload(VmafModel *model, const char *feature_name,
                                            VmafFeatureDictionary *opts_dict);

VMAF_EXPORT void vmaf_model_destroy(VmafModel *model);

typedef struct VmafModelCollection VmafModelCollection;

enum VmafModelCollectionScoreType {
    VMAF_MODEL_COLLECTION_SCORE_UNKNOWN = 0,
    VMAF_MODEL_COLLECTION_SCORE_BOOTSTRAP,
};

/**
 * Aggregate score returned from a model-collection (e.g. bootstrap
 * VMAF). Populated by `vmaf_score_pooled_model_collection`.
 */
typedef struct VmafModelCollectionScore {
    enum VmafModelCollectionScoreType type; /**< Discriminator for the score family. */
    /** Bootstrap variant: bagging score + dispersion + 95% confidence
     *  interval. Valid when @p type == VMAF_MODEL_COLLECTION_SCORE_BOOTSTRAP. */
    struct {
        double bagging_score; /**< Mean of the per-sub-model scores. */
        double stddev;        /**< Standard deviation across sub-models. */
        /** Confidence-interval block. */
        struct {
            /** 95% confidence interval [lo, hi]. */
            struct {
                double lo, hi; /**< Lower / upper 95% CI bounds. */
            } p95;
        } ci;
    } bootstrap;
} VmafModelCollectionScore;

VMAF_EXPORT int vmaf_model_collection_load(VmafModel **model,
                                           VmafModelCollection **model_collection,
                                           VmafModelConfig *cfg, const char *version);

VMAF_EXPORT int vmaf_model_collection_load_from_path(VmafModel **model,
                                                     VmafModelCollection **model_collection,
                                                     VmafModelConfig *cfg, const char *path);

VMAF_EXPORT int vmaf_model_collection_feature_overload(VmafModel *model,
                                                       VmafModelCollection **model_collection,
                                                       const char *feature_name,
                                                       VmafFeatureDictionary *opts_dict);

VMAF_EXPORT void vmaf_model_collection_destroy(VmafModelCollection *model_collection);

/**
 * Iterate through all built-in VMAF model versions.
 *
 * Pass NULL for @p prev on the first call; on subsequent calls pass the
 * previously-returned opaque handle. When iteration reaches the end the
 * function returns NULL and leaves @p *version unmodified.
 *
 * @param prev    opaque handle from the previous call, or NULL to begin.
 * @param version OUT: set to the version string of the returned model on
 *                a non-NULL return. Not modified on end-of-iteration.
 *                May itself be NULL if the caller only needs the handle.
 * @return opaque handle to the next model, or NULL after the last model.
 */
VMAF_EXPORT const void *vmaf_model_version_next(const void *prev, const char **version);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_MODEL_H */
