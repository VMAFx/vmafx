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

#include <libvmaf/feature.h>
#include <libvmaf/macros.h>

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

/**
 * @brief Bitwise flags that modify how the model emits its final score.
 *
 * Combine with bitwise-OR and assign to @ref VmafModelConfig::flags.
 *
 *   - `VMAF_MODEL_FLAGS_DEFAULT`        — no flags set; per-model defaults apply.
 *   - `VMAF_MODEL_FLAG_DISABLE_CLIP`    — do not clip predictions to [0, 100].
 *                                         Useful when probing for raw regression
 *                                         output (e.g. negative values that
 *                                         indicate strong over-prediction).
 *   - `VMAF_MODEL_FLAG_ENABLE_TRANSFORM`  — force-enable the model's score
 *                                          transform (typically the PHONE /
 *                                          4K-display transform), regardless of
 *                                          the model file's default.
 *   - `VMAF_MODEL_FLAG_DISABLE_TRANSFORM` — force-disable the score transform.
 *
 * `ENABLE_TRANSFORM` and `DISABLE_TRANSFORM` are mutually exclusive; setting
 * both yields undefined behaviour. `DISABLE_CLIP` may be combined with either
 * transform flag.
 */
enum VmafModelFlags {
    VMAF_MODEL_FLAGS_DEFAULT = 0,
    VMAF_MODEL_FLAG_DISABLE_CLIP = (1 << 0),
    VMAF_MODEL_FLAG_ENABLE_TRANSFORM = (1 << 1),
    VMAF_MODEL_FLAG_DISABLE_TRANSFORM = (1 << 2),
};

/**
 * @brief Configuration passed to `vmaf_model_load` / `vmaf_model_load_from_path`.
 *
 * Safe to zero-initialise — both fields are optional.
 */
typedef struct VmafModelConfig {
    /**
     * Optional display name for the loaded model. When NULL the loader uses
     * `"vmaf"` for built-in models and the model file's `model_name` field
     * for path-loaded models. When non-NULL the string is copied; the caller
     * retains ownership and may free it after the load call returns.
     */
    const char *name;
    /**
     * Bitwise-OR of `VmafModelFlags` values; controls score clipping and
     * transform application. Pass `VMAF_MODEL_FLAGS_DEFAULT` (0) to honour
     * the model file's defaults.
     */
    uint64_t flags;
} VmafModelConfig;

/**
 * @brief Load a built-in VMAF model by version string.
 *
 * Looks @p version up in the compile-time built-in model table (the same
 * table iterated by @ref vmaf_model_version_next) and constructs a model
 * instance from the embedded JSON payload. No filesystem access.
 *
 * Pair every successful load with a matching @ref vmaf_model_destroy when the
 * model is no longer needed.
 *
 * @param model   Out: receives the allocated model handle. Caller owns and
 *                must release via @ref vmaf_model_destroy.
 * @param cfg     Required configuration. May supply a custom display name and
 *                @ref VmafModelFlags overrides. Must not be NULL.
 * @param version Built-in model version string (e.g. `"vmaf_v0.6.1"`).
 *                Must match an entry in the built-in model table.
 *
 * @return 0 on success, or a negative errno code on error:
 *         `-EINVAL` when @p version does not match any built-in model
 *         (the loader also logs a warning), `-ENOMEM` on allocation failure.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT int vmaf_model_load(VmafModel **model, VmafModelConfig *cfg, const char *version);

/**
 * @brief Load a VMAF model from a filesystem path.
 *
 * Reads a `.json` model file from disk. Pickle (`.pkl`) is no longer
 * supported — the loader detects the extension and emits an explicit error
 * message pointing the caller at the JSON form.
 *
 * Pair every successful load with a matching @ref vmaf_model_destroy when the
 * model is no longer needed. For ONNX tiny-AI models, use
 * @ref vmaf_use_tiny_model on a live @ref VmafContext instead — this function
 * is SVM-only.
 *
 * @param model Out: receives the allocated model handle. Caller owns and
 *              must release via @ref vmaf_model_destroy.
 * @param cfg   Required configuration. May supply a custom display name and
 *              @ref VmafModelFlags overrides. Must not be NULL.
 * @param path  Filesystem path to a `.json` VMAF model file.
 *
 * @return 0 on success, or a negative errno code on error: `-ENOENT` if the
 *         path does not exist, `-EINVAL` on a malformed JSON payload or a
 *         `.pkl` extension, `-ENOMEM` on allocation failure.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT int vmaf_model_load_from_path(VmafModel **model, VmafModelConfig *cfg,
                                          const char *path);

/**
 * @brief Override per-feature options on an already-loaded model.
 *
 * Walks the model's registered features, finds entries whose name matches
 * @p feature_name, and merges the provided dictionary onto each entry's
 * existing options. Useful when the caller wants to tweak (for example) the
 * VIF enhancement gain limit without editing the model JSON on disk.
 *
 * On both success and failure, ownership of @p opts_dict transfers to this
 * call — the function releases the dictionary internally before returning.
 * The caller MUST NOT call @ref vmaf_feature_dictionary_free on @p opts_dict
 * after invoking this function, even on a non-zero return.
 *
 * @param model        Loaded model from @ref vmaf_model_load or
 *                     @ref vmaf_model_load_from_path. Must not be NULL.
 * @param feature_name NUL-terminated feature name to target (e.g.
 *                     `"float_vif"`). Must not be NULL.
 * @param opts_dict    Options dictionary populated via
 *                     @ref vmaf_feature_dictionary_set. Must not be NULL.
 *                     Ownership transfers to this call.
 *
 * @return 0 on success, or a negative errno code on error: `-EINVAL` on NULL
 *         arguments, `-ENOMEM` on dictionary-merge allocation failure.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT int vmaf_model_feature_overload(VmafModel *model, const char *feature_name,
                                            VmafFeatureDictionary *opts_dict);

/**
 * @brief Release a model and all owned resources.
 *
 * Frees the SVM (or DNN sidecar) state, the per-feature option dictionaries,
 * the score-transform tables, and the model handle itself. Safe to call with
 * a NULL @p model.
 *
 * Once destroyed, every pointer the caller may have cached from the model is
 * invalidated. Do not invoke after the model has been handed to a
 * @ref VmafModelCollection — that collection takes ownership and will
 * destroy contained models when @ref vmaf_model_collection_destroy runs.
 *
 * @param model Model handle from @ref vmaf_model_load or
 *              @ref vmaf_model_load_from_path. NULL is a no-op.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT void vmaf_model_destroy(VmafModel *model);

/**
 * @typedef VmafModelCollection
 * @brief Opaque bag of related VMAF sub-models, typically a bootstrap ensemble.
 *
 * A collection groups N sibling models that share a single feature pipeline
 * and emit an aggregate score (mean prediction plus a confidence interval).
 * Constructed by @ref vmaf_model_collection_load /
 * @ref vmaf_model_collection_load_from_path and consumed by
 * @ref vmaf_score_at_index_model_collection and
 * @ref vmaf_score_pooled_model_collection.
 *
 * Owns every contained @ref VmafModel — calling
 * @ref vmaf_model_collection_destroy frees them all.
 */
typedef struct VmafModelCollection VmafModelCollection;

/**
 * @brief Discriminates the aggregation method that produced a collection score.
 *
 *   - `UNKNOWN`   — sentinel; should not appear in a valid score struct.
 *   - `BOOTSTRAP` — `score` is the mean of N bootstrap sub-model predictions
 *                   and the `bootstrap` sub-struct carries the standard
 *                   deviation and the 95% confidence interval.
 */
enum VmafModelCollectionScoreType {
    VMAF_MODEL_COLLECTION_SCORE_UNKNOWN = 0,
    VMAF_MODEL_COLLECTION_SCORE_BOOTSTRAP,
};

/**
 * @brief Aggregate prediction from a model collection.
 *
 * Populated by @ref vmaf_score_at_index_model_collection /
 * @ref vmaf_score_pooled_model_collection. Read fields based on @p type.
 *
 * @field type                Aggregation method discriminator (see
 *                            @ref VmafModelCollectionScoreType).
 * @field bootstrap.bagging_score  Mean of the N bootstrap sub-model
 *                                 predictions.
 * @field bootstrap.stddev    Standard deviation across the N predictions.
 * @field bootstrap.ci.p95.lo Lower bound of the 95% confidence interval.
 * @field bootstrap.ci.p95.hi Upper bound of the 95% confidence interval.
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

/**
 * @brief Load a built-in VMAF model collection by version string.
 *
 * Companion to @ref vmaf_model_load that returns both a "lead" model
 * (the first sub-model in the collection, used by `vmaf_use_features_from_model`)
 * and the wrapping @ref VmafModelCollection handle. The lead model is owned
 * by the collection; do not free it independently.
 *
 * @param model            Out: receives the lead sub-model handle. Owned by
 *                         @p model_collection; do not pass to
 *                         @ref vmaf_model_destroy.
 * @param model_collection Out: receives the allocated collection handle.
 *                         Caller owns and must release via
 *                         @ref vmaf_model_collection_destroy.
 * @param cfg              Required configuration. Must not be NULL.
 * @param version          Built-in model collection version string (e.g.
 *                         `"vmaf_v0.6.1.bootstrap"`).
 *
 * @return 0 on success, or a negative errno code on error: `-EINVAL` when
 *         @p version does not match any built-in collection, `-ENOMEM` on
 *         allocation failure.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT int vmaf_model_collection_load(VmafModel **model,
                                           VmafModelCollection **model_collection,
                                           VmafModelConfig *cfg, const char *version);

/**
 * @brief Load a VMAF model collection from a filesystem path.
 *
 * Path-loading counterpart to @ref vmaf_model_collection_load. Reads a `.json`
 * bootstrap-model file from disk; the `.pkl` extension surfaces an explicit
 * unsupported-format error message.
 *
 * @param model            Out: receives the lead sub-model handle. Owned by
 *                         @p model_collection; do not pass to
 *                         @ref vmaf_model_destroy.
 * @param model_collection Out: receives the allocated collection handle.
 *                         Caller owns and must release via
 *                         @ref vmaf_model_collection_destroy.
 * @param cfg              Required configuration. Must not be NULL.
 * @param path             Filesystem path to a `.json` bootstrap-model file.
 *
 * @return 0 on success, or a negative errno code on error: `-ENOENT` if the
 *         path does not exist, `-EINVAL` on a malformed payload or a `.pkl`
 *         extension, `-ENOMEM` on allocation failure.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT int vmaf_model_collection_load_from_path(VmafModel **model,
                                                     VmafModelCollection **model_collection,
                                                     VmafModelConfig *cfg, const char *path);

/**
 * @brief Override per-feature options on every model in a collection.
 *
 * Applies @ref vmaf_model_feature_overload semantics to each sub-model in
 * the collection plus the lead model @p model, so a single override
 * propagates to the ensemble.
 *
 * Ownership of @p opts_dict transfers to this call — the function deep-copies
 * the dictionary onto each sub-model and releases the original (and every
 * temporary copy on failure) before returning. The caller MUST NOT call
 * @ref vmaf_feature_dictionary_free on @p opts_dict afterwards.
 *
 * @param model            Lead model returned by
 *                         @ref vmaf_model_collection_load /
 *                         @ref vmaf_model_collection_load_from_path. Must not
 *                         be NULL.
 * @param model_collection Collection handle (in/out: pointer is read, contents
 *                         may be mutated as features merge). Must not be NULL.
 * @param feature_name     NUL-terminated feature name to target. Must not be
 *                         NULL.
 * @param opts_dict        Options dictionary. Ownership transfers to this
 *                         call. Must not be NULL.
 *
 * @return 0 on success, or a negative errno code on error: `-EINVAL` on NULL
 *         arguments, `-ENOMEM` on dictionary-copy failure.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT int vmaf_model_collection_feature_overload(VmafModel *model,
                                                       VmafModelCollection **model_collection,
                                                       const char *feature_name,
                                                       VmafFeatureDictionary *opts_dict);

/**
 * @brief Release a model collection and every sub-model it owns.
 *
 * Iterates the contained sub-models calling @ref vmaf_model_destroy on each,
 * then frees the collection's internal arrays and the handle itself. Safe
 * to call with NULL.
 *
 * After this call the lead-model pointer returned by the matching load call
 * is invalidated — do not pass it to @ref vmaf_model_destroy.
 *
 * @param model_collection Collection handle from
 *                         @ref vmaf_model_collection_load /
 *                         @ref vmaf_model_collection_load_from_path. NULL
 *                         is a no-op.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
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
 *
 * @thread-safety Safe to call from any thread; the built-in model table is
 *               read-only after library init.
 */
VMAF_EXPORT const void *vmaf_model_version_next(const void *prev, const char **version);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_MODEL_H */
