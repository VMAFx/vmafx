/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  motion_v2 feature extractor on the Metal backend — first real kernel
 *  (T8-1c / ADR-0421), twin of the CPU `motion_v2`
 *  (core/src/feature/integer_motion_v2.c).
 *
 *  Device: integer_motion_v2.metal's `motion_v2_kernel_{8,16}bpc` blurs the
 *  frame difference (convolution is linear, so SAD(blur(prev), blur(cur)) is
 *  the sum of |blur(prev - cur)|) and stores each threadgroup's exact integer
 *  SAD. Host: the SAD is the uint64 sum of those, and the stored score is the
 *  CPU's, MIN(SAD / 256 / (w * h) * motion_fps_weight, motion_max_val), 0 for
 *  a frame without an earlier frame to difference against. flush() derives
 *  motion2_v2 and motion3_v2 of every frame from the stored scores with the
 *  CPU extractor's own function, vmaf_motion_window_flush() (motion_window.h,
 *  ADR-1478), so the twin's scores are the CPU's whenever its SADs are: a
 *  one-frame input gets motion2_v2 = motion3_v2 = 0, as on the CPU. This is
 *  the design of motion_v2_cuda, motion_v2_hip and motion_v2_sycl (ADR-1373,
 *  ADR-1382, ADR-1491; the Metal port of ADR-1498).
 *
 *  Options: the CPU table. motion_five_frame_window takes the SAD of frame n
 *  against frame n - 2 (Netflix a2b59b77): the twin keeps the luma of the
 *  last `depth` frames (1, or 2 with the option) in Shared buffers, frame n
 *  reading and then replacing slot n % depth. motion_force_zero stores 0 for
 *  every frame, as the CPU's extract() does, and runs no kernel.
 *
 *  Metallib resolution: the build embeds the compiled metallib into the
 *  libvmaf binary's __TEXT,__metallib section via the meson custom_target in
 *  `core/src/metal/meson.build`; init wraps the byte range in
 *  `dispatch_data_create` and hands it to `[device newLibraryWithData:]`.
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

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

#include "../../metal/objc_handle.h"

/* Default maximum value allowed for motion — mirrors
 * DEFAULT_MOTION_MAX_VAL in integer_motion_v2.c (the CPU reference). */
#define MOTION_V2_METAL_DEFAULT_MAX_VAL (10000.0)
#define MOTION_V2_METAL_BLOCK 16U
/* Frames between a frame and the one its SAD reads, at most (five-frame
 * window: frame n - 2). */
#define MOTION_V2_METAL_MAX_DEPTH 2U

namespace {

using MotionV2StateMetal = struct MotionV2StateMetal {
    VmafMetalKernelLifecycle lc;
    /* One uint32 SAD per threadgroup (exact: 256 x 65536 < 2^32). */
    VmafMetalKernelBuffer rb;
    VmafMetalContext *ctx;

    /* Pipeline states (one per bpc variant). Bridge-retained `void *`
     * so the C struct doesn't need to be Obj-C++ in the header. */
    void *pso_8bpc;
    void *pso_16bpc;

    /* Raw ref Y planes of the last `depth` frames (Shared MTLBuffers,
     * packed): frame n's SAD reads prev_luma[n % depth], which then takes
     * frame n's own luma. */
    void *prev_luma[MOTION_V2_METAL_MAX_DEPTH];

    size_t plane_bytes;
    size_t partials_count; /* number of threadgroups (grid_w * grid_h) */
    unsigned depth;        /* 1, or 2 with motion_five_frame_window */
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;

    /* The CPU integer_motion_v2.c options. */
    bool motion_force_zero;
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_fps_weight;
    double motion_max_val;
    bool motion_five_frame_window;
    bool motion_moving_average;

    VmafDictionary *feature_name_dict;
};
} // namespace

namespace {

/* The CPU integer_motion_v2.c table: same names, aliases, defaults, ranges
 * and flags, so co-scheduled CPU and Metal runs name features identically
 * and a model's options select the twin (ADR-1183). */
const VmafOption options[] = {
    {
        .name = "motion_force_zero",
        .help = "forces motion score to be 0",
        .alias = "force_0",
        .offset = offsetof(MotionV2StateMetal, motion_force_zero),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_factor",
        .help = "blend motion score given an offset",
        .alias = "mbf",
        .offset = offsetof(MotionV2StateMetal, motion_blend_factor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_offset",
        .help = "blend motion score starting from this offset",
        .alias = "mbo",
        .offset = offsetof(MotionV2StateMetal, motion_blend_offset),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 40.0,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_fps_weight",
        .help = "fps-aware multiplicative weight/correction",
        .alias = "mfw",
        .offset = offsetof(MotionV2StateMetal, motion_fps_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 5.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_max_val",
        .help = "maximum value allowed; larger values will be clipped to this value",
        .alias = "mmxv",
        .offset = offsetof(MotionV2StateMetal, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = MOTION_V2_METAL_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_five_frame_window",
        .help = "use five-frame temporal window",
        .alias = "mffw",
        .offset = offsetof(MotionV2StateMetal, motion_five_frame_window),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_moving_average",
        .help = "smooth motion3 with a 2-frame moving average",
        .alias = "mma",
        .offset = offsetof(MotionV2StateMetal, motion_moving_average),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {.name=nullptr}};

int build_pipelines(MotionV2StateMetal *s, id<MTLDevice> device)
{
    int load_rc = 0;
    id<MTLLibrary> const lib = vmaf_metal_library_load(device, &load_rc);
    if (lib == nil) { return load_rc; }
    NSError *err = nil;

    id<MTLFunction> const fn8  = [lib newFunctionWithName:@"motion_v2_kernel_8bpc"];
    id<MTLFunction> const fn16 = [lib newFunctionWithName:@"motion_v2_kernel_16bpc"];
    if (fn8 == nil || fn16 == nil) { return -ENODEV; }

    id<MTLComputePipelineState> const pso8 =
        [device newComputePipelineStateWithFunction:fn8 error:&err];
    id<MTLComputePipelineState> const pso16 =
        [device newComputePipelineStateWithFunction:fn16 error:&err];
    if (pso8 == nil || pso16 == nil) { return -ENODEV; }

    s->pso_8bpc  = (__bridge_retained void *)pso8;
    s->pso_16bpc = (__bridge_retained void *)pso16;
    return 0;
}

/* The luma planes of the last `depth` frames and the pipelines. */
int mv2_metal_device_setup(MotionV2StateMetal *s)
{
    void *const device_handle = vmaf_metal_context_device_handle(s->ctx);
    if (device_handle == nullptr) { return -ENODEV; }
    id<MTLDevice> const device = (__bridge id<MTLDevice>)device_handle;
    for (unsigned i = 0; i < s->depth; i++) {
        id<MTLBuffer> const plane = [device newBufferWithLength:s->plane_bytes
                                                  options:MTLResourceStorageModeShared];
        if (plane == nil) { return -ENOMEM; }
        s->prev_luma[i] = (__bridge_retained void *)plane;
    }
    return build_pipelines(s, device);
}

/* Tear down everything init() may have set up; every step tolerates a handle
 * that was never created, so this serves a failed init() and close(). Returns
 * the first error but releases everything. */
int mv2_metal_release(MotionV2StateMetal *s)
{
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
    if (s->pso_16bpc != nullptr) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc;
        s->pso_16bpc = nullptr;
    }
    if (s->pso_8bpc != nullptr) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;
        s->pso_8bpc = nullptr;
    }
    for (auto & i : s->prev_luma) {
        if (i != nullptr) {
            (void)(__bridge_transfer id<MTLBuffer>)i;
            i = nullptr;
        }
    }
    int err = vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    if (s->feature_name_dict != nullptr) {
        err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0) { rc = err; }
    }
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = nullptr;
    return rc;
}

int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                          unsigned w, unsigned h)
{
    (void)pix_fmt;
    MotionV2StateMetal *s = (MotionV2StateMetal *)fex->priv;

    /* The CPU's floor (integer_motion_v2.c::init): the 5-tap reflect-101
     * filter needs radius + 1 = 3 samples per axis. Checked before anything
     * else, so a refused size touches neither the options nor the device.
     * (The kernel's own 20x20 tile loads stay in bounds through mv2_mirror's
     * iterated fold, see integer_motion_v2.metal.) */
    if (w < 3u || h < 3u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "motion_v2_metal: frame %ux%u is below the 5-tap filter minimum 3x3; "
                 "refusing to avoid out-of-bounds mirror reads on device\n",
                 w, h);
        return -EINVAL;
    }

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->plane_bytes = (size_t)w * h * (bpc <= 8u ? 1u : 2u);
    s->partials_count = (size_t)((w + MOTION_V2_METAL_BLOCK - 1U) / MOTION_V2_METAL_BLOCK) *
                        ((h + MOTION_V2_METAL_BLOCK - 1U) / MOTION_V2_METAL_BLOCK);
    /* The CPU's min_idx (integer_motion_v2.c::extract): the SAD of frame n
     * is taken against frame n - depth. */
    s->depth = s->motion_five_frame_window ? 2U : 1U;

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err == 0) {
        err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    }
    if (err == 0) {
        err = vmaf_metal_kernel_buffer_alloc(&s->rb, s->ctx, s->partials_count * sizeof(uint32_t));
    }
    if (err == 0) {
        err = mv2_metal_device_setup(s);
    }
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == nullptr) { err = -ENOMEM; }
    }
    if (err != 0) {
        (void)mv2_metal_release(s);
    }
    return err;
}

void copy_y_plane(const VmafPicture *pic, void *dst, size_t row_bytes)
{
    const uint8_t *src = (const uint8_t *)pic->data[0];
    const size_t src_stride = pic->stride[0];
    uint8_t *out = (uint8_t *)dst;
    for (unsigned y = 0; y < pic->h[0]; y++) {
        memcpy(out + y * row_bytes, src + y * src_stride, row_bytes);
    }
}

/* The SAD kernel of `cur` against `prev`; every threadgroup writes its slot
 * of rb. Waits for the device. */
int mv2_metal_dispatch(MotionV2StateMetal *s, id<MTLCommandQueue> queue,
                              id<MTLBuffer> prev, id<MTLBuffer> cur, size_t row_bytes)
{
    id<MTLComputePipelineState> const pso = (s->bpc <= 8u)
        ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
        : (__bridge id<MTLComputePipelineState>)s->pso_16bpc;
    id<MTLCommandBuffer> const cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }
    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    if (enc == nil) { return -ENOMEM; }

    [enc setComputePipelineState:pso];
    [enc setBuffer:prev offset:0 atIndex:0];
    [enc setBuffer:cur  offset:0 atIndex:1];
    [enc setBuffer:vmaf_metal::borrow<id<MTLBuffer>>(s->rb.buffer) offset:0 atIndex:2];
    if (s->bpc <= 8u) {
        const uint32_t strides[2] = {(uint32_t)row_bytes, (uint32_t)row_bytes};
        [enc setBytes:strides length:sizeof(strides) atIndex:3];
    } else {
        const uint32_t strides[4] = {(uint32_t)row_bytes, (uint32_t)row_bytes, s->bpc, 0};
        [enc setBytes:strides length:sizeof(strides) atIndex:3];
    }
    const uint32_t dim[2] = {s->frame_w, s->frame_h};
    [enc setBytes:dim length:sizeof(dim) atIndex:4];

    const size_t tile_int_count = (size_t)20 * 21; /* MV2_TILE_H * MV2_TILE_PITCH */
    [enc setThreadgroupMemoryLength:(tile_int_count * sizeof(int32_t)) atIndex:0];

    const MTLSize tg_size = MTLSizeMake(MOTION_V2_METAL_BLOCK, MOTION_V2_METAL_BLOCK, 1);
    const MTLSize grid_size =
        MTLSizeMake((s->frame_w + MOTION_V2_METAL_BLOCK - 1U) / MOTION_V2_METAL_BLOCK,
                    (s->frame_h + MOTION_V2_METAL_BLOCK - 1U) / MOTION_V2_METAL_BLOCK, 1);
    [enc dispatchThreadgroups:grid_size threadsPerThreadgroup:tg_size];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];
    return ([cmd status] == MTLCommandBufferStatusCompleted) ? 0 : -EIO;
}

int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    MotionV2StateMetal *s = (MotionV2StateMetal *)fex->priv;

    /* motion_force_zero: collect() stores 0, as the CPU's extract() does. */
    if (s->motion_force_zero) { return 0; }

    /* The geometry stays the one init() sized the buffers for; libvmaf
     * rejects a picture of any other size before it reaches submit(). */
    const size_t row_bytes = (size_t)s->frame_w * (s->bpc <= 8u ? 1u : 2u);
    id<MTLBuffer> const slot = (__bridge id<MTLBuffer>)s->prev_luma[index % s->depth];
    if (index >= s->depth) {
        void *const device_handle = vmaf_metal_context_device_handle(s->ctx);
        void *const queue_handle  = vmaf_metal_context_queue_handle(s->ctx);
        if (device_handle == nullptr || queue_handle == nullptr) { return -ENODEV; }
        id<MTLDevice> const device = (__bridge id<MTLDevice>)device_handle;
        id<MTLBuffer> const cur = [device newBufferWithLength:s->plane_bytes
                                                options:MTLResourceStorageModeShared];
        if (cur == nil) { return -ENOMEM; }
        copy_y_plane(ref_pic, [cur contents], row_bytes);
        const int err = mv2_metal_dispatch(s, (__bridge id<MTLCommandQueue>)queue_handle, slot,
                                           cur, row_bytes);
        if (err != 0) { return err; }
    }
    /* This frame's luma replaces the frame `depth` back it was compared
     * with; frame n + depth reads it. */
    copy_y_plane(ref_pic, [slot contents], row_bytes);
    return 0;
}

/* The frame's SAD: the threadgroup sums added in uint64. */
int mv2_metal_sad(const MotionV2StateMetal *s, uint64_t *sad)
{
    const uint32_t *partials = (const uint32_t *)s->rb.host_view;
    if (partials == nullptr) { return -EIO; }
    uint64_t sum = 0U;
    for (size_t i = 0; i < s->partials_count; ++i) {
        sum += partials[i];
    }
    *sad = sum;
    return 0;
}

int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    MotionV2StateMetal *const s = (MotionV2StateMetal *)fex->priv;

    /* motion_force_zero, or no frame `depth` back (the CPU's min_idx): the
     * CPU stores 0. */
    if (s->motion_force_zero || index < s->depth) {
        return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "VMAF_integer_feature_motion_v2_sad_score",
                                                       0., index);
    }

    uint64_t sad = 0U;
    const int err = mv2_metal_sad(s, &sad);
    if (err != 0) { return err; }

    /* The CPU's score and stored value (integer_motion_v2.c::extract):
     * normalised, weighted by motion_fps_weight, capped at motion_max_val.
     * flush() derives motion2_v2 / motion3_v2 from it and does not weight it
     * again. */
    const double score = (double)sad / 256. / (s->frame_w * s->frame_h);
    const double weighted = score * s->motion_fps_weight;
    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score",
        (weighted < s->motion_max_val) ? weighted : s->motion_max_val, index);
}

/* motion2_v2 and motion3_v2 of every frame, from the stored SAD scores: the
 * CPU extractor's own derivation (integer_motion.c::vmaf_motion_window_flush(),
 * ADR-1478), with the three-frame or the five-frame window. A one-frame run
 * gets motion2_v2 = motion3_v2 = 0, as on the CPU; an empty run nothing. */
int flush_fex_metal(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionV2StateMetal *const s = (MotionV2StateMetal *)fex->priv;

    /* No frame reached init(): nothing was stored, nothing to derive. */
    if (s->feature_name_dict == nullptr) { return 1; }

    const VmafMotionWindow window = {
        .sad_feature = "VMAF_integer_feature_motion_v2_sad_score",
        .motion2_feature = "VMAF_integer_feature_motion2_v2_score",
        .motion3_feature = "VMAF_integer_feature_motion3_v2_score",
        .motion_blend_factor = s->motion_blend_factor,
        .motion_blend_offset = s->motion_blend_offset,
        .motion_max_val = s->motion_max_val,
        .motion_five_frame_window = s->motion_five_frame_window,
        .motion_moving_average = s->motion_moving_average,
    };
    const int err = vmaf_motion_window_flush(feature_collector, s->feature_name_dict, &window);
    return err ? err : 1;
}

int close_fex_metal(VmafFeatureExtractor *fex)
{
    return mv2_metal_release((MotionV2StateMetal *)fex->priv);
}

const char *provided_features[] = {"VMAF_integer_feature_motion_v2_sad_score",
                                          "VMAF_integer_feature_motion2_v2_score",
                                          "VMAF_integer_feature_motion3_v2_score", nullptr};
} // namespace

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0421 Metal
 * first-kernel motion_v2; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0421 / ADR-0278
VmafFeatureExtractor vmaf_fex_integer_motion_v2_metal = {
    .name = "motion_v2_metal",
    .init = init_fex_metal,
    .submit = submit_fex_metal,
    .collect = collect_fex_metal,
    .flush = flush_fex_metal,
    .close = close_fex_metal,
    .options = options,
    .priv_size = sizeof(MotionV2StateMetal),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL | VMAF_FEATURE_EXTRACTOR_METAL,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
} /* extern "C" */
