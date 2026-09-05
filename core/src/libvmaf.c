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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

/* Provide a dummy fallback for older glibc versions (< 2.32) that lack
 * __libc_single_threaded.  The weak attribute lets the real glibc symbol
 * take precedence when available. */
#ifdef __linux__
/* The name is dictated by glibc's ABI and must match exactly. */
/* NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage) */
__attribute__((weak)) char __libc_single_threaded = 1;
#endif

#include "libvmaf/libvmaf.h"
#include "libvmaf/feature.h"
#include "libvmaf/perceptual_weight.h"
#include "libvmaf/picture.h"

#include "cpu.h"
#include "dnn/dnn_ctx.h"
#include "dnn/tensor_io.h"
#include "feature/feature_extractor.h"
#include "feature/feature_collector.h"
#include "feature/perceptual_weight.h"
#include "metadata_handler.h"
#include "fex_ctx_vector.h"
#include "libvmaf_priv.h"
#include "log.h"
#include "model.h"
#include "output.h"
#include "picture.h"
#include "pooling_percentile.h"
#include "predict.h"
#include "thread_pool.h"
#include "vcs_version.h"

#ifdef HAVE_CUDA
#include "libvmaf/libvmaf_cuda.h"

#include "cuda/common.h"
#include "cuda/cuda_helper.cuh"
#include "cuda/drain_batch.h"
#include "cuda/picture_cuda.h"
#include "gpu_picture_pool.h"
#endif

#include "picture_pool.h"

#ifdef HAVE_SYCL
#include "libvmaf/libvmaf_sycl.h"
#include "sycl/common.h"
#include "sycl/picture_sycl.h"
#endif

#ifdef HAVE_METAL
#include "libvmaf/libvmaf_metal.h"
#include "metal/import.h"
#endif

#ifdef HAVE_HIP
#include "libvmaf/libvmaf_hip.h"
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is an
 * upstream-mirror file whose Netflix source spells the null pointer constant
 * `NULL` (every upstream sync would re-conflict against a keyword rewrite) and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef struct VmafContext {
    VmafConfiguration cfg;
    VmafFeatureCollector *feature_collector;
    RegisteredFeatureExtractors registered_feature_extractors;
    VmafFeatureExtractorContextPool *fex_ctx_pool;
    VmafThreadPool *thread_pool;
    VmafFrameSyncContext *framesync;
    VmafPicturePool *picture_pool;
#ifdef HAVE_CUDA
    struct {
        struct {
            struct {
                unsigned w, h;
                unsigned bpc;
                enum VmafPixelFormat pix_fmt;
            } pic_params;
            enum VmafCudaPicturePreallocationMethod pic_prealloc_method;
            int device_id;
            int stream_priority;
        } cfg;
        VmafCudaState state;
        VmafCudaCookie cookie;
        VmafGpuPicturePool *ring_buffer;
    } cuda;
    /* Cached result of rfe_hw_flags() (F2-B, perf-audit-pipeline-2026-05-16).
     * Recomputed lazily whenever vmaf_use_feature() registers a new extractor.
     * Avoids an O(n_extractors) linear scan on every frame in the common case
     * where the extractor set is fixed before the frame loop starts. */
    unsigned rfe_hw_flags_cache;
    bool rfe_hw_flags_dirty;
#endif
#ifdef HAVE_SYCL
    struct {
        VmafSyclState *state;
        VmafSyclPicturePool *pool;
    } sycl;
#endif
#ifdef HAVE_METAL
    /* T8-IOS (ADR-0423): caller-imported MTLDevice + IOSurface ring.
     * Ownership stays with the caller — vmaf_metal_state_free()
     * after vmaf_close(), same lifetime model as the SYCL and HIP
     * backends. */
    struct {
        VmafMetalState *state;
    } metal;
#endif
#ifdef HAVE_HIP
    /* ADR-0519: caller-imported HIP state. Same lifetime model as the
     * SYCL / Metal backends — vmaf_hip_state_free() after
     * vmaf_close(). The HIP feature extractors do not yet set the
     * VMAF_FEATURE_EXTRACTOR_HIP flag, so dispatch routes them through
     * their CPU twins; storing the state here is the wiring that
     * unblocks `vmaf --backend hip` end-to-end and the future
     * picture-buffer-type plumbing that flips the flag on. */
    struct {
        VmafHipState *state;
    } hip;
#endif
    struct {
        unsigned w, h;
        enum VmafPixelFormat pix_fmt;
        unsigned bpc;
        enum VmafPictureBufferType buf_type;
    } pic_params;
    unsigned pic_cnt;
    bool flushed;
    /* Active compute backend — set by vmaf_<backend>_import_state().
     * Zero-initialised (VMAF_BACKEND_UNKNOWN) for CPU-only contexts. */
    enum VmafBackend active_backend;
    /* Track the most recent index accepted by vmaf_read_pictures so
     * subsequent calls can enforce a monotonically-increasing index.
     * Several feature extractors (integer_motion's 3-frame blur
     * ring, motion2/motion3 sliding windows) keep internal state
     * keyed by `index % N`; submitting frames out of order silently
     * corrupts that state (Netflix#910, ADR-0152). Zero-initialised
     * is fine — `have_last_index` guards the first call. */
    unsigned last_index;
    bool have_last_index;
    VmafPicture prev_ref; // previous ref pic for PREV_REF extractors (in-order only)
    struct {
        VmafOrtSession *sess;
        VmafModelSidecar meta;
        bool has_sidecar;
        /* Tiny model input rank: 4 = NCHW image (legacy path), 2 =
         * feature-vector model (ADR-0517). */
        size_t in_rank;
        /* NCHW path: expected image dimensions. */
        int expected_w;
        int expected_h;
        /* Feature-vector path: number of features the model expects
         * (the second dim of the rank-2 input). */
        size_t n_features;
        /* Feature-vector path: number of extra inputs beyond
         * `features` (e.g. fr_regressor_v2 carries a 14-D `codec`
         * block as a second input). Zero-filled when the consumer
         * does not have a populated codec block — see
         * vmaf_ctx_dnn_run_frame. */
        size_t extra_in_width;
        float *in_buf; /* NCHW: w*h floats. feature-vec: n_features floats. */
        size_t in_elements;
        /* Scratch buffer for the optional second input (codec block).
         * NULL when the model has only one input. */
        float *extra_in_buf;
        char *feature_name; /* owned; published via feature_collector */
        size_t n_outputs;
        char *output_feature_names[VMAF_ORT_MAX_IO]; /* owned collector keys */
        /* ADR-0550 — NCHW dispatch auto-resize. Default (zero-init via
         * memset in vmaf_init) is VMAF_TINY_RESIZE_DISABLED == 0.
         * Operator must explicitly call vmaf_dnn_set_resize_mode() (or
         * pass --tiny-resize) to enable bilinear/nearest/bicubic.
         * When DISABLED (default), a size mismatch returns -ERANGE (the
         * pre-ADR-0550 behaviour, preserved for parity harnesses).
         * Cast through VmafTinyResize at the dispatch site. */
        int resize_mode;
    } dnn;
    /* Pelorus-driven perceptual spatial-pooling weighting (ADR-1118). The
     * zero-initialised state is "disabled, empty store" — fully inert, so the
     * default scoring path (and the Netflix golden pairs) is byte-identical to
     * a build without this feature. Mutated only by the
     * vmaf_set_perceptual_* entry points; read only in the pooling path. */
    VmafPerceptualWeightStore perceptual;
} VmafContext;

typedef struct BatchThreadData {
    VmafFeatureExtractorContext **fex_ctx;
    unsigned cnt;
} BatchThreadData;

static void batch_thread_data_free(void *data)
{
    BatchThreadData *td = data;
    for (unsigned i = 0; i < td->cnt; i++) {
        if (td->fex_ctx[i]) {
            (void)vmaf_feature_extractor_context_close(td->fex_ctx[i]);
            (void)vmaf_feature_extractor_context_destroy(td->fex_ctx[i]);
        }
    }
    free((void *)td->fex_ctx);
    free(td);
}

/* Bring up the worker thread pool plus the per-thread extractor-context pool
 * when the caller asked for worker threads. A failure of the second pool
 * tears the first one down again so the caller has nothing to unwind. */
static int vmaf_ctx_thread_pools_init(VmafContext *v)
{
    if (v->cfg.n_threads == 0)
        return 0;

    VmafThreadPoolConfig tpool_cfg = {
        .n_threads = v->cfg.n_threads,
        .thread_data_free = batch_thread_data_free,
    };
    int err = vmaf_thread_pool_create(&v->thread_pool, tpool_cfg);
    if (err)
        return err;
    err = vmaf_fex_ctx_pool_create(&v->fex_ctx_pool, v->cfg.n_threads);
    if (err) {
        (void)vmaf_thread_pool_destroy(v->thread_pool);
        v->thread_pool = NULL;
    }
    return err;
}

/* Bring up the per-context subsystems in dependency order. On failure every
 * subsystem created so far is torn down again in reverse order and the
 * originating error code is returned, so vmaf_init() only has to free `v`. */
static int vmaf_ctx_subsystems_init(VmafContext *v)
{
    /* ADR-0544: catch accidental duplicate extractor registrations
     * fast.  The static `feature_extractor_list[]` should hold each
     * extractor exactly once; a duplicate doubles ctx-pool entries and
     * runs init/extract/flush twice per pic.  Run the audit before any
     * other state is touched. */
    int err = vmaf_feature_extractor_list_audit();
    if (err)
        return err;

    err = vmaf_framesync_init(&(v->framesync));
    if (err)
        return err;
    err = vmaf_feature_collector_init(&(v->feature_collector));
    if (err)
        goto free_framesync;
    err = feature_extractor_vector_init(&(v->registered_feature_extractors));
    if (err)
        goto free_feature_collector;
    err = vmaf_ctx_thread_pools_init(v);
    if (err)
        goto free_feature_extractor_vector;
    return 0;

free_feature_extractor_vector:
    feature_extractor_vector_destroy(&(v->registered_feature_extractors));
free_feature_collector:
    vmaf_feature_collector_destroy(v->feature_collector);
free_framesync:
    (void)vmaf_framesync_destroy(v->framesync);
    return err;
}

int vmaf_init(VmafContext **vmaf, VmafConfiguration cfg)
{
    if (!vmaf)
        return -EINVAL;
    /* Guard against double-init: if the caller passes a non-NULL *vmaf the
     * old context would be silently overwritten and leak.  Returning -EINVAL
     * surfaces the bug immediately instead of leaking memory on every
     * subsequent initialisation path. */
    if (*vmaf)
        return -EINVAL;

    VmafContext *const v = malloc(sizeof(*v));
    if (!v)
        return -ENOMEM;
    memset(v, 0, sizeof(*v));
    v->cfg = cfg;
#ifdef HAVE_CUDA
    v->rfe_hw_flags_dirty = true; /* force recompute before the first frame */
#endif

    vmaf_init_cpu();
    /* cpumask is uint64_t in the public API; the internal mask is unsigned
     * (all defined flag bits fit in 32 bits). Truncate explicitly so that
     * -fsanitize=integer does not fire on the implicit narrowing. */
    vmaf_set_cpu_flags_mask((unsigned)(~cfg.cpumask));

    vmaf_set_log_level(cfg.log_level);

    const int err = vmaf_ctx_subsystems_init(v);
    if (err) {
        /* The caller's handle stays NULL (checked above) so it cannot be
         * passed to vmaf_close() or dereferenced after a failed vmaf_init()
         * — CERT MEM30-C. Return the failing sub-init's own code rather than
         * a hardcoded -ENOMEM so callers can distinguish OOM from a
         * configuration error (CERT ERR33-C). */
        free(v);
        return err;
    }

    *vmaf = v;
    return 0;
}

#ifdef HAVE_CUDA
static int prepare_ring_buffer(VmafContext *vmaf, unsigned w, unsigned h,
                               enum VmafPixelFormat pix_fmt, unsigned bpc)
{
    if (!vmaf)
        return -EINVAL;
    if (!w)
        return -EINVAL;
    if (!h)
        return -EINVAL;
    if (!pix_fmt)
        return -EINVAL;
    if (!bpc)
        return -EINVAL;
    /* Reject out-of-range dimensions at pool-config time — parity with
     * vmaf_picture_alloc's VMAF_PIC_DIM_MAX guard (picture.c). Fails fast
     * here rather than at first per-frame allocation; the per-allocator
     * guards in vmaf_picture_alloc / vmaf_cuda_picture_alloc_pinned are the
     * load-bearing CERT INT30-C overflow defence. 32768 = 32K, well above
     * 8K UHD (7680). */
    if (w > 32768u || h > 32768u)
        return -EINVAL;

    vmaf->cuda.cookie.pix_fmt = vmaf->pic_params.pix_fmt = pix_fmt;
    vmaf->cuda.cookie.h = vmaf->pic_params.h = h;
    vmaf->cuda.cookie.w = vmaf->pic_params.w = w;
    vmaf->cuda.cookie.bpc = vmaf->pic_params.bpc = bpc;
    vmaf->cuda.cookie.state = &vmaf->cuda.state;

    VmafGpuPicturePoolConfig cfg_buf = {
        .pic_cnt = 4,
        .cookie = &vmaf->cuda.cookie,
        .synchronize_picture_callback = vmaf_cuda_picture_synchronize,
        .alloc_picture_callback = vmaf_cuda_picture_alloc,
        .free_picture_callback = vmaf_cuda_picture_free,
    };

    return vmaf_gpu_picture_pool_init(&vmaf->cuda.ring_buffer, cfg_buf);
}

int vmaf_cuda_import_state(VmafContext *vmaf, VmafCudaState *cu_state)
{
    if (!vmaf)
        return -EINVAL;
    if (!cu_state)
        return -EINVAL;

    vmaf->cuda.state = *cu_state;
    vmaf->active_backend = VMAF_BACKEND_CUDA;

    return 0;
}

int vmaf_cuda_preallocate_pictures(VmafContext *vmaf, VmafCudaPictureConfiguration cfg)
{
    if (!vmaf)
        return -EINVAL;
    /* Guard against double-call: ring_buffer is non-NULL after the first
     * successful preallocation.  Silently overwriting it would leak the
     * existing pool and corrupt any in-flight CUDA picture references.
     * -EBUSY is the POSIX code for "resource already in use". */
    if (vmaf->cuda.ring_buffer)
        return -EBUSY;

    int err = 0;

    vmaf->cuda.cfg.pic_params.w = cfg.pic_params.w;
    vmaf->cuda.cfg.pic_params.h = cfg.pic_params.h;
    vmaf->cuda.cfg.pic_params.bpc = cfg.pic_params.bpc;
    vmaf->cuda.cfg.pic_params.pix_fmt = cfg.pic_params.pix_fmt;
    vmaf->cuda.cfg.pic_prealloc_method = cfg.pic_prealloc_method;

    switch (cfg.pic_prealloc_method) {
    case VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_NONE:
        break;
    case VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_HOST:
    case VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_HOST_PINNED:
    case VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_DEVICE:
        err = prepare_ring_buffer(vmaf, cfg.pic_params.w, cfg.pic_params.h, cfg.pic_params.pix_fmt,
                                  cfg.pic_params.bpc);
        if (err) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "problem during cuda picture preallocation\n");
            return err;
        }
        break;
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "unknown cuda picture preallocation method\n");
        return -EINVAL;
    }

    return err;
}

int vmaf_cuda_fetch_preallocated_picture(VmafContext *vmaf, VmafPicture *pic)
{
    if (!vmaf)
        return -EINVAL;
    if (!pic)
        return -EINVAL;
    if (!vmaf->cuda.ring_buffer)
        return -EINVAL;

    /* Deferred: PREALLOCATION_METHOD_HOST currently allocates a fresh
     * VmafPicture on every call instead of vending from a pre-populated pool.
     * Wiring HOST through vmaf_gpu_picture_pool_fetch() requires a separate
     * pool initialised with vmaf_picture_alloc / vmaf_picture_unref callbacks
     * (no CUDA streams, no synchronise step).  Tracked as T9.x backlog. */

    switch (vmaf->cuda.cfg.pic_prealloc_method) {
    case VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_DEVICE:
        return vmaf_gpu_picture_pool_fetch(vmaf->cuda.ring_buffer, pic);
    case VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_HOST:
        return vmaf_picture_alloc(pic, vmaf->cuda.cfg.pic_params.pix_fmt,
                                  vmaf->cuda.cfg.pic_params.bpc, vmaf->cuda.cfg.pic_params.w,
                                  vmaf->cuda.cfg.pic_params.h);
    case VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_HOST_PINNED:
        return vmaf_cuda_picture_alloc_pinned(
            pic, vmaf->cuda.cfg.pic_params.pix_fmt, vmaf->cuda.cfg.pic_params.bpc,
            vmaf->cuda.cfg.pic_params.w, vmaf->cuda.cfg.pic_params.h, &vmaf->cuda.state);
    case VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_NONE:
    default:
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "undefined cuda picture preallocation method\n");
        return -EINVAL;
    }
}

static void set_fex_cuda_state(VmafFeatureExtractorContext *fex_ctx, VmafContext *vmaf)
{
    if (fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA)
        fex_ctx->fex->cu_state = &(vmaf->cuda.state);
}

#endif

static int prepare_picture_pool(VmafContext *vmaf, unsigned pic_cnt, unsigned w, unsigned h,
                                enum VmafPixelFormat pix_fmt, unsigned bpc)
{
    if (!vmaf)
        return -EINVAL;
    if (!w || !h)
        return -EINVAL;
    if (!pic_cnt)
        return -EINVAL;

    VmafPicturePoolConfig cfg = {
        .pic_cnt = pic_cnt,
        .w = w,
        .h = h,
        .pix_fmt = pix_fmt,
        .bpc = bpc,
    };

    return vmaf_picture_pool_init(&vmaf->picture_pool, cfg);
}

static int check_picture_pool(VmafContext *vmaf)
{
    if (!vmaf->thread_pool)
        return 0;
    if (vmaf->picture_pool)
        return 0;

    // Default to 2x thread count if not explicitly preallocated
    const unsigned pic_cnt = vmaf->cfg.n_threads * 2;

    int err = prepare_picture_pool(vmaf, pic_cnt, vmaf->pic_params.w, vmaf->pic_params.h,
                                   vmaf->pic_params.pix_fmt, vmaf->pic_params.bpc);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "problem during prepare_picture_pool\n");
        return -EINVAL;
    }

    return 0;
}

int vmaf_preallocate_pictures(VmafContext *vmaf, VmafPictureConfiguration cfg)
{
    if (!vmaf)
        return -EINVAL;

    return prepare_picture_pool(vmaf, cfg.pic_cnt, cfg.pic_params.w, cfg.pic_params.h,
                                cfg.pic_params.pix_fmt, cfg.pic_params.bpc);
}

int vmaf_fetch_preallocated_picture(VmafContext *vmaf, VmafPicture *pic)
{
    if (!vmaf)
        return -EINVAL;
    if (!pic)
        return -EINVAL;
    if (!vmaf->picture_pool)
        return -EINVAL;

    return vmaf_picture_pool_fetch(vmaf->picture_pool, pic);
}

#ifdef HAVE_SYCL
int vmaf_sycl_import_state(VmafContext *vmaf, VmafSyclState *sycl_state)
{
    if (!vmaf)
        return -EINVAL;
    if (!sycl_state)
        return -EINVAL;

    vmaf->sycl.state = sycl_state;
    vmaf->active_backend = VMAF_BACKEND_SYCL;

    return 0;
}

int vmaf_sycl_preallocate_pictures(VmafContext *vmaf, VmafSyclPictureConfiguration cfg)
{
    if (!vmaf)
        return -EINVAL;
    if (!vmaf->sycl.state)
        return -EINVAL;
    if (vmaf->sycl.pool)
        return -EBUSY;

    if (cfg.pic_prealloc_method == VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_NONE)
        return 0;

    enum VmafSyclPoolMethod method;
    switch (cfg.pic_prealloc_method) {
    case VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_DEVICE:
        method = VMAF_SYCL_POOL_DEVICE;
        break;
    case VMAF_SYCL_PICTURE_PREALLOCATION_METHOD_HOST:
        method = VMAF_SYCL_POOL_HOST;
        break;
    default:
        return -EINVAL;
    }

    /* Pool depth of 2 matches the shared-frame double-buffering in
     * vmaf_sycl_shared_frame_upload — caller writes into pic N while
     * extractors still consume pic N-1. */
    const unsigned pic_cnt = 2;

    return vmaf_sycl_picture_pool_init(&vmaf->sycl.pool, vmaf->sycl.state, pic_cnt,
                                       cfg.pic_params.w, cfg.pic_params.h, cfg.pic_params.bpc,
                                       cfg.pic_params.pix_fmt, method);
}

int vmaf_sycl_picture_fetch(VmafContext *vmaf, VmafPicture *pic)
{
    if (!vmaf)
        return -EINVAL;
    if (!pic)
        return -EINVAL;

    if (vmaf->sycl.pool)
        return vmaf_sycl_picture_pool_fetch(vmaf->sycl.pool, pic);

    /* No pool configured — fall back to a host-backed picture so callers
     * that ignored vmaf_sycl_preallocate_pictures() still receive a usable
     * buffer (the shared-frame upload path handles host→device copy). */
    return vmaf_picture_alloc(pic, vmaf->pic_params.pix_fmt, vmaf->pic_params.bpc,
                              vmaf->pic_params.w, vmaf->pic_params.h);
}

int vmaf_sycl_init_frame_buffers(VmafContext *vmaf, unsigned w, unsigned h, unsigned bpc)
{
    if (!vmaf)
        return -EINVAL;
    if (!vmaf->sycl.state)
        return -EINVAL;

    vmaf->pic_params.w = w;
    vmaf->pic_params.h = h;
    vmaf->pic_params.bpc = bpc;
    vmaf->pic_params.pix_fmt = VMAF_PIX_FMT_YUV420P;

    return vmaf_sycl_shared_frame_init(vmaf->sycl.state, w, h, bpc);
}

int vmaf_sycl_get_frame_buffers(VmafContext *vmaf, void **ref, void **dis)
{
    if (!vmaf || !vmaf->sycl.state)
        return -EINVAL;

    return vmaf_sycl_shared_frame_get(vmaf->sycl.state, ref, dis);
}

int vmaf_sycl_wait_compute(VmafContext *vmaf)
{
    if (!vmaf)
        return -EINVAL;
    if (!vmaf->sycl.state)
        return -EINVAL;

    // Wait on the primary queue (VA surface imports / de-tile kernels)
    int err = vmaf_sycl_queue_wait(vmaf->sycl.state);
    if (err)
        return err;

    // Also wait on the combined compute queue (GPU feature extractors).
    // Without this, the VA import path can overwrite shared ref/dis buffers
    // while the previous frame's extractors are still reading them.
    return vmaf_sycl_combined_queue_wait(vmaf->sycl.state);
}

static void set_fex_sycl_state(VmafFeatureExtractorContext *fex_ctx, VmafContext *vmaf)
{
    if (fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_SYCL)
        fex_ctx->fex->sycl_state = vmaf->sycl.state;
}
#endif

#ifdef HAVE_METAL
/* T8-IOS (ADR-0423): caller-imported IOSurface path. The Metal runtime (T8-1b) was
 * already shipped; this PR adds the caller-imported IOSurface route
 * by stashing the external state on the VmafContext and routing
 * read_imported_pictures through vmaf_read_pictures. */
int vmaf_metal_import_state(VmafContext *vmaf, VmafMetalState *state)
{
    if (!vmaf)
        return -EINVAL;
    if (!state)
        return -EINVAL;

    vmaf->metal.state = state;
    vmaf->active_backend = VMAF_BACKEND_METAL;
    return 0;
}

int vmaf_metal_read_imported_pictures(VmafContext *vmaf, unsigned index)
{
    if (!vmaf)
        return -EINVAL;
    if (!vmaf->metal.state)
        return -EINVAL;
    if (vmaf->flushed)
        return -EINVAL;

    VmafPicture ref = {0};
    VmafPicture dis = {0};
    int err = vmaf_metal_state_build_pictures(vmaf->metal.state, index, &ref, &dis);
    if (err)
        return err;

    /* vmaf_read_pictures takes ownership and unrefs both pictures
     * (including on the error path); the import ring already
     * cleared its slot in build_pictures so no further cleanup is
     * needed here. */
    return vmaf_read_pictures(vmaf, &ref, &dis, index);
}
#endif

#ifdef HAVE_HIP
/* ADR-0519: stash the caller-imported HIP state on the VmafContext.
 * Mirrors vmaf_sycl_import_state / vmaf_metal_import_state field-for-field — ownership stays with the
 * caller, vmaf_close() clears the pointer without freeing, and the
 * caller calls vmaf_hip_state_free() after vmaf_close().
 *
 * Implementation lives here (not in libvmaf/src/hip/common.c) because
 * it needs VmafContext field-level access. The CUDA / SYCL / Metal
 * twins follow the same convention. */
int vmaf_hip_import_state(VmafContext *vmaf, VmafHipState *hip_state)
{
    if (!vmaf)
        return -EINVAL;
    if (!hip_state)
        return -EINVAL;

    vmaf->hip.state = hip_state;
    vmaf->active_backend = VMAF_BACKEND_HIP;
    return 0;
}
#endif

static void set_fex_framesync(VmafFeatureExtractorContext *fex_ctx, VmafContext *vmaf)
{
    if (fex_ctx->fex->flags & VMAF_FEATURE_FRAME_SYNC)
        fex_ctx->fex->framesync = (vmaf->framesync);
}

/* Hand a freshly created extractor context the context-owned backend state
 * its flags ask for (CUDA / SYCL device state, frame-sync). Cannot fail. */
static void fex_ctx_bind_backends(VmafFeatureExtractorContext *fex_ctx, VmafContext *vmaf)
{
#ifdef HAVE_CUDA
    set_fex_cuda_state(fex_ctx, vmaf);
#endif
#ifdef HAVE_SYCL
    set_fex_sycl_state(fex_ctx, vmaf);
#endif
    set_fex_framesync(fex_ctx, vmaf);
}

static void vmaf_ctx_dnn_free(VmafContext *vmaf)
{
    if (!vmaf)
        return;
    if (vmaf->dnn.sess) {
        vmaf_ort_close(vmaf->dnn.sess);
        vmaf->dnn.sess = NULL;
    }
    if (vmaf->dnn.has_sidecar) {
        vmaf_dnn_sidecar_free(&vmaf->dnn.meta);
        vmaf->dnn.has_sidecar = false;
    }
    free(vmaf->dnn.in_buf);
    vmaf->dnn.in_buf = NULL;
    vmaf->dnn.in_elements = 0;
    free(vmaf->dnn.extra_in_buf);
    vmaf->dnn.extra_in_buf = NULL;
    vmaf->dnn.extra_in_width = 0;
    free(vmaf->dnn.feature_name);
    vmaf->dnn.feature_name = NULL;
    for (size_t i = 0; i < vmaf->dnn.n_outputs; ++i) {
        free(vmaf->dnn.output_feature_names[i]);
        vmaf->dnn.output_feature_names[i] = NULL;
    }
    vmaf->dnn.n_outputs = 0u;
}

int vmaf_ctx_dnn_has_session(const VmafContext *ctx)
{
    return (ctx && ctx->dnn.sess) ? 1 : 0;
}

/* ADR-0519: bridge for vmaf_dnn_set_codec_context. The public symbol
 * lives in dnn_attach_api.c and forwards here so VmafContext stays
 * opaque to the DNN module. */
int vmaf_ctx_dnn_set_codec_context(VmafContext *ctx, const char *codec_name, const char *preset,
                                   int crf)
{
    if (!ctx)
        return -EINVAL;
    if (!ctx->dnn.sess)
        return -EINVAL;
    /* Codec block only exists for feature-vector models with a second
     * input. Image-rank or single-input feature-vector models cannot
     * accept a codec context. */
    if (ctx->dnn.in_rank != 2u || ctx->dnn.extra_in_buf == NULL || ctx->dnn.extra_in_width == 0u) {
        return -ENOTSUP;
    }
    if (!ctx->dnn.has_sidecar || !ctx->dnn.meta.codec_aware ||
        ctx->dnn.meta.n_encoder_vocab == 0u) {
        return -ENOTSUP;
    }
    /* Layout must be [one-hot(n_vocab), preset_norm, crf_norm]. The
     * loader already validated this at attach time, but check again so
     * a future loader bug surfaces here rather than as a memory error. */
    if (ctx->dnn.extra_in_width != ctx->dnn.meta.n_encoder_vocab + 2u) {
        return -ENOTSUP;
    }
    return vmaf_dnn_codec_block_fill(ctx->dnn.extra_in_buf, ctx->dnn.extra_in_width,
                                     (const char *const *)ctx->dnn.meta.encoder_vocab,
                                     ctx->dnn.meta.n_encoder_vocab, codec_name, preset, crf);
}

/* ADR-0543: bridge for vmaf_dnn_set_resize_mode. The public symbol in
 * dnn_attach_api.c forwards to here so VmafContext stays opaque to the
 * DNN module. The int -> enum cast happens at the dispatch site
 * (vmaf_ctx_dnn_run_frame_nchw); the enum-validity gate lives in the
 * public wrapper. */
int vmaf_ctx_dnn_set_resize_mode(VmafContext *ctx, int mode)
{
    if (!ctx)
        return -EINVAL;
    ctx->dnn.resize_mode = mode;
    return 0;
}

/* Bridge for vmaf_dnn_is_codec_aware (dnn.h public API).
 * A model is "codec-aware" iff:
 *   - a session is attached (sess != NULL),
 *   - the sidecar is present and declares codec_aware=true, AND
 *   - the codec block buffer was allocated at attach time (extra_in_width > 0).
 * All three conditions must hold; any mismatch indicates a model that either
 * has no codec block or whose sidecar was absent at load time. */
int vmaf_ctx_dnn_is_codec_aware(const VmafContext *ctx)
{
    if (!ctx || !ctx->dnn.sess)
        return 0;
    if (!ctx->dnn.has_sidecar || !ctx->dnn.meta.codec_aware)
        return 0;
    if (ctx->dnn.extra_in_width == 0u || ctx->dnn.extra_in_buf == NULL)
        return 0;
    return 1;
}

static bool dnn_feature_name_char_ok(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Fallback stack buffer for the synthesised "outputN" / "outputN_M"
 * feature-name suffix used when the ONNX session reports no usable
 * output node name. `output%zu` for a size_t-decoded `pos` fits in
 * far fewer than 32 chars on any supported host (size_t is at most
 * 20 decimal digits + the "output" prefix). */
#define VMAF_DNN_NAME_FALLBACK_BUF 32

/* Same idea with the de-duplication attempt counter appended:
 * `output%zu_%zu`. 48 bytes leaves comfortable headroom over the
 * worst case 6 + 20 + 1 + 20 + NUL = 48 bytes for two size_t
 * decimal expansions. */
#define VMAF_DNN_NAME_DEDUP_BUF 48

/* `strnlen` cap when scanning user-supplied DNN feature names.
 * Names land in libvmaf's feature-collector key string; a 1 KiB
 * upper bound is well beyond any sensible model-output identifier
 * and prevents an unterminated input from running off the end of
 * the heap allocation. CERT STR06-C. */
#define VMAF_DNN_NAME_STRNLEN_CAP 1024u

static char *dnn_make_output_feature_name(const char *base, const char *suffix, size_t pos,
                                          bool multi_output)
{
    if (!base)
        return NULL;
    if (!multi_output)
        return strdup(base);

    char fallback[VMAF_DNN_NAME_FALLBACK_BUF];
    const char *raw = suffix;
    if (!raw || !*raw) {
        (void)snprintf(fallback, sizeof(fallback), "output%zu", pos);
        raw = fallback;
    }

    const size_t base_len = strnlen(base, VMAF_DNN_NAME_STRNLEN_CAP);
    const size_t raw_len = strnlen(raw, VMAF_DNN_NAME_STRNLEN_CAP);
    char *clean = (char *)malloc(raw_len + 1u);
    if (!clean)
        return NULL;

    bool any = false;
    for (size_t i = 0; i < raw_len; ++i) {
        const char c = raw[i];
        clean[i] = dnn_feature_name_char_ok(c) ? c : '_';
        any = any || dnn_feature_name_char_ok(c);
    }
    clean[raw_len] = '\0';
    if (!any) {
        (void)snprintf(fallback, sizeof(fallback), "output%zu", pos);
        const size_t fallback_len = strlen(fallback);
        char *tmp = (char *)realloc(clean, fallback_len + 1u);
        if (!tmp) {
            free(clean);
            return NULL;
        }
        clean = tmp;
        memcpy(clean, fallback, fallback_len + 1u);
    }

    const size_t clean_len = strlen(clean);
    char *out = (char *)malloc(base_len + 1u + clean_len + 1u);
    if (!out) {
        free(clean);
        return NULL;
    }
    memcpy(out, base, base_len);
    out[base_len] = '_';
    memcpy(out + base_len + 1u, clean, clean_len + 1u);
    free(clean);
    return out;
}

static bool dnn_output_name_duplicate(char *const *names, size_t count, const char *candidate)
{
    for (size_t i = 0; i < count; ++i) {
        if (names[i] && candidate && strcmp(names[i], candidate) == 0)
            return true;
    }
    return false;
}

static void dnn_output_names_free(char **names, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        free(names[i]);
        names[i] = NULL;
    }
}

static char *dnn_make_unique_fallback_output_name(const char *base, char *const *names,
                                                  size_t count, size_t pos)
{
    char suffix[VMAF_DNN_NAME_DEDUP_BUF];
    for (size_t attempt = 0; attempt <= VMAF_ORT_MAX_IO; ++attempt) {
        (void)snprintf(suffix, sizeof(suffix), "output%zu_%zu", pos, attempt);
        char *candidate = dnn_make_output_feature_name(base, suffix, pos, true);
        if (!candidate)
            return NULL;
        if (!dnn_output_name_duplicate(names, count, candidate))
            return candidate;
        free(candidate);
    }
    return NULL;
}

static int dnn_prepare_output_feature_names(VmafContext *ctx, VmafOrtSession *sess,
                                            const VmafModelSidecar *meta, const char *base_name)
{
    size_t n_inputs = 0u;
    size_t n_outputs = 0u;
    int rc = vmaf_ort_io_count(sess, &n_inputs, &n_outputs);
    if (rc < 0)
        return rc;
    (void)n_inputs;
    if (n_outputs == 0u || n_outputs > VMAF_ORT_MAX_IO)
        return -ENOTSUP;

    char *names[VMAF_ORT_MAX_IO] = {0};
    const bool multi_output = n_outputs > 1u;
    const bool sidecar_names_match =
        meta && meta->n_output_names == n_outputs && meta->n_output_names > 0u;

    for (size_t i = 0; i < n_outputs; ++i) {
        const char *suffix = NULL;
        if (multi_output) {
            if (sidecar_names_match && meta->output_names[i] && *meta->output_names[i]) {
                suffix = meta->output_names[i];
            } else {
                suffix = vmaf_ort_output_name_at(sess, i);
            }
        }

        names[i] = dnn_make_output_feature_name(base_name, suffix, i, multi_output);
        if (!names[i]) {
            dnn_output_names_free(names, i);
            return -ENOMEM;
        }
        if (dnn_output_name_duplicate(names, i, names[i])) {
            free(names[i]);
            names[i] = dnn_make_unique_fallback_output_name(base_name, names, i, i);
            if (!names[i]) {
                dnn_output_names_free(names, i);
                return -ENOMEM;
            }
        }
    }

    ctx->dnn.n_outputs = n_outputs;
    for (size_t i = 0; i < n_outputs; ++i) {
        ctx->dnn.output_feature_names[i] = names[i];
    }
    return 0;
}

static int dnn_append_scalar_outputs(VmafContext *vmaf, const VmafOrtTensorOut *outputs,
                                     unsigned index)
{
    if (!vmaf || !outputs)
        return -EINVAL;
    if (vmaf->dnn.n_outputs == 0u || vmaf->dnn.n_outputs > VMAF_ORT_MAX_IO)
        return -EINVAL;

    for (size_t i = 0; i < vmaf->dnn.n_outputs; ++i) {
        if (outputs[i].written != 1u)
            return -ENOTSUP;
        if (!vmaf->dnn.output_feature_names[i])
            return -EINVAL;
    }
    for (size_t i = 0; i < vmaf->dnn.n_outputs; ++i) {
        int rc = vmaf_feature_collector_append(vmaf->feature_collector,
                                               vmaf->dnn.output_feature_names[i],
                                               (double)outputs[i].data[0], index);
        if (rc < 0)
            return rc;
    }
    return 0;
}

/* Helper: rank-4 NCHW path. Validates static-image shape, allocates the
 * luma scratch buffer, and writes the per-frame inference state into
 * ctx->dnn. Returns 0 on success or a negative errno.
 *
 * Symbolic-dim policy (ADR-0524):
 *   - dim 0 (N / batch): ORT reports symbolic dims as `-1`. The fork's
 *     inference loop only ever feeds one frame at a time, so a symbolic
 *     batch is folded to 1. A fixed batch > 1 is still rejected (no
 *     batched-inference scheduler in libvmaf today).
 *   - dim 1 (C / channels): must be 1 (single-channel luma). Symbolic
 *     channels are rejected — the tensor bridge has no way to fan out
 *     C > 1 without an explicit colourspace contract.
 *   - dims 2/3 (H/W): must be a known positive value. Symbolic H/W
 *     ("dynamic-resolution" exports) are rejected loudly because the
 *     scratch buffer size depends on them; the diagnostic distinguishes
 *     symbolic from "C != 1" so the failure mode is observable. */
static int dnn_validate_nchw_shape(const int64_t *in_shape, int64_t *h_out, int64_t *w_out)
{
    /* Fold a symbolic batch dim (-1) to 1 — the per-frame loop never
     * batches. Anything else outside {1, -1} is a fixed batch > 1 which
     * the runtime does not support. */
    if (in_shape[0] != 1 && in_shape[0] != -1) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "tiny-model loader: rank-4 model has fixed batch %" PRId64 "; "
                 "only batch=1 or symbolic batch (-1) is supported\n",
                 in_shape[0]);
        return -ENOTSUP;
    }
    if (in_shape[1] != 1) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "tiny-model loader: rank-4 model has channels=%" PRId64 "; "
                 "only single-channel luma (C=1) is supported\n",
                 in_shape[1]);
        return -ENOTSUP;
    }
    const int64_t h = in_shape[2];
    const int64_t w = in_shape[3];
    if (h <= 0 || w <= 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "tiny-model loader: rank-4 model has dynamic / non-positive "
                 "spatial dims (H=%" PRId64 ", W=%" PRId64 "); symbolic H/W is unsupported — "
                 "re-export with a fixed input resolution\n",
                 h, w);
        return -ENOTSUP; /* dynamic dims unsupported */
    }
    /* Reject absurd spatial dims before the (int) narrowing of expected_w/h
     * in the caller: an untrusted ONNX export can carry H/W > INT_MAX, which
     * `(int)w`/`(int)h` would silently truncate into a wrong expected
     * geometry (and a giant calloc). 32768 mirrors the picture-dimension
     * cap (VMAF_PIC_DIM_MAX in picture.c); a model needing larger input is
     * nonsensical. CERT INT31-C. (round-2 bug-hunt R2-7) */
    if (h > 32768 || w > 32768) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "tiny-model loader: rank-4 model spatial dims out of range "
                 "(H=%" PRId64 ", W=%" PRId64 "; supported max is 32768)\n",
                 h, w);
        return -ENOTSUP;
    }
    *h_out = h;
    *w_out = w;
    return 0;
}

/* Final, non-failing step shared by both attach paths: publish the session,
 * the optional sidecar and the (owned) feature name on the context. */
static void dnn_attach_commit(VmafContext *ctx, VmafOrtSession *sess, const VmafModelSidecar *meta,
                              char *name)
{
    ctx->dnn.sess = sess;
    if (meta) {
        ctx->dnn.meta = *meta;
        ctx->dnn.has_sidecar = true;
    }
    ctx->dnn.feature_name = name;
}

static int dnn_attach_nchw(VmafContext *ctx, VmafOrtSession *sess, const VmafModelSidecar *meta,
                           const int64_t *in_shape, char *name)
{
    int64_t h = 0;
    int64_t w = 0;
    int rc = dnn_validate_nchw_shape(in_shape, &h, &w);
    if (rc < 0) {
        free(name);
        return rc;
    }
    const size_t n = (size_t)w * (size_t)h;
    float *buf = (float *)calloc(n, sizeof(*buf));
    if (!buf) {
        free(name);
        return -ENOMEM;
    }
    rc = dnn_prepare_output_feature_names(ctx, sess, meta, name);
    if (rc < 0) {
        free(buf);
        free(name);
        return rc;
    }
    dnn_attach_commit(ctx, sess, meta, name);
    ctx->dnn.in_rank = 4u;
    ctx->dnn.expected_w = (int)w;
    ctx->dnn.expected_h = (int)h;
    ctx->dnn.in_buf = buf;
    ctx->dnn.in_elements = n;
    return 0;
}

/* Symbolic-dim policy (ADR-0524) for the rank-2 input: batch may be -1
 * (symbolic) or 1 — a fixed batch > 1 is rejected because the per-frame
 * inference loop only feeds a single sample per Run() call — and the
 * feature width must be a known positive value within the sidecar limit. */
static int dnn_validate_feature_vector_shape(const int64_t *in_shape, size_t *n_features)
{
    if (in_shape[0] != 1 && in_shape[0] != -1) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "tiny-model loader: feature-vector model has fixed batch %" PRId64 "; "
                 "only batch=1 or symbolic batch (-1) is supported\n",
                 in_shape[0]);
        return -ENOTSUP;
    }
    const int64_t f = in_shape[1];
    if (f <= 0 || (size_t)f > VMAF_DNN_MAX_FEATURE_NAMES) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "tiny-model loader: feature-vector model has invalid "
                 "feature width %" PRId64 "\n",
                 f);
        return -ENOTSUP;
    }
    *n_features = (size_t)f;
    return 0;
}

/* Discover the optional second input (e.g. fr_regressor_v2's 14-D `codec`
 * block), allocate its scratch buffer and pre-seed it to the conservative
 * "unknown encoder" baseline so models that gate on the one-hot still
 * produce a finite score. Single-input models leave *extra_buf NULL and
 * *extra_w zero. */
static int dnn_probe_extra_input(VmafOrtSession *sess, float **extra_buf, size_t *extra_w)
{
    size_t n_inputs = 0u;
    size_t n_outputs = 0u;
    const int rc_io = vmaf_ort_io_count(sess, &n_inputs, &n_outputs);
    if (rc_io < 0)
        return rc_io;
    (void)n_outputs;
    *extra_buf = NULL;
    *extra_w = 0u;
    if (n_inputs < 2u)
        return 0;

    int64_t extra_shape[4] = {0};
    size_t extra_rank = 0u;
    const int rc_sh = vmaf_ort_input_shape_at(sess, 1u, extra_shape, 4u, &extra_rank);
    if (rc_sh < 0)
        return rc_sh;
    /* Second-input symbolic-batch policy mirrors the primary input
     * (ADR-0524): batch may be -1 (symbolic) or 1; feature width
     * (extra_shape[1]) must be a known positive value because it
     * sizes the scratch buffer. */
    if (extra_rank != 2 || (extra_shape[0] != 1 && extra_shape[0] != -1) || extra_shape[1] <= 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "tiny-model loader: second input has unsupported "
                 "shape (rank %zu, batch %" PRId64 ", width %" PRId64 ")\n",
                 extra_rank, extra_shape[0], extra_shape[1]);
        return -ENOTSUP;
    }
    const size_t w = (size_t)extra_shape[1];
    float *buf = (float *)calloc(w, sizeof(*buf));
    if (!buf)
        return -ENOMEM;
    /* Best-effort default: when the codec block follows the v2 layout
     * (N-2 one-hot slots followed by preset_norm/crf_norm), set the
     * third-from-last slot — the "unknown" one-hot at vocab v2 index
     * 11 lives there. This matches `_encoder_onehot(N_ENCODERS-1)`
     * in train_fr_regressor_v2.py. Any consumer that needs the real
     * encoder identity must wire a dedicated API. */
    if (w >= 3u)
        buf[w - 3u] = 1.0f; /* before preset, crf */
    *extra_buf = buf;
    *extra_w = w;
    return 0;
}

/* Helper: rank-2 feature-vector path (ADR-0517). Allocates the feature
 * scratch buffer plus the optional codec-block buffer discovered by
 * dnn_probe_extra_input(). */
static int dnn_attach_feature_vector(VmafContext *ctx, VmafOrtSession *sess,
                                     const VmafModelSidecar *meta, const int64_t *in_shape,
                                     char *name)
{
    size_t n = 0u;
    int rc = dnn_validate_feature_vector_shape(in_shape, &n);
    if (rc < 0) {
        free(name);
        return rc;
    }
    float *buf = (float *)calloc(n, sizeof(*buf));
    if (!buf) {
        free(name);
        return -ENOMEM;
    }

    float *extra_buf = NULL;
    size_t extra_w = 0u;
    rc = dnn_probe_extra_input(sess, &extra_buf, &extra_w);
    if (rc < 0) {
        free(buf);
        free(name);
        return rc;
    }

    rc = dnn_prepare_output_feature_names(ctx, sess, meta, name);
    if (rc < 0) {
        free(extra_buf);
        free(buf);
        free(name);
        return rc;
    }

    dnn_attach_commit(ctx, sess, meta, name);
    ctx->dnn.in_rank = 2u;
    ctx->dnn.expected_w = 0;
    ctx->dnn.expected_h = 0;
    ctx->dnn.n_features = n;
    ctx->dnn.in_buf = buf;
    ctx->dnn.in_elements = n;
    ctx->dnn.extra_in_buf = extra_buf;
    ctx->dnn.extra_in_width = extra_w;
    return 0;
}

int vmaf_ctx_dnn_attach(VmafContext *ctx, VmafOrtSession *sess, const VmafModelSidecar *meta,
                        const int64_t *in_shape, size_t in_rank, const char *feature_name)
{
    if (!ctx || !sess || !in_shape || !feature_name)
        return -EINVAL;
    if (ctx->dnn.sess)
        return -EBUSY;

    /* Accepted ranks (ADR-0517):
     *   rank 4: NCHW [1, 1, H, W] single-channel luma image. Legacy
     *           path — the picture's luma plane is fed through
     *           vmaf_tensor_from_luma each frame.
     *   rank 2: [-1, F] feature-vector model. The host materialises
     *           the F features from the classic feature collector
     *           (canonical-6 by sidecar default) and feeds them into
     *           the model. Optional extra inputs (e.g. a `codec`
     *           block) are zero-filled at inference time.
     *
     * Anything else fails loud with a specific error message so the
     * limit is visible. */
    if (in_rank != 4 && in_rank != 2) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "tiny-model loader: model has input rank %zu, expected 2 "
                 "(feature vector) or 4 (NCHW image)\n",
                 in_rank);
        return -ENOTSUP;
    }

    char *name = strdup(feature_name);
    if (!name)
        return -ENOMEM;

    if (in_rank == 4) {
        return dnn_attach_nchw(ctx, sess, meta, in_shape, name);
    }
    return dnn_attach_feature_vector(ctx, sess, meta, in_shape, name);
}

/* Bind one scalar output slot per model output, run inference on `inputs`,
 * and publish the outputs to the feature collector under
 * vmaf->dnn.output_feature_names. ORT's -ENOSPC (an output wider than one
 * scalar) is reported as -ENOTSUP. */
static int dnn_run_and_append(VmafContext *vmaf, const VmafOrtTensorIn *inputs, size_t n_inputs,
                              unsigned index)
{
    float out_values[VMAF_ORT_MAX_IO] = {0.f};
    VmafOrtTensorOut outputs[VMAF_ORT_MAX_IO];
    memset(outputs, 0, sizeof(outputs));
    for (size_t i = 0; i < vmaf->dnn.n_outputs; ++i) {
        outputs[i].name = NULL;
        outputs[i].data = &out_values[i];
        outputs[i].capacity = 1u;
        outputs[i].written = 0u;
    }

    const int rc = vmaf_ort_run(vmaf->dnn.sess, inputs, n_inputs, outputs, vmaf->dnn.n_outputs);
    if (rc == -ENOSPC)
        return -ENOTSUP;
    if (rc < 0)
        return rc;
    return dnn_append_scalar_outputs(vmaf, outputs, index);
}

/* Fill the NCHW scratch tensor from the picture's luma plane.
 *
 * ADR-0550 — when the source frame dims don't match the model's expected
 * NCHW input shape, route through the selected resize filter. Default
 * (DISABLED / zero-init) returns -ERANGE; the operator must explicitly pass
 * --tiny-resize to enable auto-resize. The `disabled` mode preserves the
 * pre-ADR-0550 behaviour for parity harnesses.
 *
 * ADR-0976 — sidecar-driven luma normalisation was dead code: the
 * `has_norm` / `norm_mean` / `norm_std` fields on VmafModelSidecar were
 * never populated by `vmaf_dnn_sidecar_load`, so this path always selected
 * mean=NULL / std=NULL. The shipped tiny-AI pipeline bakes the affine into
 * the ONNX graph at export time (canonical pattern); models that need a
 * runtime scaler should use the generic `vmaf_dnn_session_run()` with
 * caller-managed input tensors. */
static int dnn_fill_nchw_input(VmafContext *vmaf, const VmafPicture *ref)
{
    const bool dim_mismatch =
        ((int)ref->w[0] != vmaf->dnn.expected_w || (int)ref->h[0] != vmaf->dnn.expected_h);
    const VmafTinyResize resize_mode = (VmafTinyResize)vmaf->dnn.resize_mode;
    if (dim_mismatch && resize_mode == VMAF_TINY_RESIZE_DISABLED)
        return -ERANGE;

    if (dim_mismatch) {
        return vmaf_tensor_from_luma_resize(
            (const uint8_t *)ref->data[0], (size_t)ref->stride[0], (int)ref->w[0], (int)ref->h[0],
            vmaf->dnn.expected_w, vmaf->dnn.expected_h, VMAF_TENSOR_LAYOUT_NCHW,
            VMAF_TENSOR_DTYPE_F32, NULL, NULL, resize_mode, vmaf->dnn.in_buf);
    }
    return vmaf_tensor_from_luma((const uint8_t *)ref->data[0], (size_t)ref->stride[0],
                                 vmaf->dnn.expected_w, vmaf->dnn.expected_h,
                                 VMAF_TENSOR_LAYOUT_NCHW, VMAF_TENSOR_DTYPE_F32, NULL, NULL,
                                 vmaf->dnn.in_buf);
}

static int vmaf_ctx_dnn_run_frame_nchw(VmafContext *vmaf, const VmafPicture *ref, unsigned index)
{
    if (!ref || !ref->data[0])
        return -EINVAL;

    /* The current tensor bridge operates on 8-bit luma only. 10/12-bit
     * content and multi-channel inputs are rejected loudly rather than
     * quietly truncated. */
    if (ref->bpc != 8)
        return -ENOTSUP;

    const int rc = dnn_fill_nchw_input(vmaf, ref);
    if (rc < 0)
        return rc;

    const int64_t shape[4] = {1, 1, vmaf->dnn.expected_h, vmaf->dnn.expected_w};
    const VmafOrtTensorIn input = {
        .name = NULL,
        .data = vmaf->dnn.in_buf,
        .shape = shape,
        .rank = 4u,
    };
    return dnn_run_and_append(vmaf, &input, 1u, index);
}

/* Resolve a sidecar feature name (e.g. "adm2", "vif_scale0", "motion2")
 * to the canonical libvmaf feature-collector key (e.g.
 * "VMAF_integer_feature_adm2_score"). Looks up the score at @p index;
 * on miss (extractor not registered, or motion2 retroactive write not
 * yet landed) returns 0.0 — the loaded model still produces a finite
 * inference, just with a stale slot. Returns the value. */
static double dnn_lookup_feature(VmafFeatureCollector *fc, const char *short_name, unsigned index)
{
    /* Probe both the integer- and float-extractor keys. The fork's
     * default model graph registers the integer variants, but
     * upstream-mirror callers using `--feature float_vif` etc. will
     * have only the float key populated. */
    static const struct {
        const char *short_name;
        const char *integer_key;
        const char *float_key;
    } TABLE[] = {
        {"adm2", "VMAF_integer_feature_adm2_score", "VMAF_feature_adm2_score"},
        {"vif_scale0", "VMAF_integer_feature_vif_scale0_score", "VMAF_feature_vif_scale0_score"},
        {"vif_scale1", "VMAF_integer_feature_vif_scale1_score", "VMAF_feature_vif_scale1_score"},
        {"vif_scale2", "VMAF_integer_feature_vif_scale2_score", "VMAF_feature_vif_scale2_score"},
        {"vif_scale3", "VMAF_integer_feature_vif_scale3_score", "VMAF_feature_vif_scale3_score"},
        {"motion2", "VMAF_integer_feature_motion2_score", "VMAF_feature_motion2_score"},
    };
    for (size_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); ++i) {
        if (strcmp(short_name, TABLE[i].short_name) != 0)
            continue;
        double v = 0.0;
        if (vmaf_feature_collector_get_score(fc, TABLE[i].integer_key, &v, index) == 0)
            return v;
        if (vmaf_feature_collector_get_score(fc, TABLE[i].float_key, &v, index) == 0)
            return v;
        return 0.0;
    }
    /* Unknown feature name — try as-is (some sidecars may already
     * carry the full collector key). */
    double v = 0.0;
    if (vmaf_feature_collector_get_score(fc, short_name, &v, index) == 0)
        return v;
    return 0.0;
}

/* Materialise the model's input feature vector from the classic feature
 * collector into vmaf->dnn.in_buf. When the sidecar carries a feature_names
 * list (v1 / v2 / vmaf_tiny_v4 trainers all do) it is honoured slot-by-slot;
 * when it is absent the canonical-6 order is used and any slot beyond it is
 * zero-filled. */
static void dnn_materialise_feature_vector(VmafContext *vmaf, unsigned index)
{
    static const char *const CANON6[] = {
        "adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2",
    };
    const size_t n = vmaf->dnn.n_features;
    const VmafModelSidecar *meta = vmaf->dnn.has_sidecar ? &vmaf->dnn.meta : NULL;

    for (size_t i = 0; i < n; ++i) {
        const char *short_name = NULL;
        if (meta && meta->n_features == n && meta->feature_names[i] != NULL) {
            short_name = meta->feature_names[i];
        } else if (i < sizeof(CANON6) / sizeof(CANON6[0])) {
            short_name = CANON6[i];
        } else {
            vmaf->dnn.in_buf[i] = 0.0f;
            continue;
        }
        const double raw = dnn_lookup_feature(vmaf->feature_collector, short_name, index);
        float v = (float)raw;
        /* Apply the C-side StandardScaler only when the model sidecar carries
         * mean/std values (has_feature_scaler) AND the scaler is NOT already
         * baked into the ONNX graph as Constant nodes (onnx_has_scaler).
         * vmaf_tiny_v2/v3/v4 set onnx_has_scaler=true in their sidecars
         * (ADR-0244); applying the scaler here for those models would
         * double-scale every feature and corrupt scores. */
        if (meta && meta->has_feature_scaler && !meta->onnx_has_scaler &&
            meta->feature_std[i] > 0.f) {
            v = (v - meta->feature_mean[i]) / meta->feature_std[i];
        }
        vmaf->dnn.in_buf[i] = v;
    }
}

static int vmaf_ctx_dnn_run_frame_feature_vector(VmafContext *vmaf, unsigned index)
{
    dnn_materialise_feature_vector(vmaf, index);

    const int64_t feat_shape[2] = {1, (int64_t)vmaf->dnn.n_features};
    const int64_t codec_shape[2] = {1, (int64_t)vmaf->dnn.extra_in_width};
    const VmafOrtTensorIn inputs[2] = {
        {.name = NULL, .data = vmaf->dnn.in_buf, .shape = feat_shape, .rank = 2u},
        {.name = NULL, .data = vmaf->dnn.extra_in_buf, .shape = codec_shape, .rank = 2u},
    };
    /* The codec block is the optional second input (ADR-0519). */
    const bool has_codec = vmaf->dnn.extra_in_buf != NULL && vmaf->dnn.extra_in_width > 0u;
    return dnn_run_and_append(vmaf, inputs, has_codec ? 2u : 1u, index);
}

static int vmaf_ctx_dnn_run_frame(VmafContext *vmaf, const VmafPicture *ref, unsigned index)
{
    if (!vmaf->dnn.sess)
        return 0;
    if (vmaf->dnn.in_rank == 2u)
        return vmaf_ctx_dnn_run_frame_feature_vector(vmaf, index);
    return vmaf_ctx_dnn_run_frame_nchw(vmaf, ref, index);
}

/* Release the GPU backend state held by the context. CUDA resources are
 * context-owned (ring buffer, then the per-thread drain stream, then the
 * context itself — the drain-stream destroy needs the still-live context to
 * call cuStreamDestroy, T-GPU-OPT-1 / ADR-0242). SYCL / Metal / HIP state is
 * caller-owned: vmaf_<backend>_import_state() does not transfer ownership,
 * so only the SYCL picture pool is closed here and the state pointers are
 * cleared — the caller calls vmaf_<backend>_state_free() after vmaf_close()
 * (ADR-0519). */
static void vmaf_close_backends(VmafContext *vmaf)
{
#ifdef HAVE_CUDA
    if (vmaf->cuda.ring_buffer)
        vmaf_gpu_picture_pool_close(vmaf->cuda.ring_buffer);
    if (vmaf->cuda.state.ctx)
        vmaf_cuda_drain_batch_thread_destroy(&vmaf->cuda.state);
    if (vmaf->cuda.state.ctx)
        vmaf_cuda_release(&vmaf->cuda.state);
#endif
#ifdef HAVE_SYCL
    if (vmaf->sycl.pool) {
        vmaf_sycl_picture_pool_close(vmaf->sycl.pool);
        vmaf->sycl.pool = NULL;
    }
    vmaf->sycl.state = NULL;
#endif
#ifdef HAVE_METAL
    vmaf->metal.state = NULL;
#endif
#ifdef HAVE_HIP
    vmaf->hip.state = NULL;
#endif
#if !defined(HAVE_CUDA) && !defined(HAVE_SYCL) && !defined(HAVE_METAL) && !defined(HAVE_HIP)
    (void)vmaf; /* CPU-only build: no backend state to release. */
#endif
}

int vmaf_close(VmafContext *vmaf)
{
    if (!vmaf)
        return -EINVAL;

    /* Propagate errors from cleanup helpers per CERT ERR33-C / Power-of-10 #7.
     * Use the first non-zero code; later cleanup must still run so we
     * cannot bail on the first error.
     * Guard: thread_pool is NULL when cfg.n_threads == 0 (single-threaded
     * mode); vmaf_thread_pool_wait returns -EINVAL for a NULL pool, which
     * would be a false error here. */
    int close_err = vmaf->thread_pool ? vmaf_thread_pool_wait(vmaf->thread_pool) : 0;
    if (vmaf->prev_ref.ref)
        (void)vmaf_picture_unref(&vmaf->prev_ref);
    const int framesync_err = vmaf_framesync_destroy(vmaf->framesync);
    if (!close_err)
        close_err = framesync_err;
    feature_extractor_vector_destroy(&(vmaf->registered_feature_extractors));
    vmaf_feature_collector_destroy(vmaf->feature_collector);
    /* Both pool destroys return -EINVAL for a NULL pool (single-threaded
     * mode) — not an error here, so their status is deliberately dropped. */
    (void)vmaf_thread_pool_destroy(vmaf->thread_pool);
    (void)vmaf_fex_ctx_pool_destroy(vmaf->fex_ctx_pool);
    vmaf_ctx_dnn_free(vmaf);
    /* Release the Pelorus perceptual-weight summary store (ADR-1118). Safe on a
     * zero-initialised store (no side-data ever registered) — it is a no-op. */
    vmaf_perceptual_weight_store_destroy(&vmaf->perceptual);
    if (vmaf->picture_pool)
        vmaf_picture_pool_close(vmaf->picture_pool);
    vmaf_close_backends(vmaf);
    free(vmaf);

    return close_err;
}

int vmaf_import_feature_score(VmafContext *vmaf, const char *feature_name, double value,
                              unsigned index)
{
    if (!vmaf)
        return -EINVAL;
    if (!feature_name)
        return -EINVAL;

    return vmaf_feature_collector_append(vmaf->feature_collector, feature_name, value, index);
}

/* ---- Pelorus perceptual spatial-pooling weighting (ADR-1118) ------------- *
 * GOLDEN-GATE ISOLATION: these three entry points only ever mutate
 * vmaf->perceptual; the weighting is applied in vmaf_feature_score_pooled and
 * is inert (per-frame weight == 1.0, byte-identical legacy arithmetic) unless
 * weighting is enabled AND side-data is present for the frame. The Netflix
 * golden pairs carry no side-data and MUST score bit-exact. */

int vmaf_set_perceptual_weight_enabled(VmafContext *vmaf, int enabled)
{
    if (!vmaf)
        return -EINVAL;

    vmaf->perceptual.enabled = (enabled != 0);
    return 0;
}

int vmaf_set_perceptual_weight_strength(VmafContext *vmaf, double strength)
{
    if (!vmaf)
        return -EINVAL;
    /* Reject NaN / Inf / negative — a malformed strength must never silently
     * corrupt a pooled score (CERT FLP). */
    if (isnan(strength) || !isfinite(strength) || strength < 0.0)
        return -EINVAL;

    vmaf->perceptual.strength = strength;
    vmaf->perceptual.strength_set = true;
    return 0;
}

int vmaf_set_perceptual_sidedata(VmafContext *vmaf, const uint8_t *blob, size_t len,
                                 unsigned pic_index)
{
    if (!vmaf)
        return -EINVAL;
    if (!blob)
        return -EINVAL;

    int err = vmaf_perceptual_weight_ingest(&vmaf->perceptual, blob, len, pic_index);
    if (err == -EPROTO) {
        /* ABI-major mismatch (R6): degrade to unweighted for this frame and
         * tell the operator once. Not fatal — the pooled score is still
         * produced, just without this frame's perceptual weight. */
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "perceptual_weight: ignoring Pelorus side-data for frame %u "
                 "(ABI-major mismatch); frame scored unweighted\n",
                 pic_index);
    }
    return err;
}

int vmaf_use_feature(VmafContext *vmaf, const char *feature_name, VmafFeatureDictionary *opts_dict)
{
    if (!vmaf)
        return -EINVAL;
    if (!feature_name)
        return -EINVAL;

    VmafDictionary *s = (VmafDictionary *)opts_dict;

    int err = 0;

    VmafFeatureExtractor *fex = vmaf_get_feature_extractor_by_name(feature_name);
    if (!fex)
        return -EINVAL;

    /* Netflix/vmaf#1242 ownership contract: past the argument guards above, this
     * call consumes `opts_dict` on every path. Both failure paths below used to
     * return without releasing it, which leaked the caller's dictionary exactly
     * when the caller was told not to free it. */
    VmafDictionary *d = NULL;
    if (s) {
        err = vmaf_dictionary_copy(&s, &d);
        if (err) {
            (void)vmaf_dictionary_free(&s);
            return err;
        }
        err = vmaf_dictionary_free(&s);
        if (err) {
            (void)vmaf_dictionary_free(&d);
            return err;
        }
    }

    VmafFeatureExtractorContext *fex_ctx = NULL;
    err = vmaf_feature_extractor_context_create(&fex_ctx, fex, d);
    if (err) {
        /* context_create does not release the dictionary on its own failure
         * paths (it frees only what it allocated), so the copy is ours to
         * release here. */
        (void)vmaf_dictionary_free(&d);
        return err;
    }
    fex_ctx_bind_backends(fex_ctx, vmaf);

    RegisteredFeatureExtractors *rfe = &(vmaf->registered_feature_extractors);
    err = feature_extractor_vector_append(rfe, fex_ctx, 0);
    if (err)
        err |= vmaf_feature_extractor_context_destroy(fex_ctx);

#ifdef HAVE_CUDA
    /* Invalidate the rfe_hw_flags cache so the next vmaf_read_pictures call
     * recomputes the bitmask with the newly registered extractor included. */
    vmaf->rfe_hw_flags_dirty = true;
#endif

    return err;
}

/* Compose the extractor-selection flag mask from the active backends.
 * HIP is host-pic only (no gpumask gate, no device-picture pool)
 * per ADR-0530. Metal uses caller-imported state per ADR-0423. */
static unsigned compute_fex_flags(const VmafContext *vmaf)
{
    unsigned fex_flags = 0;
#if !defined(HAVE_CUDA) && !defined(HAVE_SYCL) && !defined(HAVE_HIP) && !defined(HAVE_METAL)
    (void)vmaf; /* CPU-only build: no backend slots to inspect. */
#endif
#ifdef HAVE_CUDA
    if (!vmaf->cfg.gpumask && vmaf->cuda.state.ctx)
        fex_flags |= VMAF_FEATURE_EXTRACTOR_CUDA;
#endif
#ifdef HAVE_SYCL
    if (!vmaf->cfg.gpumask && vmaf->sycl.state)
        fex_flags |= VMAF_FEATURE_EXTRACTOR_SYCL;
#endif
#ifdef HAVE_HIP
    /* ADR-0530: HIP-flagged extractors are selected when a HIP state
     * pointer has been imported via vmaf_hip_import_state(). The HIP
     * backend is host-pic (no gpumask gate). */
    if (vmaf->hip.state)
        fex_flags |= VMAF_FEATURE_EXTRACTOR_HIP;
#endif
#ifdef HAVE_METAL
    /* Metal-flagged extractors are selected when a Metal state
     * pointer has been imported via vmaf_metal_import_state(). */
    if (vmaf->metal.state)
        fex_flags |= VMAF_FEATURE_EXTRACTOR_METAL;
#endif
    return fex_flags;
}

/* Pick the extractor that can honour every option the model sets for this
 * feature (ADR-1183).
 *
 * A GPU twin whose option table lacks one of the model's keys would silently
 * drop it and emit a differently-named feature, so the model's prediction would
 * read from a vector that never gets written. When that happens, fall back to
 * the CPU twin for this one feature and say so at INFO level; the rest of the
 * model keeps running on the device. Returns NULL when no extractor provides
 * the feature at all (the caller turns that into -EINVAL). */
static VmafFeatureExtractor *fex_honouring_model_options(VmafFeatureExtractor *fex,
                                                         const VmafModelFeature *feature)
{
    const unsigned gpu_mask = VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_SYCL |
                              VMAF_FEATURE_EXTRACTOR_HIP | VMAF_FEATURE_EXTRACTOR_METAL;

    if (!(fex->flags & gpu_mask) || !feature->opts_dict)
        return fex;

    const char *missing_key = NULL;
    if (vmaf_feature_extractor_supports_options(fex, feature->opts_dict, &missing_key))
        return fex;

    vmaf_log(VMAF_LOG_LEVEL_INFO,
             "feature '%s': %s extractor lacks option '%s', computing it on the CPU\n",
             feature->name, fex->name, missing_key);

    VmafFeatureExtractor *cpu_fex = vmaf_get_feature_extractor_by_feature_name(feature->name, 0);
    if (!cpu_fex) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not initialize feature extractor \"%s\"\n",
                 feature->name);
    }
    return cpu_fex;
}

int vmaf_use_features_from_model(VmafContext *vmaf, VmafModel *model)
{
    if (!vmaf)
        return -EINVAL;
    if (!model)
        return -EINVAL;

    int err = 0;
    const unsigned fex_flags = compute_fex_flags(vmaf);

    RegisteredFeatureExtractors *rfe = &(vmaf->registered_feature_extractors);

    for (unsigned i = 0; i < model->n_features; i++) {
        VmafFeatureExtractor *fex =
            vmaf_get_feature_extractor_by_feature_name(model->feature[i].name, fex_flags);
        if (!fex) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not initialize feature extractor \"%s\"\n",
                     model->feature[i].name);
            return -EINVAL;
        }

        fex = fex_honouring_model_options(fex, &model->feature[i]);
        if (!fex)
            return -EINVAL;

        VmafFeatureExtractorContext *fex_ctx = NULL;
        VmafDictionary *d = NULL;
        if (model->feature[i].opts_dict) {
            err = vmaf_dictionary_copy(&model->feature[i].opts_dict, &d);
            if (err)
                return err;
        }
        err = vmaf_feature_extractor_context_create(&fex_ctx, fex, d);
        if (err)
            return err;
        fex_ctx_bind_backends(fex_ctx, vmaf);
        err = feature_extractor_vector_append(rfe, fex_ctx, 0);
        if (err) {
            err |= vmaf_feature_extractor_context_destroy(fex_ctx);
            return err;
        }
    }

    err = vmaf_feature_collector_mount_model(vmaf->feature_collector, model);
    if (err)
        return err;

    return 0;
}

int vmaf_use_features_from_model_collection(VmafContext *vmaf,
                                            VmafModelCollection *model_collection)
{
    if (!vmaf)
        return -EINVAL;
    if (!model_collection)
        return -EINVAL;

    int err = 0;
    for (unsigned i = 0; i < model_collection->cnt; i++)
        err |= vmaf_use_features_from_model(vmaf, model_collection->model[i]);

    return err;
}

/* Drop the counted previous-frame reference an extractor holds (if any) and
 * clear the field. Every PREV_REF dispatch path must balance the
 * vmaf_picture_ref() it took with exactly one call here — a bare memset
 * would leak the count and exhaust the picture pool after ~pool_size frames
 * (ADR-1051). */
static void fex_release_prev_ref(VmafFeatureExtractor *fex)
{
    if (fex->prev_ref.ref)
        (void)vmaf_picture_unref(&fex->prev_ref);
    memset(&fex->prev_ref, 0, sizeof(fex->prev_ref));
}

/* Advance the context's previous-frame reference to `ref`: drop the count on
 * the frame before it and take one on the new frame. Tolerates a picture
 * without a counted buffer: on the CUDA device-only path (every registered
 * extractor carries VMAF_FEATURE_EXTRACTOR_CUDA, so rfe_hw_flags reports
 * HW_FLAG_DEVICE only and translate_picture_device never downloads) the
 * host-side picture is zero-initialised, and dereferencing its NULL `ref` in
 * vmaf_ref_fetch_increment would crash. The only PREV_REF consumer is CPU
 * integer_motion_v2, which is never registered alongside a pure CUDA
 * extractor set — skipping the update there is safe. ADR-0123. */
static void read_pictures_update_prev_ref(VmafContext *vmaf, VmafPicture *ref)
{
    if (vmaf->prev_ref.ref)
        (void)vmaf_picture_unref(&vmaf->prev_ref);
    if (ref && ref->ref)
        (void)vmaf_picture_ref(&vmaf->prev_ref, ref);
}

/* n_subsample keeps every n-th frame for stateless extractors; TEMPORAL and
 * PREV_REF extractors carry inter-frame state and must see every frame. */
static bool fex_subsample_skip(uint64_t flags, unsigned index, unsigned n_subsample)
{
    const uint64_t no_subsample_flags =
        VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_PREV_REF;
    if (flags & no_subsample_flags)
        return false;
    return (n_subsample > 1) && ((index % n_subsample) != 0);
}

struct ThreadDataBatch {
    VmafPicture ref, dist, prev_ref;
    unsigned index;
    VmafFeatureCollector *feature_collector;
    RegisteredFeatureExtractors *registered_fex;
    unsigned n_subsample;
    /* _Atomic int err: the worker thread writes this field multiple times as
     * it iterates over extractors; the thread pool runner reads it once (as
     * the function return value) to accumulate into pool->last_error.  Making
     * the field atomic prevents a TSan data-race report if a future code path
     * reads f->err without going through the function return value, and
     * documents that the field is written from a worker context
     * (iter9-tsan-race-deep finding #2). */
    _Atomic int err;
};

/* Lazily create this worker's private BatchThreadData on first use. The
 * thread pool owns it afterwards and releases it via batch_thread_data_free. */
static int batch_thread_data_ensure(void **thread_data, unsigned cnt, BatchThreadData **out)
{
    BatchThreadData *td = (BatchThreadData *)*thread_data;
    if (!td) {
        td = malloc(sizeof(*td));
        if (!td)
            return -ENOMEM;
        td->cnt = cnt;
        td->fex_ctx = (VmafFeatureExtractorContext **)calloc(td->cnt, sizeof(*td->fex_ctx));
        if (!td->fex_ctx) {
            free(td);
            return -ENOMEM;
        }
        *thread_data = td;
    }
    *out = td;
    return 0;
}

/* Extractors the CPU worker pool must not run for this frame. CUDA and SYCL
 * extractors are dispatched by their backend paths in
 * read_pictures_dispatch_one() — running them here as well would double-write
 * the feature collector — and TEMPORAL extractors run on the serial path.
 * Everything else honours n_subsample. Keep in sync with
 * read_pictures_should_skip(). */
static bool batch_extractor_skip(const VmafFeatureExtractor *shared_fex, unsigned index,
                                 unsigned n_subsample)
{
    const uint64_t not_pooled =
        VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_SYCL | VMAF_FEATURE_EXTRACTOR_TEMPORAL;
    if (shared_fex->flags & not_pooled)
        return true;
    return fex_subsample_skip(shared_fex->flags, index, n_subsample);
}

/* Create (once) this worker's private context for extractor i.
 * vmaf_feature_extractor_context_create deep-copies the shared extractor into
 * a new VmafFeatureExtractor owned by td->fex_ctx[i]; from then on
 * td->fex_ctx[i]->fex is a different heap object from the registered one, so
 * per-frame writes to its prev_ref are thread-private (ADR-0795). The shared
 * extractor is only ever read (flags / name / opts) from a worker. */
static int batch_ensure_fex_ctx(BatchThreadData *td, const struct ThreadDataBatch *f, unsigned i)
{
    if (td->fex_ctx[i])
        return 0;

    VmafFeatureExtractorContext *shared_ctx = f->registered_fex->fex_ctx[i];
    VmafDictionary *d = NULL;
    if (shared_ctx->opts_dict) {
        VmafDictionary *opts_dict = shared_ctx->opts_dict;
        const int err = vmaf_dictionary_copy(&opts_dict, &d);
        if (err)
            return err;
    }
    return vmaf_feature_extractor_context_create(&td->fex_ctx[i], shared_ctx->fex, d);
}

/* Run extractor i for the frame carried by `f` on this worker's private
 * context.
 *
 * PREV_REF extractors take an INDEPENDENT counted reference to the shared
 * snapshot f->prev_ref: vmaf_feature_extractor_context_extract() runs the
 * PREV_REF swap (feature_extractor.cpp) on success — it unrefs that count and
 * bumps frame N into fex->prev_ref with one extra count — and leaves it
 * untouched on error. Either way fex->prev_ref holds one counted reference
 * afterwards, released via fex_release_prev_ref(). The snapshot itself is
 * deliberately NOT touched here: the remaining PREV_REF extractors in the
 * batch still need it, and it is released exactly once by the caller. (The
 * old code struct-copied the snapshot and zeroed it after the first
 * extractor, starving the second PREV_REF extractor — e.g. motion_v2, which
 * always co-schedules motion — with -EINVAL on every frame.) */
static int batch_extract_one(BatchThreadData *td, struct ThreadDataBatch *f, unsigned i)
{
    VmafFeatureExtractorContext *fex_ctx = td->fex_ctx[i];
    const VmafFeatureExtractor *shared_fex = f->registered_fex->fex_ctx[i]->fex;
    const bool prev_ref = (shared_fex->flags & VMAF_FEATURE_EXTRACTOR_PREV_REF) != 0;

    /* Invariant (ADR-0795): fex_ctx->fex is this thread's private deep copy. */
    assert(fex_ctx->fex != shared_fex);
    if (prev_ref && f->prev_ref.ref)
        (void)vmaf_picture_ref(&fex_ctx->fex->prev_ref, &f->prev_ref);

    const int err = vmaf_feature_extractor_context_extract(fex_ctx, &f->ref, NULL, &f->dist, NULL,
                                                           f->index, f->feature_collector);

    if (prev_ref)
        fex_release_prev_ref(fex_ctx->fex);
    return err;
}

static int threaded_extract_batch_func(void *e, void **thread_data)
{
    struct ThreadDataBatch *f = e;
    /* f->err is _Atomic int; use atomic_store/atomic_load throughout so that
     * TSan sees proper sequenced-before edges and does not report a race if a
     * future caller reads f->err outside the function return value path
     * (iter9-tsan-race-deep finding #2). */
    atomic_store(&f->err, 0);

    BatchThreadData *td = NULL;
    int err = batch_thread_data_ensure(thread_data, f->registered_fex->cnt, &td);
    for (unsigned i = 0; !err && i < f->registered_fex->cnt; i++) {
        const VmafFeatureExtractor *shared_fex = f->registered_fex->fex_ctx[i]->fex;
        if (batch_extractor_skip(shared_fex, f->index, f->n_subsample))
            continue;
        err = batch_ensure_fex_ctx(td, f, i);
        if (!err)
            err = batch_extract_one(td, f, i);
    }
    atomic_store(&f->err, err);

    /* Release the shared prev_ref snapshot exactly once — every PREV_REF
     * extractor took and balanced its own count in batch_extract_one(). */
    if (f->prev_ref.ref)
        (void)vmaf_picture_unref(&f->prev_ref);
    (void)vmaf_picture_unref(&f->ref);
    (void)vmaf_picture_unref(&f->dist);
    return atomic_load(&f->err);
}

static int threaded_read_pictures_batch(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist,
                                        unsigned index)
{
    if (!vmaf)
        return -EINVAL;
    if (!ref)
        return -EINVAL;
    if (!dist)
        return -EINVAL;

    VmafPicture pic_a = {0};
    VmafPicture pic_b = {0};
    VmafPicture prev_ref = {0};
    (void)vmaf_picture_ref(&pic_a, ref);
    (void)vmaf_picture_ref(&pic_b, dist);

    /* Refcounted snapshot of prev_ref for the worker, stored in data.prev_ref
     * (struct copy) so the worker owns an independent ref. */
    if (vmaf->prev_ref.ref)
        (void)vmaf_picture_ref(&prev_ref, &vmaf->prev_ref);

    /* Advance vmaf->prev_ref to the current frame BEFORE enqueuing: no worker
     * runs for this frame yet, and the worker only reads data.prev_ref (the
     * snapshot above), so this races with nothing. Updating after enqueue (the
     * old order) created a TSAN race on the shared VmafRef* between the worker's
     * and the main thread's unref (iter6-tsan-race-deep finding #2). */
    read_pictures_update_prev_ref(vmaf, ref);

    struct ThreadDataBatch data = {
        .ref = pic_a,
        .dist = pic_b,
        .prev_ref = prev_ref,
        .index = index,
        .feature_collector = vmaf->feature_collector,
        .registered_fex = &vmaf->registered_feature_extractors,
        .n_subsample = vmaf->cfg.n_subsample,
        .err = 0,
    };

    const int err = vmaf_thread_pool_enqueue(vmaf->thread_pool, threaded_extract_batch_func, &data,
                                             sizeof(data));
    if (err) {
        (void)vmaf_picture_unref(&pic_a);
        (void)vmaf_picture_unref(&pic_b);
        if (prev_ref.ref)
            (void)vmaf_picture_unref(&prev_ref);
        /* done=true means the caller skips its cleanup: unref, so we own ref/dist
         * here too (success path unrefs below). Else each failed enqueue leaks a
         * pool slot and the next pool_fetch deadlocks once the pool drains. */
        return err | vmaf_picture_unref(ref) | vmaf_picture_unref(dist);
    }

    return vmaf_picture_unref(ref) | vmaf_picture_unref(dist);
}

static int validate_pic_params(VmafContext *vmaf, const VmafPicture *ref, const VmafPicture *dist)
{
    const VmafPicturePrivate *ref_priv = ref->priv;
    const VmafPicturePrivate *dist_priv = dist->priv;

    if (!vmaf->pic_params.w) {
        vmaf->pic_params.w = ref->w[0];
        vmaf->pic_params.h = ref->h[0];
        vmaf->pic_params.pix_fmt = ref->pix_fmt;
        vmaf->pic_params.bpc = ref->bpc;
    }
    vmaf->pic_params.buf_type = ref_priv->buf_type;

    if ((ref->w[0] != dist->w[0]) || (ref->w[0] != vmaf->pic_params.w))
        return -EINVAL;
    if ((ref->h[0] != dist->h[0]) || (ref->h[0] != vmaf->pic_params.h))
        return -EINVAL;
    if ((ref->pix_fmt != dist->pix_fmt) || (ref->pix_fmt != vmaf->pic_params.pix_fmt)) {
        return -EINVAL;
    }
    if ((ref->bpc != dist->bpc) || (ref->bpc != vmaf->pic_params.bpc))
        return -EINVAL;
    if (ref_priv->buf_type != dist_priv->buf_type)
        return -EINVAL;

    return 0;
}

/* The non-temporal, CPU-side half of the threaded flush.  Split out of
 * flush_context_threaded() to keep that function inside the ADR-0141
 * function-size budget after the GPU-ownership fix (ADR-1197) added its
 * skip condition. */
static int flush_non_temporal_cpu_extractors(VmafContext *vmaf)
{
    int err = 0;
    RegisteredFeatureExtractors rfe = vmaf->registered_feature_extractors;
    for (unsigned i = 0; i < rfe.cnt; i++) {
        VmafFeatureExtractorContext *fex_ctx = rfe.fex_ctx[i];
        VmafFeatureExtractor *fex = fex_ctx->fex;
        if (fex->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL)
            continue;
        if (fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA)
            continue;
        if (!fex->flush)
            continue;
        /* flush() is called directly on the shared fex (not a per-thread
         * deep copy) and may lazily allocate state in fex->priv (e.g.
         * integer_motion::flush allocates s->feature_name_dict when it
         * was never set by init).  Mark the shared context as initialised
         * so that vmaf_feature_extractor_context_close - called from
         * feature_extractor_vector_destroy at teardown - actually invokes
         * fex->close and frees whatever flush allocated.  Without this,
         * is_initialized == false causes close to return -EINVAL early,
         * leaking the dict (detected as a memory leak by ASan with
         * detect_leaks=1; root cause of ADR-1073 residual failure). */
        fex_ctx->is_initialized = true;
        int flush_err = 0;
        while (!(flush_err = fex->flush(fex, vmaf->feature_collector)))
            ;
        if (flush_err < 0)
            err |= flush_err;
    }
    return err;
}

static int flush_context_threaded(VmafContext *vmaf)
{
    int err = 0;
    err |= vmaf_thread_pool_wait(vmaf->thread_pool);
    {
        RegisteredFeatureExtractors rfe = vmaf->registered_feature_extractors;
        for (unsigned i = 0; i < rfe.cnt; i++) {
            if (!(rfe.fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL))
                continue;
            /* Leave GPU extractors entirely to flush_context_cuda /
             * flush_context_sycl, which run collect-then-flush in the same
             * order the serial path uses.  The second loop below already
             * skips CUDA deliberately; this loop did not, and that asymmetry
             * was the bug.  Flushing a temporal GPU extractor here ran its
             * tail-batch drain BEFORE the pending boundary collect, so the
             * last batch-boundary frame was emitted without the min() against
             * the following frame that motion2/motion3 are defined by, and
             * the subsequent collect in flush_context_cuda then hit a
             * duplicate write.  That duplicate surfaced as -EINVAL and was
             * misreported as "context could not be synchronized", which is
             * why `vmaf --threads N` failed on every GPU backend for every N. */
            if (rfe.fex_ctx[i]->fex->flags &
                (VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_SYCL))
                continue;
            err |= vmaf_feature_extractor_context_flush(rfe.fex_ctx[i], vmaf->feature_collector);
        }
    }

    err |= flush_non_temporal_cpu_extractors(vmaf);

    /* NB: vmaf->flushed is intentionally NOT set here.  The terminal-flush
     * decision is centralised in flush_context() so it can only flip true
     * after EVERY backend flush (CPU + CUDA + SYCL) has run successfully.
     * Setting it here would let a later CUDA-flush error early-return before
     * flush_context_sycl, dropping the final SYCL frame's scores while the
     * caller could no longer retry (vmaf_read_pictures(NULL,NULL) rejects a
     * second flush once vmaf->flushed is true). */
    return err;
}

static int flush_context_serial(VmafContext *vmaf)
{
    int err = 0;
    RegisteredFeatureExtractors rfe = vmaf->registered_feature_extractors;
    /* ADR-0530: drain HIP-flagged extractors' gpu_pending final-frame
     * collect BEFORE running their flush. The async submit/collect
     * double-buffer in `read_pictures_dispatch_one` leaves the last
     * submitted frame's collect pending; without this drain, the HIP
     * extractor's collect(N) never runs and motion2/motion3 at index
     * N-1 is never written, which trips
     * `vmaf_predict_score_at_index()` ("no feature ... at index N-1").
     * Mirrors the SYCL `flush_context_sycl` pattern. */
    for (unsigned i = 0; i < rfe.cnt; i++) {
        /* Drain any non-CUDA/SYCL extractor's gpu_pending final-frame
         * collect BEFORE running flush (e.g. HIP, Metal, or unflagged GPU extractors).
         * The async submit/collect double-buffer in `read_pictures_dispatch_one`
         * leaves the last submitted frame's collect pending. */
        if (rfe.fex_ctx[i]->gpu_pending &&
            !(rfe.fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA) &&
            !(rfe.fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_SYCL)) {
            err |= vmaf_feature_extractor_context_collect(
                rfe.fex_ctx[i], rfe.fex_ctx[i]->gpu_pending_index, vmaf->feature_collector);
            rfe.fex_ctx[i]->gpu_pending = false;
        }
        if (!(rfe.fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA) &&
            !(rfe.fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_SYCL)) {
            err |= vmaf_feature_extractor_context_flush(rfe.fex_ctx[i], vmaf->feature_collector);
        }
    }
    return err;
}

#ifdef HAVE_CUDA
static int flush_context_cuda(VmafContext *vmaf)
{
    int err = 0;
    if (!vmaf->cuda.state.ctx)
        return 0;
    /* Final-frame drain: T-GPU-OPT-1 (ADR-0242) leaves the last
     * frame's submit() events registered with the open drain batch.
     * Flush them in one syscall before the per-extractor collect()
     * loop runs, then close the batch so subsequent flush passes
     * see a clean state. The fall-through to vmaf_cuda_sync below
     * still catches any non-template-extractor stragglers. */
    err |= vmaf_cuda_drain_batch_flush(&vmaf->cuda.state);
    vmaf_cuda_drain_batch_close();
    RegisteredFeatureExtractors rfe = vmaf->registered_feature_extractors;
    for (unsigned i = 0; i < rfe.cnt; i++) {
        if (rfe.fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA) {
            /* Collect any pending double-buffered CUDA work.  The thread pool
             * path (flush_context_threaded) does not drain gpu_pending for
             * CUDA extractors, so this collect must run regardless. */
            if (rfe.fex_ctx[i]->gpu_pending) {
                err |= vmaf_feature_extractor_context_collect(
                    rfe.fex_ctx[i], rfe.fex_ctx[i]->gpu_pending_index, vmaf->feature_collector);
                rfe.fex_ctx[i]->gpu_pending = false;
            }
            /* No thread-pool special case: flush_context_threaded no longer
             * flushes GPU extractors, so this path owns them in both modes and
             * runs the same collect-then-flush order the serial path does. */
            err |= vmaf_feature_extractor_context_flush(rfe.fex_ctx[i], vmaf->feature_collector);
        }
    }
    /* Keep the extractor result and the CUDA result apart.  Folding both into
     * one variable made every extractor-side failure announce itself as a
     * synchronization failure, sending debugging at the context and the driver
     * while all four calls below were returning success. */
    const int extractor_err = err;
    int cuda_err = 0;
    CudaFunctions *cu_f = vmaf->cuda.state.f;
    cuda_err |= cu_f->cuCtxPushCurrent(vmaf->cuda.state.ctx);
    cuda_err |= cu_f->cuStreamSynchronize(vmaf->cuda.state.str);
    cuda_err |= cu_f->cuCtxSynchronize();
    cuda_err |= cu_f->cuCtxPopCurrent(NULL);
    if (cuda_err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "context could not be synchronized\n");
        return -EINVAL;
    }
    if (extractor_err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "a CUDA feature extractor failed during flush (%d)\n",
                 extractor_err);
        return -EINVAL;
    }
    return 0;
}
#endif

#ifdef HAVE_SYCL
static int flush_context_sycl(VmafContext *vmaf)
{
    int err = 0;
    if (!vmaf->sycl.state)
        return 0;
    RegisteredFeatureExtractors rfe = vmaf->registered_feature_extractors;
    // Collect any pending double-buffered SYCL work
    for (unsigned i = 0; i < rfe.cnt; i++) {
        if ((rfe.fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_SYCL) &&
            rfe.fex_ctx[i]->gpu_pending) {
            err |= vmaf_feature_extractor_context_collect(
                rfe.fex_ctx[i], rfe.fex_ctx[i]->gpu_pending_index, vmaf->feature_collector);
            rfe.fex_ctx[i]->gpu_pending = false;
        }
    }
    for (unsigned i = 0; i < rfe.cnt; i++) {
        if (rfe.fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_SYCL)
            err |= vmaf_feature_extractor_context_flush(rfe.fex_ctx[i], vmaf->feature_collector);
    }
    vmaf_sycl_queue_wait(vmaf->sycl.state);
    vmaf_sycl_print_timing(vmaf->sycl.state);
    return err;
}
#endif

static int flush_context(VmafContext *vmaf)
{
    int err = 0;
    if (vmaf->thread_pool) {
        err = flush_context_threaded(vmaf);
    } else {
        err = flush_context_serial(vmaf);
    }

#ifdef HAVE_CUDA
    /* Accumulate the CUDA-flush result instead of early-returning: a CUDA
     * error must not skip flush_context_sycl below, which is the ONLY place
     * SYCL extractors' final-frame gpu_pending collect + flush runs.  Skipping
     * it dropped the last SYCL frame's scores irrecoverably (vmaf->flushed is
     * only set once at the end, so the caller cannot re-flush). */
    err |= flush_context_cuda(vmaf);
#endif

#ifdef HAVE_SYCL
    err |= flush_context_sycl(vmaf);
#endif

    /* Only mark the context terminally flushed once every backend flush
     * succeeded.  On any error the caller may retry vmaf_read_pictures(NULL,
     * NULL) to re-run the flush pass. */
    if (!err)
        vmaf->flushed = true;
    return err;
}

#ifdef HAVE_CUDA
static int check_ring_buffer(VmafContext *vmaf)
{
    if (!vmaf->cuda.state.ctx)
        return 0;

    int err = 0;

    if (!vmaf->cuda.cfg.pic_prealloc_method && !vmaf->cuda.ring_buffer) {
        err = prepare_ring_buffer(vmaf, vmaf->pic_params.w, vmaf->pic_params.h,
                                  vmaf->pic_params.pix_fmt, vmaf->pic_params.bpc);
        if (err) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "problem during prepare_ring_buffer\n");
            return -EINVAL;
        }
    }

    return err;
}

enum {
    HW_FLAG_HOST = 1 << 0,
    HW_FLAG_DEVICE = 1 << 1,
};

static int translate_picture_host(VmafContext *vmaf, VmafPicture *pic, VmafPicture *pic_device,
                                  unsigned hw_flags)
{
    int err = 0;
    if (!(hw_flags & HW_FLAG_DEVICE))
        return err;

    //host to device

    switch (vmaf->pic_params.buf_type) {
    case VMAF_PICTURE_BUFFER_TYPE_HOST:
    case VMAF_PICTURE_BUFFER_TYPE_CUDA_HOST_PINNED:
        if (!vmaf->cuda.state.ctx)
            return -EINVAL;
        err |= vmaf_gpu_picture_pool_fetch(vmaf->cuda.ring_buffer, pic_device);
        /* Upload luma always; upload chroma when the input has it.
         * ciede_cuda (T7-23 / batch 1c part 2) is the first
         * chroma-aware CUDA extractor — older luma-only kernels
         * (psnr / motion / adm / vif / moment) just don't read
         * data[1..2]. The mild upload-bandwidth cost (~1 MB/frame
         * extra at 1080p YUV420) is preferable to a per-extractor
         * "needs_chroma" flag for now. */
        const uint8_t upload_mask = (vmaf->pic_params.pix_fmt == VMAF_PIX_FMT_YUV400P) ? 0x1 : 0x7;
        err |= vmaf_cuda_picture_upload_async(pic_device, pic, upload_mask);
        if (err) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "problem moving host pic into cuda device buffer\n");
            return err;
        }
        break;
    default:
        return -EINVAL;
    }

    return err;
}

static int translate_picture_device(VmafContext *vmaf, VmafPicture *pic, VmafPicture *pic_host,
                                    unsigned hw_flags)
{
    int err = 0;
    if (!(hw_flags & HW_FLAG_HOST))
        return err;

    //device to host

    err = vmaf_picture_alloc(pic_host, pic->pix_fmt, pic->bpc, pic->w[0], pic->h[0]);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "problem allocating host pic\n");
        return err;
    }

    err = vmaf_cuda_picture_download_async(pic, pic_host, 0x1);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "problem moving cuda pic into host buffer\n");
        return err;
    }

    /* Synchronize the per-picture stream so the async D-to-H copy is complete
     * before CPU-side feature extractors read pic_host->data[].  Without this
     * barrier the host buffer is still being written by the DMA engine when
     * the first CPU extractor (e.g. motion) dereferences it, producing
     * incorrect scores (~69–71 instead of ~100 on identical inputs). */
    CudaFunctions *cu_f = vmaf->cuda.state.f;
    int sync_err = cu_f->cuStreamSynchronize(vmaf_cuda_picture_get_stream(pic));
    if (sync_err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "problem synchronizing cuda stream after download\n");
        return sync_err;
    }

    return err;
}

static int translate_picture(VmafContext *vmaf, VmafPicture *pic, VmafPicture *pic_host,
                             VmafPicture *pic_device, unsigned hw_flags)
{
    const VmafPicturePrivate *pic_priv = pic->priv;

    switch (pic_priv->buf_type) {
    case VMAF_PICTURE_BUFFER_TYPE_HOST:
    case VMAF_PICTURE_BUFFER_TYPE_CUDA_HOST_PINNED:
        *pic_host = *pic;
        return translate_picture_host(vmaf, pic, pic_device, hw_flags);
    case VMAF_PICTURE_BUFFER_TYPE_CUDA_DEVICE:
        *pic_device = *pic;
        return translate_picture_device(vmaf, pic, pic_host, hw_flags);
    default:
        return -EINVAL;
    }
}

static unsigned rfe_hw_flags(RegisteredFeatureExtractors *rfe)
{
    if (!rfe)
        return -EINVAL;

    unsigned flags = 0;
    for (unsigned i = 0; i < rfe->cnt; i++) {
        flags |= rfe->fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA ? HW_FLAG_DEVICE :
                                                                             HW_FLAG_HOST;
    }

    return flags;
}

#endif

#ifdef HAVE_CUDA
static int read_pictures_cuda_translate(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist,
                                        unsigned hw_flags, VmafPicture *ref_host,
                                        VmafPicture *ref_device, VmafPicture *dist_host,
                                        VmafPicture *dist_device)
{
    int err = translate_picture(vmaf, ref, ref_host, ref_device, hw_flags);
    if (err)
        return err;
    /* Propagate dist translate failure: dist_device may be partially
     * populated if the call returns an error, leading to a corrupt frame
     * being passed to CUDA extractors.  The original code swallowed this
     * error with a (void) cast; mirror the ref path instead. */
    err = translate_picture(vmaf, dist, dist_host, dist_device, hw_flags);
    if (err)
        return err;
    return 0;
}

/* Unref only the device-side pictures (ref_device, dist_device).
 * Called from the done=true early-return path where threaded_read_pictures_batch
 * has already called vmaf_picture_unref on ref_host and dist_host (PR #838
 * regression: the original read_pictures_cuda_cleanup also freed the host
 * pictures, causing a double-unref that corrupted the pool free-list). */
static int read_pictures_cuda_cleanup_device_only(VmafContext *vmaf, VmafPicture *ref_device,
                                                  VmafPicture *dist_device)
{
    int err = 0;
    CudaFunctions *cu_f = vmaf->cuda.state.f;
    int _cuda_err = 0;
    if (ref_device->priv) {
        /* Cleanup path: capture CUDA errors via _cuda_err + goto so both
         * ref_device and dist_device always get unref'd even when the
         * bookkeeping cuEventRecord fails. The lingering ref on either
         * device picture would leak CUDA allocations otherwise. */
        CHECK_CUDA_GOTO(cu_f,
                        cuEventRecord(vmaf_cuda_picture_get_finished_event(ref_device),
                                      vmaf_cuda_picture_get_stream(ref_device)),
                        after_ref_event);
    after_ref_event:
        /* Deferred (T8.x): move unref into a cuStreamAddCallback/cuLaunchHostFunc
         * so the host-side unref fires when the GPU event completes rather than
         * eagerly here; requires a stable picture-callback ABI. */
        err |= vmaf_picture_unref(ref_device);
    }
    if (dist_device->priv) {
        CHECK_CUDA_GOTO(cu_f,
                        cuEventRecord(vmaf_cuda_picture_get_finished_event(dist_device),
                                      vmaf_cuda_picture_get_stream(dist_device)),
                        after_dist_event);
    after_dist_event:
        /* Deferred (T8.x): same picture-callback deferral as ref_device above. */
        err |= vmaf_picture_unref(dist_device);
    }
    if (_cuda_err && !err)
        err = _cuda_err;
    return err;
}

static int read_pictures_cuda_cleanup(VmafContext *vmaf, VmafPicture *ref_host,
                                      VmafPicture *ref_device, VmafPicture *dist_host,
                                      VmafPicture *dist_device)
{
    int err = 0;
    if (ref_host->priv)
        err |= vmaf_picture_unref(ref_host);
    if (dist_host->priv)
        err |= vmaf_picture_unref(dist_host);
    err |= read_pictures_cuda_cleanup_device_only(vmaf, ref_device, dist_device);
    return err;
}
#endif

#ifdef HAVE_SYCL
static int read_pictures_sycl_prep(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist)
{
    // SYCL upload must happen BEFORE the extractor loop because SYCL
    // queues are in-order — kernels enqueued during submit read shared
    // buffers immediately, unlike the deferred batched model.
    // Auto-initialize shared buffers on the first frame.
    if (!vmaf->sycl.state)
        return 0;
    // Double-buffered: compute reads buf[cur_compute] while upload
    // writes buf[cur_upload].  No need to wait for previous compute
    // before uploading — the buffers are disjoint.  Just wait for the
    // previous upload to finish (in-order copy_queue guarantees this
    // implicitly) before overwriting the upload slot.
    // On the very first frame, skip the wait since nothing is pending.
    if (!vmaf_sycl_get_shared_ref(vmaf->sycl.state)) {
        int err = vmaf_sycl_shared_frame_init(vmaf->sycl.state, ref->w[0], ref->h[0], ref->bpc);
        if (err)
            return err;
    }
    // DMA runs asynchronously on copy_queue while the CPU collects
    // previous-frame results below.  vmaf_sycl_graph_submit() will
    // wait for copy_queue just before replaying the command graph.
    int err = vmaf_sycl_shared_frame_upload(vmaf->sycl.state, ref, dist);

    /* Diagnostic oracle: checksum what the host-upload path just wrote.
     * pic_cnt is NOT yet incremented here (that happens in vmaf_read_pictures
     * after read_pictures_validate_and_prep returns), so vmaf->pic_cnt is
     * already the correct 0-based frame index (frame=0 on the first call).
     * The probe function internally waits for copy_queue — no separate wait
     * needed.  Gated by VMAF_SYCL_CHECKSUM=1 — zero cost when unset. */
    (void)vmaf_sycl_checksum_y_slot(vmaf->sycl.state, 1, vmaf->pic_cnt, "host");
    (void)vmaf_sycl_checksum_y_slot(vmaf->sycl.state, 0, vmaf->pic_cnt, "host");

    return err;
}
#endif

static bool read_pictures_should_skip(const VmafContext *vmaf,
                                      const VmafFeatureExtractorContext *fex_ctx, unsigned index)
{
    const uint64_t flags = fex_ctx->fex->flags;
    if (fex_subsample_skip(flags, index, vmaf->cfg.n_subsample))
        return true;

    /* CPU extractors with a thread pool go to the threaded batch path.
     * CUDA + SYCL extractors run serially via their respective dispatch loops.
     * Skipping them in the threaded batch and skipping them ALSO from the serial
     * loop here would leak — be careful to keep these in sync with
     * batch_extractor_skip(). */
    const bool gpu = (flags & (VMAF_FEATURE_EXTRACTOR_CUDA | VMAF_FEATURE_EXTRACTOR_SYCL)) != 0;
    return !gpu && vmaf->thread_pool && !(flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL);
}

/* GPU double-buffer dispatch for extractors that implement submit/collect:
 * collect the previous frame's results, then submit the current frame so GPU
 * compute of frame N-1 overlaps the CPU-side command recording of frame N.
 * On failure the PREV_REF reference taken by the caller is released; on
 * success its ownership transfers into the submitted work and the
 * extractor's collect() path releases it on the next frame. */
static int dispatch_gpu_double_buffer(VmafContext *vmaf, VmafFeatureExtractorContext *fex_ctx,
                                      VmafPicture *ref, VmafPicture *dist, unsigned index)
{
    int err = 0;
    if (fex_ctx->gpu_pending) {
        err = vmaf_feature_extractor_context_collect(fex_ctx, fex_ctx->gpu_pending_index,
                                                     vmaf->feature_collector);
        fex_ctx->gpu_pending = false;
    }
    if (!err)
        err = vmaf_feature_extractor_context_submit(fex_ctx, ref, NULL, dist, NULL, index);
    if (err) {
        fex_release_prev_ref(fex_ctx->fex);
        return err;
    }
    fex_ctx->gpu_pending = true;
    fex_ctx->gpu_pending_index = index;
    return 0;
}

static int read_pictures_dispatch_one(VmafContext *vmaf, VmafFeatureExtractorContext *fex_ctx,
                                      VmafPicture *ref, VmafPicture *dist, unsigned index)
{
    /* ADR-0778 Fix-A: use vmaf_picture_ref, not a bare struct copy, so
     * the synchronous non-GPU dispatch holds its own counted reference to
     * the previous-frame buffer.  The CUDA batch path already uses
     * vmaf_picture_ref here; this aligns the remaining code path.  Without
     * the ref, vmaf->prev_ref can be decremented by
     * read_pictures_update_prev_ref on the next frame while the extractor is
     * still reading its data, opening a use-after-free window when the
     * refcount hits zero and the pool reuses the buffer. */
    if ((fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_PREV_REF) && vmaf->prev_ref.ref)
        (void)vmaf_picture_ref(&fex_ctx->fex->prev_ref, &vmaf->prev_ref);

    if (fex_ctx->fex->submit && fex_ctx->fex->collect)
        return dispatch_gpu_double_buffer(vmaf, fex_ctx, ref, dist, index);

    const int err = vmaf_feature_extractor_context_extract(fex_ctx, ref, NULL, dist, NULL, index,
                                                           vmaf->feature_collector);
    if (fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_PREV_REF)
        fex_release_prev_ref(fex_ctx->fex);
    return err;
}

/* Upstream dispatch function. Refactoring is tracked in .workingdir2/OPEN.md.
 * `ref` / `dist` are only read on the CPU configuration cppcheck analyses,
 * but the SYCL build hands them to vmaf_sycl_shared_frame_upload(), whose
 * prototype (src/sycl/common.h) takes mutable pictures, so they cannot be
 * const-qualified for every backend. Suppression cited per ADR-0278. */
/* cppcheck-suppress constParameterPointer ; SYCL upload takes mutable pictures, see above */
static int read_pictures_validate_and_prep(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist,
                                           unsigned index)
{
    /* Enforce monotonically-increasing index: motion / motion2 / motion3
     * use sliding windows keyed by `index % N`, so out-of-order submission
     * silently corrupts their internal state (Netflix#910, ADR-0152).
     * Duplicate indices are also rejected because the sliding-window
     * state would be ambiguous. */
    if (vmaf->have_last_index && index <= vmaf->last_index)
        return -EINVAL;

    int err = validate_pic_params(vmaf, ref, dist);
    if (err)
        return err;
    err = check_picture_pool(vmaf);
    if (err)
        return err;
#ifdef HAVE_CUDA
    err = check_ring_buffer(vmaf);
    if (err)
        return err;
#endif
#ifdef HAVE_SYCL
    err = read_pictures_sycl_prep(vmaf, ref, dist);
    if (err)
        return err;
#endif
    vmaf->last_index = index;
    vmaf->have_last_index = true;
    return 0;
}

/* Per-call picture bookkeeping for vmaf_read_pictures(). `ref` / `dist`
 * point at whichever pictures the host-side consumers (DNN inference, the
 * worker pool, the prev_ref update) must see. The CUDA build additionally
 * carries the host / device translations of the caller's pictures that the
 * per-extractor dispatch selects from. */
typedef struct ReadPicturesFrame {
    VmafPicture *ref;
    VmafPicture *dist;
#ifdef HAVE_CUDA
    unsigned hw_flags;
    VmafPicture ref_host;
    VmafPicture ref_device;
    VmafPicture dist_host;
    VmafPicture dist_device;
#endif
} ReadPicturesFrame;

#ifdef HAVE_CUDA
/* CUDA fence-batching pass for ``read_pictures_extractor_loop``
 * (T-GPU-OPT-1, ADR-0242).
 *
 *  Phase 1: drain-all + collect-all (prev frame).
 *           ``vmaf_cuda_drain_batch_flush`` waits on every previously
 *           registered ``finished`` event in one host-side syscall,
 *           then per-extractor ``collect()`` runs as a buffer-read
 *           only (the lifecycle's ``drained`` flag short-circuits
 *           the per-stream cuStreamSynchronize).
 *  Phase 2: submit-all (curr frame).
 *           ``submit()`` records each extractor's ``finished`` event
 *           and registers it with the open drain batch; the next
 *           frame's drain_flush will wait on them as a group.
 *
 *  Vulkan / SYCL extractors keep their per-frame collect/submit
 *  ordering — only CUDA participates in the batch.
 */
static int read_pictures_extractor_loop_cuda(VmafContext *vmaf, VmafPicture *ref_device,
                                             VmafPicture *dist_device, unsigned index)
{
    if (!vmaf->cuda.state.ctx) {
        return 0;
    }

    /* Phase 1: batched drain + per-extractor collect of the prev
     * frame's pending GPU work. */
    int err = vmaf_cuda_drain_batch_flush(&vmaf->cuda.state);
    if (err) {
        return err;
    }
    for (unsigned i = 0; i < vmaf->registered_feature_extractors.cnt; i++) {
        VmafFeatureExtractorContext *fex_ctx = vmaf->registered_feature_extractors.fex_ctx[i];
        if (!(fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA)) {
            continue;
        }
        if (!fex_ctx->gpu_pending) {
            continue;
        }
        err = vmaf_feature_extractor_context_collect(fex_ctx, fex_ctx->gpu_pending_index,
                                                     vmaf->feature_collector);
        fex_ctx->gpu_pending = false;
        /* Release the prev_ref reference that was acquired in the Phase 2
         * submit loop (ADR-0778 Fix-B).  The GPU work has completed by the
         * time collect() returns, so it is safe to drop the ref now. */
        fex_release_prev_ref(fex_ctx->fex);
        if (err) {
            vmaf_cuda_drain_batch_close();
            return err;
        }
    }
    vmaf_cuda_drain_batch_close();

    /* Phase 2: batched submit of the curr frame. The drain batch is
     * re-opened so each extractor's submit() registers its
     * ``finished`` event for the next frame's drain_flush. */
    vmaf_cuda_drain_batch_open(&vmaf->cuda.state);
    for (unsigned i = 0; i < vmaf->registered_feature_extractors.cnt; i++) {
        VmafFeatureExtractorContext *fex_ctx = vmaf->registered_feature_extractors.fex_ctx[i];
        if (!(fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA)) {
            continue;
        }
        if (read_pictures_should_skip(vmaf, fex_ctx, index)) {
            continue;
        }
        if (!fex_ctx->fex->submit || !fex_ctx->fex->collect) {
            /* CUDA extractor without async submit/collect — falls back
             * to ``extract``. The non-CUDA pass below handles those. */
            continue;
        }
        /* Mirror the sync path (read_pictures_dispatch_one): use
         * vmaf_picture_ref rather than a bare struct copy so that the
         * CUDA-batch submit path holds its own counted reference to the
         * previous-frame buffer.  Without this, vmaf->prev_ref can be
         * decremented by read_pictures_update_prev_ref on the next frame
         * while a CUDA extractor carrying VMAF_FEATURE_EXTRACTOR_PREV_REF
         * is still reading its data — a latent UAF that becomes live the
         * moment such an extractor is registered.  ADR-0778 Fix-B. */
        if ((fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_PREV_REF) && vmaf->prev_ref.ref) {
            err = vmaf_picture_ref(&fex_ctx->fex->prev_ref, &vmaf->prev_ref);
            if (err) {
                vmaf_cuda_drain_batch_close();
                return err;
            }
        }
        err = vmaf_feature_extractor_context_submit(fex_ctx, ref_device, NULL, dist_device, NULL,
                                                    index);
        if (err) {
            fex_release_prev_ref(fex_ctx->fex);
            vmaf_cuda_drain_batch_close();
            return err;
        }
        fex_ctx->gpu_pending = true;
        fex_ctx->gpu_pending_index = index;
    }
    /* Drain batch stays open until the next frame's drain_flush —
     * Phase 1 above closes it before reopening. */
    return 0;
}
#endif /* HAVE_CUDA */

#ifdef HAVE_CUDA
/* Order this frame's device data against whoever produced it, ONCE, before any
 * CUDA extractor reads it (ADR-1199).
 *
 * With VMAF_CUDA_PICTURE_PREALLOCATION_METHOD_DEVICE the caller fetches a
 * libvmaf-owned device picture and copies into it *itself*, on a stream libvmaf
 * cannot see -- that is exactly what FFmpeg's `libvmaf_cuda` filter does via
 * hwupload. libvmaf records a picture's `ready` event only when it performs the
 * upload, so in that hand-over path the event is never recorded and every
 * extractor's `cuStreamWaitEvent(..., ready)` is vacuous. Nothing ordered the
 * kernels against the producer's write.
 *
 * Measured through the FFmpeg CUDA filter under concurrent CUDA load: 56 of 60
 * runs returned a corrupted frame without this barrier and 0 of 60 with it,
 * interleaved run-by-run so load hit both arms equally. The corruption was
 * ADM-only because ADM reads the raw planes first; the other extractors were
 * merely lucky, not synchronised.
 *
 * A full context barrier is the only construct that orders against a producer
 * whose stream is not exposed to us. It runs once per frame pair rather than
 * inside any one extractor, so a single host-side wait covers them all. Cost
 * through the FFmpeg path is within noise -- 177 ms against 179 ms, median of
 * 7, on an idle host -- because the work being waited for is the data these
 * kernels were about to read. */
static int cuda_order_pictures_against_producer(VmafContext *vmaf)
{
    if (!vmaf->cuda.state.ctx)
        return 0;

    CudaFunctions *const cu_f = vmaf->cuda.state.f;
    int err = cu_f->cuCtxPushCurrent(vmaf->cuda.state.ctx);
    if (!err) {
        err = cu_f->cuCtxSynchronize();
        (void)cu_f->cuCtxPopCurrent(NULL);
    }
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "could not order picture data against its producer (%d)\n",
                 err);
        return -EINVAL;
    }
    return 0;
}
#endif /* HAVE_CUDA */

static int read_pictures_extractor_loop(VmafContext *vmaf, ReadPicturesFrame *fr, unsigned index)
{
#ifdef HAVE_CUDA
    const int sync_err = cuda_order_pictures_against_producer(vmaf);
    if (sync_err)
        return sync_err;

    /* T-GPU-OPT-1 (ADR-0242): CUDA extractors are dispatched together
     * so their per-frame ``finished`` events can be drained in one
     * host-side syscall. Vulkan / SYCL extractors fall through to the
     * legacy per-extractor loop below (their backends own their own
     * ordering). */
    const int err =
        read_pictures_extractor_loop_cuda(vmaf, &fr->ref_device, &fr->dist_device, index);
    if (err) {
        return err;
    }
#endif
    for (unsigned i = 0; i < vmaf->registered_feature_extractors.cnt; i++) {
        VmafFeatureExtractorContext *fex_ctx = vmaf->registered_feature_extractors.fex_ctx[i];
        if (read_pictures_should_skip(vmaf, fex_ctx, index))
            continue;
#ifdef HAVE_CUDA
        /* CUDA extractors with submit+collect were already handled
         * in the batched pass above. Skip them here so we don't
         * double-submit. CUDA extractors WITHOUT async submit/collect
         * (none today, but possible for purely-synchronous kernels)
         * still need the legacy dispatch path. */
        const bool cuda = (fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA) != 0;
        if (cuda && fex_ctx->fex->submit && fex_ctx->fex->collect) {
            continue;
        }
        VmafPicture *ref = cuda ? &fr->ref_device : &fr->ref_host;
        VmafPicture *dist = cuda ? &fr->dist_device : &fr->dist_host;
#else
        VmafPicture *ref = fr->ref;
        VmafPicture *dist = fr->dist;
#endif
        const int err_one = read_pictures_dispatch_one(vmaf, fex_ctx, ref, dist, index);
        if (err_one)
            return err_one;
    }
    return 0;
}

/* Post-extractor stage: per-frame DNN inference + optional thread-pool
 * dispatch. Returns the final value vmaf_read_pictures should bubble up,
 * or a sentinel to indicate the caller should fall through to prev_ref
 * update + cleanup. */
static int read_pictures_post_extractor(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist,
                                        unsigned index, bool *done)
{
    *done = false;
    if (vmaf->dnn.sess) {
        int err = vmaf_ctx_dnn_run_frame(vmaf, ref, index);
        if (err)
            return err;
    }
    if (vmaf->thread_pool) {
        *done = true;
        return threaded_read_pictures_batch(vmaf, ref, dist, index);
    }
    return 0;
}

#ifdef HAVE_CUDA
/* CUDA: translate the caller's pictures into the host / device copies the
 * registered extractor set needs (the hw-flags bitmask is cached and only
 * recomputed after vmaf_use_feature() registers a new extractor — F2-B,
 * perf-audit-pipeline-2026-05-16). On failure the caller keeps ownership of
 * its pictures, as on any other pre-dispatch validation failure. CPU builds
 * have nothing to translate and skip the call entirely (a no-op stub would
 * leave vmaf_read_pictures() with a provably-dead error branch). */
static int read_pictures_frame_translate(VmafContext *vmaf, ReadPicturesFrame *fr)
{
    if (vmaf->rfe_hw_flags_dirty) {
        vmaf->rfe_hw_flags_cache = rfe_hw_flags(&vmaf->registered_feature_extractors);
        vmaf->rfe_hw_flags_dirty = false;
    }
    fr->hw_flags = vmaf->rfe_hw_flags_cache;
    return read_pictures_cuda_translate(vmaf, fr->ref, fr->dist, fr->hw_flags, &fr->ref_host,
                                        &fr->ref_device, &fr->dist_host, &fr->dist_device);
}
#endif /* HAVE_CUDA */

/* CUDA: after the extractor loop, hand the host translations to the
 * host-side consumers. When every registered extractor is CUDA-only
 * (hw_flags == HW_FLAG_DEVICE, no HW_FLAG_HOST bit) translate_picture_host()
 * early-returned without populating ref_host / dist_host, leaving them
 * zero-initialised — passing those to threaded_read_pictures_batch →
 * vmaf_picture_ref → vmaf_ref_fetch_increment would dereference NULL. On
 * that device-only path fr->ref / fr->dist stay the hwupload-uploaded
 * pictures provided by the caller (validated non-NULL on entry). */
static void read_pictures_frame_select_host(ReadPicturesFrame *fr)
{
#ifdef HAVE_CUDA
    if (fr->hw_flags & HW_FLAG_HOST) {
        fr->ref = &fr->ref_host;
        fr->dist = &fr->dist_host;
    }
#else
    (void)fr;
#endif
}

/* Release every picture this call still owns and fold the release status
 * into `err`. Always unref the caller's ref/dist: with the always-on picture
 * pool, leaking even one picture per frame holds a pool slot and the next
 * vmaf_picture_pool_fetch deadlocks in pthread_cond_wait once the pool
 * drains. */
static int read_pictures_frame_cleanup(VmafContext *vmaf, ReadPicturesFrame *fr, int err)
{
#ifdef HAVE_CUDA
    if (fr->hw_flags & HW_FLAG_HOST) {
        return err | read_pictures_cuda_cleanup(vmaf, &fr->ref_host, &fr->ref_device,
                                                &fr->dist_host, &fr->dist_device);
    }
#else
    (void)vmaf;
#endif
    err |= vmaf_picture_unref(fr->ref);
    err |= vmaf_picture_unref(fr->dist);
    return err;
}

/* Cleanup after threaded_read_pictures_batch took ownership of (and
 * released) the host pictures. CUDA: only the device translations are left,
 * and only when they are fresh ring-buffer allocations (HW_FLAG_HOST set);
 * on the device-only path ref_device is a struct copy of the caller's
 * picture whose lifetime the caller owns. Running the full
 * read_pictures_frame_cleanup here would double-unref the host pictures and
 * corrupt the pool free-list (PR #838 regression). */
static int read_pictures_frame_cleanup_after_batch(VmafContext *vmaf, ReadPicturesFrame *fr,
                                                   int err)
{
#ifdef HAVE_CUDA
    if (fr->hw_flags & HW_FLAG_HOST)
        err |= read_pictures_cuda_cleanup_device_only(vmaf, &fr->ref_device, &fr->dist_device);
#else
    (void)vmaf;
    (void)fr;
#endif
    return err;
}

int vmaf_read_pictures(VmafContext *vmaf, VmafPicture *ref, VmafPicture *dist, unsigned index)
{
    if (!vmaf)
        return -EINVAL;
    if (vmaf->flushed)
        return -EINVAL;
    if (!ref != !dist)
        return -EINVAL;
    if (!ref && !dist)
        return flush_context(vmaf);

    int err = read_pictures_validate_and_prep(vmaf, ref, dist, index);
    if (err)
        return err;
    /* Increment only after successful validation so a retry on transient
     * -ENOMEM does not double-count the frame and corrupt FPS / end-index. */
    vmaf->pic_cnt++;

    ReadPicturesFrame fr = {.ref = ref, .dist = dist};
#ifdef HAVE_CUDA
    err = read_pictures_frame_translate(vmaf, &fr);
    if (err)
        return err;
#endif

    err = read_pictures_extractor_loop(vmaf, &fr, index);
    if (err)
        return read_pictures_frame_cleanup(vmaf, &fr, err);

    read_pictures_frame_select_host(&fr);

    bool done = false;
    err = read_pictures_post_extractor(vmaf, fr.ref, fr.dist, index, &done);
    if (done)
        return read_pictures_frame_cleanup_after_batch(vmaf, &fr, err);
    if (err)
        return read_pictures_frame_cleanup(vmaf, &fr, err);

    read_pictures_update_prev_ref(vmaf, fr.ref);
    return read_pictures_frame_cleanup(vmaf, &fr, 0);
}

#ifdef HAVE_SYCL
int vmaf_read_pictures_sycl(VmafContext *vmaf, unsigned index)
{
    if (!vmaf)
        return -EINVAL;
    if (!vmaf->sycl.state)
        return -EINVAL;
    if (vmaf->flushed)
        return -EINVAL;

    int err = 0;

    // Ensure de-tile kernels on the primary queue have finished reading from
    // imported VA surface memory.  After this function returns, the caller
    // (FFmpeg filter) may release the AVFrame, letting the QSV hwupload pool
    // reuse the VA surface for the next frame.  Without this wait, the async
    // de-tile could race with the hwupload writing new data.
    err = vmaf_sycl_queue_wait(vmaf->sycl.state);
    if (err)
        return err;
    /* Increment only after queue_wait succeeds so a retry on error does not
     * double-count the frame (mirrors the fix to vmaf_read_pictures, ADR-1008). */
    vmaf->pic_cnt++;

    // Advance double-buffer slot and frame counter for the zero-copy VA import path.
    // Mirrors shared_frame_upload:597-599: cur_compute = cur_upload; cur_upload = 1-cur_upload.
    // The imports above wrote shared_*_buf[cur_upload]; after advance, cur_compute
    // points to that freshly-imported slot so compute kernels read the correct frame.
    // Also increments frame_counter for graph_submit/graph_wait synchronisation:
    //   - graph_wait idempotency returns stale results without this increment
    //   - graph_submit fires after every extractor instead of once per frame
    vmaf_sycl_advance_frame(vmaf->sycl.state);

    /* Diagnostic: per-frame D2H checksum of what compute will actually read.
     * pic_cnt was already incremented above, so use pic_cnt - 1 for 0-based
     * frame index (frame=0 on the first call).  Gated by VMAF_SYCL_CHECKSUM=1
     * — zero cost when unset.  Placed after advance_frame and before the
     * extractor loop so the primary queue is already drained. */
    (void)vmaf_sycl_checksum_y_slot(vmaf->sycl.state, 1, vmaf->pic_cnt - 1, "sycl");
    (void)vmaf_sycl_checksum_y_slot(vmaf->sycl.state, 0, vmaf->pic_cnt - 1, "sycl");

    // GPU extractor loop: collect previous results, then submit new work.
    // No upload needed — caller already wrote Y plane data into the shared
    // SYCL USM device buffers (e.g. via VPL Level Zero interop).
    for (unsigned i = 0; i < vmaf->registered_feature_extractors.cnt; i++) {
        VmafFeatureExtractorContext *fex_ctx = vmaf->registered_feature_extractors.fex_ctx[i];

        if (!(fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_SYCL))
            continue;

        if (!(fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_TEMPORAL)) {
            if ((vmaf->cfg.n_subsample > 1) && (index % vmaf->cfg.n_subsample))
                continue;
        }

        // Lazy initialization
        if (!fex_ctx->is_initialized) {
            err = vmaf_feature_extractor_context_init(fex_ctx, vmaf->pic_params.pix_fmt,
                                                      vmaf->pic_params.bpc, vmaf->pic_params.w,
                                                      vmaf->pic_params.h);
            if (err)
                return err;
        }

        // Collect previous frame's results (double-buffered)
        if (fex_ctx->gpu_pending) {
            err = vmaf_feature_extractor_context_collect(fex_ctx, fex_ctx->gpu_pending_index,
                                                         vmaf->feature_collector);
            fex_ctx->gpu_pending = false;
            if (err)
                return err;
        }

        // Submit current frame (GPU buffers already populated)
        err = vmaf_feature_extractor_context_submit_nocopy(fex_ctx, index);
        if (err)
            return err;
        fex_ctx->gpu_pending = true;
        fex_ctx->gpu_pending_index = index;
    }

    return err;
}

int vmaf_flush_sycl(VmafContext *vmaf)
{
    if (!vmaf)
        return -EINVAL;
    if (vmaf->flushed)
        return -EINVAL;

    int err = 0;

    if (vmaf->sycl.state) {
        RegisteredFeatureExtractors rfe = vmaf->registered_feature_extractors;
        // Collect any pending double-buffered SYCL work
        for (unsigned i = 0; i < rfe.cnt; i++) {
            if ((rfe.fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_SYCL) &&
                rfe.fex_ctx[i]->gpu_pending) {
                err |= vmaf_feature_extractor_context_collect(
                    rfe.fex_ctx[i], rfe.fex_ctx[i]->gpu_pending_index, vmaf->feature_collector);
                rfe.fex_ctx[i]->gpu_pending = false;
            }
        }
        for (unsigned i = 0; i < rfe.cnt; i++) {
            if (rfe.fex_ctx[i]->fex->flags & VMAF_FEATURE_EXTRACTOR_SYCL)
                err |=
                    vmaf_feature_extractor_context_flush(rfe.fex_ctx[i], vmaf->feature_collector);
        }
        vmaf_sycl_queue_wait(vmaf->sycl.state);
        vmaf_sycl_print_timing(vmaf->sycl.state);
    }

    if (!err)
        vmaf->flushed = true;
    return err;
}
#endif

int vmaf_register_metadata_handler(VmafContext *vmaf, VmafMetadataConfiguration cfg)
{
    if (!vmaf)
        return -EINVAL;

    return vmaf_feature_collector_register_metadata(vmaf->feature_collector, cfg);
}

/* Fence the pipeline so a read at ``index`` sees every write that is already
 * owed for it (Netflix/vmaf#1305, docs/state.md
 * T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03).
 *
 * The read entry points below used to go straight to the feature collector, so
 * a caller pulling index N-2 while N was in flight could read a slot the
 * producing thread (or the GPU collect step) had not written yet. Fencing on
 * every read would serialise the pipeline, so the callers fence only after the
 * lock-free read reports the slot unwritten: wait for the worker threads, then,
 * on CUDA, drain the batch and run the pending collect() so the slots the
 * device already computed are actually in the collector. The drain batch stays
 * open afterwards — Phase 1 of the next vmaf_read_pictures() re-flushes it, and
 * flushing twice is a no-op because the flush clears its entries.
 *
 * Returns 0 when it is worth re-reading, negative on a fence error. */
static int fence_for_read(VmafContext *vmaf, unsigned index)
{
    int err = 0;

    if (vmaf->thread_pool) {
        err = vmaf_thread_pool_wait(vmaf->thread_pool);
        if (err)
            return err;
    }

#ifdef HAVE_CUDA
    if (vmaf->cuda.state.ctx) {
        err = vmaf_cuda_drain_batch_flush(&vmaf->cuda.state);
        if (err)
            return err;
        for (unsigned i = 0; i < vmaf->registered_feature_extractors.cnt; i++) {
            VmafFeatureExtractorContext *fex_ctx = vmaf->registered_feature_extractors.fex_ctx[i];
            if (!(fex_ctx->fex->flags & VMAF_FEATURE_EXTRACTOR_CUDA))
                continue;
            if (!fex_ctx->gpu_pending)
                continue;
            if (fex_ctx->gpu_pending_index > index)
                continue;
            err = vmaf_feature_extractor_context_collect(fex_ctx, fex_ctx->gpu_pending_index,
                                                         vmaf->feature_collector);
            fex_ctx->gpu_pending = false;
            fex_release_prev_ref(fex_ctx->fex);
            if (err)
                return err;
        }
    }
#else
    (void)index;
#endif

    return 0;
}

int vmaf_feature_score_at_index(VmafContext *vmaf, const char *feature_name, double *score,
                                unsigned index)
{
    if (!vmaf)
        return -EINVAL;
    if (!feature_name)
        return -EINVAL;
    if (!score)
        return -EINVAL;

    int err = vmaf_feature_collector_get_score(vmaf->feature_collector, feature_name, score, index);
    if (err == -EAGAIN) {
        /* The slot exists but is unwritten: fence and read once more before
         * telling the caller the frame is not ready (Netflix/vmaf#1305). */
        const int fence_err = fence_for_read(vmaf, index);
        if (fence_err)
            return fence_err;
        err = vmaf_feature_collector_get_score(vmaf->feature_collector, feature_name, score, index);
    }
    return err;
}

int vmaf_score_at_index(VmafContext *vmaf, VmafModel *model, double *score, unsigned index)
{
    if (!vmaf)
        return -EINVAL;
    if (!model)
        return -EINVAL;
    if (!score)
        return -EINVAL;

    int err = vmaf_feature_collector_get_score(vmaf->feature_collector, model->name, score, index);
    /* Netflix#755 / ADR-0154: -EAGAIN from the *model* score vector means the
     * model prediction at this index has not been computed yet — the "vmaf"
     * feature vector was created when a prior index was scored (frame 0), so
     * frame 1+ slots exist in the vector but are unwritten (written=false).
     * We MUST call vmaf_predict_score_at_index to compute and write the score.
     *
     * The original guard `err != -EAGAIN` was intended to propagate -EAGAIN
     * from retroactive-write *input* features (integer_motion motion2/motion3),
     * not from the *output* model score itself.  Applying it to the model
     * score incorrectly short-circuits prediction for all frames after the
     * first, causing vmaf_score_pooled to return -EAGAIN for multi-frame
     * sequences.  ADR-1073. */
    if (err) {
        /* Netflix/vmaf#1305: the input features for this index may still be in
         * flight (worker threads, or a CUDA collect that has not run yet), so
         * fence before predicting — otherwise the prediction is computed from
         * unwritten slots. */
        const int fence_err = fence_for_read(vmaf, index);
        if (fence_err)
            return fence_err;
        err = vmaf_predict_score_at_index(model, vmaf->feature_collector, index, score, true, false,
                                          0);
    }

    return err;
}

int vmaf_score_at_index_model_collection(VmafContext *vmaf, VmafModelCollection *model_collection,
                                         VmafModelCollectionScore *score, unsigned index)
{
    if (!vmaf)
        return -EINVAL;
    if (!model_collection)
        return -EINVAL;
    if (!score)
        return -EINVAL;

    return vmaf_predict_score_at_index_model_collection(model_collection, vmaf->feature_collector,
                                                        index, score);
}

/* Accumulators for vmaf_feature_score_pooled, kept in one struct so the reduce
 * step can be factored into a helper without a long parameter list. */
typedef struct PoolAccumulators {
    unsigned pic_cnt;
    double min;
    double max;
    double sum;         /* Σ s_i (unweighted)                                  */
    double i_sum;       /* Σ 1/(s_i + 1) (unweighted harmonic)                 */
    double w_sum;       /* Σ w_i (weighted; only touched when `weighting`)     */
    double w_score_sum; /* Σ w_i·s_i                                           */
    double w_i_sum;     /* Σ w_i/(s_i + 1)                                     */
} PoolAccumulators;

/* Reduce the accumulated sums to a single pooled score per method.
 *
 * GOLDEN-GATE ISOLATION (ADR-1118): when `weighting` is false the MEAN and
 * HARMONIC_MEAN branches run the exact same float expressions as upstream, so
 * the no-side-data path (and the Netflix golden pairs) is byte-identical.
 * Returns 0 on success or -EINVAL for an unknown method. */
static int pool_reduce(const PoolAccumulators *a, enum VmafPoolingMethod pool_method,
                       bool weighting, double *score)
{
    switch (pool_method) {
    case VMAF_POOL_METHOD_MEAN:
        /* Weighted mean Σ(w_i·s_i)/Σ(w_i). With every w_i == 1 this equals
         * sum / pic_cnt, but the unweighted branch stays literally identical to
         * upstream for the golden path. w_sum > 0 always holds when active;
         * guard anyway. */
        *score = (weighting && a->w_sum > 0.) ? (a->w_score_sum / a->w_sum) : (a->sum / a->pic_cnt);
        return 0;
    case VMAF_POOL_METHOD_MIN:
        /* Re-weighting cannot reorder the min/max of the per-frame scores;
         * these are intentionally unaffected (documented in docs/backends). */
        *score = a->min;
        return 0;
    case VMAF_POOL_METHOD_MAX:
        *score = a->max;
        return 0;
    case VMAF_POOL_METHOD_HARMONIC_MEAN:
        if (weighting && a->w_sum > 0.) {
            /* Weighted harmonic mean of (s_i + 1): Σw_i / Σ(w_i/(s_i+1)) - 1. */
            *score = (a->w_i_sum > 0.) ? (a->w_sum / a->w_i_sum - 1.0) : 0.;
        } else {
            *score = (a->i_sum > 0.) ? ((double)a->pic_cnt / a->i_sum - 1.0) : 0.;
        }
        return 0;
    case VMAF_POOL_METHOD_MEDIAN:
    case VMAF_POOL_METHOD_PERC5:
    case VMAF_POOL_METHOD_PERC10:
    case VMAF_POOL_METHOD_PERC20:
        /* Percentile pooling methods ignore perceptual weights (identical
         * to MIN/MAX) and require the full sorted score vector, so they are
         * handled directly in vmaf_feature_score_pooled rather than via
         * stream accumulators. */
        return -EINVAL;
    default:
        return -EINVAL;
    }
}

int vmaf_feature_score_pooled(VmafContext *vmaf, const char *feature_name,
                              enum VmafPoolingMethod pool_method, double *score, unsigned index_low,
                              unsigned index_high)
{
    if (!vmaf)
        return -EINVAL;
    if (!feature_name)
        return -EINVAL;
    if (!score)
        return -EINVAL;
    if (index_low > index_high)
        return -EINVAL;
    if (!pool_method)
        return -EINVAL;

    if (pool_method == VMAF_POOL_METHOD_MEDIAN || pool_method == VMAF_POOL_METHOD_PERC5 ||
        pool_method == VMAF_POOL_METHOD_PERC10 || pool_method == VMAF_POOL_METHOD_PERC20) {
        const unsigned capacity = (index_high - index_low + 1);
        double *scores = (double *)malloc(capacity * sizeof(double));
        if (!scores)
            return -ENOMEM;

        unsigned pic_cnt = 0;
        for (unsigned i = index_low; i <= index_high; i++) {
            if ((vmaf->cfg.n_subsample > 1) && (i % vmaf->cfg.n_subsample))
                continue;
            double s;
            int err = vmaf_feature_score_at_index(vmaf, feature_name, &s, i);
            if (err) {
                free(scores);
                return err;
            }
            scores[pic_cnt++] = s;
        }

        if (pic_cnt == 0) {
            free(scores);
            return -EINVAL;
        }

        qsort(scores, pic_cnt, sizeof(double), score_compare);

        double perc = 50.0;
        if (pool_method == VMAF_POOL_METHOD_PERC5)
            perc = 5.0;
        else if (pool_method == VMAF_POOL_METHOD_PERC10)
            perc = 10.0;
        else if (pool_method == VMAF_POOL_METHOD_PERC20)
            perc = 20.0;

        *score = percentile(scores, pic_cnt, perc);
        free(scores);
        return 0;
    }

    /* GOLDEN-GATE ISOLATION (ADR-1118): perceptual weighting only diverges from
     * the legacy arithmetic when it is enabled AND at least one frame carries a
     * Pelorus side-data summary. When inactive — the default, and always for the
     * Netflix golden pairs (which have no side-data) — `weighting` is false and
     * the weighted accumulators are never touched, so MEAN / HARMONIC_MEAN run
     * the exact same float operations in the exact same order as upstream. */
    const bool weighting = vmaf_perceptual_weight_active(&vmaf->perceptual);

    PoolAccumulators a = {0};
    for (unsigned i = index_low; i <= index_high; i++) {
        if ((vmaf->cfg.n_subsample > 1) && (i % vmaf->cfg.n_subsample))
            continue;
        a.pic_cnt++;
        double s;
        int err = vmaf_feature_score_at_index(vmaf, feature_name, &s, i);
        if (err)
            return err;
        a.sum += s;
        a.i_sum += 1. / (s + 1.);
        if ((i == index_low) || (s < a.min))
            a.min = s;
        if ((i == index_low) || (s > a.max))
            a.max = s;
        if (weighting) {
            /* w == 1.0 for any frame without a stored summary, so a partially
             * annotated sequence still degrades cleanly per-frame. */
            const double w = vmaf_perceptual_weight_at_index(&vmaf->perceptual, i);
            a.w_sum += w;
            a.w_score_sum += w * s;
            a.w_i_sum += w / (s + 1.);
        }
    }

    /* When n_subsample skips every frame in [index_low, index_high],
     * pic_cnt stays 0 and the MEAN / HARMONIC_MEAN cases would divide
     * by zero.  Reject cleanly. */
    if (a.pic_cnt == 0)
        return -EINVAL;

    return pool_reduce(&a, pool_method, weighting, score);
}

int vmaf_score_pooled(VmafContext *vmaf, VmafModel *model, enum VmafPoolingMethod pool_method,
                      double *score, unsigned index_low, unsigned index_high)
{
    if (!vmaf)
        return -EINVAL;
    if (!model)
        return -EINVAL;
    if (!score)
        return -EINVAL;
    if (index_low > index_high)
        return -EINVAL;
    if (!pool_method)
        return -EINVAL;

    for (unsigned i = index_low; i <= index_high; i++) {
        if ((vmaf->cfg.n_subsample > 1) && (i % vmaf->cfg.n_subsample))
            continue;
        double vmaf_score;
        int err = vmaf_score_at_index(vmaf, model, &vmaf_score, i);
        if (err)
            return err;
    }

    return vmaf_feature_score_pooled(vmaf, model->name, pool_method, score, index_low, index_high);
}

int vmaf_score_pooled_model_collection(VmafContext *vmaf, VmafModelCollection *model_collection,
                                       enum VmafPoolingMethod pool_method,
                                       VmafModelCollectionScore *score, unsigned index_low,
                                       unsigned index_high)
{
    if (!vmaf)
        return -EINVAL;
    if (!model_collection)
        return -EINVAL;
    if (!score)
        return -EINVAL;
    if (index_low > index_high)
        return -EINVAL;
    if (!pool_method)
        return -EINVAL;

    int err = 0;
    for (unsigned i = index_low; i <= index_high; i++) {
        if ((vmaf->cfg.n_subsample > 1) && (i % vmaf->cfg.n_subsample))
            continue;
        VmafModelCollectionScore s;
        err = vmaf_score_at_index_model_collection(vmaf, model_collection, &s, i);
        if (err)
            return err;
    }

    score->type = VMAF_MODEL_COLLECTION_SCORE_BOOTSTRAP;

    const char *suffix_lo = "_ci_p95_lo";
    const char *suffix_hi = "_ci_p95_hi";
    const char *suffix_bagging = "_bagging";
    const char *suffix_stddev = "_stddev";
    const size_t name_sz = strlen(model_collection->name) + strlen(suffix_lo) + 1;
    /* Heap-allocated for MSVC portability (no VLAs). The buffer is short-lived
     * and freed before return. */
    char *name = (char *)calloc(1u, name_sz);
    if (!name)
        return -ENOMEM;

    (void)snprintf(name, name_sz, "%s%s", model_collection->name, suffix_bagging);
    err |= vmaf_feature_score_pooled(vmaf, name, pool_method, &score->bootstrap.bagging_score,
                                     index_low, index_high);

    (void)snprintf(name, name_sz, "%s%s", model_collection->name, suffix_stddev);
    err |= vmaf_feature_score_pooled(vmaf, name, pool_method, &score->bootstrap.stddev, index_low,
                                     index_high);

    (void)snprintf(name, name_sz, "%s%s", model_collection->name, suffix_lo);
    err |= vmaf_feature_score_pooled(vmaf, name, pool_method, &score->bootstrap.ci.p95.lo,
                                     index_low, index_high);

    (void)snprintf(name, name_sz, "%s%s", model_collection->name, suffix_hi);
    err |= vmaf_feature_score_pooled(vmaf, name, pool_method, &score->bootstrap.ci.p95.hi,
                                     index_low, index_high);

    free(name);
    return err;
}

/* Read-only accessor, but the exported prototype in
 * core/include/libvmaf/libvmaf.h takes a mutable context and the public C
 * ABI is frozen (no signature changes to the exported API in this rework), so
 * the parameter is deliberately not const-qualified here. Suppression cited
 * per ADR-0278. */
/* cppcheck-suppress constParameterPointer ; public ABI prototype is frozen, see above */
int vmaf_context_get_backend(VmafContext *vmaf, enum VmafBackend *out)
{
    if (!vmaf)
        return -EINVAL;
    if (!out)
        return -EINVAL;

    *out = vmaf->active_backend;
    return 0;
}

const char *vmaf_version(void)
{
    return VMAF_VERSION;
}

/* Open `output_path` for writing with mode 0644 so the output file is never
 * world-writable: fopen(3) defaults to 0666 & ~umask, which CodeQL flags as
 * cpp/world-writable-file-creation, so open(2) + fdopen(3) pin the
 * permission bits up front. Returns -errno of the failing call. */
static int output_file_open(const char *output_path, FILE **outfile)
{
#ifdef _WIN32
    const int outfd = _open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#else
    const int outfd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
    if (outfd < 0) {
        /* Capture errno immediately — it is clobbered by fprintf(3). */
        const int open_errno = errno;
        (void)fprintf(stderr, "could not open file: %s\n", output_path);
        return -open_errno;
    }
#ifdef _WIN32
    *outfile = _fdopen(outfd, "w");
#else
    *outfile = fdopen(outfd, "w");
#endif
    if (!*outfile) {
        /* Same: capture before fprintf clobbers errno. */
        const int fdopen_errno = errno;
        (void)fprintf(stderr, "could not open file: %s\n", output_path);
#ifdef _WIN32
        (void)_close(outfd);
#else
        (void)close(outfd);
#endif
        return -fdopen_errno;
    }
    return 0;
}

/* ADR-0606: Compute fps defensively to avoid 0.0/0.0 = NaN when either
 * pic_cnt == 0 (no frames read, e.g. import-only callers) or the timer
 * resolution is too coarse to distinguish begin and end.  NaN is handled
 * correctly by the JSON fpclassify() switch, but Apple Clang with
 * -ffast-math or a strict FP environment may generate a SIGFPE instead of
 * the IEEE-754 quiet NaN on 0.0/0.0.  Emit 0.0 explicitly in those cases
 * so the writers receive a well-defined, finite value. */
static double output_fps(const VmafContext *vmaf)
{
    const clock_t timer_elapsed =
        vmaf->feature_collector->timer.end - vmaf->feature_collector->timer.begin;
    if (vmaf->pic_cnt == 0 || timer_elapsed == 0)
        return 0.0;
    return (double)vmaf->pic_cnt / ((double)timer_elapsed / (double)CLOCKS_PER_SEC);
}

/* Route to the writer for `fmt`; -EINVAL for an unknown format. */
static int output_write(VmafContext *vmaf, enum VmafOutputFormat fmt, FILE *outfile, double fps,
                        const char *score_format)
{
    switch (fmt) {
    case VMAF_OUTPUT_FORMAT_XML:
        return vmaf_write_output_xml(vmaf, vmaf->feature_collector, outfile, vmaf->cfg.n_subsample,
                                     vmaf->pic_params.w, vmaf->pic_params.h, fps, vmaf->pic_cnt,
                                     score_format);
    case VMAF_OUTPUT_FORMAT_JSON:
        return vmaf_write_output_json(vmaf, vmaf->feature_collector, outfile, vmaf->cfg.n_subsample,
                                      fps, vmaf->pic_cnt, score_format);
    case VMAF_OUTPUT_FORMAT_CSV:
        return vmaf_write_output_csv(vmaf->feature_collector, outfile, vmaf->cfg.n_subsample,
                                     score_format);
    case VMAF_OUTPUT_FORMAT_SUB:
        return vmaf_write_output_sub(vmaf->feature_collector, outfile, vmaf->cfg.n_subsample,
                                     score_format);
    default:
        return -EINVAL;
    }
}

int vmaf_write_output_with_format(VmafContext *vmaf, const char *output_path,
                                  enum VmafOutputFormat fmt, const char *score_format)
{
    /* ADR-0602: both vmaf and output_path are annotated nonnull on macOS
     * (open(2) is __nonnull(1); vmaf->feature_collector is dereferenced
     * immediately below).  On macOS, Apple Clang and glibc both declare
     * open() with __attribute__((nonnull(1))); passing NULL skips the
     * call entirely and falls through to undefined memory with SIGSEGV
     * before the error-path even executes.  Guard both up front. */
    if (!vmaf) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmaf_write_output: vmaf context must not be NULL\n");
        return -EINVAL;
    }
    if (!vmaf->feature_collector) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmaf_write_output: feature_collector not initialised\n");
        return -EINVAL;
    }
    if (!output_path) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmaf_write_output: output_path must not be NULL\n");
        return -EINVAL;
    }

    FILE *outfile = NULL;
    int ret = output_file_open(output_path, &outfile);
    if (ret)
        return ret;

    ret = output_write(vmaf, fmt, outfile, output_fps(vmaf), score_format);

    if (fclose(outfile) != 0 && ret == 0)
        ret = -EIO;
    return ret;
}

int vmaf_write_output(VmafContext *vmaf, const char *output_path, enum VmafOutputFormat fmt)
{
    return vmaf_write_output_with_format(vmaf, output_path, fmt, NULL);
}

/*
 * Internal test accessor.
 *
 * Tests that need VmafFeatureCollector must not include libvmaf.c directly
 * while also linking libvmaf. Apple ld64 + LTO has resolved that duplicate
 * definition pattern incorrectly under allocator poisoning, crashing macOS CI
 * in writer tests. See libvmaf_priv.h for the declaration.
 */
VmafFeatureCollector *vmaf_feature_collector_get(const VmafContext *vmaf)
{
    if (!vmaf)
        return NULL;
    return vmaf->feature_collector;
}

/* NOLINTEND(modernize-use-nullptr) */
