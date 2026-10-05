/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  float_ssim feature extractor on the Metal backend (T8-1j / ADR-0421).
 *  The twin returns the CPU's scores bit for bit at scale 1 (ADR-1498; the
 *  design of the SYCL twin, ADR-1463, and of the CUDA twin, ADR-1464).
 *
 *  Three dispatches (float_ssim.metal):
 *    pass 0 -> float_ssim_horiz        (5 fp32 moment planes, pair sums)
 *    pass 1 -> float_ssim_vert_terms   (the fp64 bit pattern of every
 *                                       window's lv * cv * sv, raster order)
 *              or float_ssim_vert_lcs  (enable_lcs: lv and cv bit patterns and
 *                                       the fp32 sv of every window)
 *
 *  The host fills the float planes with picture_copy(), the CPU's own
 *  normalisation, adds every term plane in index order into one double
 *  (iqa_ssim()'s order), divides by the window count and rounds the mean to
 *  fp32 as iqa_ssim() does. The eleven-tap window is "valid" (no padding):
 *    w_h = W - 10, h_v = H - 10 windows.
 *
 *  Options (equal to float_ssim.c's table, `scale` aside):
 *    enable_lcs  - emit float_ssim_l, float_ssim_c, float_ssim_s sub-scores;
 *    enable_db   - SSIM as dB, -10*log10(1 - ssim);
 *    clip_db     - clip dB scores to the peak-derived ceiling;
 *    scale       - 0 (auto) or 1; the Metal twin runs scale 1 only (ADR-1324
 *                  gives model-selected contexts a CPU fallback above it).
 *
 *  Feature name: float_ssim.
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

#include "../picture_copy.h"
#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

#include "metal/metal_ssim_terms.h"

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

/* The window grid of the 16 x 8 threadgroups of every dispatch here. */
#define FSSIM_BLOCK_X 16u
#define FSSIM_BLOCK_Y 8u

using FloatSsimStateMetal = struct FloatSsimStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer terms;     /* u64 per window: lv * cv * sv, or lv under enable_lcs */
    VmafMetalContext *ctx;
    void *pso_horiz;                 /* float_ssim_horiz */
    void *pso_terms;                 /* float_ssim_vert_terms */
    void *pso_lcs;                   /* float_ssim_vert_lcs */
    void *hbuf_buf;                  /* intermediate 5-plane float buffer */
    void *ref_buf;                   /* the reference as float, W x H */
    void *dis_buf;                   /* the distorted frame as float, W x H */
    void *contrast_buf;              /* u64 per window: cv; NULL unless enable_lcs */
    void *structure_buf;             /* float per window: sv; NULL unless enable_lcs */

    /* Options (float_ssim.c's table). */
    bool    enable_lcs;
    bool    enable_db;
    bool    clip_db;
    int     scale;          /* 0 = auto-detect; v1: only 1 is supported. */

    VmafMtlSsimWindowParams window;  /* the vertical pass's arguments */
    double  max_db;         /* vmaf_ssim_max_db(): +inf unless clip_db */
    unsigned frame_w;
    unsigned frame_h;
    unsigned w_h;           /* W - 10 */
    unsigned h_v;           /* H - 10 */

    VmafDictionary *feature_name_dict;
};

/* ------------------------------------------------------------------ */
/* Options                                                              */
/* ------------------------------------------------------------------ */

static const VmafOption options[] = {
    {
        .name        = "enable_lcs",
        .help        = "emit luminance, contrast and structure sub-scores",
        .offset      = offsetof(FloatSsimStateMetal, enable_lcs),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name        = "enable_db",
        .help        = "output SSIM score in dB: -10*log10(1 - ssim)",
        .offset      = offsetof(FloatSsimStateMetal, enable_db),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name        = "clip_db",
        .help        = "clamp dB score to a finite maximum",
        .offset      = offsetof(FloatSsimStateMetal, clip_db),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name        = "scale",
        .help        = "decimation scale factor (0=auto, 1=no downscaling). "
                       "v1: direct Metal use requires scale=1; model dispatch falls back "
                       "to CPU when auto resolves above 1.",
        .offset      = offsetof(FloatSsimStateMetal, scale),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min         = 0,
        .max         = 10,
    },
    {.name=nullptr}
};

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static int ssim_metal_compute_scale(unsigned w, unsigned h, int override_val)
{
    if (override_val > 0) { return override_val; }
    int const scaled = (int)((float)(w < h ? w : h) / 256.0f + 0.5f);
    return (scaled < 1) ? 1 : scaled;
}

/* ADR-1324: dimensions are unavailable to the earlier option-value gate. */
static int check_context_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                               unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    (void)bpc;
    const auto *s = static_cast<const FloatSsimStateMetal *>(fex->priv);
    return ssim_metal_compute_scale(w, h, s->scale) == 1 ? 0 : -ENOTSUP;
}

static id<MTLBuffer> shared_buffer(id<MTLDevice> device, size_t bytes)
{
    return [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
}

static int build_pipelines(FloatSsimStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn_horiz = [lib newFunctionWithName:@"float_ssim_horiz"];
    id<MTLFunction> fn_terms = [lib newFunctionWithName:@"float_ssim_vert_terms"];
    id<MTLFunction> fn_lcs   = [lib newFunctionWithName:@"float_ssim_vert_lcs"];
    if (fn_horiz == nil || fn_terms == nil || fn_lcs == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso_h = [device newComputePipelineStateWithFunction:fn_horiz error:&err];
    id<MTLComputePipelineState> pso_t = [device newComputePipelineStateWithFunction:fn_terms error:&err];
    id<MTLComputePipelineState> pso_l = [device newComputePipelineStateWithFunction:fn_lcs   error:&err];
    if (pso_h == nil || pso_t == nil || pso_l == nil) { return -ENODEV; }

    s->pso_horiz = (__bridge_retained void *)pso_h;
    s->pso_terms = (__bridge_retained void *)pso_t;
    s->pso_lcs   = (__bridge_retained void *)pso_l;
    return 0;
}

/* The scale and the frame size the twin runs, the geometry and the dB
 * ceiling. */
static int configure(FloatSsimStateMetal *s, unsigned bpc, unsigned w, unsigned h)
{
    /* Validate scale: v1 supports scale=1 only. */
    const int scale = ssim_metal_compute_scale(w, h, s->scale);
    if (scale != 1) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_ssim_metal: v1 supports scale=1 only "
                 "(auto-detected scale=%d at %ux%u). "
                 "Pin --feature float_ssim_metal=scale=1 if intended.\n",
                 scale, w, h);
        return -EINVAL;
    }
    if (w < 11u || h < 11u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_ssim_metal: frame %ux%u smaller than 11x11 Gaussian footprint.\n",
                 w, h);
        return -EINVAL;
    }
    const VmafMtlSsimConstants c = vmaf_mtl_ssim_constants();
    s->frame_w = w;
    s->frame_h = h;
    s->w_h = w - 10u;
    s->h_v = h - 10u;
    s->max_db = vmaf_ssim_max_db(s->clip_db, bpc, w, h);
    s->window.horizontal_width = s->w_h;
    s->window.horizontal_height = h;
    s->window.final_width = s->w_h;
    s->window.final_height = s->h_v;
    s->window.offset = 0u;
    s->window.reserved = 0u;
    s->window.c1 = c.c1;
    s->window.c2 = c.c2;
    return 0;
}

/* The float planes, the moment planes, the term buffers and the pipelines. */
static int allocate(FloatSsimStateMetal *s)
{
    const size_t pixels = (size_t)s->frame_w * (size_t)s->frame_h;
    const size_t windows = (size_t)s->w_h * (size_t)s->h_v;
    int const err = vmaf_metal_kernel_buffer_alloc(&s->terms, s->ctx, windows * sizeof(uint64_t));
    if (err != 0) { return err; }

    void  const*dh = vmaf_metal_context_device_handle(s->ctx);
    if (dh == nullptr) { return -ENODEV; }
    id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

    id<MTLBuffer> hb = shared_buffer(device, 5u * (size_t)s->w_h * s->frame_h * sizeof(float));
    id<MTLBuffer> rb = shared_buffer(device, pixels * sizeof(float));
    id<MTLBuffer> db = shared_buffer(device, pixels * sizeof(float));
    if (hb == nil || rb == nil || db == nil) { return -ENOMEM; }
    s->hbuf_buf = (__bridge_retained void *)hb;
    s->ref_buf = (__bridge_retained void *)rb;
    s->dis_buf = (__bridge_retained void *)db;

    if (s->enable_lcs) {
        id<MTLBuffer> cb = shared_buffer(device, windows * sizeof(uint64_t));
        id<MTLBuffer> sb = shared_buffer(device, windows * sizeof(float));
        if (cb == nil || sb == nil) { return -ENOMEM; }
        s->contrast_buf = (__bridge_retained void *)cb;
        s->structure_buf = (__bridge_retained void *)sb;
    }
    return build_pipelines(s, device);
}

static int close_fex_metal(VmafFeatureExtractor *fex);

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatSsimStateMetal *s = (FloatSsimStateMetal *)fex->priv;

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

static void encode_horizontal(FloatSsimStateMetal *s, id<MTLCommandBuffer> cmd)
{
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso_horiz];
    [enc setBuffer:(__bridge id<MTLBuffer>)s->ref_buf offset:0 atIndex:0];
    [enc setBuffer:(__bridge id<MTLBuffer>)s->dis_buf offset:0 atIndex:1];
    [enc setBuffer:(__bridge id<MTLBuffer>)s->hbuf_buf offset:0 atIndex:2];
    uint32_t params[4] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h, (uint32_t)s->w_h, 0u};
    [enc setBytes:params length:sizeof(params) atIndex:3];
    MTLSize const tg   = MTLSizeMake(FSSIM_BLOCK_X, FSSIM_BLOCK_Y, 1);
    MTLSize const grid = MTLSizeMake((s->w_h + FSSIM_BLOCK_X - 1u) / FSSIM_BLOCK_X,
                               (s->frame_h + FSSIM_BLOCK_Y - 1u) / FSSIM_BLOCK_Y, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

static void encode_windows(FloatSsimStateMetal *s, id<MTLCommandBuffer> cmd)
{
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setBuffer:(__bridge id<MTLBuffer>)s->hbuf_buf offset:0 atIndex:0];
    [enc setBuffer:(__bridge id<MTLBuffer>)(void *)s->terms.buffer offset:0 atIndex:1];
    if (s->enable_lcs) {
        [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso_lcs];
        [enc setBuffer:(__bridge id<MTLBuffer>)s->contrast_buf offset:0 atIndex:2];
        [enc setBuffer:(__bridge id<MTLBuffer>)s->structure_buf offset:0 atIndex:3];
        [enc setBytes:&s->window length:sizeof(s->window) atIndex:4];
    } else {
        [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso_terms];
        [enc setBytes:&s->window length:sizeof(s->window) atIndex:2];
    }
    MTLSize const tg   = MTLSizeMake(FSSIM_BLOCK_X, FSSIM_BLOCK_Y, 1);
    MTLSize const grid = MTLSizeMake((s->w_h + FSSIM_BLOCK_X - 1u) / FSSIM_BLOCK_X,
                               (s->h_v + FSSIM_BLOCK_Y - 1u) / FSSIM_BLOCK_Y, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

/* picture_copy(): the CPU's normalisation of a plane to float. */
static void fill_float_plane(id<MTLBuffer> dst, VmafPicture *pic, unsigned width)
{
    picture_copy((float *)[dst contents], (ptrdiff_t)((size_t)width * sizeof(float)), pic, 0,
                 pic->bpc, 0);
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90; (void)dist_pic_90; (void)index;
    FloatSsimStateMetal *s = (FloatSsimStateMetal *)fex->priv;

    /* The buffers and the kernel arguments are the frame's of init(). */
    if (ref_pic->w[0] != s->frame_w || ref_pic->h[0] != s->frame_h ||
        dist_pic->w[0] != s->frame_w || dist_pic->h[0] != s->frame_h) {
        return -EINVAL;
    }
    void  const*qh = vmaf_metal_context_queue_handle(s->ctx);
    if (qh == nullptr) { return -ENODEV; }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;

    fill_float_plane((__bridge id<MTLBuffer>)s->ref_buf, ref_pic, s->frame_w);
    fill_float_plane((__bridge id<MTLBuffer>)s->dis_buf, dist_pic, s->frame_w);

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }
    encode_horizontal(s, cmd);
    encode_windows(s, cmd);
    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

/* iqa_ssim() returns every frame mean as fp32, `(float)(sum / (double)(w * h))`;
 * the twin rounds the same way, so a frame whose mean rounds to 1 scores
 * exactly 1 and enable_db reports the CPU's +inf / clip_db ceiling for it. */
static int frame_mean(const char *feature, double sum, double n_windows, unsigned index,
                      double *mean)
{
    const int err =
        vmaf_feature_finite_ratio_named("float_ssim_metal", feature, sum, n_windows, index, mean);
    if (err == 0) {
        *mean = (double)(float)*mean;
    }
    return err;
}

/* enable_lcs: the four sums become the frame means float_ssim and
 * float_ssim_{l,c,s}, published in CPU float_ssim.c order after the shared
 * SSIM validation. */
static int emit_lcs(const FloatSsimStateMetal *s, const VmafMtlSsimFrameSums *sums,
                    double n_windows, unsigned index, VmafFeatureCollector *feature_collector)
{
    static const char *const atom_names[3] = {"float_ssim_l", "float_ssim_c", "float_ssim_s"};
    const double atom_sums[3] = {sums->luminance, sums->contrast, sums->structure};
    VmafNamedScore atoms[3];
    double score = 0.0;
    int err = frame_mean("float_ssim", sums->ssim, n_windows, index, &score);
    for (unsigned k = 0u; k < 3u && err == 0; k++) {
        atoms[k].name = atom_names[k];
        err = frame_mean(atom_names[k], atom_sums[k], n_windows, index, &atoms[k].value);
    }
    if (err != 0) { return err; }
    return vmaf_ssim_emit_scores_named(feature_collector, s->feature_name_dict,
                                       "float_ssim_metal", "float_ssim", score, s->enable_db,
                                       s->max_db, atoms, 3u, index);
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    FloatSsimStateMetal  const*s = (FloatSsimStateMetal *)fex->priv;
    const uint64_t *terms = (const uint64_t *)s->terms.host_view;
    if (terms == nullptr) { return -EINVAL; }

    /* iqa_ssim()'s frame sums in its order, over the (W - 10) x (H - 10)
     * windows. */
    const size_t windows = (size_t)s->w_h * (size_t)s->h_v;
    const double n_windows = (double)s->w_h * (double)s->h_v;
    if (s->enable_lcs) {
        const uint64_t *contrast =
            (const uint64_t *)[(__bridge id<MTLBuffer>)s->contrast_buf contents];
        const float *structure =
            (const float *)[(__bridge id<MTLBuffer>)s->structure_buf contents];
        const VmafMtlSsimFrameSums sums =
            vmaf_mtl_ssim_frame_sums(terms, contrast, structure, windows);
        return emit_lcs(s, &sums, n_windows, index, feature_collector);
    }
    double score = 0.0;
    int const err = frame_mean("float_ssim", vmaf_mtl_ssim_product_sum(terms, windows), n_windows,
                         index, &score);
    if (err != 0) { return err; }
    return vmaf_ssim_emit_score_named(feature_collector, s->feature_name_dict,
                                      "float_ssim_metal", "float_ssim", score, s->enable_db,
                                      s->max_db, index);
}

static void release_object(void **handle)
{
    if (*handle != nullptr) {
        (void)(__bridge_transfer id)*handle;
        *handle = nullptr;
    }
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatSsimStateMetal *s = (FloatSsimStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    release_object(&s->pso_lcs);
    release_object(&s->pso_terms);
    release_object(&s->pso_horiz);
    release_object(&s->structure_buf);
    release_object(&s->contrast_buf);
    release_object(&s->dis_buf);
    release_object(&s->ref_buf);
    release_object(&s->hbuf_buf);

    const int err = vmaf_metal_kernel_buffer_free(&s->terms, s->ctx);
    if (err != 0 && rc == 0) { rc = err; }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = nullptr; }
    return rc;
}

static const char *provided_features[] = {
    "float_ssim", "float_ssim_l", "float_ssim_c", "float_ssim_s", nullptr
};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend, ADR-0589 ssim LCS dB parity; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0589 / ADR-0278
VmafFeatureExtractor vmaf_fex_float_ssim_metal = {
    .name              = "float_ssim_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = nullptr,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(FloatSsimStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 2,
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
    .context_check         = check_context_metal,
    .context_fallback_name = "float_ssim",
};
} /* extern "C" */
