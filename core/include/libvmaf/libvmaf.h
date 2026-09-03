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

#ifndef LIBVMAF_LIBVMAF_H
#define LIBVMAF_LIBVMAF_H

#include <stdint.h>
#include <stdio.h>

#include <libvmaf/macros.h>
#include <libvmaf/model.h>
#include <libvmaf/picture.h>
#include <libvmaf/feature.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum  VmafLogLevel
 * @brief Verbosity tier used by libvmaf's stderr logger.
 *
 * Set on @ref VmafConfiguration::log_level at @ref vmaf_init. Messages with a
 * severity numerically <= the configured level are emitted; everything else is
 * suppressed.
 *
 *   - `VMAF_LOG_LEVEL_NONE`    — silence the logger entirely.
 *   - `VMAF_LOG_LEVEL_ERROR`   — only unrecoverable errors.
 *   - `VMAF_LOG_LEVEL_WARNING` — errors + recoverable warnings (default for
 *                                most CLI invocations).
 *   - `VMAF_LOG_LEVEL_INFO`    — adds per-frame / per-feature info breadcrumbs.
 *   - `VMAF_LOG_LEVEL_DEBUG`   — adds diagnostics intended for libvmaf
 *                                developers — very chatty, not stable across
 *                                versions.
 *
 * Stable enumerator values — append-only across libvmaf releases.
 */
enum VmafLogLevel {
    VMAF_LOG_LEVEL_NONE = 0,
    VMAF_LOG_LEVEL_ERROR,
    VMAF_LOG_LEVEL_WARNING,
    VMAF_LOG_LEVEL_INFO,
    VMAF_LOG_LEVEL_DEBUG,
};

/**
 * @enum VmafBackend
 * @brief Compute backend active in a `VmafContext`.
 *
 * Returned by `vmaf_context_get_backend()` to allow callers to introspect
 * which compute backend was imported into a context at run time.  New values
 * may be appended in future releases; callers must treat unknown values as
 * `VMAF_BACKEND_UNKNOWN`.
 */
enum VmafBackend {
    VMAF_BACKEND_UNKNOWN = 0, /**< No GPU state imported; CPU-only context. */
    VMAF_BACKEND_CUDA = 1,    /**< CUDA backend (vmaf_cuda_import_state()). */
    VMAF_BACKEND_SYCL = 2,    /**< SYCL backend (vmaf_sycl_import_state()). */
    VMAF_BACKEND_METAL = 3,   /**< Metal backend (vmaf_metal_import_state()). */
    VMAF_BACKEND_HIP = 4,     /**< HIP/ROCm backend (vmaf_hip_import_state()). */
    VMAF_BACKEND_VULKAN = 5,  /**< Vulkan backend (reserved; ADR-0726 removed Vulkan). */
};

/**
 * @enum  VmafOutputFormat
 * @brief Output serialisation format for @ref vmaf_write_output and
 *        @ref vmaf_write_output_with_format.
 *
 *   - `VMAF_OUTPUT_FORMAT_NONE` — sentinel; the writer rejects this value and
 *                                 emits no file.
 *   - `VMAF_OUTPUT_FORMAT_XML`  — hierarchical XML matching the upstream
 *                                 reference format.
 *   - `VMAF_OUTPUT_FORMAT_JSON` — single JSON document with `frames` +
 *                                 `pooled_metrics` sections.
 *   - `VMAF_OUTPUT_FORMAT_CSV`  — one row per frame, one column per registered
 *                                 feature; pooled scores appended as trailing
 *                                 rows.
 *   - `VMAF_OUTPUT_FORMAT_SUB`  — SubRip-style (.srt) burn-in subtitles, one
 *                                 cue per frame, suitable for `ffmpeg -vf
 *                                 subtitles=…` overlay debugging.
 *
 * Stable enumerator values — append-only across libvmaf releases.
 */
enum VmafOutputFormat {
    VMAF_OUTPUT_FORMAT_NONE = 0,
    VMAF_OUTPUT_FORMAT_XML,
    VMAF_OUTPUT_FORMAT_JSON,
    VMAF_OUTPUT_FORMAT_CSV,
    VMAF_OUTPUT_FORMAT_SUB,
};

/**
 * @enum  VmafPoolingMethod
 * @brief Temporal pooling strategy applied over a frame interval.
 *
 * Passed to @ref vmaf_score_pooled, @ref vmaf_score_pooled_model_collection,
 * and @ref vmaf_feature_score_pooled to select how per-frame scores are
 * reduced to a single summary value.
 *
 *   - `VMAF_POOL_METHOD_UNKNOWN`        — sentinel; rejected by pooling functions.
 *   - `VMAF_POOL_METHOD_MIN`            — minimum per-frame score over the interval.
 *   - `VMAF_POOL_METHOD_MAX`            — maximum per-frame score over the interval.
 *   - `VMAF_POOL_METHOD_MEAN`           — arithmetic mean.
 *   - `VMAF_POOL_METHOD_HARMONIC_MEAN`  — harmonic mean (penalises outlier lows more
 *                                        than the arithmetic mean; typical for VMAF
 *                                        "phone" / 4K display reporting).
 *
 * Stable enumerator values — append-only across libvmaf releases for ABI
 * compatibility. `VMAF_POOL_METHOD_NB` is a count sentinel and NOT a stable
 * value; see its inline doc.
 */
enum VmafPoolingMethod {
    VMAF_POOL_METHOD_UNKNOWN = 0,
    VMAF_POOL_METHOD_MIN,
    VMAF_POOL_METHOD_MAX,
    VMAF_POOL_METHOD_MEAN,
    VMAF_POOL_METHOD_HARMONIC_MEAN,
/**
     * Count sentinel — retained for backwards compatibility only.
     *
     * NOT a stable API value: its numeric value increases whenever a new
     * pooling method is appended, so do not switch on it, persist it, or
     * pass it across an ABI boundary. New code should switch on the named
     * values and treat unknown discriminants as `VMAF_POOL_METHOD_UNKNOWN`.
     *
     * @deprecated Use the named VMAF_POOL_METHOD_* enumerators. This
     *             sentinel will be removed in a future major release.
     */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(VMAF_BUILDING_LIBVMAF)
    VMAF_POOL_METHOD_NB __attribute__((deprecated("use named VMAF_POOL_METHOD_* values; "
                                                  "NB is not a stable count sentinel")))
#else
    VMAF_POOL_METHOD_NB
#endif
};

/**
 * @struct VmafConfiguration
 * @brief  Configuration needed to initialize a `VmafContext`
 *
 * @param log_level   How verbose the logger is.
 *
 * @param n_threads   How many threads can be used to run
 *                    feature extractors concurrently.
 *
 * @param n_subsample Compute scores only every N frames.
 *                    Note that setting an even value for N can lead to
 *                    inaccurate results. For more detail, see
 *                    https://github.com/Netflix/vmaf/issues/1214
 *
 * @param cpumask     Restrict permitted CPU instruction sets. The bit
 *                    layout is architecture-specific — only the bits
 *                    listed for the host architecture take effect; bits
 *                    set for a different architecture are silently
 *                    ignored.
 *
 *                    On x86 / x86_64:
 *                      if cpumask & 1:  disable SSE2
 *                      if cpumask & 2:  disable SSE3 / SSSE3
 *                      if cpumask & 4:  disable SSE4.1
 *                      if cpumask & 8:  disable AVX2
 *                      if cpumask & 16: disable AVX512
 *                      if cpumask & 32: disable AVX512ICL
 *
 *                    On arm64 / aarch64:
 *                      if cpumask & 1:  disable NEON (forces scalar)
 *                      if cpumask & 2:  disable SVE2
 *
 * @param gpumask     Restrict permitted GPU operations. Any non-zero
 *                    value disables the GPU feature-extractor selection
 *                    for both the CUDA and SYCL backends (the runtime
 *                    falls back to the CPU implementation). The HIP
 *                    backend is host-pic and is not gated by gpumask —
 *                    its activation is controlled solely by whether a
 *                    HIP state pointer was imported via
 *                    `vmaf_hip_import_state()`. ADR-0530.
 */
typedef struct VmafConfiguration {
    enum VmafLogLevel log_level; /**< Logger verbosity. */
    unsigned n_threads;          /**< Worker thread count; 0 = library default. */
    unsigned n_subsample;        /**< Compute scores only every N frames; 0/1 = every frame. */
    uint64_t cpumask;            /**< CPU-ISA disable bitmask; see struct doc above. */
    uint64_t gpumask;            /**< GPU-feature disable bitmask; see struct doc above. */
} VmafConfiguration;

/**
 * @typedef VmafContext
 * @brief   Opaque per-measurement-session handle owning the feature pipeline.
 *
 * Allocated by @ref vmaf_init, released by @ref vmaf_close. A single context
 * pins one set of registered feature extractors, one optional GPU backend
 * state (CUDA / SYCL / HIP / Metal, imported via the corresponding
 * `vmaf_*_import_state` entry point), zero or more attached models / model
 * collections / tiny-AI ONNX sessions, and the per-frame score table.
 *
 * Not thread-safe — each context is owned by exactly one driver thread at a
 * time. Callers that want concurrent measurements run multiple contexts in
 * parallel; libvmaf's own per-extractor thread pool (configured via
 * @ref VmafConfiguration::n_threads) handles intra-frame parallelism.
 */
typedef struct VmafContext VmafContext;

/**
 * Allocate and open a VMAF instance.
 *
 * @param vmaf The VMAF instance to open.
 *             To be used in further libvmaf api calls.
 *             $vmaf will be set to the allocated context.
 *             Context should be cleaned up with `vmaf_close()` when finished.
 *
 * @param cfg  Configuration parameters.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_init(VmafContext **vmaf, VmafConfiguration cfg);

/**
 * Register feature extractors required by a specific `VmafModel`.
 * This may be called multiple times using different models.
 * In this case, the registered feature extractors will form a set, and any
 * features required by multiple models will only be extracted once.
 *
 * @param vmaf  The VMAF context allocated with `vmaf_init()`.
 *
 * @param model Opaque model context.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_use_features_from_model(VmafContext *vmaf, VmafModel *model);

/**
 * Register feature extractors required by a specific `VmafModelCollection`
 * Like `vmaf_use_features_from_model()`, this function may be called
 * multiple times using different model collections.
 *
 * @param vmaf             The VMAF context allocated with `vmaf_init()`.
 *
 * @param model_collection Opaque model collection context.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_use_features_from_model_collection(VmafContext *vmaf,
                                                        VmafModelCollection *model_collection);

/**
 * Register specific feature extractor.
 * Useful when a specific/additional feature is required, usually one which
 * is not already provided by a model via `vmaf_use_features_from_model()`.
 * This may be called multiple times. `VmafContext` takes ownership of the
 * `VmafFeatureDictionary` (`opts_dict`) on every path EXCEPT the
 * argument-validation guards: if this returns `-EINVAL` because `vmaf` or
 * `feature_name` was NULL, or because `feature_name` names no registered
 * feature, nothing was consumed and the caller must release the dictionary
 * with `vmaf_feature_dictionary_free()`. On any other return — success or
 * failure — the dictionary has already been released internally and the
 * caller MUST NOT free it. See <libvmaf/feature.h> for the shared contract
 * (Netflix/vmaf#1242).
 *
 * @param vmaf         The VMAF context allocated with `vmaf_init()`.
 *
 * @param feature_name Name of feature.
 *
 * @param opts_dict    Feature extractor options set via
 *                     `vmaf_feature_dictionary_set()`. If no special options
 *                     are required this parameter can be set to NULL.
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_use_feature(VmafContext *vmaf, const char *feature_name,
                                 VmafFeatureDictionary *opts_dict);

/**
 * Import an external feature score.
 * Useful when pre-computed feature scores are available.
 * Also useful in the case where there is no libvmaf feature extractor
 * implementation for a required feature.
 *
 * @param vmaf         The VMAF context allocated with `vmaf_init()`.
 *
 * @param feature_name Name of feature.
 *
 * @param value        Score.
 *
 * @param index        Picture index.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_import_feature_score(VmafContext *vmaf, const char *feature_name, double value,
                                          unsigned index);

/**
 * Read a pair of pictures and queue them for eventual feature extraction.
 * This should be called after feature extractors are registered via
 * `vmaf_use_features_from_model()` and/or `vmaf_use_feature()`.
 * `VmafContext` will take ownership of both `VmafPicture`s (`ref` and `dist`)
 * and `vmaf_picture_unref()`.
 *
 * When you're done reading pictures call this function again with both `ref`
 * and `dist` set to NULL to flush all feature extractors.
 *
 * @param vmaf  The VMAF context allocated with `vmaf_init()`.
 *
 * @param ref   Reference picture.
 *
 * @param dist  Distorted picture.
 *
 * @param index Picture index.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_read_pictures(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist,
                                   unsigned index);

/**
 * Predict VMAF score at specific index.
 *
 * @param vmaf   The VMAF context allocated with `vmaf_init()`.
 *
 * @param model  Opaque model context.
 *
 * @param score  Predicted score (out).
 *
 * @param index  Picture index.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_score_at_index(VmafContext *vmaf, VmafModel *model, double *score,
                                    unsigned index);

/**
 * Predict VMAF score at specific index, using a model collection.
 *
 * @param vmaf              The VMAF context allocated with `vmaf_init()`.
 *
 * @param model_collection  Opaque model collection context.
 *
 * @param index             Picture index.
 *
 * @param score             Predicted score.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_score_at_index_model_collection(VmafContext *vmaf,
                                                     VmafModelCollection *model_collection,
                                                     VmafModelCollectionScore *score,
                                                     unsigned index);

/**
 * Fetch feature score at specific index.
 *
 * @param vmaf          The VMAF context allocated with `vmaf_init()`.
 *
 * @param feature_name  Name of the feature to fetch.
 *
 * @param index         Picture index.
 *
 * @param score         Score.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_feature_score_at_index(VmafContext *vmaf, const char *feature_name,
                                            double *score, unsigned index);

/**
 * Pooled VMAF score for a specific interval.
 *
 * @param vmaf         The VMAF context allocated with `vmaf_init()`.
 *
 * @param model        Opaque model context.
 *
 * @param pool_method  Temporal pooling method to use.
 *
 * @param score        Pooled score.
 *
 * @param index_low    Low picture index of pooling interval.
 *
 * @param index_high   High picture index of pooling interval.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_score_pooled(VmafContext *vmaf, VmafModel *model,
                                  enum VmafPoolingMethod pool_method, double *score,
                                  unsigned index_low, unsigned index_high);

/**
 * Pooled VMAF score for a specific interval, using a model collection.
 *
 * @param vmaf              The VMAF context allocated with `vmaf_init()`.
 *
 * @param model_collection  Opaque model collection context.
 *
 * @param pool_method       Temporal pooling method to use.
 *
 * @param score             Pooled score.
 *
 * @param index_low         Low picture index of pooling interval.
 *
 * @param index_high        High picture index of pooling interval.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_score_pooled_model_collection(VmafContext *vmaf,
                                                   VmafModelCollection *model_collection,
                                                   enum VmafPoolingMethod pool_method,
                                                   VmafModelCollectionScore *score,
                                                   unsigned index_low, unsigned index_high);

/**
 * Pooled feature score for a specific interval.
 *
 * @param vmaf          The VMAF context allocated with `vmaf_init()`.
 *
 * @param feature_name  Name of the feature to fetch.
 *
 * @param pool_method   Temporal pooling method to use.
 *
 * @param score         Pooled score.
 *
 * @param index_low     Low picture index of pooling interval.
 *
 * @param index_high    High picture index of pooling interval.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_feature_score_pooled(VmafContext *vmaf, const char *feature_name,
                                          enum VmafPoolingMethod pool_method, double *score,
                                          unsigned index_low, unsigned index_high);

/**
 * @struct VmafPictureConfiguration
 * @brief  Picture-pool configuration for @ref vmaf_preallocate_pictures.
 *
 * Pre-allocates a fixed-size pool of @ref VmafPicture buffers so that the
 * per-frame hot path avoids `malloc` / `free`. Required when
 * @ref VmafConfiguration::n_threads > 1: every concurrent decode thread holds
 * one picture from the pool, so @p pic_cnt should be at least `n_threads * 2`
 * (one for ref, one for dis) to avoid stalls.
 *
 * Safe to zero-initialise. @p pic_cnt == 0 disables preallocation; the caller
 * is then responsible for its own @ref vmaf_picture_alloc / unref cycle.
 *
 * @field pic_params         Per-picture geometry shared by every pool slot:
 *                           luma width, luma height, bits per component, and
 *                           planar pixel format. All slots are allocated to
 *                           the same dimensions; mixing resolutions in one
 *                           session requires a fresh @ref vmaf_init.
 * @field pic_params.w       Luma width in samples.
 * @field pic_params.h       Luma height in samples.
 * @field pic_params.bpc     Bits per component (8, 10, 12, or 16).
 * @field pic_params.pix_fmt Planar pixel format (see @ref VmafPixelFormat).
 * @field pic_cnt            Number of pool slots to allocate. 0 disables the
 *                           pool entirely.
 */
typedef struct VmafPictureConfiguration {
    /** Per-picture shape (width/height/bpc/pixel-format). */
    struct {
        unsigned w, h;                /**< Per-plane width / height. */
        unsigned bpc;                 /**< Bits per component. */
        enum VmafPixelFormat pix_fmt; /**< Pixel format. */
    } pic_params;
    unsigned pic_cnt; /**< Pool size — count of pre-allocated pictures. */
} VmafPictureConfiguration;

/**
 * Preallocate pictures for use with multi-threaded feature extraction.
 * Pictures are allocated once and automatically returned to the pool when
 * fully unref'd, avoiding repeated allocation/deallocation overhead.
 *
 * @param vmaf VMAF context allocated with `vmaf_init()`.
 *
 * @param cfg  Picture configuration including dimensions and pool size.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_preallocate_pictures(VmafContext *vmaf, VmafPictureConfiguration cfg);

/**
 * Fetch a preallocated picture from the picture pool.
 * The picture must be returned to the pool via vmaf_picture_unref() when done.
 * Pictures automatically return to the pool when their reference count reaches zero.
 *
 * @param vmaf VMAF context initialized with `vmaf_preallocate_pictures()`.
 *
 * @param pic  Output picture from the pool.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_fetch_preallocated_picture(VmafContext *vmaf, VmafPicture *pic);

/**
 * Close a VMAF instance and free all associated memory.
 *
 * @param vmaf The VMAF instance to close.
 *             The pointer becomes invalid after this call returns.
 *             Callers must not dereference or pass @p vmaf to any libvmaf
 *             function after `vmaf_close()` returns.  To guard against
 *             accidental use-after-free, set the pointer to NULL immediately
 *             after calling this function:
 *
 *             @code
 *               vmaf_close(ctx);
 *               ctx = NULL;
 *             @endcode
 *
 *             Calling `vmaf_close(NULL)` returns `-EINVAL` harmlessly;
 *             however passing a dangling (already-freed) pointer is
 *             undefined behaviour and is **not** detected.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_close(VmafContext *vmaf);

/**
 * Write VMAF stats to an output file.
 *
 * @param vmaf         The VMAF context allocated with `vmaf_init()`.
 *
 * @param output_path  Output file path.
 *
 * @param fmt          Output file format.
 *                     See `enum VmafOutputFormat` for options.
 *
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_write_output(VmafContext *vmaf, const char *output_path,
                                  enum VmafOutputFormat fmt);

/**
 * @brief Write VMAF stats to an output file with a caller-controlled score format.
 *
 * Identical to `vmaf_write_output()`, but lets the caller specify the printf
 * format string used for every score value emitted to the file (XML / JSON /
 * CSV / SUB). Pass NULL to use the default format (`"%.6f"` — 6 decimal
 * places, matching the Netflix upstream default). The format string must
 * accept exactly one `double` argument (e.g. `"%.6f"`, `"%.10f"`, `"%.17g"`)
 * and is not validated; an invalid format will produce undefined output. The
 * string is read each time a score is written; callers must keep it valid for
 * the duration of the call.
 *
 * To opt into IEEE-754 round-trip lossless output pass `"%.17g"` (this is
 * what the CLI's `--precision=max` flag does internally).
 *
 * @param vmaf          The VMAF context allocated with `vmaf_init()`.
 * @param output_path   Output file path.
 * @param fmt           Output file format (see `enum VmafOutputFormat`).
 * @param score_format  printf format string for score values, or NULL to use
 *                      the default `"%.6f"`.
 *
 * @return 0 on success, or < 0 (a negative errno code) on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT int vmaf_write_output_with_format(VmafContext *vmaf, const char *output_path,
                                              enum VmafOutputFormat fmt, const char *score_format);

/**
 * @brief Introspect the compute backend active in a `VmafContext`.
 *
 * Returns the backend that was imported into @vmaf via one of the
 * `vmaf_<backend>_import_state()` family of functions.  Returns
 * `VMAF_BACKEND_UNKNOWN` (0) for CPU-only contexts where no GPU state has
 * been imported.
 *
 * @param vmaf  The VMAF context allocated with `vmaf_init()`.
 * @param out   Receives the active backend.  Must not be NULL.
 *
 * @return 0 on success, or -EINVAL when @vmaf or @out is NULL.
 */
VMAF_EXPORT int vmaf_context_get_backend(VmafContext *vmaf, enum VmafBackend *out);

/**
 * @brief Return the libvmaf version string (e.g. "3.2.1").
 *
 * @return NUL-terminated string owned by the library. Valid for the
 *         lifetime of the process; do not free.
 *
 * @thread-safety Safe to call from any thread.
 */
VMAF_EXPORT const char *vmaf_version(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_LIBVMAF_H */
