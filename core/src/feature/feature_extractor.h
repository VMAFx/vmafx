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

#ifndef __VMAF_FEATURE_EXTRACTOR_H__
#define __VMAF_FEATURE_EXTRACTOR_H__

/* In C++ mode, <stdatomic.h> does not define atomic_int as a usable type
 * on GCC/Clang or MSVC — use <atomic> + using-declaration instead.
 * In C mode keep the canonical <stdatomic.h> path (ADR-0772). */
#if defined(__cplusplus)
#include <atomic>
using std::atomic_int;
#else
#include <stdatomic.h>
#endif
#include <stdint.h>
#include <stdlib.h>

#include "config.h"
#include "dict.h"
#include "feature_characteristics.h"
#include "framesync.h"
#include "feature_collector.h"
#include "opt.h"

#include "libvmaf/picture.h"

#ifdef HAVE_CUDA
#include "cuda/common.h"
#endif

enum VmafFeatureExtractorFlags {
    VMAF_FEATURE_EXTRACTOR_TEMPORAL = 1 << 0,
    VMAF_FEATURE_EXTRACTOR_CUDA = 1 << 1,
    VMAF_FEATURE_FRAME_SYNC = 1 << 2,
    VMAF_FEATURE_EXTRACTOR_PREV_REF = 1 << 3,
    VMAF_FEATURE_EXTRACTOR_SYCL = 1 << 4,
    VMAF_FEATURE_EXTRACTOR_VULKAN = 1 << 5,
    /* Reserved for the HIP runtime PR (T7-10b). The first-consumer PR
     * (T7-10 / ADR-0241) registers `vmaf_fex_psnr_hip` without setting
     * this bit — the picture buffer-type plumbing for HIP arrives with
     * the runtime. The bit number is reserved here so the runtime PR
     * can adopt it without an enum reshuffle. */
    VMAF_FEATURE_EXTRACTOR_HIP = 1 << 6,
    /* Metal runtime (T8-1 / ADR-0421). Set on every registered
     * Obj-C++ extractor under feature/metal/ so that the serial
     * flush path drains their final-frame collect() — mirrors the
     * HIP / SYCL drain branches in flush_context_serial. */
    VMAF_FEATURE_EXTRACTOR_METAL = 1 << 7,
};

typedef struct VmafFeatureExtractor {
    const char *name; ///< Name of feature extractor.
    /**
     * Initialization callback. Optional, preallocate fex->priv buffers here.
     *
     * @param     fex self.
     * @param pix_fmt VmafPixelFormat of all subsequent pictures.
     * @param     bpc Bitdepth of all subsequent pictures.
     * @param       w Width of all subsequent pictures.
     * @param       h Height of all subsequent pictures.
     */
    int (*init)(struct VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                unsigned w, unsigned h);
    /**
     * Feature extraction callback. Called for every pair of pictures. Unless
     * the VMAF_FEATURE_EXTRACTOR_TEMPORAL flag is set, there is no guarantee
     * that this callback is called in any specific order.
     *
     *
     * @param               fex self.
     * @param           ref_pic Reference VmafPicture.
     * @param        ref_pic_90 Reference VmafPicture, translated 90 degrees.
     * @param          dist_pic Distorted VmafPicture.
     * @param       dist_pic_90 Distorted VmafPicture, translated 90 degrees.
     * @param             index Picture index.
     * @param feature_collector VmafFeatureCollector used to write out scores.
     */
    int (*extract)(struct VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector);
    /**
     * Buffer flush callback. Optional.
     * Called only when the VMAF_FEATURE_EXTRACTOR_TEMPORAL flag is set.
     *
     * @param               fex self.
     * @param feature_collector VmafFeatureCollector used to write out scores.
     */
    int (*flush)(struct VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector);
    /**
     * Close callback. Optional, clean up fex->priv buffers here.
     *
     * @param               fex self.
     */
    int (*close)(struct VmafFeatureExtractor *fex);
    /**
     * Async submit callback. Optional. When set, the framework calls submit()
     * for all GPU extractors first (recording & submitting GPU work without
     * blocking), then calls collect() to wait for results and write scores.
     * This allows the CPU to overlap command preparation across extractors.
     *
     * Parameters mirror extract(), except feature_collector is deferred.
     */
    int (*submit)(struct VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                  VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index);
    /**
     * Async collect callback. Called after submit() to wait for GPU completion,
     * download results, and write scores to the feature collector.
     */
    int (*collect)(struct VmafFeatureExtractor *fex, unsigned index,
                   VmafFeatureCollector *feature_collector);
    const VmafOption *options;      ///< Optional initialization options.
    void *priv;                     ///< Custom data.
    size_t priv_size;               ///< sizeof private data.
    uint64_t flags;                 ///< Feauture extraction flags, binary or'd.
    const char **provided_features; ///< Provided feature list, NULL terminated.

#ifdef HAVE_CUDA
    VmafCudaState *cu_state; ///< VmafCudaState, set by framework
#endif
#ifdef HAVE_SYCL
    struct VmafSyclState *sycl_state; ///< VmafSyclState, set by framework
#endif
    /* HAVE_VULKAN block removed per ADR-0726 (Vulkan backend dropped
     * 2026-05-28). The VMAF_FEATURE_EXTRACTOR_VULKAN bit below is kept
     * as a reserved gap to preserve the ABI numbering of subsequent
     * flag values (notably _HIP and _METAL). */

    VmafFrameSyncContext *framesync;
    VmafPicture prev_ref; ///< Previous reference picture, set by framework.

    /**
     * Per-feature characteristics descriptor — drives the per-backend
     * dispatch_strategy modules in core/src/{cuda,sycl,hip,metal}/.
     * Defaults to all-zero (= no preference) for unseeded extractors;
     * backends fall back to current global behaviour. See ADR-0181.
     */
    VmafFeatureCharacteristics chars;

} VmafFeatureExtractor;

#ifdef __cplusplus
extern "C" {
#endif

VmafFeatureExtractor *vmaf_get_feature_extractor_by_name(const char *name);
VmafFeatureExtractor *vmaf_get_feature_extractor_by_feature_name(const char *name, unsigned flags);

/* ADR-0544: Audit feature_extractor_list[] for accidental duplicate
 * registrations (same `name` string registered more than once).
 * Returns 0 if every entry is unique, -EINVAL otherwise.  Each
 * duplicate is logged at VMAF_LOG_LEVEL_ERROR.  Called once from
 * `vmaf_init()` so a regression caught locally fails fast instead of
 * silently doubling pool-entry init / extract / flush counts. */
int vmaf_feature_extractor_list_audit(void);

/**
 * @brief Check whether a feature extractor supports all options in @p opts_dict.
 *
 * @param fex         Feature extractor descriptor.
 * @param opts_dict   Dictionary of options to validate (may be NULL).
 * @param missing_key If non-NULL, receives a pointer to the first unsupported key
 *                    (or NULL if all options are supported).
 * @return true if all options are supported (or opts_dict is NULL/empty), false otherwise.
 */
bool vmaf_feature_extractor_supports_options(const VmafFeatureExtractor *fex,
                                             const VmafDictionary *opts_dict,
                                             const char **missing_key);

enum VmafFeatureExtractorContextFlags {
    VMAF_FEATURE_EXTRACTOR_CONTEXT_DO_NOT_OVERWRITE = 1 << 0,
};

typedef struct VmafFeatureExtractorContext {
    bool is_initialized, is_closed;
    VmafDictionary *opts_dict;
    VmafFeatureExtractor *fex;
    bool gpu_pending;           ///< Has pending GPU submit awaiting collect
    unsigned gpu_pending_index; ///< Frame index of pending GPU work
} VmafFeatureExtractorContext;

int vmaf_feature_extractor_context_create(VmafFeatureExtractorContext **fex_ctx,
                                          VmafFeatureExtractor *fex, VmafDictionary *opts_dict);

int vmaf_feature_extractor_context_init(VmafFeatureExtractorContext *fex_ctx,
                                        enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                                        unsigned h);

int vmaf_feature_extractor_context_extract(VmafFeatureExtractorContext *fex_ctx, VmafPicture *ref,
                                           VmafPicture *ref_90, VmafPicture *dist,
                                           VmafPicture *dist_90, unsigned pic_index,
                                           VmafFeatureCollector *vfc);

int vmaf_feature_extractor_context_submit(VmafFeatureExtractorContext *fex_ctx, VmafPicture *ref,
                                          VmafPicture *ref_90, VmafPicture *dist,
                                          VmafPicture *dist_90, unsigned pic_index);

// Submit for zero-copy GPU path: no VmafPicture needed.
// Caller must ensure extractor is initialized via
// vmaf_feature_extractor_context_init() before first call.
int vmaf_feature_extractor_context_submit_nocopy(VmafFeatureExtractorContext *fex_ctx,
                                                 unsigned pic_index);

int vmaf_feature_extractor_context_collect(VmafFeatureExtractorContext *fex_ctx, unsigned pic_index,
                                           VmafFeatureCollector *vfc);

int vmaf_feature_extractor_context_flush(VmafFeatureExtractorContext *fex_ctx,
                                         VmafFeatureCollector *vfc);

int vmaf_feature_extractor_context_close(VmafFeatureExtractorContext *fex_ctx);

int vmaf_feature_extractor_context_delete(VmafFeatureExtractorContext *fex_ctx);

int vmaf_feature_extractor_context_destroy(VmafFeatureExtractorContext *fex_ctx);

/* Hoisted out of VmafFeatureExtractorContextPool so that C++ TUs
 * (e.g. feature_extractor.cpp) can reference struct fex_list_entry
 * by its unqualified tag — in C++ a struct tag nested inside a
 * typedef struct is scoped to that struct (ADR-0772). */
struct fex_list_entry {
    VmafFeatureExtractor *fex;
    VmafDictionary *opts_dict;
    struct {
        VmafFeatureExtractorContext *fex_ctx;
        bool in_use;
    } *ctx_list;
    atomic_int capacity, in_use;
    pthread_cond_t full;
};

typedef struct VmafFeatureExtractorContextPool {
    struct fex_list_entry *fex_list;
    unsigned cnt, capacity;
    pthread_mutex_t lock;
    unsigned n_threads;
} VmafFeatureExtractorContextPool;

int vmaf_fex_ctx_pool_create(VmafFeatureExtractorContextPool **pool, unsigned n_threads);

int vmaf_fex_ctx_pool_aquire(VmafFeatureExtractorContextPool *pool, VmafFeatureExtractor *fex,
                             VmafDictionary *opts_dict, VmafFeatureExtractorContext **fex_ctx);

int vmaf_fex_ctx_pool_release(VmafFeatureExtractorContextPool *pool,
                              VmafFeatureExtractorContext *fex_ctx);

int vmaf_fex_ctx_pool_flush(VmafFeatureExtractorContextPool *pool,
                            VmafFeatureCollector *feature_collector);

int vmaf_fex_ctx_pool_destroy(VmafFeatureExtractorContextPool *pool);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __VMAF_FEATURE_EXTRACTOR_H__ */
