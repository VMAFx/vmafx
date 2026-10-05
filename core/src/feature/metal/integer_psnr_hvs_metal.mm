/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  psnr_hvs feature extractor on the Metal backend — Metal twin of the
 *  CUDA reference core/src/feature/cuda/integer_psnr_hvs_cuda.c (+
 *  integer_psnr_hvs/psnr_hvs_score.cu) and the CPU reference
 *  core/src/feature/third_party/xiph/psnr_hvs.c (feature "psnr_hvs").
 *
 *  Per-plane single-dispatch design: one MTLComputeCommandEncoder per
 *  plane (Y, Cb, Cr), one threadgroup per output 8x8 image block
 *  (sliding window, step=7), 64 threads/threadgroup (8x8).
 *
 *  ADR-1397 / ADR-1401 / ADR-1498: the scores are the CPU extractor's bit
 *  for bit. The kernel stores the 64 terms calc_psnrhvs() adds per block
 *  (integer_psnr_hvs.metal, arithmetic in metal_psnr_hvs_math.h), and
 *  collect() hands each plane's terms to vmaf_psnr_hvs_plane_score(), which
 *  adds them into one float in the CPU's order; the combined score and the
 *  dB values come from vmaf_psnr_hvs_combined_score() and
 *  vmaf_psnr_hvs_score_db(), as in the CUDA, HIP and SYCL twins. The
 *  masking table is the CPU's double product stored as float, formed here
 *  with vmaf_psnr_hvs_mask_value() because MSL has no double. The readback
 *  is 256 bytes per block (about 65 MB for a 3840x2160 4:2:0 frame).
 *
 *  Provided features (mirroring CPU / CUDA / SYCL exactly):
 *    psnr_hvs_y, psnr_hvs_cb, psnr_hvs_cr, psnr_hvs.
 *  Per-plane dB:    psnr_hvs_<p> = 10 * -log10(plane_score).
 *  Combined score:  enable_chroma ? 0.8*Y + 0.1*(Cb+Cr) : Y, then
 *                   psnr_hvs = 10 * -log10(combined).
 *
 *  Option (matching the CPU psnr_hvs extractor):
 *    enable_chroma — default TRUE. Upstream Netflix unconditionally
 *    computes the YCbCr-weighted score; the fork-added option lets a
 *    caller fall back to luma-only. Defaulting true keeps parity with
 *    the CPU "psnr_hvs" extractor (and the SYCL twin, which also
 *    defaults true). YUV400P forces luma-only regardless.
 *
 *  bpc: the 8bpc kernel reads raw uchar; the 16bpc kernel reads raw
 *  ushort (no scaler division — the raw 9- to 12-bit values, as the CPU
 *  reads them). Rejects bpc > 12, as the CPU does, and planes smaller than
 *  the 8x8 block, as the CUDA, HIP and SYCL twins do (the CPU extractor
 *  scores such a plane as NaN).
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
#include "log.h"
#include "libvmaf/picture.h"

#include "feature/psnr_hvs_score.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

#include "metal/metal_psnr_hvs_math.h"

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

#define PSNR_HVS_NUM_PLANES 3
#define PSNR_HVS_BLOCK 8u
#define PSNR_HVS_STEP  7u

static_assert(VMAF_MTL_HVS_TERMS == VMAF_PSNR_HVS_TERMS_PER_BLOCK,
              "the kernel stores what vmaf_psnr_hvs_plane_score() sums per block");
static_assert(VMAF_MTL_HVS_PLANES == PSNR_HVS_NUM_PLANES, "one CSF table per plane");

using PsnrHvsStateMetal = struct PsnrHvsStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb[PSNR_HVS_NUM_PLANES]; /* 64 float terms per block, per plane */
    VmafMetalContext *ctx;
    void *pso_8bpc;
    void *pso_16bpc;
    void *csf_buf[PSNR_HVS_NUM_PLANES];            /* MTLBuffer holding the 64 CSF floats */
    void *mask_buf[PSNR_HVS_NUM_PLANES];           /* MTLBuffer: the CPU's 64 masking entries */

    /* Option (matching CPU psnr_hvs). */
    bool     enable_chroma;

    unsigned n_planes;           /* 1 (luma-only / YUV400P) or 3 */
    unsigned bpc;
    unsigned width[PSNR_HVS_NUM_PLANES];
    unsigned height[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_x[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_y[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks[PSNR_HVS_NUM_PLANES];

    VmafDictionary *feature_name_dict;
};

static const VmafOption options[] = {
    {
        .name        = "enable_chroma",
        .help        = "enable calculation for chroma channels",
        .offset      = offsetof(PsnrHvsStateMetal, enable_chroma),
        .type        = VMAF_OPT_TYPE_BOOL,
        /* Default true mirrors the CPU "psnr_hvs" extractor (and the
         * SYCL twin): preserves upstream-equivalent YCbCr-weighted
         * output for callers that don't set the option. */
        .default_val = {.b = true},
    },
    {.name=nullptr}
};

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static int build_pipelines(PsnrHvsStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn8  = [lib newFunctionWithName:@"integer_psnr_hvs_8bpc"];
    id<MTLFunction> fn16 = [lib newFunctionWithName:@"integer_psnr_hvs_16bpc"];
    if (fn8 == nil || fn16 == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso8  = [device newComputePipelineStateWithFunction:fn8  error:&err];
    id<MTLComputePipelineState> pso16 = [device newComputePipelineStateWithFunction:fn16 error:&err];
    if (pso8 == nil || pso16 == nil) { return -ENODEV; }

    s->pso_8bpc  = (__bridge_retained void *)pso8;
    s->pso_16bpc = (__bridge_retained void *)pso16;
    return 0;
}

static void free_csf_buffers(PsnrHvsStateMetal *s)
{
    for (int p = 0; p < PSNR_HVS_NUM_PLANES; ++p) {
        if (s->csf_buf[p]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->csf_buf[p];
            s->csf_buf[p] = nullptr;
        }
        if (s->mask_buf[p]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->mask_buf[p];
            s->mask_buf[p] = nullptr;
        }
    }
}

/* The plane's CSF table and calc_psnrhvs()'s masking table derived from it,
 * (csf * 0.3885746225901003)^2 taken in double and stored as float
 * (vmaf_psnr_hvs_mask_value()): the kernel has no double. */
static int upload_plane_tables(PsnrHvsStateMetal *s, id<MTLDevice> device, unsigned p)
{
    float mask[VMAF_MTL_HVS_TERMS];
    for (unsigned k = 0; k < VMAF_MTL_HVS_TERMS; ++k) {
        mask[k] = vmaf_psnr_hvs_mask_value(vmaf_mtl_hvs_csf[p][k]);
    }
    id<MTLBuffer> cb = [device newBufferWithBytes:vmaf_mtl_hvs_csf[p]
                                           length:VMAF_MTL_HVS_TERMS * sizeof(float)
                                          options:MTLResourceStorageModeShared];
    id<MTLBuffer> mb = [device newBufferWithBytes:mask
                                           length:VMAF_MTL_HVS_TERMS * sizeof(float)
                                          options:MTLResourceStorageModeShared];
    if (cb == nil || mb == nil) { return -ENOMEM; }
    s->csf_buf[p]  = (__bridge_retained void *)cb;
    s->mask_buf[p] = (__bridge_retained void *)mb;
    return 0;
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    PsnrHvsStateMetal *s = (PsnrHvsStateMetal *)fex->priv;

    if (bpc > 12u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "psnr_hvs_metal: invalid bitdepth (%u); bpc must be <= 12\n", bpc);
        return -EINVAL;
    }
    if (w < PSNR_HVS_BLOCK || h < PSNR_HVS_BLOCK) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "psnr_hvs_metal: input %ux%u smaller than 8x8 block\n", w, h);
        return -EINVAL;
    }

    s->bpc = bpc;

    s->width[0]  = w;
    s->height[0] = h;
    if (pix_fmt == VMAF_PIX_FMT_YUV400P) {
        s->n_planes = 1u;
        s->width[1] = s->width[2] = 0u;
        s->height[1] = s->height[2] = 0u;
    } else {
        switch (pix_fmt) {
        case VMAF_PIX_FMT_YUV420P:
            s->width[1]  = s->width[2]  = (w + 1u) >> 1;
            s->height[1] = s->height[2] = (h + 1u) >> 1;
            break;
        case VMAF_PIX_FMT_YUV422P:
            s->width[1]  = s->width[2]  = (w + 1u) >> 1;
            s->height[1] = s->height[2] = h;
            break;
        case VMAF_PIX_FMT_YUV444P:
            s->width[1]  = s->width[2]  = w;
            s->height[1] = s->height[2] = h;
            break;
        default:
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "psnr_hvs_metal: unsupported pix_fmt\n");
            return -EINVAL;
        }
        s->n_planes = PSNR_HVS_NUM_PLANES;
        if (!s->enable_chroma) {
            s->n_planes = 1u;
            s->width[1] = s->width[2] = 0u;
            s->height[1] = s->height[2] = 0u;
        }
    }

    for (unsigned p = 0; p < s->n_planes; ++p) {
        if (s->width[p] < PSNR_HVS_BLOCK || s->height[p] < PSNR_HVS_BLOCK) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "psnr_hvs_metal: plane %u dims %ux%u smaller than 8x8 block\n", p,
                     s->width[p], s->height[p]);
            return -EINVAL;
        }
        s->num_blocks_x[p] = (s->width[p] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1u;
        s->num_blocks_y[p] = (s->height[p] - PSNR_HVS_BLOCK) / PSNR_HVS_STEP + 1u;
        s->num_blocks[p]   = s->num_blocks_x[p] * s->num_blocks_y[p];
    }

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    for (unsigned p = 0; p < s->n_planes; ++p) {
        err = vmaf_metal_kernel_buffer_alloc(
            &s->rb[p], s->ctx,
            (size_t)s->num_blocks[p] * VMAF_PSNR_HVS_TERMS_PER_BLOCK * sizeof(float));
        if (err != 0) {
            for (unsigned q = 0; q < p; ++q) {
                (void)vmaf_metal_kernel_buffer_free(&s->rb[q], s->ctx);
            }
            goto fail_lc;
        }
    }

    {
        void  const*dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == nullptr) { err = -ENODEV; goto fail_rb; }
        id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

        for (unsigned p = 0; p < s->n_planes; ++p) {
            err = upload_plane_tables(s, device, p);
            if (err != 0) { goto fail_csf; }
        }

        err = build_pipelines(s, device);
    }
    if (err != 0) { goto fail_csf; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                      fex->options, s);
    if (s->feature_name_dict == nullptr) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = nullptr; }
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = nullptr; }
fail_csf:
    free_csf_buffers(s);
fail_rb:
    for (unsigned p = 0; p < s->n_planes; ++p) {
        (void)vmaf_metal_kernel_buffer_free(&s->rb[p], s->ctx);
    }
fail_lc:
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
fail_ctx:
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = nullptr;
    return err;
}

static int dispatch_plane(PsnrHvsStateMetal *s, id<MTLDevice> device, id<MTLCommandQueue> queue,
                          id<MTLComputePipelineState> pso, VmafPicture *ref_pic,
                          VmafPicture *dis_pic, unsigned p)
{
    const unsigned pw = s->width[p];
    const unsigned ph = s->height[p];
    const size_t px_bytes  = (s->bpc <= 8u) ? 1u : 2u;
    const size_t row_bytes = (size_t)pw * px_bytes;
    const size_t plane_bytes = row_bytes * ph;

    id<MTLBuffer> ref_buf = [device newBufferWithLength:plane_bytes
                                               options:MTLResourceStorageModeShared];
    id<MTLBuffer> dis_buf = [device newBufferWithLength:plane_bytes
                                               options:MTLResourceStorageModeShared];
    if (ref_buf == nil || dis_buf == nil) { return -ENOMEM; }
    {
        uint8_t *rd = (uint8_t *)[ref_buf contents];
        uint8_t *dd = (uint8_t *)[dis_buf contents];
        for (unsigned y = 0; y < ph; ++y) {
            memcpy(rd + y * row_bytes, (uint8_t *)ref_pic->data[p] + y * ref_pic->stride[p],
                   row_bytes);
            memcpy(dd + y * row_bytes, (uint8_t *)dis_pic->data[p] + y * dis_pic->stride[p],
                   row_bytes);
        }
    }

    id<MTLBuffer> term_buf = (__bridge id<MTLBuffer>)(void *)s->rb[p].buffer;
    id<MTLBuffer> csf_buf  = (__bridge id<MTLBuffer>)s->csf_buf[p];
    id<MTLBuffer> mask_buf = (__bridge id<MTLBuffer>)s->mask_buf[p];

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    /* No clear: the grid is exactly the plane's blocks, and every thread of
     * every block stores the term of its coefficient. */
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_buf offset:0 atIndex:0];
    [enc setBuffer:dis_buf offset:0 atIndex:1];
    [enc setBuffer:term_buf offset:0 atIndex:2];
    [enc setBuffer:csf_buf offset:0 atIndex:3];
    [enc setBuffer:mask_buf offset:0 atIndex:6];
    uint32_t dims[4] = {(uint32_t)pw, (uint32_t)ph,
                        (uint32_t)s->num_blocks_x[p], (uint32_t)s->num_blocks_y[p]};
    [enc setBytes:dims length:sizeof(dims) atIndex:4];
    uint32_t strides[2] = {(uint32_t)row_bytes, (uint32_t)row_bytes};
    [enc setBytes:strides length:sizeof(strides) atIndex:5];

    MTLSize const tg   = MTLSizeMake(PSNR_HVS_BLOCK, PSNR_HVS_BLOCK, 1);
    MTLSize const grid = MTLSizeMake(s->num_blocks_x[p], s->num_blocks_y[p], 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90; (void)dist_pic_90; (void)index;
    PsnrHvsStateMetal *s = (PsnrHvsStateMetal *)fex->priv;

    void  const*dh = vmaf_metal_context_device_handle(s->ctx);
    void  const*qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == nullptr || qh == nullptr) { return -ENODEV; }

    id<MTLDevice>       device = (__bridge id<MTLDevice>)dh;
    id<MTLCommandQueue>  queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLComputePipelineState> pso = (s->bpc <= 8u)
        ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
        : (__bridge id<MTLComputePipelineState>)s->pso_16bpc;

    for (unsigned p = 0; p < s->n_planes; ++p) {
        int const err = dispatch_plane(s, device, queue, pso, ref_pic, dist_pic, p);
        if (err != 0) { return err; }
    }
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    PsnrHvsStateMetal  const*s = (PsnrHvsStateMetal *)fex->priv;

    /* calc_psnrhvs()'s running float sum over the stored terms, in its order
     * (vmaf_psnr_hvs_plane_score(), ADR-1397). */
    double plane_score[PSNR_HVS_NUM_PLANES] = {0.0, 0.0, 0.0};
    for (unsigned p = 0; p < s->n_planes; ++p) {
        const float *plane_terms = (const float *)s->rb[p].host_view;
        if (plane_terms == nullptr) { return -EINVAL; }
        plane_score[p] = vmaf_psnr_hvs_plane_score(plane_terms, s->num_blocks[p], s->bpc);
    }

    int err = 0;
    static const char *const plane_features[PSNR_HVS_NUM_PLANES] = {"psnr_hvs_y", "psnr_hvs_cb",
                                                                    "psnr_hvs_cr"};
    for (unsigned p = 0; p < s->n_planes; ++p) {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       plane_features[p],
                                                       vmaf_psnr_hvs_score_db(plane_score[p]),
                                                       index);
    }
    /* Luma alone when chroma is disabled or the input is 4:0:0, else
     * 0.8 Y + 0.1 (Cb + Cr): the CPU's extract() expression. */
    const double combined = vmaf_psnr_hvs_combined_score(plane_score, s->n_planes);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "psnr_hvs", vmaf_psnr_hvs_score_db(combined),
                                                   index);
    return err;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    PsnrHvsStateMetal *s = (PsnrHvsStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = nullptr; }
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = nullptr; }
    free_csf_buffers(s);

    for (unsigned p = 0; p < s->n_planes; ++p) {
        int const err = vmaf_metal_kernel_buffer_free(&s->rb[p], s->ctx);
        if (err != 0 && rc == 0) { rc = err; }
    }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = nullptr; }
    return rc;
}

static const char *provided_features[] = {"psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr", "psnr_hvs",
                                          nullptr};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_integer_psnr_hvs_metal = {
    .name              = "integer_psnr_hvs_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = nullptr,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(PsnrHvsStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = PSNR_HVS_NUM_PLANES,
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
