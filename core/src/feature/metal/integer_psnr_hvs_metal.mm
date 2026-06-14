/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  psnr_hvs feature extractor on the Metal backend — Metal twin of the
 *  CUDA reference libvmaf/src/feature/cuda/integer_psnr_hvs_cuda.c (+
 *  integer_psnr_hvs/psnr_hvs_score.cu) and the CPU reference
 *  libvmaf/src/feature/third_party/xiph/psnr_hvs.c (feature "psnr_hvs").
 *
 *  Per-plane single-dispatch design: one MTLComputeCommandEncoder per
 *  plane (Y, Cb, Cr), one threadgroup per output 8x8 image block
 *  (sliding window, step=7), 64 threads/threadgroup (8x8). Each block
 *  writes a single float partial; the host sums the partials in a float
 *  register (matching the CPU's `ret`) then divides by pixels and
 *  samplemax^2 to obtain the per-plane raw score, exactly as the CUDA /
 *  SYCL twins do in their collect() steps.
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
 *  ushort (no scaler division — full 10/12-bit values, matching CPU).
 *  Rejects bpc > 12 and planes smaller than the 8x8 block (matches CPU
 *  / CUDA).
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

#define PSNR_HVS_NUM_PLANES 3
#define PSNR_HVS_BLOCK 8u
#define PSNR_HVS_STEP  7u

/* Per-plane CSF tables — same constants as csf_y / csf_cb420 /
 * csf_cr420 in third_party/xiph/psnr_hvs.c (row-major, 64 entries). */
static const float CSF_TABLES[PSNR_HVS_NUM_PLANES][64] = {
    /* Y (csf_y) */
    {1.6193873005f,   2.2901594831f,   2.08509755623f,  1.48366094411f,  1.00227514334f,
     0.678296995242f, 0.466224900598f, 0.3265091542f,   2.2901594831f,   1.94321815382f,
     2.04793073064f,  1.68731108984f,  1.2305666963f,   0.868920337363f, 0.61280991668f,
     0.436405793551f, 2.08509755623f,  2.04793073064f,  1.34329019223f,  1.09205635862f,
     0.875748795257f, 0.670882927016f, 0.501731932449f, 0.372504254596f, 1.48366094411f,
     1.68731108984f,  1.09205635862f,  0.772819797575f, 0.605636379554f, 0.48309405692f,
     0.380429446972f, 0.295774038565f, 1.00227514334f,  1.2305666963f,   0.875748795257f,
     0.605636379554f, 0.448996256676f, 0.352889268808f, 0.283006984131f, 0.226951348204f,
     0.678296995242f, 0.868920337363f, 0.670882927016f, 0.48309405692f,  0.352889268808f,
     0.27032073436f,  0.215017739696f, 0.17408067321f,  0.466224900598f, 0.61280991668f,
     0.501731932449f, 0.380429446972f, 0.283006984131f, 0.215017739696f, 0.168869545842f,
     0.136153931001f, 0.3265091542f,   0.436405793551f, 0.372504254596f, 0.295774038565f,
     0.226951348204f, 0.17408067321f,  0.136153931001f, 0.109083846276f},
    /* Cb (csf_cb420) */
    {1.91113096927f,  2.46074210438f,  1.18284184739f,  1.14982565193f,  1.05017074788f,
     0.898018824055f, 0.74725392039f,  0.615105596242f, 2.46074210438f,  1.58529308355f,
     1.21363250036f,  1.38190029285f,  1.33100189972f,  1.17428548929f,  0.996404342439f,
     0.830890433625f, 1.18284184739f,  1.21363250036f,  0.978712413627f, 1.02624506078f,
     1.03145147362f,  0.960060382087f, 0.849823426169f, 0.731221236837f, 1.14982565193f,
     1.38190029285f,  1.02624506078f,  0.861317501629f, 0.801821139099f, 0.751437590932f,
     0.685398513368f, 0.608694761374f, 1.05017074788f,  1.33100189972f,  1.03145147362f,
     0.801821139099f, 0.676555426187f, 0.605503172737f, 0.55002013668f,  0.495804539034f,
     0.898018824055f, 1.17428548929f,  0.960060382087f, 0.751437590932f, 0.605503172737f,
     0.514674450957f, 0.454353482512f, 0.407050308965f, 0.74725392039f,  0.996404342439f,
     0.849823426169f, 0.685398513368f, 0.55002013668f,  0.454353482512f, 0.389234902883f,
     0.342353999733f, 0.615105596242f, 0.830890433625f, 0.731221236837f, 0.608694761374f,
     0.495804539034f, 0.407050308965f, 0.342353999733f, 0.295530605237f},
    /* Cr (csf_cr420) */
    {2.03871978502f,  2.62502345193f,  1.26180942886f,  1.11019789803f,  1.01397751469f,
     0.867069376285f, 0.721500455585f, 0.593906509971f, 2.62502345193f,  1.69112867013f,
     1.17180569821f,  1.3342742857f,   1.28513006198f,  1.13381474809f,  0.962064122248f,
     0.802254508198f, 1.26180942886f,  1.17180569821f,  0.944981930573f, 0.990876405848f,
     0.995903384143f, 0.926972725286f, 0.820534991409f, 0.706020324706f, 1.11019789803f,
     1.3342742857f,   0.990876405848f, 0.831632933426f, 0.77418706195f,  0.725539939514f,
     0.661776842059f, 0.587716619023f, 1.01397751469f,  1.28513006198f,  0.995903384143f,
     0.77418706195f,  0.653238524286f, 0.584635025748f, 0.531064164893f, 0.478717061273f,
     0.867069376285f, 1.13381474809f,  0.926972725286f, 0.725539939514f, 0.584635025748f,
     0.496936637883f, 0.438694579826f, 0.393021669543f, 0.721500455585f, 0.962064122248f,
     0.820534991409f, 0.661776842059f, 0.531064164893f, 0.438694579826f, 0.375820256136f,
     0.330555063063f, 0.593906509971f, 0.802254508198f, 0.706020324706f, 0.587716619023f,
     0.478717061273f, 0.393021669543f, 0.330555063063f, 0.285345396658f}};

typedef struct PsnrHvsStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb[PSNR_HVS_NUM_PLANES]; /* float block partials per plane */
    VmafMetalContext *ctx;
    void *pso_8bpc;
    void *pso_16bpc;
    void *csf_buf[PSNR_HVS_NUM_PLANES];            /* MTLBuffer holding the 64 CSF floats */

    /* Option (matching CPU psnr_hvs). */
    bool     enable_chroma;

    int32_t  samplemax_sq;       /* ((1 << bpc) - 1)^2 */
    unsigned n_planes;           /* 1 (luma-only / YUV400P) or 3 */
    unsigned bpc;
    unsigned width[PSNR_HVS_NUM_PLANES];
    unsigned height[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_x[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks_y[PSNR_HVS_NUM_PLANES];
    unsigned num_blocks[PSNR_HVS_NUM_PLANES];

    VmafDictionary *feature_name_dict;
} PsnrHvsStateMetal;

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
    {0}
};

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static int build_pipelines(PsnrHvsStateMetal *s, id<MTLDevice> device)
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
            s->csf_buf[p] = NULL;
        }
    }
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
    const int32_t samplemax = (int32_t)((1u << bpc) - 1u);
    s->samplemax_sq = samplemax * samplemax;

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
        err = vmaf_metal_kernel_buffer_alloc(&s->rb[p], s->ctx,
                                             (size_t)s->num_blocks[p] * sizeof(float));
        if (err != 0) {
            for (unsigned q = 0; q < p; ++q) {
                (void)vmaf_metal_kernel_buffer_free(&s->rb[q], s->ctx);
            }
            goto fail_lc;
        }
    }

    {
        void *dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == NULL) { err = -ENODEV; goto fail_rb; }
        id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

        for (unsigned p = 0; p < s->n_planes; ++p) {
            id<MTLBuffer> cb = [device newBufferWithBytes:CSF_TABLES[p]
                                                   length:64 * sizeof(float)
                                                  options:MTLResourceStorageModeShared];
            if (cb == nil) { err = -ENOMEM; goto fail_csf; }
            s->csf_buf[p] = (__bridge_retained void *)cb;
        }

        err = build_pipelines(s, device);
    }
    if (err != 0) { goto fail_csf; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                      fex->options, s);
    if (s->feature_name_dict == NULL) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = NULL; }
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = NULL; }
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
    s->ctx = NULL;
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

    id<MTLBuffer> par_buf = (__bridge id<MTLBuffer>)(void *)s->rb[p].buffer;
    id<MTLBuffer> csf_buf = (__bridge id<MTLBuffer>)s->csf_buf[p];

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    /* Zero the partials buffer — out-of-grid blocks emit 0 anyway, but
     * this also covers any padding slots defensively. */
    {
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit fillBuffer:par_buf range:NSMakeRange(0, (size_t)s->num_blocks[p] * sizeof(float))
                   value:0];
        [blit endEncoding];
    }

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_buf offset:0 atIndex:0];
    [enc setBuffer:dis_buf offset:0 atIndex:1];
    [enc setBuffer:par_buf offset:0 atIndex:2];
    [enc setBuffer:csf_buf offset:0 atIndex:3];
    uint32_t dims[4] = {(uint32_t)pw, (uint32_t)ph,
                        (uint32_t)s->num_blocks_x[p], (uint32_t)s->num_blocks_y[p]};
    [enc setBytes:dims length:sizeof(dims) atIndex:4];
    uint32_t strides[2] = {(uint32_t)row_bytes, (uint32_t)row_bytes};
    [enc setBytes:strides length:sizeof(strides) atIndex:5];

    MTLSize tg   = MTLSizeMake(PSNR_HVS_BLOCK, PSNR_HVS_BLOCK, 1);
    MTLSize grid = MTLSizeMake(s->num_blocks_x[p], s->num_blocks_y[p], 1);
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

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }

    id<MTLDevice>       device = (__bridge id<MTLDevice>)dh;
    id<MTLCommandQueue>  queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLComputePipelineState> pso = (s->bpc <= 8u)
        ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
        : (__bridge id<MTLComputePipelineState>)s->pso_16bpc;

    for (unsigned p = 0; p < s->n_planes; ++p) {
        int err = dispatch_plane(s, device, queue, pso, ref_pic, dist_pic, p);
        if (err != 0) { return err; }
    }
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    PsnrHvsStateMetal *s = (PsnrHvsStateMetal *)fex->priv;

    /* Per-plane reduction matching the CPU float `ret` register
     * semantics: sum block partials in float, then divide by pixels and
     * samplemax^2 (same as the CUDA / SYCL collect()). */
    double plane_score[PSNR_HVS_NUM_PLANES] = {0.0, 0.0, 0.0};
    for (unsigned p = 0; p < s->n_planes; ++p) {
        const float *parts = (const float *)s->rb[p].host_view;
        float ret = 0.0f;
        if (parts != NULL) {
            for (unsigned i = 0; i < s->num_blocks[p]; ++i) {
                ret += parts[i];
            }
        }
        const int pixels = (int)(s->num_blocks[p] * 64u);
        ret /= (float)pixels;
        ret /= (float)s->samplemax_sq;
        plane_score[p] = (double)ret;
    }

    int err = 0;
    static const char *const plane_features[PSNR_HVS_NUM_PLANES] = {"psnr_hvs_y", "psnr_hvs_cb",
                                                                    "psnr_hvs_cr"};
    for (unsigned p = 0; p < s->n_planes; ++p) {
        const double db = 10.0 * (-1.0 * log10(plane_score[p]));
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       plane_features[p], db, index);
    }
    /* Combined: luma-only when chroma disabled / YUV400P, else the
     * standard 0.8*Y + 0.1*(Cb+Cr) weighted combination (matches CPU). */
    const double combined = (s->n_planes == 1u) ?
                                plane_score[0] :
                                0.8 * plane_score[0] + 0.1 * (plane_score[1] + plane_score[2]);
    const double db_combined = 10.0 * (-1.0 * log10(combined));
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "psnr_hvs", db_combined, index);
    return err;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    PsnrHvsStateMetal *s = (PsnrHvsStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_16bpc) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc; s->pso_16bpc = NULL; }
    if (s->pso_8bpc)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;  s->pso_8bpc  = NULL; }
    free_csf_buffers(s);

    for (unsigned p = 0; p < s->n_planes; ++p) {
        int err = vmaf_metal_kernel_buffer_free(&s->rb[p], s->ctx);
        if (err != 0 && rc == 0) { rc = err; }
    }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = NULL; }
    return rc;
}

static const char *provided_features[] = {"psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr", "psnr_hvs",
                                          NULL};

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
    .flush             = NULL,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(PsnrHvsStateMetal),
    .provided_features = provided_features,
    .flags             = 0,
    .chars = {
        .n_dispatches_per_frame = PSNR_HVS_NUM_PLANES,
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
