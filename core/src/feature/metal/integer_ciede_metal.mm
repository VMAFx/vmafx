/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  ciede2000 feature extractor on the Metal backend (Metal parity
 *  sweep — ciede twin). Dispatches `integer_ciede_kernel_{8,16}bpc`
 *  from integer_ciede.metal.
 *
 *  CPU reference: core/src/feature/ciede.c (.name="ciede", feature
 *  "ciede2000"). Bit-exact GPU reference for the float per-pixel
 *  math: core/src/feature/cuda/integer_ciede/ciede_score.cu and its
 *  SYCL twin core/src/feature/sycl/integer_ciede_sycl.cpp (both
 *  measured places=4 on real hardware).
 *
 *  Pipeline per frame:
 *    1. Host nearest-neighbour upscale of U/V to luma resolution
 *       (mirrors ciede.c::scale_chroma_planes / the SYCL twin's
 *       upscale_plane) into six MTLBuffers (Y/U/V × ref/dis), all at
 *       luma resolution.
 *    2. One kernel dispatch: per-pixel YUV→Lab→ΔE2000, reduced to one
 *       float partial per threadgroup (no 64-bit MSL atomics).
 *    3. Host accumulates partials in double, divides by W·H, applies
 *       the CPU's `45 - 20·log10(mean_dE)` transform.
 *
 *  Score is reported under the feature key "ciede2000" — identical to
 *  the CPU / CUDA / SYCL extractors, so the cross-backend gate
 *  (ADR-0214) compares like keys.
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
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

typedef struct CiedeStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb;        /* float partials, grid_w × grid_h */
    VmafMetalContext *ctx;
    void *pso_8bpc;
    void *pso_16bpc;

    size_t plane_bytes;              /* one luma-res plane, in bytes */
    size_t partials_count;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    enum VmafPixelFormat pix_fmt;

    VmafDictionary *feature_name_dict;
} CiedeStateMetal;

static const VmafOption options[] = {{0}};

static int build_pipelines(CiedeStateMetal *s, id<MTLDevice> device)
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
    CiedeStateMetal *s = (CiedeStateMetal *)fex->priv;

    s->frame_w     = w;
    s->frame_h     = h;
    s->bpc         = bpc;
    s->pix_fmt     = pix_fmt;
    s->plane_bytes = (size_t)w * h * (bpc <= 8u ? 1u : 2u);

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

/* Nearest-neighbour upscale of plane `p` of `pic` to luma resolution
 * (out_w × out_h), into `dst`. Mirrors ciede.c::scale_chroma_planes
 * and the SYCL twin's upscale_plane<T>(): chroma rows are repeated
 * vertically when ss_ver, columns when ss_hor. */
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

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90; (void)dist_pic_90; (void)index;
    CiedeStateMetal *s = (CiedeStateMetal *)fex->priv;

    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    const unsigned w = s->frame_w;
    const unsigned h = s->frame_h;
    const size_t bpp = (s->bpc <= 8u ? 1u : 2u);
    const size_t plane_bytes = (size_t)w * h * bpp;
    const size_t row_bytes   = (size_t)w * bpp;

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }

    id<MTLDevice>       device = (__bridge id<MTLDevice>)dh;
    id<MTLCommandQueue>  queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLBuffer>      par_buf = (__bridge id<MTLBuffer>)(void *)s->rb.buffer;
    id<MTLComputePipelineState> pso = (s->bpc <= 8u)
        ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
        : (__bridge id<MTLComputePipelineState>)s->pso_16bpc;

    /* Six luma-res planes: Y/U/V for ref and dis. */
    id<MTLBuffer> bufs[6];
    for (int i = 0; i < 6; ++i) {
        bufs[i] = [device newBufferWithLength:plane_bytes options:MTLResourceStorageModeShared];
        if (bufs[i] == nil) { return -ENOMEM; }
    }

    if (s->bpc <= 8u) {
        upscale_plane<uint8_t>(0, ref_pic,  [bufs[0] contents], w, h, s->pix_fmt);
        upscale_plane<uint8_t>(1, ref_pic,  [bufs[1] contents], w, h, s->pix_fmt);
        upscale_plane<uint8_t>(2, ref_pic,  [bufs[2] contents], w, h, s->pix_fmt);
        upscale_plane<uint8_t>(0, dist_pic, [bufs[3] contents], w, h, s->pix_fmt);
        upscale_plane<uint8_t>(1, dist_pic, [bufs[4] contents], w, h, s->pix_fmt);
        upscale_plane<uint8_t>(2, dist_pic, [bufs[5] contents], w, h, s->pix_fmt);
    } else {
        upscale_plane<uint16_t>(0, ref_pic,  [bufs[0] contents], w, h, s->pix_fmt);
        upscale_plane<uint16_t>(1, ref_pic,  [bufs[1] contents], w, h, s->pix_fmt);
        upscale_plane<uint16_t>(2, ref_pic,  [bufs[2] contents], w, h, s->pix_fmt);
        upscale_plane<uint16_t>(0, dist_pic, [bufs[3] contents], w, h, s->pix_fmt);
        upscale_plane<uint16_t>(1, dist_pic, [bufs[4] contents], w, h, s->pix_fmt);
        upscale_plane<uint16_t>(2, dist_pic, [bufs[5] contents], w, h, s->pix_fmt);
    }

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit fillBuffer:par_buf range:NSMakeRange(0, s->partials_count * sizeof(float)) value:0];
    [blit endEncoding];

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    for (int i = 0; i < 6; ++i) {
        [enc setBuffer:bufs[i] offset:0 atIndex:i];
    }
    [enc setBuffer:par_buf offset:0 atIndex:6];
    uint32_t st[2] = {(uint32_t)row_bytes, (uint32_t)s->bpc};
    [enc setBytes:st length:sizeof(st) atIndex:7];
    uint32_t dim[2] = {(uint32_t)w, (uint32_t)h};
    [enc setBytes:dim length:sizeof(dim) atIndex:8];

    MTLSize tg   = MTLSizeMake(16, 16, 1);
    MTLSize grid = MTLSizeMake((w + 15) / 16, (h + 15) / 16, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    CiedeStateMetal *s = (CiedeStateMetal *)fex->priv;

    const float *parts = (const float *)s->rb.host_view;
    double total = 0.0;
    if (parts != NULL) {
        for (size_t i = 0; i < s->partials_count; ++i) {
            total += (double)parts[i];
        }
    }
    const double n_pix   = (double)s->frame_w * (double)s->frame_h;
    const double mean_de = (n_pix > 0.0) ? (total / n_pix) : 0.0;
    const double score   = 45.0 - 20.0 * log10(mean_de);

    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "ciede2000", score, index);
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    CiedeStateMetal *s = (CiedeStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = NULL; }
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = NULL; }

    int err = vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = NULL; }
    return rc;
}

static const char *provided_features[] = {"ciede2000", NULL};

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
    .flush               = NULL,
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
