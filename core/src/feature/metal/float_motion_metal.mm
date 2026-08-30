/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  float_motion feature extractor on the Metal backend (T8-1h / ADR-0421).
 *  Dispatches `float_motion_kernel_{8,16}bpc` from float_motion.metal.
 *
 *  Temporal: keeps a ping-pong float MTLBuffer for the blurred ref frame.
 *  Frame 0: blur into prev_blurred; emit motion=0, motion2=0.
 *  Frame N: SAD(cur_blurred, prev_blurred); emit motion and motion2.
 *
 *  Score: sad_sum / (W * H). motion2 = min(prev, cur).
 *  Feature names: VMAF_feature_motion_score, VMAF_feature_motion2_score.
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

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

typedef struct FloatMotionStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb;        /* float sad_parts, grid_w × grid_h */
    VmafMetalContext *ctx;
    void *pso_8bpc;
    void *pso_16bpc;

    /* Previous blurred frame (float, W × H, unified memory). */
    void *prev_blur_buf;           /* __bridge_retained id<MTLBuffer> */
    void *cur_blur_buf;            /* __bridge_retained id<MTLBuffer> */
    size_t blur_buf_size;          /* W × H × sizeof(float) */

    double prev_motion_score;
    double motion_fps_weight;
    size_t partials_count;
    unsigned frame_index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;

    VmafDictionary *feature_name_dict;
} FloatMotionStateMetal;

static const VmafOption options[] = {
    {
        .name    = "motion_fps_weight",
        .alias   = "mfw",
        .help    = "fps-aware multiplicative weight/correction",
        .offset  = offsetof(FloatMotionStateMetal, motion_fps_weight),
        .type    = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min     = 0.0,
        .max     = 5.0,
        .flags   = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0},
};

static int build_pipelines(FloatMotionStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn8  = [lib newFunctionWithName:@"float_motion_kernel_8bpc"];
    id<MTLFunction> fn16 = [lib newFunctionWithName:@"float_motion_kernel_16bpc"];
    if (fn8 == nil || fn16 == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso8  = [device newComputePipelineStateWithFunction:fn8  error:&err];
    id<MTLComputePipelineState> pso16 = [device newComputePipelineStateWithFunction:fn16 error:&err];
    if (pso8 == nil || pso16 == nil) { return -ENODEV; }

    s->pso_8bpc  = (__bridge_retained void *)pso8;
    s->pso_16bpc = (__bridge_retained void *)pso16;
    return 0;
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatMotionStateMetal *s = (FloatMotionStateMetal *)fex->priv;

    s->frame_w          = w;
    s->frame_h          = h;
    s->bpc              = bpc;
    s->frame_index      = 0;
    s->prev_motion_score = 0.0;
    s->blur_buf_size    = (size_t)w * h * sizeof(float);

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    {
        const size_t grid_w = (w + 15) / 16;
        const size_t grid_h = (h + 15) / 16;
        s->partials_count   = grid_w * grid_h;
        err = vmaf_metal_kernel_buffer_alloc(&s->rb, s->ctx,
                                             s->partials_count * sizeof(float));
    }
    if (err != 0) { goto fail_lc; }

    {
        void *dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == NULL) { err = -ENODEV; goto fail_rb; }
        id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

        id<MTLBuffer> prev = [device newBufferWithLength:s->blur_buf_size
                                                 options:MTLResourceStorageModeShared];
        id<MTLBuffer> cur  = [device newBufferWithLength:s->blur_buf_size
                                                 options:MTLResourceStorageModeShared];
        if (prev == nil || cur == nil) { err = -ENOMEM; goto fail_rb; }
        s->prev_blur_buf = (__bridge_retained void *)prev;
        s->cur_blur_buf  = (__bridge_retained void *)cur;

        err = build_pipelines(s, device);
    }
    if (err != 0) { goto fail_blurs; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                      fex->options, s);
    if (s->feature_name_dict == NULL) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = NULL; }
    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = NULL; }
fail_blurs:
    if (s->cur_blur_buf)  { (void)(__bridge_transfer id<MTLBuffer>)s->cur_blur_buf;  s->cur_blur_buf  = NULL; }
    if (s->prev_blur_buf) { (void)(__bridge_transfer id<MTLBuffer>)s->prev_blur_buf; s->prev_blur_buf = NULL; }
fail_rb:
    (void)vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
fail_lc:
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
fail_ctx:
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90; (void)dist_pic_90; (void)dist_pic;
    FloatMotionStateMetal *s = (FloatMotionStateMetal *)fex->priv;

    s->frame_w     = ref_pic->w[0];
    s->frame_h     = ref_pic->h[0];
    s->frame_index = index;

    const size_t row_bytes = (size_t)s->frame_w * (s->bpc <= 8u ? 1u : 2u);
    const size_t plane_bytes = row_bytes * s->frame_h;

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }

    id<MTLDevice>       device  = (__bridge id<MTLDevice>)dh;
    id<MTLCommandQueue>  queue  = (__bridge id<MTLCommandQueue>)qh;
    id<MTLBuffer>     prev_buf  = (__bridge id<MTLBuffer>)s->prev_blur_buf;
    id<MTLBuffer>     cur_buf   = (__bridge id<MTLBuffer>)s->cur_blur_buf;
    id<MTLBuffer>     sad_buf   = (__bridge id<MTLBuffer>)(void *)s->rb.buffer;
    id<MTLComputePipelineState> pso = (s->bpc <= 8u)
        ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
        : (__bridge id<MTLComputePipelineState>)s->pso_16bpc;

    id<MTLBuffer> ref_buf = [device newBufferWithLength:plane_bytes options:MTLResourceStorageModeShared];
    if (ref_buf == nil) { return -ENOMEM; }
    {
        uint8_t *rd = (uint8_t *)[ref_buf contents];
        for (unsigned y = 0; y < s->frame_h; y++) {
            memcpy(rd + y * row_bytes,
                   (uint8_t *)ref_pic->data[0] + y * ref_pic->stride[0],
                   row_bytes);
        }
    }

    const uint32_t compute_sad = (index > 0) ? 1u : 0u;
    const uint32_t blur_stride = (uint32_t)s->frame_w; /* float stride in elements */

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit fillBuffer:sad_buf range:NSMakeRange(0, s->partials_count * sizeof(float)) value:0];
    [blit endEncoding];

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_buf  offset:0 atIndex:0];  /* current frame raw pixels */
    [enc setBuffer:prev_buf offset:0 atIndex:1];  /* previous blurred */
    [enc setBuffer:cur_buf  offset:0 atIndex:2];  /* output: current blurred */
    [enc setBuffer:sad_buf  offset:0 atIndex:3];  /* sad_parts */
    {
        uint32_t st[4] = {(uint32_t)row_bytes, blur_stride, (uint32_t)s->bpc, compute_sad};
        [enc setBytes:st length:sizeof(st) atIndex:4];
    }
    uint32_t dim[2] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h};
    [enc setBytes:dim length:sizeof(dim) atIndex:5];

    MTLSize tg   = MTLSizeMake(16, 16, 1);
    MTLSize grid = MTLSizeMake((s->frame_w + 15) / 16, (s->frame_h + 15) / 16, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];

    /* Swap prev ↔ cur for the next frame. */
    void *tmp        = s->prev_blur_buf;
    s->prev_blur_buf = s->cur_blur_buf;
    s->cur_blur_buf  = tmp;

    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    FloatMotionStateMetal *s = (FloatMotionStateMetal *)fex->priv;

    double motion_score = 0.0;
    if (index > 0) {
        const float *parts = (const float *)s->rb.host_view;
        double sad_sum = 0.0;
        if (parts != NULL) {
            for (size_t i = 0; i < s->partials_count; ++i) {
                sad_sum += (double)parts[i];
            }
        }
        const double n_pix = (double)s->frame_w * (double)s->frame_h;
        motion_score = (n_pix > 0.0) ? (sad_sum / n_pix) : 0.0;
    }

    int err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict,
        "VMAF_feature_motion_score", motion_score, index);
    if (err != 0) { return err; }

    if (index == 0) {
        /* First frame: no previous, emit motion2 = 0 at index 0. Mirrors
         * the HIP / CUDA twins, which write the index-0 motion2 here. */
        err = vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict,
            "VMAF_feature_motion2_score", 0.0, index);
        if (err != 0) { return err; }
    } else if (index == 1) {
        /* Second frame: skip motion2 — the index-0 value was already
         * written by the index==0 branch above. No-op for motion2 here,
         * matching the HIP / CUDA twins' `index == 1` guard. */
    } else {
        /* index >= 2: emit motion2 = min(prev, cur) at index - 1. Apply
         * fps weight to both operands before the min; identity when
         * motion_fps_weight = 1.0.  Mirrors the HIP and CUDA twin patterns. */
        const double w_cur  = motion_score * s->motion_fps_weight;
        const double w_prev = s->prev_motion_score * s->motion_fps_weight;
        const double motion2 = (w_cur < w_prev) ? w_cur : w_prev;
        err = vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict,
            "VMAF_feature_motion2_score", motion2, index - 1);
        if (err != 0) { return err; }
    }

    s->prev_motion_score = motion_score;
    return 0;
}

static int flush_fex_metal(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    FloatMotionStateMetal *s = (FloatMotionStateMetal *)fex->priv;
    if (s->frame_index >= 1) {
        /* Tail emission: apply fps weight; identity when motion_fps_weight = 1.0. */
        (void)vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict,
            "VMAF_feature_motion2_score",
            s->prev_motion_score * s->motion_fps_weight, s->frame_index);
    }
    return 1;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatMotionStateMetal *s = (FloatMotionStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_16bpc)    { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc;   s->pso_16bpc    = NULL; }
    if (s->pso_8bpc)     { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;    s->pso_8bpc     = NULL; }
    if (s->cur_blur_buf) { (void)(__bridge_transfer id<MTLBuffer>)s->cur_blur_buf;              s->cur_blur_buf  = NULL; }
    if (s->prev_blur_buf){ (void)(__bridge_transfer id<MTLBuffer>)s->prev_blur_buf;             s->prev_blur_buf = NULL; }

    int err = vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = NULL; }
    return rc;
}

static const char *provided_features[] = {
    "VMAF_feature_motion_score", "VMAF_feature_motion2_score", NULL
};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_float_motion_metal = {
    .name              = "float_motion_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = flush_fex_metal,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(FloatMotionStateMetal),
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
