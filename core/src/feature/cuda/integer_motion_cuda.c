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

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

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
#define MOTION_BOOL_OPTION(option_name, member, option_alias, option_help)                         \
    {                                                                                              \
        .name = option_name,                                                                       \
        .alias = option_alias,                                                                     \
        .help = option_help,                                                                       \
        .offset = offsetof(MotionStateCuda, member),                                               \
        .type = VMAF_OPT_TYPE_BOOL,                                                                \
        .default_val.b = false,                                                                    \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }
#define MOTION_DOUBLE_OPTION(option_name, member, option_alias, option_help, option_default,       \
                             option_min, option_max)                                               \
    {                                                                                              \
        .name = option_name,                                                                       \
        .alias = option_alias,                                                                     \
        .help = option_help,                                                                       \
        .offset = offsetof(MotionStateCuda, member),                                               \
        .type = VMAF_OPT_TYPE_DOUBLE,                                                              \
        .default_val.d = option_default,                                                           \
        .min = option_min,                                                                         \
        .max = option_max,                                                                         \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }

static const VmafOption options[] = {
    {.name = "debug",
     .help = "debug mode: enable additional output",
     .offset = offsetof(MotionStateCuda, debug),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val.b = false},
    MOTION_BOOL_OPTION("motion_force_zero", motion_force_zero, NULL,
                       "forcing motion score to zero"),
    MOTION_DOUBLE_OPTION("motion_blend_factor", motion_blend_factor, "mbf",
                         "blend motion score given an offset", 1.0, 0.0, 1.0),
    MOTION_DOUBLE_OPTION("motion_blend_offset", motion_blend_offset, "mbo",
                         "blend motion score starting from this offset", 40.0, 0.0, 1000.0),
    MOTION_DOUBLE_OPTION("motion_fps_weight", motion_fps_weight, "mfw",
                         "fps-aware multiplicative weight/correction", 1.0, 0.0, 5.0),
    MOTION_DOUBLE_OPTION("motion_max_val", motion_max_val, "mmxv",
                         "maximum value allowed; larger values will be clipped to this value",
                         MOTION_CUDA_DEFAULT_MAX_VAL, 0.0, 10000.0),
    MOTION_BOOL_OPTION(
        "motion_five_frame_window", motion_five_frame_window, "mffw",
        "use five-frame temporal window (NOT YET SUPPORTED on CUDA — T3-15(c) deferred)"),
    MOTION_BOOL_OPTION("motion_moving_average", motion_moving_average, "mma",
                       "use moving average for motion3 scores after first frame"),
    MOTION_BOOL_OPTION(
        "motion_add_uv", motion_add_uv, "mau",
        "include U and V plane SADs (NOT YET SUPPORTED on CUDA — ADR-0989 deferred)"),
    {0}};

#undef MOTION_DOUBLE_OPTION
#undef MOTION_BOOL_OPTION

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
    /* ``score2`` already carries ``motion_fps_weight`` and the
     * ``motion_max_val`` clip: every caller applies both before handing the
     * value over, exactly as the CPU reference does once in extract()
     * (integer_motion.c:372).  Re-weighting here would square the factor
     * whenever ``motion_fps_weight != 1.0``.  ADR-1216. */
    double const blended = motion_blend(score2, s->motion_blend_factor, s->motion_blend_offset);
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

static int validate_motion_configuration(const MotionStateCuda *s, unsigned w, unsigned h)
{
    if (s->motion_five_frame_window) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "motion_cuda: motion_five_frame_window=true is not yet supported on CUDA "
                 "(T3-15(c) deferred). Use the CPU extractor `motion` instead.\n");
        return -ENOTSUP;
    }
    if (s->motion_add_uv) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "motion_cuda: motion_add_uv=true is not yet supported on CUDA "
                 "(ADR-0989 deferred). Use the SYCL extractor `motion_sycl` instead.\n");
        return -ENOTSUP;
    }
    if (h < 3u || w < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_cuda: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }
    return 0;
}

static int motion_cuda_status(CUresult result, const char *operation)
{
    if (result == CUDA_SUCCESS)
        return 0;
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_cuda: CUDA operation %s failed with code %d\n",
             operation, (int)result);
    return vmaf_cuda_result_to_errno((int)result);
}

static void keep_first_motion_error(int *first_error, int error)
{
    if (!*first_error && error)
        *first_error = error;
}

static void record_motion_error(int *first_error, int error, const char *operation)
{
    if (!error)
        return;
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_cuda: %s failed with error %d\n", operation, error);
    keep_first_motion_error(first_error, error);
}

static int load_motion_runtime(VmafFeatureExtractor *fex)
{
    MotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int err = motion_cuda_status(cu_f->cuCtxPushCurrent(fex->cu_state->ctx), "cuCtxPushCurrent");
    if (err)
        return err;

    keep_first_motion_error(&err, motion_cuda_status(cu_f->cuStreamCreateWithPriority(
                                                         &s->str, CU_STREAM_NON_BLOCKING, 0),
                                                     "cuStreamCreateWithPriority"));
    if (!err)
        keep_first_motion_error(
            &err,
            motion_cuda_status(cu_f->cuEventCreate(&s->event, CU_EVENT_DEFAULT), "cuEventCreate"));
    if (!err)
        keep_first_motion_error(
            &err, motion_cuda_status(cu_f->cuModuleLoadData(&s->module, motion_score_ptx),
                                     "cuModuleLoadData"));
    if (!err)
        keep_first_motion_error(&err, motion_cuda_status(cu_f->cuModuleGetFunction(
                                                             &s->funcbpc16, s->module,
                                                             "calculate_motion_score_kernel_16bpc"),
                                                         "cuModuleGetFunction(16bpc)"));
    if (!err)
        keep_first_motion_error(&err, motion_cuda_status(cu_f->cuModuleGetFunction(
                                                             &s->funcbpc8, s->module,
                                                             "calculate_motion_score_kernel_8bpc"),
                                                         "cuModuleGetFunction(8bpc)"));
    keep_first_motion_error(&err,
                            motion_cuda_status(cu_f->cuCtxPopCurrent(NULL), "cuCtxPopCurrent"));
    return err;
}

static int release_motion_runtime(VmafFeatureExtractor *fex, bool synchronize)
{
    MotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    int err = 0;
    if (synchronize && s->str)
        keep_first_motion_error(
            &err, motion_cuda_status(cu_f->cuStreamSynchronize(s->str), "cuStreamSynchronize"));
    if (s->str) {
        keep_first_motion_error(
            &err, motion_cuda_status(cu_f->cuStreamDestroy(s->str), "cuStreamDestroy"));
        s->str = 0;
    }
    if (s->event) {
        keep_first_motion_error(
            &err, motion_cuda_status(cu_f->cuEventDestroy(s->event), "cuEventDestroy"));
        s->event = 0;
    }
    if (s->module) {
        keep_first_motion_error(
            &err, motion_cuda_status(cu_f->cuModuleUnload(s->module), "cuModuleUnload"));
        s->module = NULL;
    }
    return err;
}

static int release_motion_buffers(VmafFeatureExtractor *fex)
{
    MotionStateCuda *s = fex->priv;
    int err = 0;
    for (unsigned i = 0; i < 2u; i++) {
        if (s->blur[i]) {
            record_motion_error(&err, vmaf_cuda_buffer_free(fex->cu_state, s->blur[i]),
                                "vmaf_cuda_buffer_free(blur)");
            free(s->blur[i]);
            s->blur[i] = NULL;
        }
    }
    for (unsigned i = 0; i < MOTION_BATCH_DEPTH; i++) {
        if (s->sad[i]) {
            record_motion_error(&err, vmaf_cuda_buffer_free(fex->cu_state, s->sad[i]),
                                "vmaf_cuda_buffer_free(sad)");
            free(s->sad[i]);
            s->sad[i] = NULL;
        }
    }
    if (s->sad_host) {
        record_motion_error(&err, vmaf_cuda_buffer_host_free(fex->cu_state, s->sad_host),
                            "vmaf_cuda_buffer_host_free");
        s->sad_host = NULL;
    }
    record_motion_error(&err, vmaf_dictionary_free(&s->feature_name_dict), "dictionary free");
    return err;
}

static int release_motion_resources(VmafFeatureExtractor *fex, bool synchronize)
{
    int err = release_motion_runtime(fex, synchronize);
    const int buffer_err = release_motion_buffers(fex);
    keep_first_motion_error(&err, buffer_err);
    return err;
}

static int allocate_motion_buffers(VmafFeatureExtractor *fex, unsigned w, unsigned h)
{
    MotionStateCuda *s = fex->priv;
    const size_t blur_bytes = sizeof(uint16_t) * (size_t)w * (size_t)h;
    int err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->blur[0], blur_bytes);
    if (!err)
        err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->blur[1], blur_bytes);
    for (unsigned i = 0; !err && i < MOTION_BATCH_DEPTH; i++)
        err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->sad[i], sizeof(uint64_t));
    if (!err) {
        err = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->sad_host,
                                          MOTION_BATCH_DEPTH * sizeof(uint64_t));
    }
    if (err)
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "motion_cuda: buffer allocation failed with error %d\n",
                 err);
    return err;
}

static void configure_force_zero(VmafFeatureExtractor *fex)
{
    fex->extract = extract_force_zero;
    fex->submit = NULL;
    fex->collect = NULL;
    fex->flush = NULL;
    fex->close = NULL;
}

static void reset_motion_state(MotionStateCuda *s)
{
    s->calculate_motion_score = calculate_motion_score;
    s->score = 0;
    s->frame_index = 0;
    s->prev_motion3_blended = 0.0;
    s->last_batch_boundary = -1;
    for (unsigned i = 0; i < MOTION_BATCH_DEPTH; i++)
        s->score_ring[i] = 0.0;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    /* The motion kernel is luma-only and selects its path from each picture's
     * runtime bpc in submit(); these callback fields are therefore irrelevant. */
    (void)pix_fmt;
    (void)bpc;
    MotionStateCuda *s = fex->priv;
    int err = validate_motion_configuration(s, w, h);
    if (err)
        return err;
    if (s->motion_force_zero) {
        configure_force_zero(fex);
        return 0;
    }

    reset_motion_state(s);
    err = load_motion_runtime(fex);
    if (!err)
        err = allocate_motion_buffers(fex, w, h);
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    if (err) {
        const int cleanup_err = release_motion_resources(fex, false);
        if (cleanup_err) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "motion_cuda: cleanup after init failure also failed with error %d\n",
                     cleanup_err);
        }
    }
    return err;
}

/* Idempotent append-if-not-written. Probes via get_score to suppress the
 * duplicate-write warning that fires when `flush_context_cuda`'s pending
 * collect (T-GPU-OPT-1 / PR #312) already wrote the same (feature, index)
 * pair. Returns 0 on success, negative errno on real failure. */
static int append_if_unwritten(VmafFeatureCollector *fc, VmafDictionary *dict, const char *feature,
                               double value, unsigned index)
{
    VmafDictionaryEntry *entry = vmaf_dictionary_get(&dict, feature, 0);
    const char *fn = entry ? entry->val : feature;
    double existing;
    if (vmaf_feature_collector_get_score(fc, fn, &existing, index) == 0)
        return 0;
    return vmaf_feature_collector_append(fc, fn, value, index);
}

static inline double normalize_and_scale_sad(uint64_t sad, unsigned w, unsigned h)
{
    /* Compute end-to-end in double, matching the CPU twin
     * (integer_motion.c: (double)sad / 256. / (w * h)). A prior
     * intermediate (float) cast truncated to single precision and the
     * unsigned (w * h) divisor risked overflow on large frames. */
    return (double)sad / 256.0 / ((double)w * (double)h);
}

static int emit_batch_scores(MotionStateCuda *s, VmafFeatureCollector *fc, unsigned batch_start,
                             unsigned batch_end, double score_before_batch);

static int read_motion_scores(VmafFeatureExtractor *fex, unsigned start, unsigned end)
{
    MotionStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));
    for (unsigned i = start; i <= end; i++) {
        const unsigned slot = i % MOTION_BATCH_DEPTH;
        CHECK_CUDA_RETURN(cu_f,
                          cuMemcpyDtoHAsync(&s->sad_host[slot], (CUdeviceptr)s->sad[slot]->data,
                                            sizeof(uint64_t), s->str));
    }
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->str));
    for (unsigned i = start; i <= end; i++) {
        const unsigned slot = i % MOTION_BATCH_DEPTH;
        s->score_ring[slot] = normalize_and_scale_sad(s->sad_host[slot], s->frame_w, s->frame_h);
    }
    return 0;
}

static int flush_pending_motion_batch(VmafFeatureExtractor *fex,
                                      VmafFeatureCollector *feature_collector, unsigned flush_start)
{
    MotionStateCuda *s = fex->priv;
    const int err = read_motion_scores(fex, flush_start, s->index);
    if (err)
        return err;

    const double score_before =
        (flush_start > 1u) ? s->score_ring[(flush_start - 1u) % MOTION_BATCH_DEPTH] : 0.0;
    s->score = s->score_ring[s->index % MOTION_BATCH_DEPTH];
    return emit_batch_scores(s, feature_collector, flush_start, s->index, score_before);
}

static int emit_trailing_motion_scores(MotionStateCuda *s, VmafFeatureCollector *feature_collector)
{
    const double last_motion2 = MIN(s->score * s->motion_fps_weight, s->motion_max_val);
    int err = append_if_unwritten(feature_collector, s->feature_name_dict,
                                  "VMAF_integer_feature_motion2_score", last_motion2, s->index);
    if (err >= 0) {
        const double motion3_score = motion3_postprocess_cuda(s, last_motion2);
        const int motion3_err =
            append_if_unwritten(feature_collector, s->feature_name_dict,
                                "VMAF_integer_feature_motion3_score", motion3_score, s->index);
        if (motion3_err < 0)
            err = motion3_err;
    }
    return (err < 0) ? err : !err;
}

static int flush_fex_cuda(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionStateCuda *s = fex->priv;
    if (s->index == 0) {
        /* Single-frame video (frame 0 only): back-fill motion3_score for index 0
         * with 0.0 (mirrors CPU integer_motion.c when n <= min_idx).
         *
         * The engine drains flush in a loop — `while (!(err = fex->flush(...)))`
         * in feature_extractor.cpp — so a flush that keeps returning 0 never
         * terminates. Append at most once and then report 1 ("nothing more to
         * append"), which is what the legacy single-frame path returned. */
        const int backfill_err = append_if_unwritten(feature_collector, s->feature_name_dict,
                                                     "VMAF_integer_feature_motion3_score", 0.0, 0);
        return (backfill_err < 0) ? backfill_err : 1;
    }

    const int pending_start = s->last_batch_boundary + 1;
    if ((int)s->index >= pending_start) {
        const unsigned flush_start = ((unsigned)pending_start < 1u) ? 1u : (unsigned)pending_start;
        if (flush_start <= s->index) {
            const int err = flush_pending_motion_batch(fex, feature_collector, flush_start);
            if (err < 0)
                return err;
        }
    }
    return emit_trailing_motion_scores(s, feature_collector);
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
            err |= vmaf_feature_collector_append_with_dict(
                fc, s->feature_name_dict, "VMAF_integer_feature_motion_score", cur_score, i);
        }

        if (i == 1) {
            /* index 1: back-fill motion3 for index 0 using score[1].
             * No min(prev,cur) yet — mirrors CPU integer_motion.c. */
            double const score_clipped = MIN(cur_score * s->motion_fps_weight, s->motion_max_val);
            double const m3 = motion3_postprocess_cuda(s, score_clipped);
            err |= vmaf_feature_collector_append_with_dict(
                fc, s->feature_name_dict, "VMAF_integer_feature_motion3_score", m3, 0);
        }
        if (i > 1) {
            double const motion2 = (prev_score < cur_score) ? prev_score : cur_score;
            double const motion2_clipped = MIN(motion2 * s->motion_fps_weight, s->motion_max_val);
            err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                           "VMAF_integer_feature_motion2_score",
                                                           motion2_clipped, i - 1);
            double const m3 = motion3_postprocess_cuda(s, motion2_clipped);
            err |= vmaf_feature_collector_append_with_dict(
                fc, s->feature_name_dict, "VMAF_integer_feature_motion3_score", m3, i - 1);
        }
    }
    /* Restore frame_index to what the caller set it to (batch_end + 1). */
    s->frame_index = saved_frame_index;
    return err;
}

static int emit_initial_motion_scores(MotionStateCuda *s, VmafFeatureCollector *feature_collector)
{
    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_integer_feature_motion2_score", 0., 0);
    if (s->debug) {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_score", 0., 0);
    }
    s->frame_index++;
    return err;
}

static int collect_motion_boundary(VmafFeatureExtractor *fex, unsigned index,
                                   VmafFeatureCollector *feature_collector)
{
    MotionStateCuda *s = fex->priv;
    const unsigned batch_start =
        (index >= (unsigned)MOTION_BATCH_DEPTH) ? (index - MOTION_BATCH_DEPTH + 1) : 1u;
    const int err = read_motion_scores(fex, batch_start, index);
    if (err)
        return err;

    const double score_before = s->score;
    s->score = s->score_ring[index % MOTION_BATCH_DEPTH];
    s->frame_index++;
    s->last_batch_boundary = (int)index;
    return emit_batch_scores(s, feature_collector, batch_start, index, score_before);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    MotionStateCuda *s = fex->priv;
    if (index == 0)
        return emit_initial_motion_scores(s, feature_collector);

    const bool is_boundary = index % MOTION_BATCH_DEPTH == MOTION_BATCH_DEPTH - 1;
    if (is_boundary)
        return collect_motion_boundary(fex, index, feature_collector);
    s->frame_index++;
    return 0;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    return release_motion_resources(fex, true);
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

/* NOLINTEND(modernize-use-nullptr) */
