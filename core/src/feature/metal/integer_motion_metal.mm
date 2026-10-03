/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  integer_motion feature extractor on the Metal backend (T8-1i / ADR-0421),
 *  the twin of the CPU `motion` extractor (core/src/feature/integer_motion.c).
 *  Dispatches `integer_motion_kernel_{8,16}bpc` from integer_motion.metal.
 *
 *  The kernel differences two raw luma planes before it blurs, as the CPU
 *  does (ADR-1498; the design of ADR-1371 / ADR-1372): a ring of packed raw
 *  planes, two slots, or three with motion_five_frame_window (ADR-1491):
 *  frame n writes slot n % ring and the SAD is taken against slot
 *  (n + 1) % ring, the frame ring - 1 back. No blurred frame is kept.
 *
 *  Scores, as integer_motion.c::extract() and flush() write them:
 *    - collect() appends VMAF_integer_feature_motion_sad_score on every frame:
 *      0 before the first frame with a SAD (frame 1, or 2 with the five-frame
 *      window) and under motion_force_zero, otherwise
 *      MIN(sad / 256 / (w * h) * motion_fps_weight, motion_max_val); with
 *      `debug` the same value as VMAF_integer_feature_motion_score;
 *    - flush() derives motion2 and motion3 of every frame from those with the
 *      CPU's own function, vmaf_motion_window_flush() (motion_window.h,
 *      ADR-1478), for both windows.
 *  The option table is the CPU's, so a model or request that sets one of its
 *  options keeps the twin (ADR-1183).
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* feature_extractor.h uses `#if defined(__cplusplus)` to include <atomic>
 * (Xcode 16.4 / macOS 15 libc++ emits "templates must have C++ linkage"
 * when that header is pulled into an extern "C" block — ADR-fix macOS-Metal). */
#include "feature_extractor.h"

extern "C" {
#include "dict.h"
#include "feature_collector.h"
#include "feature_name.h"
#include "libvmaf/picture.h"
#include "log.h"
#include "motion_window.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

/* integer_motion.c's default maximum value allowed for motion. */
#define DEFAULT_MOTION_MAX_VAL (10000.0)

/* Raw planes kept: the current frame and the previous one, and with the
 * five-frame window the one before that, against which the SAD is taken. */
#define MOTION_METAL_MAX_RING 3u

typedef struct IntegerMotionStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb;        /* uint32 SAD per threadgroup, grid_w × grid_h */
    VmafMetalContext *ctx;
    void *pso_8bpc;
    void *pso_16bpc;

    void *raw[MOTION_METAL_MAX_RING]; /* __bridge_retained id<MTLBuffer>, packed luma */
    unsigned ring;                    /* 2, or 3 with motion_five_frame_window */
    size_t row_bytes;
    size_t plane_bytes;
    size_t partials_count;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    bool flushed;

    /* integer_motion.c's MotionState options. */
    double motion_max_val;
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_fps_weight;
    bool motion_five_frame_window;
    bool motion_moving_average;
    bool motion_force_zero;
    bool debug;

    VmafDictionary *feature_name_dict;
} IntegerMotionStateMetal;

/* integer_motion.c's options[], entry for entry. */
static const VmafOption options[] = {
    {
        .name = "motion_force_zero",
        .alias = "force_0",
        .help = "forcing motion score to zero",
        .offset = offsetof(IntegerMotionStateMetal, motion_force_zero),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_factor",
        .alias = "mbf",
        .help = "blend motion score given an offset",
        .offset = offsetof(IntegerMotionStateMetal, motion_blend_factor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_offset",
        .alias = "mbo",
        .help = "blend motion score starting from this offset",
        .offset = offsetof(IntegerMotionStateMetal, motion_blend_offset),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 40.0},
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_fps_weight",
        .alias = "mfw",
        .help = "fps-aware multiplicative weight/correction",
        .offset = offsetof(IntegerMotionStateMetal, motion_fps_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min = 0.0,
        .max = 5.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_max_val",
        .alias = "mmxv",
        .help = "maximum value allowed; larger values will be clipped to this value",
        .offset = offsetof(IntegerMotionStateMetal, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = DEFAULT_MOTION_MAX_VAL},
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_five_frame_window",
        .alias = "mffw",
        .help = "use five-frame temporal window",
        .offset = offsetof(IntegerMotionStateMetal, motion_five_frame_window),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_moving_average",
        .alias = "mma",
        .help = "use moving average for motion scores after first frame",
        .offset = offsetof(IntegerMotionStateMetal, motion_moving_average),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(IntegerMotionStateMetal, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {nullptr}};

static int build_pipelines(IntegerMotionStateMetal *s, id<MTLDevice> device)
{
    const size_t blob_size = (size_t)(libvmaf_metallib_end - libvmaf_metallib_start);
    if (blob_size == 0) { return -ENODEV; }

    dispatch_data_t data = dispatch_data_create(
        libvmaf_metallib_start, blob_size,
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (data == NULL) { return -ENOMEM; }

    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithData:data error:&err];
    if (lib == nil) { return -ENODEV; }

    id<MTLFunction> fn8  = [lib newFunctionWithName:@"integer_motion_kernel_8bpc"];
    id<MTLFunction> fn16 = [lib newFunctionWithName:@"integer_motion_kernel_16bpc"];
    if (fn8 == nil || fn16 == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso8  = [device newComputePipelineStateWithFunction:fn8  error:&err];
    id<MTLComputePipelineState> pso16 = [device newComputePipelineStateWithFunction:fn16 error:&err];
    if (pso8 == nil || pso16 == nil) { return -ENODEV; }

    s->pso_8bpc  = (__bridge_retained void *)pso8;
    s->pso_16bpc = (__bridge_retained void *)pso16;
    return 0;
}

/* Releases every device object the state holds; each slot may be empty. */
static int release_device_state(IntegerMotionStateMetal *s)
{
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = NULL; }
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = NULL; }
    for (unsigned i = 0; i < MOTION_METAL_MAX_RING; i++) {
        if (s->raw[i]) { (void)(__bridge_transfer id<MTLBuffer>)s->raw[i]; s->raw[i] = NULL; }
    }
    const int err = vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = NULL; }
    return rc;
}

/* The ring of raw planes and the pipelines, on a context `s->ctx` holds. */
static int alloc_device_state(IntegerMotionStateMetal *s)
{
    void *dh = vmaf_metal_context_device_handle(s->ctx);
    if (dh == NULL) { return -ENODEV; }
    id<MTLDevice> device = (__bridge id<MTLDevice>)dh;
    for (unsigned i = 0; i < s->ring; i++) {
        id<MTLBuffer> plane = [device newBufferWithLength:s->plane_bytes
                                                  options:MTLResourceStorageModeShared];
        if (plane == nil) { return -ENOMEM; }
        s->raw[i] = (__bridge_retained void *)plane;
    }
    return build_pipelines(s, device);
}

static int init_device(IntegerMotionStateMetal *s)
{
    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err == 0) {
        err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    }
    if (err == 0) {
        err = vmaf_metal_kernel_buffer_alloc(&s->rb, s->ctx, s->partials_count * sizeof(uint32_t));
    }
    if (err == 0) {
        err = alloc_device_state(s);
    }
    return err;
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    IntegerMotionStateMetal *s = (IntegerMotionStateMetal *)fex->priv;

    /* The CPU's floor (integer_motion.c::init): the 5-tap reflect-101 filter
     * needs radius + 1 = 3 samples per axis. The kernel's tile loads stay in
     * the plane at every size through vmaf_mtl_motion_mirror()'s clamp. */
    if (w < 3u || h < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_metal: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    s->frame_w        = w;
    s->frame_h        = h;
    s->bpc            = bpc;
    s->flushed        = false;
    s->ring           = s->motion_five_frame_window ? 3u : 2u;
    s->row_bytes      = (size_t)w * (bpc <= 8u ? 1u : 2u);
    s->plane_bytes    = s->row_bytes * h;
    s->partials_count = (size_t)((w + 15u) / 16u) * (size_t)((h + 15u) / 16u);

    int err = init_device(s);
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                          fex->options, s);
        if (s->feature_name_dict == NULL) { err = -ENOMEM; }
    }
    if (err != 0) {
        (void)release_device_state(s);
    }
    return err;
}

/* The luma plane of `pic` into ring slot `slot`, rows packed. */
static void upload_plane(IntegerMotionStateMetal *s, const VmafPicture *pic, unsigned slot)
{
    id<MTLBuffer> plane = (__bridge id<MTLBuffer>)s->raw[slot];
    uint8_t *dst = (uint8_t *)[plane contents];
    for (unsigned y = 0; y < s->frame_h; y++) {
        memcpy(dst + (size_t)y * s->row_bytes,
               (const uint8_t *)pic->data[0] + (size_t)y * (size_t)pic->stride[0], s->row_bytes);
    }
}

/* The SAD kernel on ring slots `prev` and `cur`, waited for. */
static int run_sad_kernel(IntegerMotionStateMetal *s, unsigned prev, unsigned cur)
{
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (qh == NULL) { return -ENODEV; }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:(s->bpc <= 8u)
                                     ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
                                     : (__bridge id<MTLComputePipelineState>)s->pso_16bpc];
    [enc setBuffer:(__bridge id<MTLBuffer>)s->raw[prev] offset:0 atIndex:0];
    [enc setBuffer:(__bridge id<MTLBuffer>)s->raw[cur] offset:0 atIndex:1];
    [enc setBuffer:(__bridge id<MTLBuffer>)(void *)s->rb.buffer offset:0 atIndex:2];
    const uint32_t params[2] = {(uint32_t)s->bpc, 0u};
    [enc setBytes:params length:sizeof(params) atIndex:3];
    const uint32_t dim[2] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h};
    [enc setBytes:dim length:sizeof(dim) atIndex:4];
    [enc dispatchThreadgroups:MTLSizeMake((s->frame_w + 15u) / 16u, (s->frame_h + 15u) / 16u, 1)
        threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

/* The first frame with a SAD: integer_motion.c::extract()'s min_idx. */
static unsigned sad_min_index(const IntegerMotionStateMetal *s)
{
    return s->ring - 1u;
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90; (void)dist_pic_90; (void)dist_pic;
    IntegerMotionStateMetal *s = (IntegerMotionStateMetal *)fex->priv;

    /* motion_force_zero reports 0 and computes no SAD, as the CPU does. */
    if (s->motion_force_zero) { return 0; }
    if (ref_pic->w[0] != s->frame_w || ref_pic->h[0] != s->frame_h) { return -EINVAL; }

    upload_plane(s, ref_pic, index % s->ring);
    if (index < sad_min_index(s)) { return 0; }
    return run_sad_kernel(s, (index + 1u) % s->ring, index % s->ring);
}

/* integer_motion.c::extract()'s score of frame `index`. */
static double sad_score(const IntegerMotionStateMetal *s, unsigned index)
{
    if (s->motion_force_zero || index < sad_min_index(s)) { return 0.; }
    const uint32_t *parts = (const uint32_t *)s->rb.host_view;
    uint64_t sad = 0u;
    for (size_t i = 0; parts != NULL && i < s->partials_count; ++i) {
        sad += parts[i];
    }
    const unsigned w = s->frame_w;
    const unsigned h = s->frame_h;
    const double score = (double)sad / 256. / (w * h) * s->motion_fps_weight;
    return (score < s->motion_max_val) ? score : s->motion_max_val;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    IntegerMotionStateMetal *s = (IntegerMotionStateMetal *)fex->priv;
    const double score = sad_score(s, index);

    /* Every frame, as the CPU's three sites write it: before the first SAD,
     * under motion_force_zero, and the measured score. */
    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "VMAF_integer_feature_motion_sad_score",
                                                      score, index);
    if (err != 0 || !s->debug) { return err; }
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_motion_score", score,
                                                   index);
}

/* motion2 and motion3 of every frame from the SAD scores, with the CPU's
 * flush() (integer_motion.c, motion_window.h, ADR-1478). Once. */
static int flush_fex_metal(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    IntegerMotionStateMetal *s = (IntegerMotionStateMetal *)fex->priv;
    if (s->flushed || s->feature_name_dict == NULL) { return 1; }

    const VmafMotionWindow window = {
        .sad_feature = "VMAF_integer_feature_motion_sad_score",
        .motion2_feature = "VMAF_integer_feature_motion2_score",
        .motion3_feature = "VMAF_integer_feature_motion3_score",
        .motion_blend_factor = s->motion_blend_factor,
        .motion_blend_offset = s->motion_blend_offset,
        .motion_max_val = s->motion_max_val,
        .motion_five_frame_window = s->motion_five_frame_window,
        .motion_moving_average = s->motion_moving_average,
    };
    const int err = vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);
    if (err != 0) { return err; }
    s->flushed = true;
    return 1;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    IntegerMotionStateMetal *s = (IntegerMotionStateMetal *)fex->priv;
    int rc = release_device_state(s);
    if (s->feature_name_dict) {
        const int err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0) { rc = err; }
    }
    return rc;
}

/* integer_motion.c's provided_features[], the SAD score first. */
static const char *provided_features[] = {
    "VMAF_integer_feature_motion_sad_score",
    "VMAF_integer_feature_motion_score",
    "VMAF_integer_feature_motion2_score",
    "VMAF_integer_feature_motion3_score",
    NULL
};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend, ADR-0421 motion port; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0421 / ADR-0278
VmafFeatureExtractor vmaf_fex_integer_motion_metal = {
    .name              = "integer_motion_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = flush_fex_metal,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(IntegerMotionStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 1,
        .is_reduction_only      = true,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
