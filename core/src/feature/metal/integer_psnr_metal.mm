/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  integer_psnr feature extractor on the Metal backend (T8-1g / ADR-0421),
 *  twin of the CPU `psnr` (core/src/feature/integer_psnr.c).
 *
 *  The device reduces each plane's squared differences to one exact uint64
 *  per threadgroup (integer_psnr.metal); the host adds those in uint64, so the
 *  SSE is the CPU's integer. Every option then acts on it through
 *  core/src/feature/psnr_score.h, the helpers integer_psnr.c calls too, so the
 *  scores are the CPU's bit for bit (the design of psnr_sycl, psnr_cuda and
 *  psnr_hip: ADR-1365, ADR-1373, ADR-1382; the Metal port of ADR-1498):
 *    - `reduced_hbd_peak` -> vmaf_psnr_peak()
 *    - `min_sse`          -> vmaf_psnr_max() (per-plane ceiling)
 *    - `uncapped`         -> vmaf_psnr_from_mse() (ADR-1193)
 *    - `enable_mse`       -> `mse_{y,cb,cr}` after each `psnr_*`
 *    - `enable_apsnr`     -> per-plane SSE and sample totals across frames,
 *                            published by flush() as `apsnr_*` aggregates
 *                            (vmaf_psnr_aggregate()).
 *  TEMPORAL, as the CPU extractor: with --subsample N > 1 every frame still
 *  reaches the twin, so the apsnr totals cover the clip.
 *
 *  enable_chroma (default true): when false or pix_fmt == YUV400P, only the
 *  luma plane is dispatched (ADR-0453). Chroma planes take the CPU's ceiling
 *  subsampling per pixel format (Research-0094).
 */

#include <cerrno>
#include <cfloat>
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
#include "psnr_score.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

#define PSNR_NUM_PLANES 3U
#define PSNR_BLOCK 16U

using IntegerPsnrStateMetal = struct IntegerPsnrStateMetal {
    VmafMetalKernelLifecycle lc;
    /* One exact uint64 SSE per threadgroup, per active plane. */
    VmafMetalKernelBuffer rb[PSNR_NUM_PLANES];
    VmafMetalContext *ctx;
    void *pso_8bpc;
    void *pso_16bpc;

    /* Per-plane geometry (luma = [0], Cb = [1], Cr = [2]). */
    unsigned width[PSNR_NUM_PLANES];
    unsigned height[PSNR_NUM_PLANES];
    unsigned bpc;
    /* Number of active planes (1 for YUV400 or enable_chroma=false). */
    unsigned n_planes;
    /* vmaf_psnr_peak() of bpc and `reduced_hbd_peak`. */
    uint32_t peak;
    /* Per-plane vmaf_psnr_max(): (6 * bpc) + 12, or the `min_sse` ceiling. */
    double psnr_max[PSNR_NUM_PLANES];

    /* The CPU integer_psnr.c options. */
    bool enable_chroma;
    bool enable_mse;
    bool enable_apsnr;
    bool reduced_hbd_peak;
    double min_sse;
    bool uncapped;

    /* `enable_apsnr` totals across frames, published by flush(). */
    VmafPsnrClipSse apsnr_sse[PSNR_NUM_PLANES];
    uint64_t apsnr_n_pixels[PSNR_NUM_PLANES];

    VmafDictionary *feature_name_dict;
};

/* The CPU integer_psnr.c table: same names, defaults, ranges and flags. */
static const VmafOption options[] = {
    {
        .name = "enable_chroma",
        .help = "enable calculation for chroma channels",
        .offset = offsetof(IntegerPsnrStateMetal, enable_chroma),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = true,
    },
    {
        .name = "enable_mse",
        .help = "enable MSE calculation",
        .offset = offsetof(IntegerPsnrStateMetal, enable_mse),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "enable_apsnr",
        .help = "enable APSNR calculation",
        .offset = offsetof(IntegerPsnrStateMetal, enable_apsnr),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "reduced_hbd_peak",
        .help = "reduce hbd peak value to align with scaled 8-bit content",
        .offset = offsetof(IntegerPsnrStateMetal, reduced_hbd_peak),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "min_sse",
        .help = "constrain the minimum possible sse",
        .offset = offsetof(IntegerPsnrStateMetal, min_sse),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.0,
        .min = 0.0,
        .max = DBL_MAX,
    },
    {
        .name = "uncapped",
        .help = "report the true PSNR instead of truncating at the psnr_max ceiling "
                "(an all-zero SSE still reports psnr_max)",
        .offset = offsetof(IntegerPsnrStateMetal, uncapped),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {.name=nullptr}};

static const char *const psnr_name[PSNR_NUM_PLANES] = {"psnr_y", "psnr_cb", "psnr_cr"};
static const char *const mse_name[PSNR_NUM_PLANES] = {"mse_y", "mse_cb", "mse_cr"};
static const char *const apsnr_name[PSNR_NUM_PLANES] = {"apsnr_y", "apsnr_cb", "apsnr_cr"};

/* Threadgroups (and SSE slots) of plane p. */
static size_t psnr_metal_groups(const IntegerPsnrStateMetal *s, unsigned p)
{
    const size_t gx = (s->width[p] + PSNR_BLOCK - 1U) / PSNR_BLOCK;
    const size_t gy = (s->height[p] + PSNR_BLOCK - 1U) / PSNR_BLOCK;
    return gx * gy;
}

static int build_pipelines(IntegerPsnrStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn8  = [lib newFunctionWithName:@"integer_psnr_kernel_8bpc"];
    id<MTLFunction> fn16 = [lib newFunctionWithName:@"integer_psnr_kernel_16bpc"];
    if (fn8 == nil || fn16 == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso8 =
        [device newComputePipelineStateWithFunction:fn8 error:&err];
    id<MTLComputePipelineState> pso16 =
        [device newComputePipelineStateWithFunction:fn16 error:&err];
    if (pso8 == nil || pso16 == nil) { return -ENODEV; }

    s->pso_8bpc  = (__bridge_retained void *)pso8;
    s->pso_16bpc = (__bridge_retained void *)pso16;
    return 0;
}

static int psnr_metal_load_pipelines(IntegerPsnrStateMetal *s)
{
    void *const dh = vmaf_metal_context_device_handle(s->ctx);
    if (dh == nullptr) { return -ENODEV; }
    return build_pipelines(s, (__bridge id<MTLDevice>)dh);
}

/* Per-plane geometry, as CPU integer_psnr.c::init derives it: YUV400, and
 * enable_chroma=false on any other format, use luma only; chroma takes the
 * ceiling of the subsampled size (Research-0094). */
static void psnr_metal_init_geometry(IntegerPsnrStateMetal *s, enum VmafPixelFormat pix_fmt,
                                     unsigned w, unsigned h)
{
    s->width[0] = w;
    s->height[0] = h;
    s->n_planes = (pix_fmt == VMAF_PIX_FMT_YUV400P || !s->enable_chroma) ? 1U : PSNR_NUM_PLANES;
    const unsigned ss_hor = (pix_fmt != VMAF_PIX_FMT_YUV444P) ? 1U : 0U;
    const unsigned ss_ver = (pix_fmt == VMAF_PIX_FMT_YUV420P) ? 1U : 0U;
    for (unsigned p = 1U; p < PSNR_NUM_PLANES; p++) {
        s->width[p] = (s->n_planes > 1U) ? (w + ss_hor) >> ss_hor : 0U;
        s->height[p] = (s->n_planes > 1U) ? (h + ss_ver) >> ss_ver : 0U;
    }
}

/* Peak, per-plane psnr_max and empty APSNR totals, as CPU integer_psnr.c::init
 * derives them (psnr_score.h). Inactive planes keep the default ceiling: a
 * zero plane size would turn a min_sse ceiling into -inf, and nothing reads it. */
static void psnr_metal_init_scores(IntegerPsnrStateMetal *s, unsigned bpc)
{
    s->bpc = bpc;
    s->peak = vmaf_psnr_peak(bpc, s->reduced_hbd_peak);
    for (unsigned p = 0; p < PSNR_NUM_PLANES; p++) {
        const double min_sse = (p < s->n_planes) ? s->min_sse : 0.0;
        s->psnr_max[p] = vmaf_psnr_max(bpc, s->peak, min_sse, s->width[p], s->height[p]);
        s->apsnr_sse[p].lo = 0u;
        s->apsnr_sse[p].hi = 0u;
        s->apsnr_n_pixels[p] = 0U;
    }
}

/* Tear down everything init() may have set up; every step tolerates a handle
 * that was never created, so this serves a failed init() and close(). Returns
 * the first error but releases everything. */
static int psnr_metal_release(IntegerPsnrStateMetal *s)
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
    for (auto & p : s->rb) {
        const int e = vmaf_metal_kernel_buffer_free(&p, s->ctx);
        if (e != 0 && rc == 0) { rc = e; }
    }
    if (s->feature_name_dict != nullptr) {
        const int d = vmaf_dictionary_free(&s->feature_name_dict);
        if (d != 0 && rc == 0) { rc = d; }
    }
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = nullptr;
    return rc;
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    IntegerPsnrStateMetal *s = (IntegerPsnrStateMetal *)fex->priv;
    psnr_metal_init_geometry(s, pix_fmt, w, h);
    psnr_metal_init_scores(s, bpc);

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err == 0) {
        err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    }
    for (unsigned p = 0; p < s->n_planes && err == 0; p++) {
        err = vmaf_metal_kernel_buffer_alloc(&s->rb[p], s->ctx,
                                             psnr_metal_groups(s, p) * sizeof(uint64_t));
    }
    if (err == 0) {
        err = psnr_metal_load_pipelines(s);
    }
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == nullptr) { err = -ENOMEM; }
    }
    if (err != 0) {
        (void)psnr_metal_release(s);
    }
    return err;
}

/* Plane p of `pic`, packed (row pitch = width * bytes per sample), in a new
 * Shared buffer; nil when the allocation fails. */
static id<MTLBuffer> psnr_metal_upload_plane(const IntegerPsnrStateMetal *s, id<MTLDevice> device,
                                             const VmafPicture *pic, unsigned p)
{
    const size_t row_bytes = (size_t)s->width[p] * (s->bpc <= 8U ? 1U : 2U);
    id<MTLBuffer> buf = [device newBufferWithLength:row_bytes * s->height[p]
                                            options:MTLResourceStorageModeShared];
    if (buf == nil) { return nil; }
    uint8_t *dst = (uint8_t *)[buf contents];
    const uint8_t *src = (const uint8_t *)pic->data[p];
    for (unsigned y = 0; y < s->height[p]; y++) {
        memcpy(dst + (size_t)y * row_bytes, src + (size_t)y * (size_t)pic->stride[p], row_bytes);
    }
    return buf;
}

/* One plane's SSE kernel; every threadgroup writes its slot of rb[p]. */
static int dispatch_plane(IntegerPsnrStateMetal *s, id<MTLDevice> device,
                          id<MTLCommandQueue> queue, id<MTLComputePipelineState> pso,
                          const VmafPicture *ref_pic, const VmafPicture *dis_pic, unsigned p)
{
    id<MTLBuffer> ref_buf = psnr_metal_upload_plane(s, device, ref_pic, p);
    id<MTLBuffer> dis_buf = psnr_metal_upload_plane(s, device, dis_pic, p);
    if (ref_buf == nil || dis_buf == nil) { return -ENOMEM; }

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    if (enc == nil) { return -ENOMEM; }

    const uint32_t row_bytes = (uint32_t)(s->width[p] * (s->bpc <= 8U ? 1U : 2U));
    const uint32_t strides[2] = {row_bytes, row_bytes};
    const uint32_t dim[2] = {s->width[p], s->height[p]};
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_buf offset:0 atIndex:0];
    [enc setBuffer:dis_buf offset:0 atIndex:1];
    [enc setBuffer:(__bridge id<MTLBuffer>)(void *)s->rb[p].buffer offset:0 atIndex:2];
    [enc setBytes:strides length:sizeof(strides) atIndex:3];
    [enc setBytes:dim length:sizeof(dim) atIndex:4];

    const MTLSize tg = MTLSizeMake(PSNR_BLOCK, PSNR_BLOCK, 1);
    const MTLSize grid = MTLSizeMake((s->width[p] + PSNR_BLOCK - 1U) / PSNR_BLOCK,
                                     (s->height[p] + PSNR_BLOCK - 1U) / PSNR_BLOCK, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];
    return ([cmd status] == MTLCommandBufferStatusCompleted) ? 0 : -EIO;
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    IntegerPsnrStateMetal *s = (IntegerPsnrStateMetal *)fex->priv;

    void *const dh = vmaf_metal_context_device_handle(s->ctx);
    void *const qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == nullptr || qh == nullptr) { return -ENODEV; }

    id<MTLDevice> device = (__bridge id<MTLDevice>)dh;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLComputePipelineState> pso = (s->bpc <= 8U)
        ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
        : (__bridge id<MTLComputePipelineState>)s->pso_16bpc;

    for (unsigned p = 0; p < s->n_planes; ++p) {
        const int err = dispatch_plane(s, device, queue, pso, ref_pic, dist_pic, p);
        if (err != 0) { return err; }
    }
    return 0;
}

/* The plane's SSE: the threadgroup sums added in uint64, an exact integer
 * whatever the order, as the CPU's row sums are. */
static int psnr_metal_plane_sse(const IntegerPsnrStateMetal *s, unsigned p, uint64_t *sse)
{
    const uint64_t *parts = (const uint64_t *)s->rb[p].host_view;
    if (parts == nullptr) { return -EIO; }
    const size_t n = psnr_metal_groups(s, p);
    uint64_t sum = 0U;
    for (size_t i = 0; i < n; i++) {
        sum += parts[i];
    }
    *sse = sum;
    return 0;
}

/* Score one plane in CPU order (integer_psnr.c::psnr / psnr_hbd): `psnr_*`,
 * then `mse_*` when `enable_mse` is set. `enable_apsnr` folds the SSE into
 * the clip totals that flush_fex_metal() publishes. */
static int psnr_metal_emit_plane(IntegerPsnrStateMetal *s, unsigned p, unsigned index,
                                 VmafFeatureCollector *feature_collector)
{
    uint64_t sse = 0U;
    int err = psnr_metal_plane_sse(s, p, &sse);
    if (err != 0) { return err; }
    if (s->enable_apsnr) {
        vmaf_psnr_clip_sse_add(&s->apsnr_sse[p], sse);
        s->apsnr_n_pixels[p] += (uint64_t)s->height[p] * s->width[p];
    }
    const double mse = ((double)sse) / (s->width[p] * s->height[p]);
    const double psnr =
        vmaf_psnr_from_mse(mse, (double)s->peak * s->peak, s->psnr_max[p], s->uncapped);
    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  psnr_name[p], psnr, index);
    if (err == 0 && s->enable_mse) {
        err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      mse_name[p], mse, index);
    }
    return err;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    IntegerPsnrStateMetal *s = (IntegerPsnrStateMetal *)fex->priv;
    for (unsigned p = 0; p < s->n_planes; ++p) {
        const int err = psnr_metal_emit_plane(s, p, index, feature_collector);
        if (err != 0) { return err; }
    }
    return 0;
}

/* `enable_apsnr`: the clip-aggregate APSNR of every active plane, exactly as
 * CPU integer_psnr.c::flush publishes it. libvmaf collects the last pending
 * frame of a Metal extractor before it flushes (flush_context_serial()), so
 * the totals are complete. */
static int flush_fex_metal(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    const IntegerPsnrStateMetal *s = (const IntegerPsnrStateMetal *)fex->priv;
    int err = 0;
    if (s->enable_apsnr) {
        for (unsigned p = 0; p < s->n_planes; p++) {
            const double apsnr =
                vmaf_psnr_aggregate(s->peak, s->apsnr_sse[p], s->apsnr_n_pixels[p], s->psnr_max[p]);
            err |= vmaf_feature_collector_set_aggregate(feature_collector, apsnr_name[p], apsnr);
        }
    }
    return (err < 0) ? err : !err;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    return psnr_metal_release((IntegerPsnrStateMetal *)fex->priv);
}

static const char *provided_features[] = {"psnr_y", "psnr_cb", "psnr_cr", nullptr};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_integer_psnr_metal = {
    .name              = "integer_psnr_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = flush_fex_metal,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(IntegerPsnrStateMetal),
    .provided_features = provided_features,
    /* TEMPORAL like CPU integer_psnr.c: with --subsample N > 1 every frame
     * still reaches collect(), so the enable_apsnr totals cover the clip. */
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL | VMAF_FEATURE_EXTRACTOR_TEMPORAL,
    .chars = {
        .n_dispatches_per_frame = PSNR_NUM_PLANES,
        .is_reduction_only      = true,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
