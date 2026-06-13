/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
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
#include <math.h>
#include <string.h>

#include "common.h"
#include "cpu.h"
#include "common/alignment.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "cuda/integer_motion_cuda.h"
#include "log.h"
#include "mem.h"
#include "motion_blend_tools.h"
#include "picture.h"
#include "picture_cuda.h"
#include "cuda_helper.cuh"

/* Default upper clamp on motion / motion2 / motion3 — mirrors
 * DEFAULT_MOTION_MAX_VAL in libvmaf/src/feature/integer_motion.c.
 * T3-15(c) / ADR-0219. */
#define MOTION_CUDA_DEFAULT_MAX_VAL (10000.0)

/* Number of kernel launches to pipeline before a single cuStreamSynchronize
 * drains all pending device-to-host copies (ADR-0845).
 *
 * At 576p the kernel takes ~7 µs but the per-frame sync overhead is ~12.7 ms;
 * batching 8 frames into one sync reduces per-frame host overhead by ~7/8.
 * Must be a power-of-two and >= 2. Increasing beyond 8 gives diminishing
 * returns once the sync overhead (fixed ~2 ms) is amortised over > 4 frames.
 */
#define MOTION_BATCH_DEPTH 8

typedef struct MotionStateCuda {
    CUevent event;
    CUfunction funcbpc8, funcbpc16;
    CUstream str;
    VmafCudaBuffer *blur[2];
    /* Per-slot device SAD accumulators (one per batch frame slot,
     * ADR-0845). Each submit zeroes its slot and the kernel atomic-adds
     * into it; collect batch-drains all MOTION_BATCH_DEPTH slots in one
     * cuStreamSynchronize instead of one sync per frame. */
    VmafCudaBuffer *sad[MOTION_BATCH_DEPTH];
    /* Single pinned host array of MOTION_BATCH_DEPTH uint64_t values.
     * Index: sad_host[index % MOTION_BATCH_DEPTH]. Allocated once via
     * vmaf_cuda_buffer_host_alloc with size = MOTION_BATCH_DEPTH * 8. */
    uint64_t *sad_host;
    /* Per-slot score ring: holds normalized SAD scores until the batch
     * boundary collect emits all MOTION_BATCH_DEPTH frames at once. */
    double score_ring[MOTION_BATCH_DEPTH];
    /* Index of the last batch-boundary where we synced and emitted scores.
     * Initialised to -1. Used to avoid re-emitting in flush(). */
    int last_batch_boundary;
    unsigned index;
    unsigned frame_index;      /* count of frames processed so far (for motion3) */
    unsigned frame_w, frame_h; // stored by submit for collect
    double score;
    /* motion3 post-processing state — tracks the last *unaveraged*
     * blended score so the moving-average rule cascades correctly,
     * mirroring the CPU MotionState.previous_score field. */
    double prev_motion3_blended;
    bool debug;
    bool motion_force_zero;
    bool motion_five_frame_window; /* rejected with -ENOTSUP — see init() */
    bool motion_moving_average;
    bool motion_add_uv; /* rejected with -ENOTSUP — see init(); ADR-0989 */
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_fps_weight;
    double motion_max_val;
    int (*calculate_motion_score)(const VmafPicture *src, VmafCudaBuffer *src_blurred,
                                  const VmafCudaBuffer *prev_blurred, VmafCudaBuffer *sad,
                                  unsigned width, unsigned height, ptrdiff_t src_stride,
                                  ptrdiff_t blurred_stride, unsigned src_bpc, CUfunction funcbpc8,
                                  CUfunction funcbpc16, CudaFunctions *cu_f, CUstream stream);
    /* PTX module backing the motion kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;
    VmafDictionary *feature_name_dict;
} MotionStateCuda;

/* Options table — mirrors libvmaf/src/feature/integer_motion.c.
 * The motion3-related post-processing options drive a host-side
 * derivation from motion2 (see collect()). T3-15(c) / ADR-0219. */
static const VmafOption options[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(MotionStateCuda, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "motion_force_zero",
        .help = "forcing motion score to zero",
        .offset = offsetof(MotionStateCuda, motion_force_zero),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_factor",
        .alias = "mbf",
        .help = "blend motion score given an offset",
        .offset = offsetof(MotionStateCuda, motion_blend_factor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_offset",
        .alias = "mbo",
        .help = "blend motion score starting from this offset",
        .offset = offsetof(MotionStateCuda, motion_blend_offset),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 40.0,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_fps_weight",
        .alias = "mfw",
        .help = "fps-aware multiplicative weight/correction",
        .offset = offsetof(MotionStateCuda, motion_fps_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 5.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_max_val",
        .alias = "mmxv",
        .help = "maximum value allowed; larger values will be clipped to this value",
        .offset = offsetof(MotionStateCuda, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = MOTION_CUDA_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_five_frame_window",
        .alias = "mffw",
        .help = "use five-frame temporal window (NOT YET SUPPORTED on CUDA — T3-15(c) deferred)",
        .offset = offsetof(MotionStateCuda, motion_five_frame_window),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_moving_average",
        .alias = "mma",
        .help = "use moving average for motion3 scores after first frame",
        .offset = offsetof(MotionStateCuda, motion_moving_average),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_add_uv",
        .alias = "mau",
        .help = "include U and V plane SADs (NOT YET SUPPORTED on CUDA — ADR-0989 deferred)",
        .offset = offsetof(MotionStateCuda, motion_add_uv),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0}};

static int extract_force_zero(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                              VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                              VmafPicture *dist_pic_90, unsigned index,
                              VmafFeatureCollector *feature_collector)
{
    MotionStateCuda *s = fex->priv;

    (void)fex;
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;

    int err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion2_score", 0., index);

    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion3_score", 0., index);

    if (!s->debug)
        return err;

    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion_score", 0., index);

    return err;
}

/* ------------------------------------------------------------------ */
/* motion3 post-processing — pure host-side scalar work.              */
/*                                                                     */
/* Mirrors libvmaf/src/feature/integer_motion.c lines 510-560 and    */
/* the Vulkan twin in motion_vulkan.c. T3-15(c) / ADR-0219.          */
/* ------------------------------------------------------------------ */
static double motion3_postprocess_cuda(MotionStateCuda *s, double score2)
{
    double const weighted = score2 * s->motion_fps_weight;
    double const blended = motion_blend(weighted, s->motion_blend_factor, s->motion_blend_offset);
    double const clipped = MIN(blended, s->motion_max_val);
    double const previous_unaveraged = s->prev_motion3_blended;
    s->prev_motion3_blended = clipped;
    /* Guard threshold accounts for ``s->frame_index`` having been
     * pre-incremented in collect() before this function runs.  At
     * the framework's collect(index=1) call the CPU reference
     * (integer_motion.c:523) evaluates ``index > 1`` = false and
     * skips the moving average; here ``frame_index`` is already 2
     * because of the pre-increment, so the matching condition is
     * ``frame_index > 2`` (cuda-reviewer 2026-05-09). */
    if (s->motion_moving_average && s->frame_index > 2) {
        return (clipped + previous_unaveraged) / 2.0;
    }
    return clipped;
}

static int calculate_motion_score(const VmafPicture *src, VmafCudaBuffer *src_blurred,
                                  const VmafCudaBuffer *prev_blurred, VmafCudaBuffer *sad,
                                  unsigned width, unsigned height, ptrdiff_t src_stride,
                                  ptrdiff_t blurred_stride, unsigned src_bpc, CUfunction funcbpc8,
                                  CUfunction funcbpc16, CudaFunctions *cu_f, CUstream stream)
{
    int block_dim_x = 16;
    int block_dim_y = 16;
    int grid_dim_x = DIV_ROUND_UP(width, block_dim_x);
    int grid_dim_y = DIV_ROUND_UP(height, block_dim_y);

    if (src_bpc == 8) {
        void *kernelParams[] = {
            (void *)src, (void *)src_blurred, (void *)prev_blurred, (void *)sad, &width,
            &height,     &src_stride,         &blurred_stride};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(funcbpc8, grid_dim_x, grid_dim_y, 1, block_dim_x,
                                               block_dim_y, 1, 0, stream, kernelParams, NULL));
    } else {
        void *kernelParams[] = {
            (void *)src, (void *)src_blurred, (void *)prev_blurred, (void *)sad, &width,
            &height,     &src_stride,         &blurred_stride};
        CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(funcbpc16, grid_dim_x, grid_dim_y, 1, block_dim_x,
                                               block_dim_y, 1, 0, stream, kernelParams, NULL));
    }
    return 0;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    MotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    /* Reject the 5-frame window mode explicitly. CPU mode keeps a
     * 5-deep blur ring + computes a second SAD pair (i-2 ↔ i-4); the
     * GPU ports today still use a 2-deep ring. Failing loud with
     * -ENOTSUP keeps callers off a silent-wrong-answer code path.
     * See ADR-0219. */
    if (s->motion_five_frame_window) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "motion_cuda: motion_five_frame_window=true is not yet supported on CUDA "
                 "(T3-15(c) deferred). Use the CPU extractor `motion` instead.\n");
        return -ENOTSUP;
    }

    /* motion_add_uv kernel port deferred — ADR-0989. SYCL is the lead
     * backend; CUDA / Vulkan / HIP / Metal will follow. */
    if (s->motion_add_uv) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "motion_cuda: motion_add_uv=true is not yet supported on CUDA "
                 "(ADR-0989 deferred). Use the SYCL extractor `motion_sycl` instead.\n");
        return -ENOTSUP;
    }

    /* The 5-tap CUDA kernel uses reflect-101 mirror padding on device;
     * mirror() returns 2*sup - idx - 2, which is negative when sup < 3.
     * Refuse smaller frames up front to prevent out-of-bounds device
     * reads.  Minimum: filter_width/2 + 1 = 3. */
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_cuda: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuStreamCreateWithPriority(&s->str, CU_STREAM_NON_BLOCKING, 0), fail);
    /* ADR-1090 — graduated labels so earlier allocations are freed when a
     * later step fails; previously all paths jumped to `fail` which only
     * popped the context, leaking the stream and event. */
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&s->event, CU_EVENT_DEFAULT), fail_after_stream);

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, motion_score_ptx), fail_after_event);

    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc16, s->module, "calculate_motion_score_kernel_16bpc"),
        fail_after_module);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc8, s->module, "calculate_motion_score_kernel_8bpc"),
        fail_after_module);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);

    if (s->motion_force_zero) {
        fex->extract = extract_force_zero;
        fex->submit = NULL;
        fex->collect = NULL;
        fex->flush = NULL;
        fex->close = NULL;
        return 0;
    }

    s->calculate_motion_score = calculate_motion_score;

    int ret = 0;

    s->score = 0;
    s->frame_index = 0;
    s->prev_motion3_blended = 0.0;
    s->last_batch_boundary = -1;
    for (int b = 0; b < MOTION_BATCH_DEPTH; b++)
        s->score_ring[b] = 0.0;

    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->blur[0], sizeof(uint16_t) * w * h);
    if (ret)
        goto free_ref;
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->blur[1], sizeof(uint16_t) * w * h);
    if (ret)
        goto free_ref;
    /* Allocate MOTION_BATCH_DEPTH device SAD slots (ADR-0845).
     * Each slot is 8 bytes; slots are zeroed per-frame in submit(). */
    for (int b = 0; b < MOTION_BATCH_DEPTH; b++) {
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->sad[b], sizeof(uint64_t));
        if (ret)
            goto free_ref;
    }
    /* Single pinned host buffer — MOTION_BATCH_DEPTH × 8 bytes. */
    ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->sad_host,
                                       MOTION_BATCH_DEPTH * sizeof(uint64_t));
    if (ret)
        goto free_ref;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        goto free_ref;

    return 0;

free_ref:
    if (s->blur[0]) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->blur[0]);
        free(s->blur[0]);
    }
    if (s->blur[1]) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->blur[1]);
        free(s->blur[1]);
    }
    for (int b = 0; b < MOTION_BATCH_DEPTH; b++) {
        if (s->sad[b]) {
            ret |= vmaf_cuda_buffer_free(fex->cu_state, s->sad[b]);
            free(s->sad[b]);
        }
    }
    if (s->sad_host) {
        ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->sad_host);
        s->sad_host = NULL;
    }
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    (void)ret; // accumulated cleanup status intentionally discarded on error path

    return -ENOMEM;

fail_after_module:
    (void)cu_f->cuModuleUnload(s->module);
    s->module = NULL;
fail_after_event:
    (void)cu_f->cuEventDestroy(s->event);
    s->event = 0;
fail_after_stream:
    (void)cu_f->cuStreamDestroy(s->str);
    s->str = 0;
fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

/* Idempotent append-if-not-written. Probes via get_score to suppress the
 * duplicate-write warning that fires when `flush_context_cuda`'s pending
 * collect (T-GPU-OPT-1 / PR #312) already wrote the same (feature, index)
 * pair. Returns 0 on success, negative errno on real failure. */
static int append_if_unwritten(VmafFeatureCollector *fc, const char *feature, double value,
                               unsigned index)
{
    double existing;
    if (vmaf_feature_collector_get_score(fc, feature, &existing, index) == 0)
        return 0;
    return vmaf_feature_collector_append(fc, feature, value, index);
}

static inline double normalize_and_scale_sad(uint64_t sad, unsigned w, unsigned h)
{
    return (float)(sad / 256.) / (w * h);
}

static int emit_batch_scores(MotionStateCuda *s, VmafFeatureCollector *fc, unsigned batch_start,
                             unsigned batch_end, double score_before_batch);

static int flush_fex_cuda(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int ret = 0;

    /* Flush handles the final partial batch: any frames after the last
     * batch-boundary collect that have not yet been synced or emitted
     * (ADR-0845).
     *
     * s->last_batch_boundary tracks the highest index for which we
     * already synced + emitted in a batch-boundary collect().  Any
     * frames from (last_batch_boundary + 1) to s->index need flushing
     * now.  When s->index == 0 there is nothing to do.  When the last
     * frame happened to be exactly a batch boundary, flush() has
     * nothing pending and only handles the final motion2/motion3
     * emission (the same as the legacy path). */
    if (s->index == 0)
        return 1; /* Return 1 = "no score to append"; matches legacy. */

    const int pending_start = s->last_batch_boundary + 1;

    if ((int)s->index >= pending_start) {
        /* There are frames in the partial tail batch that were NOT
         * synced/emitted by a boundary collect(). Drain them now.
         * flush_start clamps to 1 to skip frame 0, which never
         * produces a valid SAD (no prev_blurred at index 0). */
        const unsigned flush_start = ((unsigned)pending_start < 1u) ? 1u : (unsigned)pending_start;

        if (flush_start > s->index) {
            /* Nothing to flush (only frame 0 was pending). */
            goto flush_emit_trailing;
        }

        CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));

        /* DtoH only the slots that have pending data (frames flush_start..s->index). */
        for (unsigned i = flush_start; i <= s->index; i++) {
            const unsigned s_slot = i % MOTION_BATCH_DEPTH;
            CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(&s->sad_host[s_slot],
                                                      (CUdeviceptr)s->sad[s_slot]->data,
                                                      sizeof(uint64_t), s->str));
        }
        CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));

        for (unsigned i = flush_start; i <= s->index; i++) {
            const unsigned s_slot = i % MOTION_BATCH_DEPTH;
            s->score_ring[s_slot] =
                normalize_and_scale_sad(s->sad_host[s_slot], s->frame_w, s->frame_h);
        }

        const double score_before =
            (flush_start > 1u) ? s->score_ring[(flush_start - 1u) % MOTION_BATCH_DEPTH] : 0.0;
        s->score = s->score_ring[s->index % MOTION_BATCH_DEPTH];

        ret = emit_batch_scores(s, feature_collector, flush_start, s->index, score_before);
        if (ret < 0)
            return ret;
    }
flush_emit_trailing:;

    /* Emit the trailing motion2/motion3 for the last frame (mirrors the
     * legacy flush path and CPU integer_motion.c:563). Uses
     * append_if_unwritten so that if the batch-boundary collect() already
     * emitted this pair we don't warn about a duplicate write. */
    double const last_motion2 = MIN(s->score * s->motion_fps_weight, s->motion_max_val);
    ret = append_if_unwritten(feature_collector, "VMAF_integer_feature_motion2_score", last_motion2,
                              s->index);
    if (ret >= 0) {
        double const motion3_score = motion3_postprocess_cuda(s, last_motion2);
        int ret_m3 = append_if_unwritten(feature_collector, "VMAF_integer_feature_motion3_score",
                                         motion3_score, s->index);
        if (ret_m3 < 0)
            ret = ret_m3;
    }

    return (ret < 0) ? ret : !ret;
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    MotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    const unsigned src_blurred_idx = (index + 0) % 2;
    const unsigned prev_blurred_idx = (index + 1) % 2;

    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);

    // Wait for the dist picture upload to complete on the picture stream
    // before any work that reads the SAD accumulator.
    CHECK_CUDA_RETURN(cu_f,
                      cuStreamWaitEvent(pic_stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                        CU_EVENT_WAIT_DEFAULT));

    const unsigned slot = index % MOTION_BATCH_DEPTH;

    /* Reset only this frame's SAD slot on pic_stream (ADR-0845).
     * The kernel atomicAdd's into sad[slot]->data; the memset must run
     * on the same stream as the kernel so they are strictly ordered.
     * All MOTION_BATCH_DEPTH slots are allocated independently so
     * zeroing slot N never races the previous batch's DtoH on slot N. */
    CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(s->sad[slot]->data, 0, sizeof(uint64_t), pic_stream));

    // Compute motion score (blur + SAD) into the per-slot device accumulator
    int err = s->calculate_motion_score(
        ref_pic, s->blur[src_blurred_idx], s->blur[prev_blurred_idx], s->sad[slot], ref_pic->w[0],
        ref_pic->h[0], ref_pic->stride[0], sizeof(uint16_t) * ref_pic->w[0], ref_pic->bpc,
        s->funcbpc8, s->funcbpc16, cu_f, pic_stream);
    if (err)
        return err;

    /* Record the kernel-finished event on pic_stream, then chain s->str to
     * wait for it. The DtoH readback is NOT queued here — it happens in
     * collect() at batch boundaries to amortise sync overhead (ADR-0845). */
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->event, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->str, s->event, CU_EVENT_WAIT_DEFAULT));
    return 0;
}

/* Emit accumulated scores for frames [batch_start .. batch_end] using the
 * values in s->score_ring[].  This helper is called after a batch-boundary
 * cuStreamSynchronize has made all DtoH copies visible on the host.
 *
 * batch_start / batch_end are INCLUSIVE frame indices (not ring slots).
 * The caller must have populated s->score_ring[i % MOTION_BATCH_DEPTH]
 * for every i in [batch_start, batch_end] from the DtoH'd sad_host values.
 *
 * score_before_batch is the normalized SAD score for the frame immediately
 * BEFORE batch_start (i.e. s->score at entry), used to compute motion2 for
 * index == batch_start when batch_start > 1. It may be 0.0 for batch_start <= 1. */
static int emit_batch_scores(MotionStateCuda *s, VmafFeatureCollector *fc, unsigned batch_start,
                             unsigned batch_end, double score_before_batch)
{
    int err = 0;
    /* Snapshot frame_index as it stands at entry so we can restore it
     * after the loop. During the loop we temporarily set s->frame_index
     * to the value it would have had at each collect(i) call (i + 1),
     * so that motion3_postprocess_cuda's moving-average guard fires at
     * the correct frame boundary. Without this, frames in the middle of
     * the batch would incorrectly trigger the guard because frame_index
     * is already at batch_end + 1 when emit_batch_scores is called. */
    const unsigned saved_frame_index = s->frame_index;
    for (unsigned i = batch_start; i <= batch_end; i++) {
        /* Set frame_index to the value it would have had if collect(i)
         * ran sequentially.  frame_index is incremented at the end of
         * each collect(); for frame i, it is i + 1 right before the
         * motion3_postprocess call. */
        s->frame_index = i + 1;

        const double cur_score = s->score_ring[i % MOTION_BATCH_DEPTH];
        const double prev_score =
            (i == batch_start) ? score_before_batch : s->score_ring[(i - 1) % MOTION_BATCH_DEPTH];

        if (s->debug) {
            err |= vmaf_feature_collector_append(fc, "VMAF_integer_feature_motion_score", cur_score,
                                                 i);
        }

        if (i == 1) {
            /* index 1: back-fill motion3 for index 0 using score[1].
             * No min(prev,cur) yet — mirrors CPU integer_motion.c. */
            double const score_clipped = MIN(cur_score * s->motion_fps_weight, s->motion_max_val);
            double const m3 = motion3_postprocess_cuda(s, score_clipped);
            err |= vmaf_feature_collector_append(fc, "VMAF_integer_feature_motion3_score", m3, 0);
        }
        if (i > 1) {
            double const motion2 = (prev_score < cur_score) ? prev_score : cur_score;
            double const motion2_clipped = MIN(motion2 * s->motion_fps_weight, s->motion_max_val);
            err |= vmaf_feature_collector_append(fc, "VMAF_integer_feature_motion2_score",
                                                 motion2_clipped, i - 1);
            double const m3 = motion3_postprocess_cuda(s, motion2_clipped);
            err |=
                vmaf_feature_collector_append(fc, "VMAF_integer_feature_motion3_score", m3, i - 1);
        }
    }
    /* Restore frame_index to what the caller set it to (batch_end + 1). */
    s->frame_index = saved_frame_index;
    return err;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    MotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    /* Frame 0: emit zeros and return — no SAD computed for the first frame. */
    if (index == 0) {
        int err = vmaf_feature_collector_append(feature_collector,
                                                "VMAF_integer_feature_motion2_score", 0., 0);
        if (s->debug) {
            err |= vmaf_feature_collector_append(feature_collector,
                                                 "VMAF_integer_feature_motion_score", 0., 0);
        }
        s->frame_index++;
        return err;
    }

    /* For frames 1 .. MOTION_BATCH_DEPTH-2 (non-boundary, non-first):
     * record the SAD score in the ring and defer sync + emit to the
     * batch boundary (ADR-0845). The kernel is already running or
     * complete on the GPU; s->str carries the chained event. */
    const unsigned slot = index % MOTION_BATCH_DEPTH;
    const bool is_boundary = (slot == (MOTION_BATCH_DEPTH - 1));

    if (!is_boundary) {
        /* Non-boundary collect — nothing to emit yet; the DtoH and sync
         * happen at the next batch-boundary collect. frame_index is still
         * incremented so motion3_postprocess_cuda's guard condition stays
         * correct at the boundary emit. */
        s->frame_index++;
        return 0;
    }

    /* === Batch boundary: sync, DtoH all slots, compute + emit scores === */

    /* Wait for all MOTION_BATCH_DEPTH kernel chained events on s->str.
     * After this, all device SAD[slot] values are guaranteed stable. */
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));

    /* Determine which frame indices this batch covers.
     * The batch runs from (index - MOTION_BATCH_DEPTH + 1) to index,
     * but frame 0 never contributes a SAD value, so batch_start >= 1. */
    const unsigned batch_end = index;
    const unsigned batch_start_raw =
        (index >= (unsigned)MOTION_BATCH_DEPTH) ? (index - MOTION_BATCH_DEPTH + 1) : 1u;

    /* Queue all DtoH copies on s->str (in slot order for coalescing). */
    for (unsigned i = batch_start_raw; i <= batch_end; i++) {
        const unsigned s_slot = i % MOTION_BATCH_DEPTH;
        CHECK_CUDA_RETURN(cu_f,
                          cuMemcpyDtoHAsync(&s->sad_host[s_slot], (CUdeviceptr)s->sad[s_slot]->data,
                                            sizeof(uint64_t), s->str));
    }
    /* Single sync to drain all queued DtoH copies. */
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));

    /* Populate score_ring[] from the freshly-read host values. */
    for (unsigned i = batch_start_raw; i <= batch_end; i++) {
        const unsigned s_slot = i % MOTION_BATCH_DEPTH;
        s->score_ring[s_slot] =
            normalize_and_scale_sad(s->sad_host[s_slot], s->frame_w, s->frame_h);
    }

    /* score_before_batch: the normalized SAD from the frame just before
     * this batch.  s->score was set to score_ring[(batch_start - 1) % BATCH]
     * at the previous batch boundary (or 0 for the very first batch). */
    const double score_before = s->score;

    /* Update s->score to the last frame's value for use by flush() and
     * by the next batch's score_before. */
    s->score = s->score_ring[batch_end % MOTION_BATCH_DEPTH];

    /* Advance frame_index for the boundary frame itself (non-boundary
     * frames already incremented it in their collect() calls above). */
    s->frame_index++;

    s->last_batch_boundary = (int)index;

    return emit_batch_scores(s, feature_collector, batch_start_raw, batch_end, score_before);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    MotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    /* Close path must continue unwinding every allocation even when a
     * CUDA call fails — bailing on the first error would leak buffers.
     * Each CHECK is independent and we OR the errnos into ret. */
    int _cuda_err = 0;
    CHECK_CUDA_GOTO(cu_f, cuStreamSynchronize(s->str), after_stream_sync);
after_stream_sync:
    CHECK_CUDA_GOTO(cu_f, cuStreamDestroy(s->str), after_stream_destroy);
after_stream_destroy:
    CHECK_CUDA_GOTO(cu_f, cuEventDestroy(s->event), after_event1_destroy);
after_event1_destroy:;

    int ret = _cuda_err;

    if (s->blur[0]) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->blur[0]);
        free(s->blur[0]);
    }
    if (s->blur[1]) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->blur[1]);
        free(s->blur[1]);
    }
    /* Free all MOTION_BATCH_DEPTH device SAD slots (ADR-0845). */
    for (int b = 0; b < MOTION_BATCH_DEPTH; b++) {
        if (s->sad[b]) {
            ret |= vmaf_cuda_buffer_free(fex->cu_state, s->sad[b]);
            free(s->sad[b]);
        }
    }
    /* Free the pinned host buffer (MOTION_BATCH_DEPTH × 8 bytes).
     * Allocated via vmaf_cuda_buffer_host_alloc() in init_fex_cuda(). */
    if (s->sad_host) {
        ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->sad_host);
        s->sad_host = NULL;
    }
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    if (cu_f && s->module)
        (void)cu_f->cuModuleUnload(s->module);

    return ret;
}

/* T3-15(c) / ADR-0219: motion3_score is now provided (3-frame mode
 * only). The 5-frame window mode remains deferred — init() rejects
 * it with -ENOTSUP. */
static const char *provided_features[] = {"VMAF_integer_feature_motion_score",
                                          "VMAF_integer_feature_motion2_score",
                                          "VMAF_integer_feature_motion3_score", NULL};

VmafFeatureExtractor vmaf_fex_integer_motion_cuda = {
    .name = "motion_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .flush = flush_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(MotionStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_CUDA,
};
