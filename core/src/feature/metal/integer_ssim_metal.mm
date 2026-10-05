/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  integer_ssim feature extractor on the Metal backend (feature name `ssim`).
 *  The twin returns the CPU's score bit for bit (ADR-1498; the design of the
 *  SYCL twin, ADR-1443, and of the CUDA and HIP twins, ADR-1424, ADR-1438).
 *
 *  Two passes (integer_ssim.metal):
 *    pass 0 -> integer_ssim_horiz_{8,16}bpc  (5 int64 moment planes)
 *    pass 1 -> integer_ssim_vert_terms       (the fp64 bit pattern of every
 *                                             pixel's term, raster order)
 *
 *  integer_ssim.c::calc_ssim() adds every pixel's fp64 term into one double,
 *  row after row, and divides by the sum of the window weights. The kernel
 *  forms that term without fp64 (metal_integer_ssim_math.h, the reference's
 *  operations on values in 64-bit integers) and stores it unreduced; the
 *  host adds the read-back plane in index order (issim_frame_sum()). The
 *  weight of a window is the product of its two tap sums, so the frame's
 *  weight sum is the product of the two line sums (issim_line_weight()).
 *
 *  Options (equal to integer_ssim.c's table):
 *    enable_db   — write SSIM as dB, -10 * log10(1 - ssim);
 *    clip_db     — clip dB scores to the peak-derived ceiling.
 *  Both go through the CPU's helpers (vmaf_ssim_max_db(),
 *  vmaf_ssim_emit_ratio_score_named(), nonfinite_score.h).
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
#include "dict.h"
#include "feature_collector.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "log.h"
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

#include "../../metal/objc_handle.h"

#include "metal/metal_integer_ssim_math.h"


/* integer_ssim.c's SSIM_K1 and SSIM_K2. */
#define ISSIM_K1 (0.01 * 0.01)
#define ISSIM_K2 (0.03 * 0.03)

/* The moment planes of pass 0: mux, muy, x2, xy, y2. */
#define ISSIM_MOMENT_PLANES 5u

using IntegerSsimStateMetal = struct IntegerSsimStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer terms;     /* fp64 bit pattern per pixel, raster order */
    VmafMetalContext *ctx;
    void *pso_horiz_8;               /* integer_ssim_horiz_8bpc  (pass 0) */
    void *pso_horiz_16;              /* integer_ssim_horiz_16bpc (pass 0) */
    void *pso_vert;                  /* integer_ssim_vert_terms  (pass 1) */
    void *hbuf_buf;                  /* intermediate 5-plane int64 buffer */

    /* Options (integer_ssim.c's table). */
    bool    enable_db;
    bool    clip_db;

    double  max_db;          /* vmaf_ssim_max_db(): +inf unless clip_db */
    /* calc_ssim()'s `ssimw`: the sum of every window's weight. */
    int64_t total_weight;
    /* Kernel arguments: the frame and the fp64 bit patterns of
     * fl64(sm * sm * SSIM_K1) and fl64(sm * sm * SSIM_K2). */
    VmafMtlIssimParams params;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;

    VmafDictionary *feature_name_dict;
};

/* ------------------------------------------------------------------ */
/* Options                                                              */
/* ------------------------------------------------------------------ */

static const VmafOption options[] = {
    {
        .name        = "enable_db",
        .help        = "write SSIM values as dB: -10*log10(1-ssim)",
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
    {.name=nullptr}
};

/* ------------------------------------------------------------------ */
/* Host arithmetic                                                      */
/* ------------------------------------------------------------------ */

/* The fp64 bit pattern of integer_ssim.c's `sm * sm * k`, the factor of c1
 * (k = SSIM_K1) or c2 (k = SSIM_K2) that does not depend on the window. */
static uint64_t issim_stabiliser_bits(unsigned bpc, double k)
{
    const int samplemax = (1 << bpc) - 1;
    const double sm = (double)samplemax;
    const double value = sm * sm * k;
    uint64_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* The sum of the window weights along a line of `extent` samples. */
static int64_t issim_line_weight(unsigned extent)
{
    int64_t weight = 0;
    for (unsigned position = 0u; position < extent; position++) {
        weight += vmaf_mtl_issim_tap_weight(vmaf_mtl_issim_tap_range(position, extent));
    }
    return weight;
}

/* calc_ssim()'s `ssim` accumulator: the terms of the plane added into one
 * double in index order, which is the reference's raster order. */
static double issim_frame_sum(const uint64_t *terms, size_t count)
{
    double sum = 0.0;
    for (size_t i = 0u; i < count; i++) {
        double term = 0.0;
        memcpy(&term, &terms[i], sizeof(term));
        sum += term;
    }
    return sum;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

static int build_pipelines(IntegerSsimStateMetal *s, id<MTLDevice> device)
{
    int load_rc = 0;
    id<MTLLibrary> lib = vmaf_metal_library_load(device, &load_rc);
    if (lib == nil) { return load_rc; }
    NSError *err = nil;

    id<MTLFunction> fn_h8  = [lib newFunctionWithName:@"integer_ssim_horiz_8bpc"];
    id<MTLFunction> fn_h16 = [lib newFunctionWithName:@"integer_ssim_horiz_16bpc"];
    id<MTLFunction> fn_vert = [lib newFunctionWithName:@"integer_ssim_vert_terms"];
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

/* The frame, the weight sum, the stabilisers and the dB ceiling. */
static int configure(IntegerSsimStateMetal *s, unsigned bpc, unsigned w, unsigned h)
{
    if (w < 1u || h < 1u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "integer_ssim_metal: invalid frame size %ux%u.\n", w, h);
        return -EINVAL;
    }
    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->max_db = vmaf_ssim_max_db(s->clip_db, bpc, w, h);
    s->total_weight = issim_line_weight(w) * issim_line_weight(h);
    s->params.width = w;
    s->params.height = h;
    s->params.k1_bits = issim_stabiliser_bits(bpc, ISSIM_K1);
    s->params.k2_bits = issim_stabiliser_bits(bpc, ISSIM_K2);
    return 0;
}

/* The term plane, the moment planes and the pipelines. */
static int allocate(IntegerSsimStateMetal *s)
{
    const size_t pixels = (size_t)s->frame_w * (size_t)s->frame_h;
    int const err = vmaf_metal_kernel_buffer_alloc(&s->terms, s->ctx, pixels * sizeof(uint64_t));
    if (err != 0) { return err; }

    void *const dh = vmaf_metal_context_device_handle(s->ctx);
    if (dh == nullptr) { return -ENODEV; }
    id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

    id<MTLBuffer> hb = [device newBufferWithLength:ISSIM_MOMENT_PLANES * pixels * sizeof(int64_t)
                                           options:MTLResourceStorageModeShared];
    if (hb == nil) { return -ENOMEM; }
    s->hbuf_buf = (__bridge_retained void *)hb;
    return build_pipelines(s, device);
}

static int close_fex_metal(VmafFeatureExtractor *fex);

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    IntegerSsimStateMetal *s = (IntegerSsimStateMetal *)fex->priv;

    int err = configure(s, bpc, w, h);
    if (err != 0) { return err; }

    err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }
    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err == 0) { err = allocate(s); }
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                          fex->options, s);
        if (s->feature_name_dict == nullptr) { err = -ENOMEM; }
    }
    if (err != 0) {
        /* close_fex_metal releases what was made; it tolerates the rest. */
        (void)close_fex_metal(fex);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Per frame                                                            */
/* ------------------------------------------------------------------ */

/* One packed copy of the luma plane of `pic`, `row_bytes` per row. */
static id<MTLBuffer> upload_luma(id<MTLDevice> device, const VmafPicture *pic,
                                 size_t row_bytes, unsigned height)
{
    id<MTLBuffer> buf = [device newBufferWithLength:row_bytes * height
                                            options:MTLResourceStorageModeShared];
    if (buf == nil) { return nil; }
    uint8_t *dst = (uint8_t *)[buf contents];
    const uint8_t *src = (const uint8_t *)pic->data[0];
    for (unsigned y = 0; y < height; ++y) {
        memcpy(dst + (size_t)y * row_bytes, src + (size_t)y * pic->stride[0], row_bytes);
    }
    return buf;
}

static void encode_horizontal(IntegerSsimStateMetal *s, id<MTLCommandBuffer> cmd,
                              id<MTLBuffer> ref_buf, id<MTLBuffer> dis_buf, size_t row_bytes)
{
    id<MTLComputePipelineState> pso_h = (s->bpc <= 8u)
        ? (__bridge id<MTLComputePipelineState>)s->pso_horiz_8
        : (__bridge id<MTLComputePipelineState>)s->pso_horiz_16;
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso_h];
    [enc setBuffer:ref_buf offset:0 atIndex:0];
    [enc setBuffer:dis_buf offset:0 atIndex:1];
    [enc setBuffer:(__bridge id<MTLBuffer>)s->hbuf_buf offset:0 atIndex:2];
    uint32_t params[4] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h,
                          (uint32_t)row_bytes, (uint32_t)row_bytes};
    [enc setBytes:params length:sizeof(params) atIndex:3];
    MTLSize const tg   = MTLSizeMake(16, 8, 1);
    MTLSize const grid = MTLSizeMake((s->frame_w + 15u) / 16u, (s->frame_h + 7u) / 8u, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

static void encode_terms(IntegerSsimStateMetal *s, id<MTLCommandBuffer> cmd)
{
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso_vert];
    [enc setBuffer:(__bridge id<MTLBuffer>)s->hbuf_buf offset:0 atIndex:0];
    [enc setBuffer:vmaf_metal::borrow<id<MTLBuffer>>(s->terms.buffer) offset:0 atIndex:1];
    [enc setBytes:&s->params length:sizeof(s->params) atIndex:2];
    MTLSize const tg   = MTLSizeMake(16, 8, 1);
    MTLSize const grid = MTLSizeMake((s->frame_w + 15u) / 16u, (s->frame_h + 7u) / 8u, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90; (void)dist_pic_90; (void)index;
    IntegerSsimStateMetal *s = (IntegerSsimStateMetal *)fex->priv;

    /* The buffers, the weight sum and the kernel arguments are the frame's
     * of init(). */
    if (ref_pic->w[0] != s->frame_w || ref_pic->h[0] != s->frame_h ||
        dist_pic->w[0] != s->frame_w || dist_pic->h[0] != s->frame_h) {
        return -EINVAL;
    }

    void *const dh = vmaf_metal_context_device_handle(s->ctx);
    void *const qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == nullptr || qh == nullptr) { return -ENODEV; }
    id<MTLDevice>       device = (__bridge id<MTLDevice>)dh;
    id<MTLCommandQueue>  queue = (__bridge id<MTLCommandQueue>)qh;

    /* Raw (un-normalised) integer samples, packed rows. */
    const size_t row_bytes = (size_t)s->frame_w * ((s->bpc <= 8u) ? 1u : 2u);
    id<MTLBuffer> ref_buf = upload_luma(device, ref_pic, row_bytes, s->frame_h);
    id<MTLBuffer> dis_buf = upload_luma(device, dist_pic, row_bytes, s->frame_h);
    if (ref_buf == nil || dis_buf == nil) { return -ENOMEM; }

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }
    encode_horizontal(s, cmd, ref_buf, dis_buf, row_bytes);
    encode_terms(s, cmd);
    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    IntegerSsimStateMetal *const s = (IntegerSsimStateMetal *)fex->priv;
    const uint64_t *terms = (const uint64_t *)s->terms.host_view;
    if (terms == nullptr) { return -EINVAL; }

    /* calc_ssim(): the frame sum in the reference's order, then ssim / ssimw. */
    const double total_ssim = issim_frame_sum(terms, (size_t)s->frame_w * s->frame_h);
    return vmaf_ssim_emit_ratio_score_named(feature_collector, s->feature_name_dict,
                                            "integer_ssim_metal", "ssim", total_ssim,
                                            (double)s->total_weight, s->enable_db, s->max_db,
                                            index);
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    IntegerSsimStateMetal *s = (IntegerSsimStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_vert)     { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_vert;     s->pso_vert     = nullptr; }
    if (s->pso_horiz_16) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_horiz_16; s->pso_horiz_16 = nullptr; }
    if (s->pso_horiz_8)  { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_horiz_8;  s->pso_horiz_8  = nullptr; }
    if (s->hbuf_buf)     { (void)(__bridge_transfer id<MTLBuffer>)s->hbuf_buf;                   s->hbuf_buf     = nullptr; }

    const int err = vmaf_metal_kernel_buffer_free(&s->terms, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = nullptr; }
    return rc;
}

static const char *provided_features[] = {
    "ssim", nullptr
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
    .flush             = nullptr,
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
