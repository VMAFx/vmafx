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
#include "drain_batch.h"
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

typedef struct MotionStateCuda {
    CUevent event, finished;
    CUfunction funcbpc8, funcbpc16;
    CUstream str;
    VmafCudaBuffer *blur[2];
    VmafCudaBuffer *sad;
    uint64_t *sad_host;
    /* Engine-scope fence batching opt-in flag (T-GPU-OPT-1, ADR-0242).
     * Set by ``vmaf_cuda_drain_batch_flush`` when this submit's
     * ``finished`` event was waited on as part of the batched drain;
     * the ``collect()`` path then skips its ``cuStreamSynchronize``
     * and resets the flag for the next frame. */
    bool drained;
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
        .default_val.b = true,
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

int calculate_motion_score(const VmafPicture *src, VmafCudaBuffer *src_blurred,
                           const VmafCudaBuffer *prev_blurred, VmafCudaBuffer *sad, unsigned width,
                           unsigned height, ptrdiff_t src_stride, ptrdiff_t blurred_stride,
                           unsigned src_bpc, CUfunction funcbpc8, CUfunction funcbpc16,
                           CudaFunctions *cu_f, CUstream stream)
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
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&s->event, CU_EVENT_DEFAULT), fail);
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&s->finished, CU_EVENT_DEFAULT), fail);

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, motion_score_ptx), fail);

    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc16, s->module, "calculate_motion_score_kernel_16bpc"),
        fail);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->funcbpc8, s->module, "calculate_motion_score_kernel_8bpc"),
        fail);

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

    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->blur[0], sizeof(uint16_t) * w * h);
    if (ret)
        goto free_ref;
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->blur[1], sizeof(uint16_t) * w * h);
    if (ret)
        goto free_ref;
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->sad, sizeof(uint64_t));
    if (ret)
        goto free_ref;
    ret |= vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->sad_host, sizeof(uint64_t));
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
    if (s->sad) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->sad);
        free(s->sad);
    }
    if (s->sad_host) {
        ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->sad_host);
        s->sad_host = NULL;
    }
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    (void)ret; // accumulated cleanup status intentionally discarded on error path

    return -ENOMEM;

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

static int flush_fex_cuda(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int ret = 0;
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));

    if (s->index > 0) {
        /* Mirror CPU integer_motion.c:563 — motion2_score is the
         * weighted-and-clipped value, NOT the raw normalized SAD.
         * The previous code emitted ``s->score`` (raw) which only
         * matched CPU when motion_fps_weight=1.0 and the clip never
         * tripped (cuda-reviewer 2026-05-09). */
        double const last_motion2 = MIN(s->score * s->motion_fps_weight, s->motion_max_val);
        ret = append_if_unwritten(feature_collector, "VMAF_integer_feature_motion2_score",
                                  last_motion2, s->index);
        if (ret >= 0) {
            double const motion3_score = motion3_postprocess_cuda(s, last_motion2);
            int ret_m3 = append_if_unwritten(
                feature_collector, "VMAF_integer_feature_motion3_score", motion3_score, s->index);
            if (ret_m3 < 0)
                ret = ret_m3;
        }
    }

    return (ret < 0) ? ret : !ret;
}

static inline double normalize_and_scale_sad(uint64_t sad, unsigned w, unsigned h)
{
    return (float)(sad / 256.) / (w * h);
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

    /* Reset device SAD on pic_stream (NOT s->str). The kernel below
     * launches on pic_stream and atomicAdd's into ``s->sad``; running
     * the memset on a different non-blocking stream (s->str) would
     * race the kernel because no event pair links them. Mirrors the
     * pattern in integer_motion_v2_cuda.c (T-GPU-OPT cleanup,
     * 2026-05-09). */
    CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(s->sad->data, 0, sizeof(uint64_t), pic_stream));

    // Compute motion score (blur + SAD)
    int err = s->calculate_motion_score(
        ref_pic, s->blur[src_blurred_idx], s->blur[prev_blurred_idx], s->sad, ref_pic->w[0],
        ref_pic->h[0], ref_pic->stride[0], sizeof(uint16_t) * ref_pic->w[0], ref_pic->bpc,
        s->funcbpc8, s->funcbpc16, cu_f, pic_stream);
    if (err)
        return err;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->event, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->str, s->event, CU_EVENT_WAIT_DEFAULT));

    if (index == 0)
        return 0; // No SAD to download for frame 0

    // Download SAD for collect
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->sad_host, (CUdeviceptr)s->sad->data,
                                              sizeof(*s->sad_host), s->str));
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->finished, s->str));
    /* Engine-scope fence batching opt-in (T-GPU-OPT-1, ADR-0242).
     * Best-effort: registration failure (overflow / no batch open)
     * silently degrades to the per-stream sync below. */
    (void)vmaf_cuda_drain_batch_register_event(s->finished, &s->drained);
    return 0;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    MotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    if (s->drained) {
        s->drained = false;
    } else {
        CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));
    }

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

    double score_prev = s->score;
    s->score = normalize_and_scale_sad(*s->sad_host, s->frame_w, s->frame_h);
    s->frame_index++;

    int err = 0;
    if (s->debug) {
        err |= vmaf_feature_collector_append(feature_collector, "VMAF_integer_feature_motion_score",
                                             s->score, index);
    }

    /* Match CPU integer_motion.c: at index == 1 the framework
     * back-fills motion3_score for index 0 using the just-arrived
     * motion (no prev to take min with yet). At index >= 2 emit
     * motion2/motion3 at index-1 using min(prev, cur). */
    if (index == 1) {
        double const score_clipped = MIN(s->score * s->motion_fps_weight, s->motion_max_val);
        double const motion3_score = motion3_postprocess_cuda(s, score_clipped);
        err |= vmaf_feature_collector_append(
            feature_collector, "VMAF_integer_feature_motion3_score", motion3_score, index - 1);
    }

    if (index > 1) {
        double const motion2 = score_prev < s->score ? score_prev : s->score;
        /* Mirror CPU integer_motion.c:563 — motion2_score is the
         * weighted-and-clipped value, not the raw min(prev, cur).
         * The previous code emitted ``motion2`` raw which only
         * matched CPU when motion_fps_weight=1.0 (cuda-reviewer
         * 2026-05-09). */
        double const motion2_clipped = MIN(motion2 * s->motion_fps_weight, s->motion_max_val);
        err |= vmaf_feature_collector_append(
            feature_collector, "VMAF_integer_feature_motion2_score", motion2_clipped, index - 1);
        double const motion3_score = motion3_postprocess_cuda(s, motion2_clipped);
        err |= vmaf_feature_collector_append(
            feature_collector, "VMAF_integer_feature_motion3_score", motion3_score, index - 1);
    }

    return err;
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
after_event1_destroy:
    CHECK_CUDA_GOTO(cu_f, cuEventDestroy(s->finished), after_event2_destroy);
after_event2_destroy:;

    int ret = _cuda_err;

    if (s->blur[0]) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->blur[0]);
        free(s->blur[0]);
    }
    if (s->blur[1]) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->blur[1]);
        free(s->blur[1]);
    }
    if (s->sad) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->sad);
        free(s->sad);
    }
    /* Free the pinned host-side mirror of the SAD accumulator allocated
     * via vmaf_cuda_buffer_host_alloc() in init_fex_cuda(). Missing this
     * leaked one page-locked uint64 per init/close cycle (cuda-reviewer
     * 2026-05-09). */
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
