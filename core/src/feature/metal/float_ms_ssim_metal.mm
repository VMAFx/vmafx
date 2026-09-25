/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  float_ms_ssim feature extractor on the Metal backend (T8-2b / ADR-0490 / ADR-1334).
 *  Port of `core/src/feature/float_ms_ssim.c` — same 5-scale pyramid,
 *  same Wang weights, same host accumulation logic, float-precision pixels.
 *
 *  Algorithm summary:
 *    1. Host normalises ref+cmp Y-plane -> float in [0, 255] (CPU loop).
 *    2. ms_ssim_decimate kernel builds pyramid levels 1-4 (x ref + cmp).
 *    3. Per scale: ms_ssim_horiz -> ms_ssim_vert_lcs, both in a single
 *       MTLCommandBuffer. DtoH is implicit (Shared storage on Apple
 *       unified memory — no explicit copy needed).
 *    4. Host reduces per-WG partials x 3 in double precision per scale,
 *       applies Wang weights for the final product combine.
 *
 *  Metallib resolution: same embedded-blob pattern as every other Metal
 *  feature extractor — reads the __TEXT,__metallib section compiled via
 *  xcrun from float_ms_ssim.metal.
 *
 *  Min-dim guard (ADR-0153): 11 x 2^4 = 176 x 176 enforced in init().
 *
 *  enable_lcs: when set, emits the 15 extra per-scale metrics
 *  float_ms_ssim_{l,c,s}_scale{0..4}. Default path output is
 *  bit-identical to the CPU float_ms_ssim extractor.
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
#include "feature/nonfinite_score.h"
#include "float_ms_ssim_option_semantics.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

#define MS_SSIM_MAX_PLANES    3
#define MS_SSIM_SCALES         5
#define MS_SSIM_GAUSSIAN_LEN  11
#define MS_SSIM_K             11
#define MS_SSIM_BLOCK_X       16
#define MS_SSIM_BLOCK_Y        8

static const float g_alphas[MS_SSIM_SCALES] = {0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.1333f};
static const float g_betas[MS_SSIM_SCALES]  = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};
static const float g_gammas[MS_SSIM_SCALES] = {0.0448f, 0.2856f, 0.3001f, 0.2363f, 0.1333f};

typedef struct MsSsimPlaneGeometryMetal {
    unsigned width;
    unsigned height;
    unsigned scale_w[MS_SSIM_SCALES];
    unsigned scale_h[MS_SSIM_SCALES];
    unsigned scale_w_h[MS_SSIM_SCALES];   /* w_h = scale_w[i] - 10 */
    unsigned scale_w_f[MS_SSIM_SCALES];   /* w_f = scale_w[i] - 10 */
    unsigned scale_h_f[MS_SSIM_SCALES];   /* h_f = scale_h[i] - 10 */
    unsigned scale_grid_w[MS_SSIM_SCALES];
    unsigned scale_grid_h[MS_SSIM_SCALES];
    unsigned scale_block_count[MS_SSIM_SCALES];
} MsSsimPlaneGeometryMetal;

typedef struct FloatMsSsimStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;

    /* Pipeline states for the three MS-SSIM kernels. */
    void *pso_decimate;
    void *pso_horiz;
    void *pso_vert_lcs;

    /* Pyramid buffers: 5 levels x ref + cmp per plane (float, Shared storage). */
    void *pyramid_ref[MS_SSIM_MAX_PLANES][MS_SSIM_SCALES];
    void *pyramid_cmp[MS_SSIM_MAX_PLANES][MS_SSIM_SCALES];

    /* Intermediate 5-plane horiz buffer (w_h x H x 5 floats);
     * sized for scale 0 of plane 0 (largest). */
    void *hbuf;

    /* Per-scale partials buffers per plane: l, c, s (float, Shared storage). */
    void *l_partials[MS_SSIM_MAX_PLANES][MS_SSIM_SCALES];
    void *c_partials[MS_SSIM_MAX_PLANES][MS_SSIM_SCALES];
    void *s_partials[MS_SSIM_MAX_PLANES][MS_SSIM_SCALES];

    unsigned width;
    unsigned height;
    unsigned bpc;
    float    scaler;  /* raw -> [0,255] multiplier inverse for >8bpc */

    /* Per-plane geometry. */
    MsSsimPlaneGeometryMetal geom[MS_SSIM_MAX_PLANES];

    float c1;
    float c2;
    float c3;

    bool enable_lcs;
    bool enable_db;
    bool clip_db;
    bool enable_chroma;
    unsigned n_planes;
    double max_db;

    unsigned index;
    VmafDictionary *feature_name_dict;
} FloatMsSsimStateMetal;

static const VmafOption options[] = {
    {
        .name        = "enable_lcs",
        .help        = "enable luminance, contrast and structure intermediate output",
        .offset      = offsetof(FloatMsSsimStateMetal, enable_lcs),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name        = "enable_db",
        .help        = "write scores in dB",
        .offset      = offsetof(FloatMsSsimStateMetal, enable_db),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name        = "clip_db",
        .help        = "clip dB scores",
        .offset      = offsetof(FloatMsSsimStateMetal, clip_db),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name        = "enable_chroma",
        .help        = "enable calculation for chroma channels",
        .offset      = offsetof(FloatMsSsimStateMetal, enable_chroma),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {0},
};

static int build_pipelines(FloatMsSsimStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn_dec  = [lib newFunctionWithName:@"ms_ssim_decimate"];
    id<MTLFunction> fn_hor  = [lib newFunctionWithName:@"ms_ssim_horiz"];
    id<MTLFunction> fn_vlcs = [lib newFunctionWithName:@"ms_ssim_vert_lcs"];
    if (fn_dec == nil || fn_hor == nil || fn_vlcs == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso_dec =
        [device newComputePipelineStateWithFunction:fn_dec  error:&err];
    id<MTLComputePipelineState> pso_hor =
        [device newComputePipelineStateWithFunction:fn_hor  error:&err];
    id<MTLComputePipelineState> pso_vlcs =
        [device newComputePipelineStateWithFunction:fn_vlcs error:&err];
    if (pso_dec == nil || pso_hor == nil || pso_vlcs == nil) { return -ENODEV; }

    s->pso_decimate = (__bridge_retained void *)pso_dec;
    s->pso_horiz    = (__bridge_retained void *)pso_hor;
    s->pso_vert_lcs = (__bridge_retained void *)pso_vlcs;
    return 0;
}

static void release_metal_psos(FloatMsSsimStateMetal *s)
{
    if (s->pso_vert_lcs) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_vert_lcs;
        s->pso_vert_lcs = NULL;
    }
    if (s->pso_horiz) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_horiz;
        s->pso_horiz = NULL;
    }
    if (s->pso_decimate) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_decimate;
        s->pso_decimate = NULL;
    }
}

static void release_metal_buffers(FloatMsSsimStateMetal *s)
{
    for (unsigned p = 0; p < MS_SSIM_MAX_PLANES; ++p) {
        for (int i = 0; i < MS_SSIM_SCALES; ++i) {
            if (s->s_partials[p][i]) {
                (void)(__bridge_transfer id<MTLBuffer>)s->s_partials[p][i];
                s->s_partials[p][i] = NULL;
            }
            if (s->c_partials[p][i]) {
                (void)(__bridge_transfer id<MTLBuffer>)s->c_partials[p][i];
                s->c_partials[p][i] = NULL;
            }
            if (s->l_partials[p][i]) {
                (void)(__bridge_transfer id<MTLBuffer>)s->l_partials[p][i];
                s->l_partials[p][i] = NULL;
            }
            if (s->pyramid_cmp[p][i]) {
                (void)(__bridge_transfer id<MTLBuffer>)s->pyramid_cmp[p][i];
                s->pyramid_cmp[p][i] = NULL;
            }
            if (s->pyramid_ref[p][i]) {
                (void)(__bridge_transfer id<MTLBuffer>)s->pyramid_ref[p][i];
                s->pyramid_ref[p][i] = NULL;
            }
        }
    }
    if (s->hbuf) {
        (void)(__bridge_transfer id<MTLBuffer>)s->hbuf;
        s->hbuf = NULL;
    }
}

static int alloc_metal_buffers(FloatMsSsimStateMetal *s, id<MTLDevice> device)
{
    for (unsigned p = 0; p < s->n_planes; ++p) {
        const MsSsimPlaneGeometryMetal *geom = &s->geom[p];
        for (int i = 0; i < MS_SSIM_SCALES; ++i) {
            const size_t bytes = (size_t)geom->scale_w[i] * geom->scale_h[i] * sizeof(float);
            id<MTLBuffer> br = [device newBufferWithLength:bytes
                                                   options:MTLResourceStorageModeShared];
            id<MTLBuffer> bc = [device newBufferWithLength:bytes
                                                   options:MTLResourceStorageModeShared];
            if (br == nil || bc == nil) { return -ENOMEM; }
            s->pyramid_ref[p][i] = (__bridge_retained void *)br;
            s->pyramid_cmp[p][i] = (__bridge_retained void *)bc;

            const size_t pb = (size_t)geom->scale_block_count[i] * sizeof(float);
            id<MTLBuffer> bl = [device newBufferWithLength:pb
                                                   options:MTLResourceStorageModeShared];
            id<MTLBuffer> bc2 = [device newBufferWithLength:pb
                                                    options:MTLResourceStorageModeShared];
            id<MTLBuffer> bs = [device newBufferWithLength:pb
                                                   options:MTLResourceStorageModeShared];
            if (bl == nil || bc2 == nil || bs == nil) { return -ENOMEM; }
            s->l_partials[p][i] = (__bridge_retained void *)bl;
            s->c_partials[p][i] = (__bridge_retained void *)bc2;
            s->s_partials[p][i] = (__bridge_retained void *)bs;
        }
    }

    const size_t hbuf_bytes =
        5u * (size_t)s->geom[0].scale_w_h[0] * s->geom[0].scale_h[0] * sizeof(float);
    id<MTLBuffer> hb = [device newBufferWithLength:hbuf_bytes
                                           options:MTLResourceStorageModeShared];
    if (hb == nil) { return -ENOMEM; }
    s->hbuf = (__bridge_retained void *)hb;
    return 0;
}

static int check_chroma_min_dim(const VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                                unsigned w, unsigned h, unsigned min_dim)
{
    unsigned chroma_w = 0u;
    unsigned chroma_h = 0u;
    vmaf_metal_ms_ssim_plane_dimensions(pix_fmt, 1u, w, h, &chroma_w, &chroma_h);

    if (chroma_w >= min_dim && chroma_h >= min_dim) {
        return 0;
    }

    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "%s: enable_chroma needs every plane to clear the pyramid minimum, "
             "but %ux%u luma gives %ux%u chroma and the %d-level %d-tap pyramid "
             "requires at least %ux%u. Use at least %ux%u luma for this pixel "
             "format, or leave enable_chroma off to score luma only.\n",
             fex->name, w, h, chroma_w, chroma_h, MS_SSIM_SCALES, MS_SSIM_GAUSSIAN_LEN,
             min_dim, min_dim, pix_fmt == VMAF_PIX_FMT_YUV444P ? min_dim : min_dim << 1u,
             pix_fmt == VMAF_PIX_FMT_YUV420P ? min_dim << 1u : min_dim);
    return -EINVAL;
}

static void init_plane_geometry(MsSsimPlaneGeometryMetal *geom)
{
    geom->scale_w[0] = geom->width;
    geom->scale_h[0] = geom->height;
    for (int i = 1; i < MS_SSIM_SCALES; ++i) {
        geom->scale_w[i] = (geom->scale_w[i - 1] / 2u) + (geom->scale_w[i - 1] & 1u);
        geom->scale_h[i] = (geom->scale_h[i - 1] / 2u) + (geom->scale_h[i - 1] & 1u);
    }
    for (int i = 0; i < MS_SSIM_SCALES; ++i) {
        geom->scale_w_h[i] = geom->scale_w[i] - (unsigned)(MS_SSIM_K - 1);
        geom->scale_w_f[i] = geom->scale_w_h[i];
        geom->scale_h_f[i] = geom->scale_h[i] - (unsigned)(MS_SSIM_K - 1);
        geom->scale_grid_w[i] =
            (geom->scale_w_f[i] + (unsigned)MS_SSIM_BLOCK_X - 1u) / (unsigned)MS_SSIM_BLOCK_X;
        geom->scale_grid_h[i] =
            (geom->scale_h_f[i] + (unsigned)MS_SSIM_BLOCK_Y - 1u) / (unsigned)MS_SSIM_BLOCK_Y;
        geom->scale_block_count[i] = geom->scale_grid_w[i] * geom->scale_grid_h[i];
    }
}

static int validate_dimensions(VmafFeatureExtractor *fex, FloatMsSsimStateMetal *s,
                               enum VmafPixelFormat pix_fmt, unsigned w, unsigned h)
{
    s->n_planes = vmaf_metal_ms_ssim_active_planes(s->enable_chroma, pix_fmt);

    /* ADR-0153 minimum resolution guard. */
    const unsigned min_dim = (unsigned)MS_SSIM_GAUSSIAN_LEN << (MS_SSIM_SCALES - 1u);
    if (w < min_dim || h < min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "%s: input resolution %ux%u is too small; the %d-level "
                 "%d-tap MS-SSIM pyramid requires at least %ux%u (Netflix#1414)\n",
                 fex->name, w, h, MS_SSIM_SCALES, MS_SSIM_GAUSSIAN_LEN, min_dim, min_dim);
        return -EINVAL;
    }

    if (s->enable_chroma) {
        const int chroma_err = check_chroma_min_dim(fex, pix_fmt, w, h, min_dim);
        if (chroma_err) { return chroma_err; }
    }
    return 0;
}

static void init_state_geometry(FloatMsSsimStateMetal *s, enum VmafPixelFormat pix_fmt,
                                unsigned bpc, unsigned w, unsigned h)
{
    /* ADR-1334 extends ADR-1221's geometry-derived ceiling to Metal. */
    s->max_db = vmaf_metal_ms_ssim_max_db(s->clip_db, bpc, w, h);

    s->width  = w;
    s->height = h;
    s->bpc    = bpc;
    s->scaler = (bpc <= 8u) ? 1.0f : ((bpc == 10u) ? 4.0f : ((bpc == 12u) ? 16.0f : 256.0f));

    for (unsigned p = 0; p < s->n_planes; ++p) {
        MsSsimPlaneGeometryMetal *geom = &s->geom[p];
        vmaf_metal_ms_ssim_plane_dimensions(pix_fmt, p, w, h, &geom->width,
                                            &geom->height);
        init_plane_geometry(geom);
    }

    const float L = 255.0f, K1 = 0.01f, K2 = 0.03f;
    s->c1 = (K1 * L) * (K1 * L);
    s->c2 = (K2 * L) * (K2 * L);
    s->c3 = s->c2 * 0.5f;
}

static int init_metal_device_context(FloatMsSsimStateMetal *s)
{
    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }
    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = NULL;
        return err;
    }

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    if (dh == NULL) {
        (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = NULL;
        return -ENODEV;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

    err = alloc_metal_buffers(s, device);
    if (err != 0) { goto fail_alloc; }
    err = build_pipelines(s, device);
    if (err != 0) { goto fail_alloc; }
    return 0;

fail_alloc:
    release_metal_buffers(s);
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    FloatMsSsimStateMetal *s = (FloatMsSsimStateMetal *)fex->priv;

    int err = validate_dimensions(fex, s, pix_fmt, w, h);
    if (err != 0) { return err; }

    init_state_geometry(s, pix_fmt, bpc, w, h);

    err = init_metal_device_context(s);
    if (err != 0) { return err; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features,
                                                      fex->options, s);
    if (s->feature_name_dict == NULL) {
        release_metal_psos(s);
        release_metal_buffers(s);
        (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = NULL;
        return -ENOMEM;
    }
    return 0;
}

/* Normalise ref/cmp picture plane -> float in [0, 255]. */
static void fill_float_plane(VmafPicture *pic, unsigned plane, id<MTLBuffer> dst,
                             unsigned w, unsigned h, float inv_scaler, unsigned bpc)
{
    float *out = (float *)[dst contents];
    if (bpc <= 8u) {
        for (unsigned y = 0; y < h; ++y) {
            const uint8_t *row =
                (const uint8_t *)pic->data[plane] + (size_t)y * pic->stride[plane];
            for (unsigned x = 0; x < w; ++x) {
                out[y * w + x] = (float)row[x];
            }
        }
    } else {
        for (unsigned y = 0; y < h; ++y) {
            const uint16_t *row =
                (const uint16_t *)((const uint8_t *)pic->data[plane] + (size_t)y * pic->stride[plane]);
            for (unsigned x = 0; x < w; ++x) {
                out[y * w + x] = (float)row[x] * inv_scaler;
            }
        }
    }
}

static void encode_plane_decimate(id<MTLCommandBuffer> cmd,
                                  id<MTLComputePipelineState> pso_dec,
                                  FloatMsSsimStateMetal *s, unsigned plane)
{
    const MsSsimPlaneGeometryMetal *geom = &s->geom[plane];
    for (int i = 0; i < MS_SSIM_SCALES - 1; ++i) {
        const uint32_t dims[4] = {
            (uint32_t)geom->scale_w[i], (uint32_t)geom->scale_h[i],
            (uint32_t)geom->scale_w[i + 1], (uint32_t)geom->scale_h[i + 1],
        };
        const size_t grid_x = (geom->scale_w[i + 1] + 15u) / 16u;
        const size_t grid_y = (geom->scale_h[i + 1] + 15u) / 16u;

        for (int side = 0; side < 2; ++side) {
            id<MTLBuffer> src_buf = (side == 0)
                ? (__bridge id<MTLBuffer>)s->pyramid_ref[plane][i]
                : (__bridge id<MTLBuffer>)s->pyramid_cmp[plane][i];
            id<MTLBuffer> dst_buf = (side == 0)
                ? (__bridge id<MTLBuffer>)s->pyramid_ref[plane][i + 1]
                : (__bridge id<MTLBuffer>)s->pyramid_cmp[plane][i + 1];

            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:pso_dec];
            [enc setBuffer:src_buf offset:0 atIndex:0];
            [enc setBuffer:dst_buf offset:0 atIndex:1];
            [enc setBytes:dims length:sizeof(dims) atIndex:2];
            MTLSize tg   = MTLSizeMake(16, 16, 1);
            MTLSize grid = MTLSizeMake(grid_x, grid_y, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
            [enc endEncoding];
        }
    }
}

static void encode_plane_scales(id<MTLCommandBuffer> cmd,
                                id<MTLComputePipelineState> pso_hor,
                                id<MTLComputePipelineState> pso_vlcs,
                                id<MTLBuffer> hbuf,
                                FloatMsSsimStateMetal *s, unsigned plane)
{
    const MsSsimPlaneGeometryMetal *geom = &s->geom[plane];
    for (int i = 0; i < MS_SSIM_SCALES; ++i) {
        id<MTLBuffer> ref_i = (__bridge id<MTLBuffer>)s->pyramid_ref[plane][i];
        id<MTLBuffer> cmp_i = (__bridge id<MTLBuffer>)s->pyramid_cmp[plane][i];
        id<MTLBuffer> lp    = (__bridge id<MTLBuffer>)s->l_partials[plane][i];
        id<MTLBuffer> cp    = (__bridge id<MTLBuffer>)s->c_partials[plane][i];
        id<MTLBuffer> sp    = (__bridge id<MTLBuffer>)s->s_partials[plane][i];

        const uint32_t hor_params[4] = {
            (uint32_t)geom->scale_w[i], (uint32_t)geom->scale_h[i],
            (uint32_t)geom->scale_w_h[i], 0u,
        };
        const size_t hor_gx = (geom->scale_w_h[i] + 15u) / 16u;
        const size_t hor_gy = (geom->scale_h[i]   +  7u) /  8u;

        id<MTLComputeCommandEncoder> enc_hor = [cmd computeCommandEncoder];
        [enc_hor setComputePipelineState:pso_hor];
        [enc_hor setBuffer:ref_i  offset:0 atIndex:0];
        [enc_hor setBuffer:cmp_i  offset:0 atIndex:1];
        [enc_hor setBuffer:hbuf   offset:0 atIndex:2];
        [enc_hor setBytes:hor_params length:sizeof(hor_params) atIndex:3];
        MTLSize tg_hor   = MTLSizeMake(16, 8, 1);
        MTLSize grid_hor = MTLSizeMake(hor_gx, hor_gy, 1);
        [enc_hor dispatchThreadgroups:grid_hor threadsPerThreadgroup:tg_hor];
        [enc_hor endEncoding];

        const uint32_t vlcs_params[4] = {
            (uint32_t)geom->scale_w_h[i], (uint32_t)geom->scale_h[i],
            (uint32_t)geom->scale_w_f[i], (uint32_t)geom->scale_h_f[i],
        };
        const float vlcs_consts[4] = {s->c1, s->c2, s->c3, 0.0f};
        const uint32_t grid_dim[2] = {
            (uint32_t)geom->scale_grid_w[i],
            (uint32_t)geom->scale_grid_h[i],
        };

        id<MTLComputeCommandEncoder> enc_vlcs = [cmd computeCommandEncoder];
        [enc_vlcs setComputePipelineState:pso_vlcs];
        [enc_vlcs setBuffer:hbuf offset:0 atIndex:0];
        [enc_vlcs setBuffer:lp   offset:0 atIndex:1];
        [enc_vlcs setBuffer:cp   offset:0 atIndex:2];
        [enc_vlcs setBuffer:sp   offset:0 atIndex:3];
        [enc_vlcs setBytes:vlcs_params length:sizeof(vlcs_params) atIndex:4];
        [enc_vlcs setBytes:vlcs_consts length:sizeof(vlcs_consts) atIndex:5];
        [enc_vlcs setBytes:grid_dim    length:sizeof(grid_dim)    atIndex:6];
        MTLSize tg_vlcs   = MTLSizeMake(MS_SSIM_BLOCK_X, MS_SSIM_BLOCK_Y, 1);
        MTLSize grid_vlcs = MTLSizeMake(geom->scale_grid_w[i], geom->scale_grid_h[i], 1);
        [enc_vlcs dispatchThreadgroups:grid_vlcs threadsPerThreadgroup:tg_vlcs];
        [enc_vlcs endEncoding];
    }
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    FloatMsSsimStateMetal *s = (FloatMsSsimStateMetal *)fex->priv;

    s->index = index;

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLCommandBuffer> cmd  = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    const float inv = 1.0f / s->scaler;
    id<MTLComputePipelineState> pso_dec  =
        (__bridge id<MTLComputePipelineState>)s->pso_decimate;
    id<MTLComputePipelineState> pso_hor  =
        (__bridge id<MTLComputePipelineState>)s->pso_horiz;
    id<MTLComputePipelineState> pso_vlcs =
        (__bridge id<MTLComputePipelineState>)s->pso_vert_lcs;
    id<MTLBuffer> hbuf = (__bridge id<MTLBuffer>)s->hbuf;

    for (unsigned p = 0; p < s->n_planes; ++p) {
        const MsSsimPlaneGeometryMetal *geom = &s->geom[p];
        id<MTLBuffer> pyr_ref0 = (__bridge id<MTLBuffer>)s->pyramid_ref[p][0];
        id<MTLBuffer> pyr_cmp0 = (__bridge id<MTLBuffer>)s->pyramid_cmp[p][0];
        fill_float_plane(ref_pic, p, pyr_ref0, geom->scale_w[0], geom->scale_h[0], inv, s->bpc);
        fill_float_plane(dist_pic, p, pyr_cmp0, geom->scale_w[0], geom->scale_h[0], inv, s->bpc);

        encode_plane_decimate(cmd, pso_dec, s, p);
        encode_plane_scales(cmd, pso_hor, pso_vlcs, hbuf, s, p);
    }

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

static const char *const ms_ssim_feature_names[MS_SSIM_MAX_PLANES] = {
    "float_ms_ssim",
    "float_ms_ssim_cb",
    "float_ms_ssim_cr",
};

static int reduce_plane_means(const FloatMsSsimStateMetal *s, unsigned plane, unsigned index,
                              double *l_means, double *c_means, double *s_means,
                              double *out_msssim)
{
    const MsSsimPlaneGeometryMetal *geom = &s->geom[plane];
    for (int i = 0; i < MS_SSIM_SCALES; ++i) {
        const float *lp =
            (const float *)[(__bridge id<MTLBuffer>)s->l_partials[plane][i] contents];
        const float *cp =
            (const float *)[(__bridge id<MTLBuffer>)s->c_partials[plane][i] contents];
        const float *sp =
            (const float *)[(__bridge id<MTLBuffer>)s->s_partials[plane][i] contents];

        double tl = 0.0, tc = 0.0, ts = 0.0;
        for (unsigned j = 0; j < geom->scale_block_count[i]; ++j) {
            tl += (double)lp[j];
            tc += (double)cp[j];
            ts += (double)sp[j];
        }
        const double n = (double)geom->scale_w_f[i] * (double)geom->scale_h_f[i];
        l_means[i] = (n > 0.0) ? (tl / n) : 0.0;
        c_means[i] = (n > 0.0) ? (tc / n) : 0.0;
        s_means[i] = (n > 0.0) ? (ts / n) : 0.0;

        const VmafNamedScore atoms[] = {
            {.name = "float_ms_ssim_l", .value = l_means[i]},
            {.name = "float_ms_ssim_c", .value = c_means[i]},
            {.name = "float_ms_ssim_s", .value = s_means[i]},
        };
        if (vmaf_feature_validate_finite_scores_named("float_ms_ssim_metal", atoms, 3u,
                                                       index)) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "float_ms_ssim_metal: invalid atom set at frame %u "
                     "(plane=%u scale=%d)\n",
                     index, plane, i);
            return -EINVAL;
        }
    }

    double msssim = 1.0;
    for (int i = 0; i < MS_SSIM_SCALES; ++i) {
        msssim *= pow(l_means[i], (double)g_alphas[i]) *
                  pow(c_means[i], (double)g_betas[i]) *
                  pow(fabs(s_means[i]), (double)g_gammas[i]);
    }
    *out_msssim = msssim;
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    FloatMsSsimStateMetal *s = (FloatMsSsimStateMetal *)fex->priv;

    double plane_scores[MS_SSIM_MAX_PLANES] = {0.0};
    double l_means[MS_SSIM_MAX_PLANES][MS_SSIM_SCALES] = {{0.0}};
    double c_means[MS_SSIM_MAX_PLANES][MS_SSIM_SCALES] = {{0.0}};
    double s_means[MS_SSIM_MAX_PLANES][MS_SSIM_SCALES] = {{0.0}};

    for (unsigned plane = 0; plane < s->n_planes; ++plane) {
        int err = reduce_plane_means(s, plane, index, l_means[plane], c_means[plane],
                                     s_means[plane], &plane_scores[plane]);
        if (err) { return err; }
    }

    for (unsigned plane = 0; plane < s->n_planes; ++plane) {
        double prepared_score = 0.0;
        int const err =
            vmaf_ssim_prepare_score_named(ms_ssim_feature_names[plane], plane_scores[plane],
                                          s->enable_db, s->max_db, index, &prepared_score);
        if (err) { return err; }
    }

    int err = 0;
    for (unsigned plane = 0; plane < s->n_planes && !err; ++plane) {
        if (plane == 0u) {
            err = vmaf_ms_ssim_emit_scores(feature_collector, s->feature_name_dict,
                                           "float_ms_ssim_metal", "float_ms_ssim",
                                           plane_scores[0], s->enable_db, s->max_db,
                                           l_means[0], c_means[0], s_means[0],
                                           MS_SSIM_SCALES, s->enable_lcs, index);
        } else {
            err = vmaf_ssim_emit_score_named(feature_collector, s->feature_name_dict,
                                             "float_ms_ssim_metal", ms_ssim_feature_names[plane],
                                             plane_scores[plane], s->enable_db, s->max_db, index);
        }
    }
    return err;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatMsSsimStateMetal *s = (FloatMsSsimStateMetal *)fex->priv;

    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    release_metal_psos(s);
    release_metal_buffers(s);

    if (s->feature_name_dict) {
        int err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0) { rc = err; }
    }
    if (s->ctx) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = NULL;
    }
    return rc;
}

static const char *provided_features[] = {
    "float_ms_ssim",
    "float_ms_ssim_cb",
    "float_ms_ssim_cr",
    NULL,
};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend, ADR-0490 ms_ssim port; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0490 / ADR-0278
VmafFeatureExtractor vmaf_fex_float_ms_ssim_metal = {
    .name              = "float_ms_ssim_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = NULL,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(FloatMsSsimStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 3 * MS_SSIM_SCALES + 2 * (MS_SSIM_SCALES - 1),
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
