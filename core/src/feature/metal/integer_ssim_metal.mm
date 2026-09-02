/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  integer_ssim feature extractor on the Metal backend.
 *  Integer (fixed-point) twin of float_ssim_metal.mm — mirrors the
 *  float twin's two-pass dispatch scaffolding verbatim and swaps the
 *  float arithmetic for the fixed-point arithmetic of the CPU integer
 *  reference libvmaf/src/feature/integer_ssim.c (feature name `ssim`).
 *
 *  Two-pass dispatch:
 *    pass 0 -> integer_ssim_horiz_{8,16}bpc  (6 int64 moment planes)
 *    pass 1 -> integer_ssim_vert_combine     (per-WG float partials)
 *
 *  Unlike float_ssim, the integer path:
 *   - operates on raw integer pixels (NO [0,255] normalisation / scaler);
 *   - uses the 9-tap fixed-point Gaussian {2,9,28,55,68,55,28,9,2}
 *     (gaussian_filter_init(1.5, 5), KERNEL_WEIGHT=256) for BOTH the
 *     horizontal and vertical passes — see integer_ssim.metal;
 *   - covers the FULL W x H grid with edge-truncated convolution (the
 *     per-pixel weight `w` shrinks near edges), so the host divides
 *     sum(ssim_term) by sum(weight) rather than by a fixed pixel count;
 *   - has NO luminance/contrast/structure sub-scores.
 *
 *  Partials: two float buffers, grid_w x grid_h each — `ssim_parts`
 *  (per-WG sum of SSIM terms) and `ssimw_parts` (per-WG sum of weights).
 *  Host: score = sum(ssim_parts) / sum(ssimw_parts).
 *
 *  Options (matching integer_ssim.c CPU parity):
 *    enable_db   — convert SSIM to dB: score = -10*log10(1 - ssim).
 *    clip_db     — clamp dB output to a peak-derived finite maximum.
 *
 *  Feature name: ssim.
 */

#include <errno.h>
#include <math.h>
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
#include "log.h"
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

typedef struct IntegerSsimStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb;        /* float ssim_term partials, grid_w x grid_h */
    VmafMetalKernelBuffer rbw;       /* float weight partials, grid_w x grid_h */
    VmafMetalContext *ctx;
    void *pso_horiz_8;               /* integer_ssim_horiz_8bpc  (pass 0) */
    void *pso_horiz_16;              /* integer_ssim_horiz_16bpc (pass 0) */
    void *pso_vert;                  /* integer_ssim_vert_combine (pass 1) */
    void *hbuf_buf;                  /* intermediate 6-plane int64 buffer */

    /* Options (matching integer_ssim.c CPU). */
    bool    enable_db;
    bool    clip_db;

    double  max_db;          /* INFINITY unless clip_db */
    uint32_t samplemax;      /* (1 << bpc) - 1 */
    size_t  hbuf_size;
    size_t  partials_count;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;

    VmafDictionary *feature_name_dict;
} IntegerSsimStateMetal;

/* ------------------------------------------------------------------ */
/* Options                                                              */
/* ------------------------------------------------------------------ */

static const VmafOption options[] = {
    {
        .name        = "enable_db",
        .help        = "write SSIM values as dB: -10*log10(1 - ssim)",
        .offset      = offsetof(IntegerSsimStateMetal, enable_db),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name        = "clip_db",
        .help        = "clip dB scores to a peak-derived ceiling",
        .offset      = offsetof(IntegerSsimStateMetal, clip_db),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {0}
};

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static double ssim_to_db(double ssim, double max_db)
{
    const double db = -10.0 * log10(1.0 - ssim);
    return (db < max_db) ? db : max_db;
}

static int build_pipelines(IntegerSsimStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn_h8  = [lib newFunctionWithName:@"integer_ssim_horiz_8bpc"];
    id<MTLFunction> fn_h16 = [lib newFunctionWithName:@"integer_ssim_horiz_16bpc"];
    id<MTLFunction> fn_vert = [lib newFunctionWithName:@"integer_ssim_vert_combine"];
    if (fn_h8 == nil || fn_h16 == nil || fn_vert == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso_h8  = [device newComputePipelineStateWithFunction:fn_h8  error:&err];
    id<MTLComputePipelineState> pso_h16 = [device newComputePipelineStateWithFunction:fn_h16 error:&err];
    id<MTLComputePipelineState> pso_v   = [device newComputePipelineStateWithFunction:fn_vert error:&err];
    if (pso_h8 == nil || pso_h16 == nil || pso_v == nil) { return -ENODEV; }

    s->pso_horiz_8  = (__bridge_retained void *)pso_h8;
    s->pso_horiz_16 = (__bridge_retained void *)pso_h16;
    s->pso_vert     = (__bridge_retained void *)pso_v;
    return 0;
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    IntegerSsimStateMetal *s = (IntegerSsimStateMetal *)fex->priv;

    if (w < 1u || h < 1u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "integer_ssim_metal: invalid frame size %ux%u.\n", w, h);
        return -EINVAL;
    }

    s->frame_w   = w;
    s->frame_h   = h;
    s->bpc       = bpc;
    s->samplemax = (1u << bpc) - 1u;

    /* Derive max_db used by clip_db. Mirrors integer_ssim.c::init. */
    if (s->clip_db) {
        const unsigned peak = (1u << bpc) - 1u;
        const double mse  = 0.5 / ((double)w * (double)h);
        s->max_db = ceil(10.0 * log10((double)(peak * peak) / mse));
    } else {
        s->max_db = INFINITY;
    }

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    {
        /* SSIM partials grid is over the full W x H output. */
        const size_t grid_w = (s->frame_w + 15u) / 16u;
        const size_t grid_h = (s->frame_h +  7u) /  8u;
        s->partials_count   = grid_w * grid_h;
        err = vmaf_metal_kernel_buffer_alloc(&s->rb, s->ctx,
                                             s->partials_count * sizeof(float));
    }
    if (err != 0) { goto fail_lc; }

    err = vmaf_metal_kernel_buffer_alloc(&s->rbw, s->ctx,
                                         s->partials_count * sizeof(float));
    if (err != 0) { goto fail_rb; }

    {
        void *dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == NULL) { err = -ENODEV; goto fail_rbw; }
        id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

        /* hbuf: 6 planes x W x H int64 (mux, muy, x2, xy, y2, w). */
        s->hbuf_size = 6u * (size_t)s->frame_w * (size_t)s->frame_h * sizeof(int64_t);
        id<MTLBuffer> hb = [device newBufferWithLength:s->hbuf_size
                                               options:MTLResourceStorageModeShared];
        if (hb == nil) { err = -ENOMEM; goto fail_rbw; }
        s->hbuf_buf = (__bridge_retained void *)hb;

        err = build_pipelines(s, device);
    }
    if (err != 0) { goto fail_hbuf; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                      fex->options, s);
    if (s->feature_name_dict == NULL) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    if (s->pso_vert)     { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_vert;     s->pso_vert     = NULL; }
    if (s->pso_horiz_16) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_horiz_16; s->pso_horiz_16 = NULL; }
    if (s->pso_horiz_8)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_horiz_8;  s->pso_horiz_8  = NULL; }
fail_hbuf:
    if (s->hbuf_buf) { (void)(__bridge_transfer id<MTLBuffer>)s->hbuf_buf; s->hbuf_buf = NULL; }
fail_rbw:
    (void)vmaf_metal_kernel_buffer_free(&s->rbw, s->ctx);
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
    IntegerSsimStateMetal *s = (IntegerSsimStateMetal *)fex->priv;

    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];

    const size_t px_bytes  = (s->bpc <= 8u) ? 1u : 2u;
    const size_t row_bytes = (size_t)s->frame_w * px_bytes;
    const size_t plane_bytes = row_bytes * s->frame_h;

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }

    id<MTLDevice>       device = (__bridge id<MTLDevice>)dh;
    id<MTLCommandQueue>  queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLBuffer>    hbuf_buf  = (__bridge id<MTLBuffer>)s->hbuf_buf;
    id<MTLBuffer>    par_buf   = (__bridge id<MTLBuffer>)(void *)s->rb.buffer;
    id<MTLBuffer>    parw_buf  = (__bridge id<MTLBuffer>)(void *)s->rbw.buffer;

    /* Upload ref and dis as raw (un-normalised) integer pixels — the
     * Metal buffers are tightly packed (stride == w*px_bytes), so the
     * kernel strides are row_bytes. */
    id<MTLBuffer> ref_buf = [device newBufferWithLength:plane_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> dis_buf = [device newBufferWithLength:plane_bytes options:MTLResourceStorageModeShared];
    if (ref_buf == nil || dis_buf == nil) { return -ENOMEM; }
    {
        uint8_t *rd = (uint8_t *)[ref_buf contents];
        uint8_t *dd = (uint8_t *)[dis_buf contents];
        for (unsigned y = 0; y < s->frame_h; ++y) {
            memcpy(rd + y * row_bytes, (uint8_t *)ref_pic->data[0]  + y * ref_pic->stride[0],  row_bytes);
            memcpy(dd + y * row_bytes, (uint8_t *)dist_pic->data[0] + y * dist_pic->stride[0], row_bytes);
        }
    }

    const size_t grid_w = (s->frame_w + 15u) / 16u;
    const size_t grid_h = (s->frame_h +  7u) /  8u;

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    {
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit fillBuffer:par_buf  range:NSMakeRange(0, s->partials_count * sizeof(float)) value:0];
        [blit fillBuffer:parw_buf range:NSMakeRange(0, s->partials_count * sizeof(float)) value:0];
        [blit endEncoding];
    }

    /* Pass 0: horizontal moment accumulation -> hbuf (6 int64 planes). */
    {
        id<MTLComputePipelineState> pso_h = (s->bpc <= 8u)
            ? (__bridge id<MTLComputePipelineState>)s->pso_horiz_8
            : (__bridge id<MTLComputePipelineState>)s->pso_horiz_16;
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:pso_h];
        [enc setBuffer:ref_buf  offset:0 atIndex:0];
        [enc setBuffer:dis_buf  offset:0 atIndex:1];
        [enc setBuffer:hbuf_buf offset:0 atIndex:2];
        uint32_t params[4] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h,
                              (uint32_t)row_bytes, (uint32_t)row_bytes};
        [enc setBytes:params length:sizeof(params) atIndex:3];
        MTLSize tg   = MTLSizeMake(16, 8, 1);
        MTLSize grid = MTLSizeMake(grid_w, grid_h, 1);
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
    }

    /* Pass 1: vertical accumulation + SSIM combine -> partials. */
    {
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso_vert];
        [enc setBuffer:hbuf_buf offset:0 atIndex:0];
        [enc setBuffer:par_buf  offset:0 atIndex:1];
        [enc setBuffer:parw_buf offset:0 atIndex:2];
        uint32_t params[4] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h,
                              (uint32_t)s->samplemax, 0};
        [enc setBytes:params length:sizeof(params) atIndex:3];
        uint32_t gdim[2] = {(uint32_t)grid_w, (uint32_t)grid_h};
        [enc setBytes:gdim length:sizeof(gdim) atIndex:4];
        MTLSize tg      = MTLSizeMake(16, 8, 1);
        MTLSize grid_sz = MTLSizeMake(grid_w, grid_h, 1);
        [enc dispatchThreadgroups:grid_sz threadsPerThreadgroup:tg];
        [enc endEncoding];
    }

    [cmd commit];
    [cmd waitUntilCompleted];
    (void)plane_bytes;
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    IntegerSsimStateMetal *s = (IntegerSsimStateMetal *)fex->priv;

    const float *parts  = (const float *)s->rb.host_view;
    const float *partsw = (const float *)s->rbw.host_view;
    double ssim_sum  = 0.0;
    double ssimw_sum = 0.0;
    if (parts != NULL && partsw != NULL) {
        for (size_t i = 0; i < s->partials_count; ++i) {
            ssim_sum  += (double)parts[i];
            ssimw_sum += (double)partsw[i];
        }
    }
    double ssim = (ssimw_sum > 0.0) ? (ssim_sum / ssimw_sum) : 1.0;

    if (s->enable_db) {
        ssim = ssim_to_db(ssim, s->max_db);
    }

    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "ssim", ssim, index);
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    IntegerSsimStateMetal *s = (IntegerSsimStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_vert)     { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_vert;     s->pso_vert     = NULL; }
    if (s->pso_horiz_16) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_horiz_16; s->pso_horiz_16 = NULL; }
    if (s->pso_horiz_8)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_horiz_8;  s->pso_horiz_8  = NULL; }
    if (s->hbuf_buf)     { (void)(__bridge_transfer id<MTLBuffer>)s->hbuf_buf;                   s->hbuf_buf     = NULL; }

    int err = vmaf_metal_kernel_buffer_free(&s->rbw, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    err = vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = NULL; }
    return rc;
}

static const char *provided_features[] = {
    "ssim", NULL
};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_integer_ssim_metal = {
    .name              = "integer_ssim_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = NULL,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(IntegerSsimStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 2,
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
