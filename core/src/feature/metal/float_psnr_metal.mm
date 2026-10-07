/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_psnr feature extractor on the Metal backend (T8-1d / ADR-0421).
 *  Dispatches `float_psnr_kernel_{8,16}bpc` from float_psnr.metal.
 *
 *  Noise: the kernel stores one uint64 per threadgroup of 256 pixels of one
 *  row, the exact sum of the CPU's float squares in units of 1 / scaler^2
 *  (ADR-1498, the design of ADR-1455); float_psnr_noise() adds each row's
 *  segments exactly and the rows into a double in order, as float_psnr.c
 *  adds its rows (vmaf_float_psnr_row_noise(), ADR-1499), and divides as the
 *  CPU does.
 *
 *  Score: peak² / max(mse, 1e-10) via 10·log10. `psnr_max` is reported
 *  verbatim for a zero-noise pair (infinity sentinel) and, unless the
 *  `uncapped` option is set, also truncates every computed value above it
 *  (ADR-1193 / T-UPSTREAM-1109).
 *  Peak / psnr_max table matches float_psnr_vulkan.c::init().
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
#include "float_psnr_rows.h"
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

/* Pixels per threadgroup: one segment of one row; FPSNR_THREADS_PER_GROUP
 * in float_psnr.metal. */
#define FPSNR_SEGMENT 256u

typedef struct FloatPsnrStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb;        /* uint64 row-segment sums, per_row × h */
    VmafMetalContext *ctx;
    void *pso_8bpc;
    void *pso_16bpc;

    double peak;
    double psnr_max;
    /* `uncapped` option: mirrors CPU float_psnr.c. When true, psnr_max
     * keeps only its zero-noise infinity-sentinel role and stops
     * truncating genuinely computed values. Default false keeps every
     * shipped score unchanged. See ADR-1193 / T-UPSTREAM-1109. */
    bool uncapped;
    size_t plane_bytes;
    size_t partials_count;
    unsigned per_row; /* threadgroups (256-pixel segments) per row */
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;

    VmafDictionary *feature_name_dict;
} FloatPsnrStateMetal;

static const VmafOption options[] = {
    {
        .name = "uncapped",
        .help = "report the true PSNR instead of truncating at the psnr_max ceiling "
                "(a zero-noise pair still reports psnr_max)",
        .offset = offsetof(FloatPsnrStateMetal, uncapped),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {0}};

static int build_pipelines(FloatPsnrStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn8  = [lib newFunctionWithName:@"float_psnr_kernel_8bpc"];
    id<MTLFunction> fn16 = [lib newFunctionWithName:@"float_psnr_kernel_16bpc"];
    if (fn8 == nil || fn16 == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso8 =
        [device newComputePipelineStateWithFunction:fn8 error:&err];
    if (pso8 == nil) { return -ENODEV; }
    id<MTLComputePipelineState> pso16 =
        [device newComputePipelineStateWithFunction:fn16 error:&err];
    if (pso16 == nil) { return -ENODEV; }

    s->pso_8bpc  = (__bridge_retained void *)pso8;
    s->pso_16bpc = (__bridge_retained void *)pso16;
    return 0;
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatPsnrStateMetal *s = (FloatPsnrStateMetal *)fex->priv;

    s->frame_w     = w;
    s->frame_h     = h;
    s->bpc         = bpc;
    s->plane_bytes = (size_t)w * h * (bpc <= 8u ? 1u : 2u);

    /* Peak / psnr_max table — matches float_psnr_vulkan.c::init. */
    if (bpc == 8)       { s->peak = 255.0;         s->psnr_max = 60.0; }
    else if (bpc == 10) { s->peak = 255.75;        s->psnr_max = 72.0; }
    else if (bpc == 12) { s->peak = 255.9375;      s->psnr_max = 84.0; }
    else if (bpc == 16) { s->peak = 255.99609375;  s->psnr_max = 108.0; }
    else                { return -EINVAL; } /* 9, 11, 13 to 15 bits: not yet run on a device (WP13-5) */

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    {
        s->per_row        = (w + FPSNR_SEGMENT - 1u) / FPSNR_SEGMENT;
        s->partials_count = (size_t)s->per_row * h;
        err = vmaf_metal_kernel_buffer_alloc(&s->rb, s->ctx,
                                             s->partials_count * sizeof(uint64_t));
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
    FloatPsnrStateMetal *s = (FloatPsnrStateMetal *)fex->priv;

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

    /* Build ref/dis host-side staging and copy into MTLBuffers. */
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
    [blit fillBuffer:par_buf range:NSMakeRange(0, s->partials_count * sizeof(uint64_t)) value:0];
    [blit endEncoding];

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_buf offset:0 atIndex:0];
    [enc setBuffer:dis_buf offset:0 atIndex:1];
    [enc setBuffer:par_buf offset:0 atIndex:2];
    uint32_t st[2] = {(uint32_t)row_bytes, (uint32_t)row_bytes};
    [enc setBytes:st length:sizeof(st) atIndex:3];
    uint32_t dim[2] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h};
    [enc setBytes:dim length:sizeof(dim) atIndex:4];

    /* One threadgroup per 256-pixel segment of one row (ADR-1499). */
    MTLSize tg   = MTLSizeMake(FPSNR_SEGMENT, 1, 1);
    MTLSize grid = MTLSizeMake(s->per_row, s->frame_h, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

/* float_psnr_noise - the frame's mean squared difference, as float_psnr.c's.
 *
 * The group sums are exact integers of the CPU's own terms in units of
 * 1 / scaler^2, each group a segment of one row (ADR-1455). The CPU's rows
 * are those rows' exact sums, added row after row into a double, which
 * vmaf_float_psnr_row_noise() repeats, past 2^53 units included (ADR-1499).
 * Dividing by scaler^2, a power of two, and by the pixel count are the CPU's
 * operations.
 */
static double float_psnr_noise(const FloatPsnrStateMetal *s)
{
    const uint64_t *partials = (const uint64_t *)s->rb.host_view;
    if (partials == NULL) {
        return 0.0;
    }
    const double total = vmaf_float_psnr_row_noise(partials, s->frame_h, s->per_row);
    const double scaler = (double)(1u << (s->bpc - 8u));
    const double n_pix = (double)s->frame_w * (double)s->frame_h;
    return (total / (scaler * scaler)) / n_pix;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    FloatPsnrStateMetal *s = (FloatPsnrStateMetal *)fex->priv;

    const double mse = float_psnr_noise(s);
    /* Match CPU float_psnr.c — a zero-noise pair reports psnr_max as the
     * infinity sentinel; the truncation applies only when `uncapped` is
     * false. See ADR-1193 / T-UPSTREAM-1109. */
    const double noise = (mse < 1e-10) ? 1e-10 : mse;
    double score;
    if (!s->uncapped) {
        /* Pre-ADR-1193 expression verbatim — bit-identical default. */
        score = 10.0 * log10((s->peak * s->peak) / noise);
        if (score > s->psnr_max) { score = s->psnr_max; }
    } else if (mse <= 0.0) {
        score = s->psnr_max; /* infinity sentinel */
    } else {
        score = 10.0 * log10((s->peak * s->peak) / noise);
    }

    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "float_psnr", score, index);
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatPsnrStateMetal *s = (FloatPsnrStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = NULL; }
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = NULL; }

    int err = vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = NULL; }
    return rc;
}

static const char *provided_features[] = {"float_psnr", NULL};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_float_psnr_metal = {
    .name                = "float_psnr_metal",
    .init                = init_fex_metal,
    .submit              = submit_fex_metal,
    .collect             = collect_fex_metal,
    .flush               = NULL,
    .close               = close_fex_metal,
    .options             = options,
    .priv_size           = sizeof(FloatPsnrStateMetal),
    .provided_features   = provided_features,
    .flags               = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 1,
        .is_reduction_only      = true,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
