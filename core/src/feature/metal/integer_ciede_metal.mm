/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2019 Joshua Holmer
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND MIT
 *
 *  ciede2000 feature extractor on the Metal backend (ADR-0421; the CPU's
 *  arithmetic since ADR-1498). Dispatches `integer_ciede_kernel_{8,16}bpc`
 *  from integer_ciede.metal.
 *
 *  CPU reference: core/src/feature/ciede.c (.name="ciede", feature
 *  "ciede2000"). The kernel runs ciede.c's statements in fp32 pairs
 *  (feature/ciede_ff_math.h, the arithmetic of the SYCL and HIP twins,
 *  ADR-1436 and ADR-1448, on the Metal primitives of metal_ciede_math.h).
 *
 *  Pipeline per frame:
 *    1. Host nearest-neighbour upscale of U/V to luma resolution, as
 *       ciede.c::scale_chroma_planes() defines the pixel inputs, into six
 *       MTLBuffers (Y/U/V x ref/dis) at luma resolution.
 *    2. One kernel dispatch: per pixel YUV -> L*a*b* -> CIEDE2000, one float
 *       per pixel at its raster position, nothing reduced on the device.
 *    3. Host adds the plane with ciede_frame_sum(), one double in raster
 *       order as extract() does, and applies extract()'s score expression.
 *
 *  ciede.c's constants are fp64 expressions of the bit depth, and Metal has
 *  no fp64 type: init() evaluates make_constants() here, and every dispatch
 *  hands the kernel the result.
 *
 *  Score is reported under the feature key "ciede2000" -- identical to the
 *  CPU / CUDA / SYCL / HIP extractors, so the cross-backend gate (ADR-0214)
 *  compares like keys.
 */

#include <cerrno>
#include <cmath>
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
#include "ciede_frame_sum.h"
#include "dict.h"
#include "feature_collector.h"
#include "feature_name.h"
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

/* feature/ciede_ff_math.h on the host, for make_constants() (C++20). */
#include "metal_ciede_math.h"

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

using CiedeStateMetal = struct CiedeStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb;        /* one float per pixel, raster order */
    VmafMetalContext *ctx;
    void *pso_8bpc;
    void *pso_16bpc;

    vmaf_metal_ciede::Constants constants; /* ciede.c's, for the frame's bit depth */
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    enum VmafPixelFormat pix_fmt;

    VmafDictionary *feature_name_dict;
};

/* The CPU extractor has no options. */
static const VmafOption options[] = {{0}};

static int build_pipelines(CiedeStateMetal *s, id<MTLDevice> device)
{
    const size_t blob_size = (size_t)(libvmaf_metallib_end - libvmaf_metallib_start);
    if (blob_size == 0) { return -ENODEV; }

    dispatch_data_t const data = dispatch_data_create(
        libvmaf_metallib_start, blob_size,
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (data == nullptr) { return -ENOMEM; }

    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithData:data error:&err];
    if (lib == nil) { return -ENODEV; }

    id<MTLFunction> fn8  = [lib newFunctionWithName:@"integer_ciede_kernel_8bpc"];
    id<MTLFunction> fn16 = [lib newFunctionWithName:@"integer_ciede_kernel_16bpc"];
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
    if (pix_fmt == VMAF_PIX_FMT_YUV400P) { return -EINVAL; }
    /* make_constants() takes the depths a picture can have. */
    if (bpc < 8u || bpc > 16u) { return -EINVAL; }
    CiedeStateMetal *s = (CiedeStateMetal *)fex->priv;

    s->frame_w     = w;
    s->frame_h     = h;
    s->bpc         = bpc;
    s->pix_fmt     = pix_fmt;
    s->constants   = vmaf_metal_ciede::make_constants(bpc);

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    /* One float per pixel: the kernel's values, read back whole. */
    err = vmaf_metal_kernel_buffer_alloc(&s->rb, s->ctx, (size_t)w * h * sizeof(float));
    if (err != 0) { goto fail_lc; }

    {
        void *const dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == nullptr) { err = -ENODEV; goto fail_rb; }
        err = build_pipelines(s, (__bridge id<MTLDevice>)dh);
    }
    if (err != 0) { goto fail_rb; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                      fex->options, s);
    if (s->feature_name_dict == nullptr) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = nullptr; }
    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = nullptr; }
fail_rb:
    (void)vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
fail_lc:
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
fail_ctx:
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = nullptr;
    return err;
}

/* Nearest-neighbour upscale of plane `p` of `pic` to luma resolution
 * (out_w × out_h), into `dst`, as ciede.c::scale_chroma_planes() does:
 * column j reads chroma column j / 2 when ss_hor, and the chroma row advances
 * after every odd output row when ss_ver. */
template <typename T>
static void upscale_plane(unsigned p, const VmafPicture *pic, void *dst, unsigned out_w,
                          unsigned out_h, enum VmafPixelFormat pix_fmt)
{
    const int ss_hor = (p > 0u) && (pix_fmt != VMAF_PIX_FMT_YUV444P);
    const int ss_ver = (p > 0u) && (pix_fmt == VMAF_PIX_FMT_YUV420P);
    const T *in_buf = (const T *)pic->data[p];
    T *out_buf = (T *)dst;
    const ptrdiff_t in_stride_t = (ptrdiff_t)pic->stride[p] / (ptrdiff_t)sizeof(T);
    for (unsigned i = 0; i < out_h; i++) {
        for (unsigned j = 0; j < out_w; j++) {
            unsigned in_x = ss_hor ? (j >> 1) : j;
            out_buf[j] = in_buf[in_x];
        }
        unsigned in_row_step = ss_ver ? (i & 1u) : 1u;
        in_buf += in_row_step * in_stride_t;
        out_buf += out_w;
    }
}

/* The three planes of `pic` at luma resolution, packed, into `dst`. */
static void upscale_picture(const CiedeStateMetal *s, const VmafPicture *pic, void *const dst[3])
{
    for (unsigned p = 0; p < 3u; p++) {
        if (s->bpc <= 8u) {
            upscale_plane<uint8_t>(p, pic, dst[p], s->frame_w, s->frame_h, s->pix_fmt);
        } else {
            upscale_plane<uint16_t>(p, pic, dst[p], s->frame_w, s->frame_h, s->pix_fmt);
        }
    }
}

/* A grid of threadgroups the pipeline accepts that covers the frame; the
 * kernel has no threadgroup memory, so any shape works. */
static void ciede_dispatch_shape(id<MTLComputePipelineState> pso, unsigned w, unsigned h,
                                 MTLSize *tg, MTLSize *grid)
{
    const NSUInteger tw = pso.threadExecutionWidth > 0 ? pso.threadExecutionWidth : 1;
    const NSUInteger max_th = pso.maxTotalThreadsPerThreadgroup / tw;
    const NSUInteger th = max_th > 0 ? max_th : 1;
    *tg   = MTLSizeMake(tw, th, 1);
    *grid = MTLSizeMake((w + tw - 1) / tw, (h + th - 1) / th, 1);
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90; (void)dist_pic_90; (void)index;
    CiedeStateMetal *s = (CiedeStateMetal *)fex->priv;
    /* The readback holds init()'s frame size. */
    if (ref_pic->w[0] != s->frame_w || ref_pic->h[0] != s->frame_h) { return -EINVAL; }

    void *const dh = vmaf_metal_context_device_handle(s->ctx);
    void *const qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == nullptr || qh == nullptr) { return -ENODEV; }
    id<MTLDevice>       device = (__bridge id<MTLDevice>)dh;
    id<MTLCommandQueue>  queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLBuffer>    terms_buf = (__bridge id<MTLBuffer>)(void *)s->rb.buffer;
    id<MTLComputePipelineState> pso = (s->bpc <= 8u)
        ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
        : (__bridge id<MTLComputePipelineState>)s->pso_16bpc;

    /* Six luma-res planes: Y/U/V for ref and dis. */
    const size_t plane_bytes = (size_t)s->frame_w * s->frame_h * (s->bpc <= 8u ? 1u : 2u);
    id<MTLBuffer> bufs[6];
    void *planes[6];
    for (int i = 0; i < 6; ++i) {
        bufs[i] = [device newBufferWithLength:plane_bytes options:MTLResourceStorageModeShared];
        if (bufs[i] == nil) { return -ENOMEM; }
        planes[i] = [bufs[i] contents];
    }
    upscale_picture(s, ref_pic, planes);
    upscale_picture(s, dist_pic, planes + 3);

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    if (enc == nil) { return -ENOMEM; }
    [enc setComputePipelineState:pso];
    for (int i = 0; i < 6; ++i) {
        [enc setBuffer:bufs[i] offset:0 atIndex:i];
    }
    [enc setBuffer:terms_buf offset:0 atIndex:6];
    const uint32_t dim[2] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h};
    [enc setBytes:dim length:sizeof(dim) atIndex:7];
    [enc setBytes:&s->constants length:sizeof(s->constants) atIndex:8];

    MTLSize tg;
    MTLSize grid;
    ciede_dispatch_shape(pso, s->frame_w, s->frame_h, &tg, &grid);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];
    return [cmd status] == MTLCommandBufferStatusCompleted ? 0 : -EIO;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    CiedeStateMetal *const s = (CiedeStateMetal *)fex->priv;

    const float *terms = (const float *)s->rb.host_view;
    if (terms == nullptr) { return -EINVAL; }
    /* extract()'s sum and score, over the whole plane in raster order. */
    const double de00_sum = ciede_frame_sum(terms, (size_t)s->frame_w * s->frame_h);
    const double score = 45. - 20. * log10(de00_sum / (s->frame_w * s->frame_h));

    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "ciede2000", score, index);
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    CiedeStateMetal *s = (CiedeStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = nullptr; }
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = nullptr; }

    int const err = vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = nullptr; }
    return rc;
}

static const char *provided_features[] = {"ciede2000", nullptr};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_integer_ciede_metal = {
    .name                = "integer_ciede_metal",
    .init                = init_fex_metal,
    .submit              = submit_fex_metal,
    .collect             = collect_fex_metal,
    .flush               = nullptr,
    .close               = close_fex_metal,
    .options             = options,
    .priv_size           = sizeof(CiedeStateMetal),
    .provided_features   = provided_features,
    .flags               = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 1,
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
