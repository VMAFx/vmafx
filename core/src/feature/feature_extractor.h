/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

/* NOLINTBEGIN(modernize-deprecated-headers,modernize-use-using,performance-enum-size,bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp):
 * clang-tidy has no compile command for a header, so it analyses this one
 * under the nearest matching translation unit — `feature_extractor.cpp` —
 * and therefore parses every construct below as C++. This is a C header:
 * roughly a hundred C translation units include it (every `.c` feature
 * extractor under `core/src/feature/` and its CUDA / HIP registrations,
 * `core/src/libvmaf.c`, `core/src/predict.c`, `core/src/model.c`), so none
 * of the rewrites clang-tidy proposes is expressible here.
 * `modernize-deprecated-headers` (<cstdint>, <cstdlib>) and
 * `modernize-use-using` (`using` in place of `typedef`) are C++-only
 * spellings; C has neither. `performance-enum-size` wants a fixed underlying
 * type on the two flag enums — C23 spells that `enum E : uint8_t`, but MSVC
 * does not document it under `/std:clatest` and `Build — Windows
 * (MSVC + CUDA)` is a required check, the same unverifiable-on-a-required-lane
 * constraint ADR-1138 records for C `nullptr`.
 * `bugprone-reserved-identifier` / `cert-dcl37-c` / `cert-dcl51-cpp` fire on
 * the `__VMAF_FEATURE_EXTRACTOR_H__` include guard: that spelling is upstream
 * Netflix's and is shared by every header under `core/src/`, so renaming one
 * file's guard buys nothing and costs upstream-sync parity — the posture
 * ADR-0148 and ADR-0150 already record for upstream-mirror identifiers.
 * Applied to this file under ADR-0141 because this PR touches it, and
 * file-scoped rather than eight separate NOLINTNEXTLINE markers because that
 * is the shape ADR-1138 prescribes for the same C-parsed-as-C++ artefact in
 * C sources. */
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
    /* Rust twin of a CPU extractor (ADR-1713), registered by
     * core/src/rust/shim/rust_twins.cpp as `<c name>_rust`. Lookup by feature
     * name skips it unless this flag is requested, so the C extractor stays
     * the default; vmaf_feature_extractor_impl_select() swaps it in. */
    VMAF_FEATURE_EXTRACTOR_RUST = 1 << 8,
};

struct VmafFeatureExtractorContext;

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
     * Advance callback. Optional, for an extractor that writes the score of a
     * frame only once later frames are in (the motion2 / motion3 window,
     * ADR-2090): append every such score the collector's contents now make
     * final, never one that a later frame could still change. The engine calls
     * it on the thread that feeds frames, on the registered extractor, after
     * each frame it accepts and after a read fence; never at the same time as
     * this instance's extract(), collect() or flush(). With worker threads that
     * instance's init() has not run: build what the call needs on first use.
     * flush() appends the rest. Returns 0 or a negative errno.
     *
     * @param               fex self.
     * @param feature_collector VmafFeatureCollector used to read and write scores.
     */
    int (*advance)(struct VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector);
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
    /**
     * Name of a score the extractor files without the option suffix when its
     * `debug` option is set, or NULL. Two contexts that claim the same name
     * would write one key twice, so vmaf_use_feature() refuses the second
     * (ADR-2056). The unsuffixed name is a contract the Netflix tests read.
     */
    const char *unsuffixed_debug_key;

#ifdef HAVE_CUDA
    VmafCudaState *cu_state; ///< VmafCudaState, set by framework
#endif
#ifdef HAVE_SYCL
    struct VmafSyclState *sycl_state; ///< VmafSyclState, set by framework
#endif
#ifdef HAVE_HIP
    /** The frame planes the HIP twins of a context share (ADR-1408), set by
     *  the framework; NULL when the extractor is driven without a context,
     *  and it then uploads into buffers of its own. */
    struct VmafHipSharedFrame *hip_frame;
    /** The HIP device the twin creates its context on
     *  (vmaf_hip_context_new()), set by the framework from the context's
     *  imported VmafHipState; 0 when no state is imported, the device
     *  vmaf_hip_state_init() picks for -1. */
    int hip_device_index;
#endif
    /* HAVE_VULKAN block removed per ADR-0726 (Vulkan backend dropped
     * 2026-05-28). The VMAF_FEATURE_EXTRACTOR_VULKAN bit below is kept
     * as a reserved gap to preserve the ABI numbering of subsequent
     * flag values (notably _HIP and _METAL). */

    VmafFrameSyncContext *framesync;
    VmafPicture prev_ref; ///< Previous reference picture, set by framework.
    /** Reference picture from two frames ago (n-2), set by the framework for
     *  a VMAF_FEATURE_EXTRACTOR_PREV_REF extractor whose reads_prev_prev_ref()
     *  answers true; empty before frame 2 and for every other extractor
     *  (Netflix a2b59b77, ADR-1478). */
    VmafPicture prev_prev_ref;
    /**
     * Optional, for VMAF_FEATURE_EXTRACTOR_PREV_REF extractors: whether this
     * instance, with the options it was given, reads `prev_prev_ref`. Called
     * after the options are parsed into `priv`, before init(). The context
     * keeps the reference picture of frame n-2, and a preallocated picture
     * pool must then hold at least four pictures, only while a registered
     * extractor answers true (ADR-1478). NULL means it does not.
     */
    bool (*reads_prev_prev_ref)(const struct VmafFeatureExtractor *fex);

    /**
     * Optional, for VMAF_FEATURE_EXTRACTOR_SYCL extractors: whether this
     * instance, with the options it was given, computes from the luma plane
     * of the shared device frame alone. Only such an extractor runs on the
     * zero-copy path (vmaf_read_pictures_sycl()), which hands no host picture
     * and has no chroma on the device; every other registered extractor makes
     * that call fail with -ENOTSUP and an error naming it (ADR-1688). Called
     * after the options are parsed into `priv`, before init(). NULL means the
     * extractor reads host pictures (chroma, or a host copy of luma).
     */
    bool (*reads_shared_luma_only)(const struct VmafFeatureExtractor *fex);

    /**
     * Optional (Netflix/vmaf 33e5f0aca, ADR-2795). At registration, when
     * `incoming` would become a second context of this same extractor with
     * other options, the registry offers it to each registered context of the
     * same name that is not yet initialized. Return 1 when `existing` absorbed
     * `incoming` (the registry then destroys `incoming`, as for a duplicate), 0
     * to decline, or a negative errno. Integer ADM folds a second viewing
     * distance into one context this way.
     */
    int (*merge)(struct VmafFeatureExtractorContext *existing,
                 struct VmafFeatureExtractorContext *incoming);

    /**
     * Optional (ADR-2795): add entries to the feature-name dictionary an
     * instance built from `provided_features` with its parsed options in
     * `priv`. Called by the extractor's own init() and by the Rust twin shim,
     * so both file the same names. Returns 0 or a negative errno.
     */
    int (*extend_name_dict)(const struct VmafFeatureExtractor *fex, VmafDictionary **dict);

    /**
     * Per-feature characteristics descriptor — drives the per-backend
     * dispatch_strategy modules in core/src/{cuda,sycl,hip,metal}/.
     * Defaults to all-zero (= no preference) for unseeded extractors;
     * backends fall back to current global behaviour. See ADR-0181.
     */
    VmafFeatureCharacteristics chars;

    /**
     * Optional first-frame capability check. The callback runs after options
     * have been parsed but before backend initialization, when actual picture
     * dimensions are known. Return -ENOTSUP to request the named CPU fallback
     * for a model-selected context; direct extractor selection ignores this
     * hook and retains the backend init error contract (ADR-1324).
     */
    int (*context_check)(struct VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                         unsigned bpc, unsigned w, unsigned h);
    const char *context_fallback_name;

} VmafFeatureExtractor;

#ifdef __cplusplus
extern "C" {
#endif

VmafFeatureExtractor *vmaf_get_feature_extractor_by_name(const char *name);
VmafFeatureExtractor *vmaf_get_feature_extractor_by_feature_name(const char *name, unsigned flags);

/**
 * @brief Find the device twin of a CPU feature extractor (ADR-1359).
 *
 * Looks up each feature @p cpu_fex provides, in order, with
 * vmaf_get_feature_extractor_by_feature_name() and returns the first result
 * that carries one of @p flags.
 *
 * @return the twin, or NULL when @p cpu_fex is NULL, @p flags is 0, or no
 *         extractor carrying one of @p flags provides any of its features.
 */
VmafFeatureExtractor *vmaf_get_feature_extractor_twin(const VmafFeatureExtractor *cpu_fex,
                                                      unsigned flags);

/**
 * @brief Count the device twins no CPU extractor reaches (ADR-1359).
 *
 * `--backend <gpu> --feature <cpu name>` and model dispatch find a device
 * twin through the features its CPU extractor declares in
 * `provided_features`. A twin whose feature names no CPU extractor declares
 * is registered but only runs when named, and the CPU extractor runs in its
 * place with a "no twin" warning. Each such twin is logged at
 * VMAF_LOG_LEVEL_ERROR.
 *
 * @return the number of extractors carrying a CUDA, SYCL, HIP or Metal flag
 *         for which vmaf_get_feature_extractor_twin() returns them for no
 *         CPU extractor; 0 when every twin is reachable.
 */
int vmaf_feature_extractor_twin_audit(void);

/**
 * @brief Extractor at index @p i of the Rust registry, or NULL past its end.
 */
typedef VmafFeatureExtractor *(*VmafRustExtractorAtFn)(unsigned i);

/**
 * @brief Make the Rust extractors part of the registry (ADR-1713).
 *
 * Called by vmaf_rust_twins_install() (core/src/rust/shim/rust_twins.cpp,
 * built with enable_rust_features only). Every registry walk in this file then
 * visits the static list and then @p rust_at(0), @p rust_at(1), ... until it
 * returns NULL. A build without Rust never calls it, and nothing in this file
 * refers to a Rust symbol.
 */
void vmaf_feature_extractor_install_rust_registry(VmafRustExtractorAtFn rust_at);

/**
 * @brief Apply VMAF_FEATURE_IMPL to an extractor chosen for registration.
 *
 * `VMAF_FEATURE_IMPL` unset or `c`: @p *selected = @p fex. `rust`: a CPU
 * extractor (no device or Rust flag) is replaced by its Rust twin
 * (vmaf_get_feature_extractor_twin(fex, VMAF_FEATURE_EXTRACTOR_RUST)), logged
 * at INFO; without a twin the C extractor stays and a WARNING names it.
 * Device twins and explicitly named Rust twins are kept. The variable is read
 * once per process (vmaf_gpu_dispatch_env_get()).
 *
 * @return 0, or -EINVAL for NULL arguments or another value of the variable.
 */
int vmaf_feature_extractor_impl_select(VmafFeatureExtractor *fex, VmafFeatureExtractor **selected);

/**
 * @brief The implementation VMAF_FEATURE_IMPL asks for (ADR-1713).
 *
 * One reader of the variable for every Rust path (extractors, prediction).
 *
 * @return 0 when it is unset, empty or `c`; 1 when it is `rust`; -EINVAL
 *         (logged) for another value.
 */
int vmaf_feature_impl_rust_requested(void);

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

/**
 * @brief Check option names and extractor-specific value capabilities.
 *
 * Invalid values remain the normal option parser's responsibility. This
 * helper reports only valid values that a declared default-only option cannot
 * execute, allowing model-driven GPU selection to fall back to the CPU twin.
 *
 * @param fex             Feature extractor descriptor.
 * @param opts_dict       Dictionary of options to validate (may be NULL).
 * @param unsupported_key If non-NULL, receives the first unknown option or
 *                        valid value that the extractor cannot execute.
 * @return true when every named option can be executed by @p fex.
 */
bool vmaf_feature_extractor_honours_options(const VmafFeatureExtractor *fex,
                                            const VmafDictionary *opts_dict,
                                            const char **unsupported_key);

/**
 * @brief Whether @p fex reads the reference picture of frame n-2.
 *
 * True only for a VMAF_FEATURE_EXTRACTOR_PREV_REF extractor whose
 * reads_prev_prev_ref() answers true for the options in its `priv`
 * (ADR-1478). False for NULL.
 */
bool vmaf_feature_extractor_reads_prev_prev_ref(const VmafFeatureExtractor *fex);

/**
 * @brief Whether @p fex runs on the SYCL zero-copy path.
 *
 * True only for a VMAF_FEATURE_EXTRACTOR_SYCL extractor whose
 * reads_shared_luma_only() answers true for the options in its `priv`
 * (ADR-1688). False for NULL, for a CPU extractor and for a SYCL extractor
 * without the hook.
 */
bool vmaf_feature_extractor_reads_shared_luma_only(const VmafFeatureExtractor *fex);

enum VmafFeatureExtractorContextFlags {
    VMAF_FEATURE_EXTRACTOR_CONTEXT_DO_NOT_OVERWRITE = 1 << 0,
};

typedef struct VmafFeatureExtractorContext {
    bool is_initialized, is_closed;
    /**
     * A close callback owns state created by an init attempt.
     *
     * Set before invoking a CUDA fex->init so a failed, partially-complete
     * device initialization is still visible to teardown. Cleared only after
     * close succeeds. This is intentionally distinct from is_initialized,
     * which becomes true only after init completes successfully. Non-CUDA
     * close callbacks retain their established successful-init-only contract.
     */
    bool close_required;
    VmafDictionary *opts_dict;
    VmafFeatureExtractor *fex;
    bool allow_context_fallback; ///< Model dispatch may replace an unsupported GPU twin (ADR-1324)
    bool gpu_pending;            ///< Has pending GPU submit awaiting collect
    unsigned gpu_pending_index;  ///< Frame index of pending GPU work
    /**
     * The extractor registered with submit() and collect(): libvmaf drives it
     * through the double-buffer path on the thread that calls
     * vmaf_read_pictures(), and the worker pool must never run it, whatever
     * backend flag it carries (a twin that is reachable by name only, such as
     * adm_hip, carries none). Decided once, when the context is created, so
     * the caller's dispatch loop and the workers agree for the context's
     * whole life even if init() swaps the callbacks.
     */
    bool caller_thread_dispatch;
} VmafFeatureExtractorContext;

int vmaf_feature_extractor_context_create(VmafFeatureExtractorContext **fex_ctx,
                                          const VmafFeatureExtractor *fex,
                                          VmafDictionary *opts_dict);

/**
 * The unsuffixed debug key @p fex_ctx will write (VmafFeatureExtractor::
 * unsuffixed_debug_key), or NULL when the extractor declares none or its
 * `debug` option is off. Reads the options the context parsed at creation.
 */
const char *vmaf_feature_extractor_context_debug_key(const VmafFeatureExtractorContext *fex_ctx);

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
 * typedef struct is scoped to that struct (ADR-0772).
 *
 * ADR-0772: this C-compatible layout has no C++ constructor. Every slot is
 * separately allocated by get_fex_list_entry() and value-initialized by
 * init_fex_list_slot(), which stores both atomics and initializes the
 * condition variable before publication. Keep cppcheck's official POSIX
 * library model enabled for the pthread fields; preserve uninitialized-use
 * checks. See docs/research/fex-pool-growth-2026-09-08.md. Entries remain at
 * fixed addresses until pool destruction, including across waits. Each entry
 * owns a by-value descriptor snapshot; framework-managed runtime pointers are
 * refreshed under the pool lock before lazy context creation.
 * Consumer TUs such as fex_ctx_vector.cpp cannot see the factory assignments;
 * their std::atomic members trigger constructor analysis of these four raw
 * fields. Suppress only that declaration warning, not uninitialized reads. */
struct fex_list_entry {
    // cppcheck-suppress uninitMemberVarNoCtor
    VmafFeatureExtractor fex;
    // cppcheck-suppress uninitMemberVarNoCtor
    VmafDictionary *opts_dict;
    struct {
        VmafFeatureExtractorContext *fex_ctx;
        bool in_use;
        // cppcheck-suppress uninitMemberVarNoCtor
    } *ctx_list;
    atomic_int capacity, in_use;
    // cppcheck-suppress uninitMemberVarNoCtor
    pthread_cond_t full;
};

/* ADR-0772: vmaf_fex_ctx_pool_create() zero-initializes this C-compatible
 * aggregate, then initializes capacity, thread count and mutex before a
 * successful return. No other construction site exists in the source tree. */
typedef struct VmafFeatureExtractorContextPool {
    struct fex_list_entry **fex_list;
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

/** Close every initialized or partially initialized context without freeing ownership. */
int vmaf_fex_ctx_pool_close(VmafFeatureExtractorContextPool *pool);

/** Destroy a pool only after vmaf_fex_ctx_pool_close() has succeeded. */
int vmaf_fex_ctx_pool_destroy(VmafFeatureExtractorContextPool *pool);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __VMAF_FEATURE_EXTRACTOR_H__ */
/* NOLINTEND(modernize-deprecated-headers,modernize-use-using,performance-enum-size,bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
