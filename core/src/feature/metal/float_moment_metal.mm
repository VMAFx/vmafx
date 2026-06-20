/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  float_moment feature extractor on the Metal backend (T8-1e / ADR-0421).
 *  Dispatches `float_moment_kernel_{8,16}bpc` from float_moment.metal.
 *
 *  Four float partials per WG (interleaved):
 *    base = (bid.y * grid_w + bid.x) * 4
 *    [base+0] = ref_1st_partial, [base+1] = dis_1st_partial,
 *    [base+2] = ref_2nd_partial, [base+3] = dis_2nd_partial
 *
 *  Host divides sums by (W * H) to get moment values.
 *  Feature names: float_moment_ref1st, float_moment_dis1st,
 *                 float_moment_ref2nd, float_moment_dis2nd.
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

typedef struct FloatMomentStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb;        /* 4 × grid_w × grid_h float partials */
    VmafMetalContext *ctx;
    void *pso_8bpc;
    void *pso_16bpc;

    size_t plane_bytes;
    size_t partials_count;   /* grid_w × grid_h */
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;

    VmafDictionary *feature_name_dict;
} FloatMomentStateMetal;

static const VmafOption options[] = {{0}};

static int build_pipelines(FloatMomentStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn8  = [lib newFunctionWithName:@"float_moment_kernel_8bpc"];
    id<MTLFunction> fn16 = [lib newFunctionWithName:@"float_moment_kernel_16bpc"];
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
    FloatMomentStateMetal *s = (FloatMomentStateMetal *)fex->priv;

    s->frame_w     = w;
    s->frame_h     = h;
    s->bpc         = bpc;
    s->plane_bytes = (size_t)w * h * (bpc <= 8u ? 1u : 2u);

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    {
        const size_t grid_w = (w + 15) / 16;
        const size_t grid_h = (h + 15) / 16;
        s->partials_count   = grid_w * grid_h;
        /* 4 floats per WG: ref1, dis1, ref2, dis2 */
        err = vmaf_metal_kernel_buffer_alloc(&s->rb, s->ctx,
                                             s->partials_count * 4u * sizeof(float));
    }
    if (err != 0) { goto fail_lc; }

    {
        void *dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == NULL) { err = -ENODEV; goto fail_rb; }
        err = build_pipelines(s, (__bridge id<MTLDevice>)dh);
    }
    if (err != 0) { goto fail_rb; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                      fex->options, s);
    if (s->feature_name_dict == NULL) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = NULL; }
    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = NULL; }
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
    (void)ref_pic_90; (void)dist_pic_90; (void)index;
    FloatMomentStateMetal *s = (FloatMomentStateMetal *)fex->priv;

    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    const size_t row_bytes = (size_t)s->frame_w * (s->bpc <= 8u ? 1u : 2u);

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }

    id<MTLDevice>      device = (__bridge id<MTLDevice>)dh;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLBuffer>    par_buf  = (__bridge id<MTLBuffer>)(void *)s->rb.buffer;
    id<MTLComputePipelineState> pso = (s->bpc <= 8u)
        ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
        : (__bridge id<MTLComputePipelineState>)s->pso_16bpc;

    id<MTLBuffer> ref_buf = [device newBufferWithLength:s->plane_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> dis_buf = [device newBufferWithLength:s->plane_bytes options:MTLResourceStorageModeShared];
    if (ref_buf == nil || dis_buf == nil) { return -ENOMEM; }
    {
        uint8_t *rd = (uint8_t *)[ref_buf contents];
        uint8_t *dd = (uint8_t *)[dis_buf contents];
        for (unsigned y = 0; y < s->frame_h; y++) {
            memcpy(rd + y * row_bytes, (uint8_t *)ref_pic->data[0] + y * ref_pic->stride[0], row_bytes);
            memcpy(dd + y * row_bytes, (uint8_t *)dist_pic->data[0] + y * dist_pic->stride[0], row_bytes);
        }
    }

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit fillBuffer:par_buf range:NSMakeRange(0, s->partials_count * 4u * sizeof(float)) value:0];
    [blit endEncoding];

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_buf offset:0 atIndex:0];
    [enc setBuffer:dis_buf offset:0 atIndex:1];
    [enc setBuffer:par_buf offset:0 atIndex:2];
    if (s->bpc <= 8u) {
        uint32_t st[2] = {(uint32_t)row_bytes, (uint32_t)row_bytes};
        [enc setBytes:st length:sizeof(st) atIndex:3];
    } else {
        uint32_t st[4] = {(uint32_t)row_bytes, (uint32_t)row_bytes, (uint32_t)s->bpc, 0};
        [enc setBytes:st length:sizeof(st) atIndex:3];
    }
    uint32_t dim[2] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h};
    [enc setBytes:dim length:sizeof(dim) atIndex:4];

    MTLSize tg   = MTLSizeMake(16, 16, 1);
    MTLSize grid = MTLSizeMake((s->frame_w + 15) / 16, (s->frame_h + 15) / 16, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    FloatMomentStateMetal *s = (FloatMomentStateMetal *)fex->priv;

    const float *parts = (const float *)s->rb.host_view;
    double sum[4] = {0.0, 0.0, 0.0, 0.0};
    if (parts != NULL) {
        for (size_t i = 0; i < s->partials_count; ++i) {
            const size_t base = i * 4u;
            sum[0] += (double)parts[base + 0];
            sum[1] += (double)parts[base + 1];
            sum[2] += (double)parts[base + 2];
            sum[3] += (double)parts[base + 3];
        }
    }
    const double n_pix = (double)s->frame_w * (double)s->frame_h;
    const double ref1 = (n_pix > 0.0) ? (sum[0] / n_pix) : 0.0;
    const double dis1 = (n_pix > 0.0) ? (sum[1] / n_pix) : 0.0;
    const double ref2 = (n_pix > 0.0) ? (sum[2] / n_pix) : 0.0;
    const double dis2 = (n_pix > 0.0) ? (sum[3] / n_pix) : 0.0;

    int err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "float_moment_ref1st", ref1, index);
    if (err != 0) { return err; }
    err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "float_moment_dis1st", dis1, index);
    if (err != 0) { return err; }
    err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "float_moment_ref2nd", ref2, index);
    if (err != 0) { return err; }
    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "float_moment_dis2nd", dis2, index);
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatMomentStateMetal *s = (FloatMomentStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = NULL; }
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = NULL; }

    int err = vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = NULL; }
    return rc;
}

static const char *provided_features[] = {
    "float_moment_ref1st", "float_moment_dis1st",
    "float_moment_ref2nd", "float_moment_dis2nd",
    NULL
};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_float_moment_metal = {
    .name              = "float_moment_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = NULL,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(FloatMomentStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 1,
        .is_reduction_only      = true,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
