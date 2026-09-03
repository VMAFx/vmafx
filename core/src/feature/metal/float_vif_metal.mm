/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  float_vif feature extractor on the Metal backend. ObjC++ wrapper that
 *  drives the two MSL kernels in `float_vif.metal` (float_vif_compute,
 *  float_vif_decimate). Port of the CUDA twin
 *  `core/src/feature/cuda/float_vif/float_vif_score.cu` + wrapper and the
 *  SYCL twin `core/src/feature/sycl/float_vif_sycl.cpp` — same 4-scale
 *  separable Gaussian pyramid, same per-pixel VIF statistic, same
 *  per-workgroup (num, den) float partial-sum reduction, same host-side
 *  double-precision sum + per-scale division.
 *
 *  Algorithm summary (per frame):
 *    1. Host copies ref/dis Y-plane raw bytes into Shared MTLBuffers.
 *    2. Scale 0: float_vif_compute reads raw, subtracts 128, applies the
 *       17-tap separable Gaussian, runs the VIF stat, reduces (num, den).
 *    3. Scales 1..3: float_vif_decimate (V->H Gaussian at the previous
 *       scale's dims, sampled 2x) builds the decimated float pyramid, then
 *       float_vif_compute runs the VIF stat at that scale.
 *    4. Host (collect): sum per-WG partials in double, divide num/den per
 *       scale -> four VMAF_feature_vif_scale{0..3}_score values (+ optional
 *       debug aggregates), identical to float_vif.c / float_vif_sycl.cpp.
 *
 *  Multi-scale buffer strategy: like the SYCL twin, the float pyramid uses
 *  two ping-pong buffers per side (ref/dis) sized to scale 1 (the largest
 *  decimated scale); decimate writes scale n into the ping-pong slot read
 *  by scale n+1's decimate. Per-scale (num, den) partials are separate
 *  Shared buffers sized to that scale's workgroup count. All buffers are
 *  allocated once in init() and reused every frame (zero per-frame heap
 *  traffic), matching the CPU/CUDA/SYCL "allocate once" contract.
 *
 *  Param contract: this kernel hardcodes the CUDA/SYCL default parameters
 *  (kernelscale=1.0, vif_sigma_nsq=2.0, vif_enhn_gain_limit=100.0). init()
 *  rejects any non-default option with -EINVAL, exactly like the SYCL twin
 *  (`if (s->vif_kernelscale != 1.0) return -EINVAL;`). vif_skip_scale0 and
 *  debug are host-side only and supported.
 *
 *  Metallib resolution: embedded-blob pattern shared by every Metal feature
 *  extractor — reads the __TEXT,__metallib section compiled via xcrun from
 *  float_vif.metal.
 *
 *  Parity: places=4 (1e-4, ADR-0214) — the same gate the CUDA/SYCL
 *  float_vif parity tests assert.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* feature_extractor.h pulled in before the extern "C" block (libc++ on
 * Xcode 16.x emits "templates must have C++ linkage" when <atomic> is
 * dragged into an extern "C" scope — same workaround as the other .mm). */
#include "feature_extractor.h"

extern "C" {
#include "dict.h"
#include "feature_collector.h"
#include "feature_name.h"
#include "log.h"
#include "vif_tools.h"
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

#define FVIF_SCALES 4
#define FVIF_BX     16
#define FVIF_BY     16

typedef struct FloatVifStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;

    /* Pipeline states for the two VIF kernels. */
    void *pso_compute;
    void *pso_decimate;

    /* Raw ref/dis Y-plane uploads (Shared storage, packed to width*bpp). */
    void *raw_ref;
    void *raw_dis;

    /* Ping-pong decimated float pyramid (sized to scale 1, the largest
     * decimated scale): [side][slot]. side 0=ref, 1=dis. */
    void *pyr_ref[2];
    void *pyr_dis[2];

    /* Per-scale (num, den) partials (Shared storage). */
    void *num_partials[FVIF_SCALES];
    void *den_partials[FVIF_SCALES];

    unsigned width;
    unsigned height;
    unsigned bpc;

    unsigned scale_w[FVIF_SCALES];
    unsigned scale_h[FVIF_SCALES];
    unsigned wg_count[FVIF_SCALES];

    /* Host-side option mirror of the CPU/SYCL twins. */
    bool   debug;
    double vif_enhn_gain_limit;
    double vif_kernelscale;
    double vif_sigma_nsq;
    bool   vif_skip_scale0;

    unsigned index;
    VmafDictionary *feature_name_dict;
} FloatVifStateMetal;

/* Options mirror the CPU float_vif.c / SYCL float_vif_sycl.cpp tables. The
 * compute kernel only realises the default parameter set; init() rejects a
 * non-default kernelscale/sigma_nsq/egl (matches the SYCL twin's EINVAL). */
static const VmafOption options[] = {
    {
        .name        = "debug",
        .help        = "debug mode: enable additional output",
        .offset      = offsetof(FloatVifStateMetal, debug),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name        = "vif_enhn_gain_limit",
        .alias       = "egl",
        .help        = "enhancement gain imposed on vif, must be >= 1.0, "
                       "where 1.0 means the gain is completely disabled",
        .offset      = offsetof(FloatVifStateMetal, vif_enhn_gain_limit),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 100.0},
        .min         = 1.0,
        .max         = 100.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_kernelscale",
        .alias       = "ks",
        .help        = "scaling factor for the gaussian kernel "
                       "(Metal path realises kernelscale=1.0 only)",
        .offset      = offsetof(FloatVifStateMetal, vif_kernelscale),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 1.0},
        .min         = 0.1,
        .max         = 4.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_sigma_nsq",
        .alias       = "snsq",
        .help        = "neural noise variance",
        .offset      = offsetof(FloatVifStateMetal, vif_sigma_nsq),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 2.0},
        .min         = 0.0,
        .max         = 5.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_skip_scale0",
        .alias       = "ssclz",
        .help        = "when set, skip scale 0 calculations",
        .offset      = offsetof(FloatVifStateMetal, vif_skip_scale0),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0},
};

static int build_pipelines(FloatVifStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn_compute  = [lib newFunctionWithName:@"float_vif_compute"];
    id<MTLFunction> fn_decimate = [lib newFunctionWithName:@"float_vif_decimate"];
    if (fn_compute == nil || fn_decimate == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso_c =
        [device newComputePipelineStateWithFunction:fn_compute  error:&err];
    id<MTLComputePipelineState> pso_d =
        [device newComputePipelineStateWithFunction:fn_decimate error:&err];
    if (pso_c == nil || pso_d == nil) { return -ENODEV; }

    s->pso_compute  = (__bridge_retained void *)pso_c;
    s->pso_decimate = (__bridge_retained void *)pso_d;
    return 0;
}

static void release_buffers(FloatVifStateMetal *s)
{
    for (int i = 0; i < FVIF_SCALES; ++i) {
        if (s->num_partials[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->num_partials[i];
            s->num_partials[i] = NULL;
        }
        if (s->den_partials[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->den_partials[i];
            s->den_partials[i] = NULL;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (s->pyr_ref[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->pyr_ref[i];
            s->pyr_ref[i] = NULL;
        }
        if (s->pyr_dis[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->pyr_dis[i];
            s->pyr_dis[i] = NULL;
        }
    }
    if (s->raw_ref) {
        (void)(__bridge_transfer id<MTLBuffer>)s->raw_ref;
        s->raw_ref = NULL;
    }
    if (s->raw_dis) {
        (void)(__bridge_transfer id<MTLBuffer>)s->raw_dis;
        s->raw_dis = NULL;
    }
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatVifStateMetal *s = (FloatVifStateMetal *)fex->priv;

    /* Metal kernel realises the default kernelscale only (matches SYCL twin
     * `if (s->vif_kernelscale != 1.0) return -EINVAL;`). */
    if (s->vif_kernelscale != 1.0) { return -EINVAL; }

    s->width  = w;
    s->height = h;
    s->bpc    = bpc;

    /* Per-scale geometry: halve each dimension (no border crop — matches
     * VIF_OPT_HANDLE_BORDERS). */
    s->scale_w[0] = w;
    s->scale_h[0] = h;
    for (int i = 1; i < FVIF_SCALES; ++i) {
        s->scale_w[i] = s->scale_w[i - 1] / 2u;
        s->scale_h[i] = s->scale_h[i - 1] / 2u;
    }
    /* Cross-backend parity with the CPU floor (ADR-0214 places=4). This check
     * used to be `scale_w[FVIF_SCALES - 1] == 0`, i.e. `w >> 3 == 0`, an
     * effective floor of 8 -- which admitted the 8..15px range that walks the
     * reflect-101 mirror out of the plane at scale 3 (Netflix/vmaf#1582, the
     * same defect fixed on the CPU path). The binding constraint is scale 3,
     * so the real minimum at the default kernelscale is 16.
     * vif_get_min_dim() is the single source of truth shared with
     * float_vif.c; see its derivation in vif_tools.c. */
    const int vif_min_dim = vif_get_min_dim((float)s->vif_kernelscale);
    if (w < (unsigned)vif_min_dim || h < (unsigned)vif_min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_vif_metal: width and height must be >= %d for the "
                 "four-scale VIF ladder (got %ux%u)\n",
                 vif_min_dim, w, h);
        return -EINVAL;
    }

    for (int i = 0; i < FVIF_SCALES; ++i) {
        const unsigned gx = (s->scale_w[i] + (unsigned)FVIF_BX - 1u) / (unsigned)FVIF_BX;
        const unsigned gy = (s->scale_h[i] + (unsigned)FVIF_BY - 1u) / (unsigned)FVIF_BY;
        s->wg_count[i] = gx * gy;
    }

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    {
        void *dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == NULL) { err = -ENODEV; goto fail_lc; }
        id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

        const size_t bpp = (bpc <= 8u) ? 1u : 2u;
        const size_t raw_bytes = (size_t)w * h * bpp;
        id<MTLBuffer> rr = [device newBufferWithLength:raw_bytes
                                              options:MTLResourceStorageModeShared];
        id<MTLBuffer> rd = [device newBufferWithLength:raw_bytes
                                              options:MTLResourceStorageModeShared];
        if (rr == nil || rd == nil) { err = -ENOMEM; goto fail_bufs; }
        s->raw_ref = (__bridge_retained void *)rr;
        s->raw_dis = (__bridge_retained void *)rd;

        /* Ping-pong float pyramid sized to scale 1 (largest decimated). */
        const size_t fbytes = (size_t)s->scale_w[1] * s->scale_h[1] * sizeof(float);
        for (int i = 0; i < 2; ++i) {
            id<MTLBuffer> pr = [device newBufferWithLength:fbytes
                                                  options:MTLResourceStorageModeShared];
            id<MTLBuffer> pd = [device newBufferWithLength:fbytes
                                                  options:MTLResourceStorageModeShared];
            if (pr == nil || pd == nil) { err = -ENOMEM; goto fail_bufs; }
            s->pyr_ref[i] = (__bridge_retained void *)pr;
            s->pyr_dis[i] = (__bridge_retained void *)pd;
        }

        for (int i = 0; i < FVIF_SCALES; ++i) {
            const size_t pb = (size_t)s->wg_count[i] * sizeof(float);
            id<MTLBuffer> bn = [device newBufferWithLength:pb
                                                  options:MTLResourceStorageModeShared];
            id<MTLBuffer> bd = [device newBufferWithLength:pb
                                                  options:MTLResourceStorageModeShared];
            if (bn == nil || bd == nil) { err = -ENOMEM; goto fail_bufs; }
            s->num_partials[i] = (__bridge_retained void *)bn;
            s->den_partials[i] = (__bridge_retained void *)bd;
        }

        err = build_pipelines(s, device);
    }
    if (err != 0) { goto fail_pso; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    if (s->pso_decimate) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_decimate;
        s->pso_decimate = NULL;
    }
    if (s->pso_compute) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_compute;
        s->pso_compute = NULL;
    }
fail_bufs:
    release_buffers(s);
fail_lc:
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
fail_ctx:
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

/* Copy the ref/dis Y-plane raw bytes (packed, stride = width*bpp) into the
 * shared upload buffer. Matches the SYCL copy_y_plane staging. */
static void fill_raw_plane(VmafPicture *pic, id<MTLBuffer> dst, unsigned w, unsigned h,
                           unsigned bpc)
{
    uint8_t *out = (uint8_t *)[dst contents];
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t dst_row = (size_t)w * bpp;
    for (unsigned y = 0; y < h; ++y) {
        const uint8_t *src = (const uint8_t *)pic->data[0] + (size_t)y * pic->stride[0];
        memcpy(out + (size_t)y * dst_row, src, dst_row);
    }
}

static void encode_compute(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                           id<MTLBuffer> raw_ref, id<MTLBuffer> raw_dis, id<MTLBuffer> ref_f,
                           id<MTLBuffer> dis_f, id<MTLBuffer> num_p, id<MTLBuffer> den_p,
                           int scale, unsigned width, unsigned height, unsigned f_stride,
                           unsigned bpc, unsigned raw_stride, unsigned grid_x)
{
    const uint32_t params[4] = {width, height, f_stride, bpc};
    const uint32_t cfg[4]    = {(uint32_t)scale, raw_stride, grid_x, 0u};

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:raw_ref offset:0 atIndex:0];
    [enc setBuffer:raw_dis offset:0 atIndex:1];
    [enc setBuffer:ref_f   offset:0 atIndex:2];
    [enc setBuffer:dis_f   offset:0 atIndex:3];
    [enc setBuffer:num_p   offset:0 atIndex:4];
    [enc setBuffer:den_p   offset:0 atIndex:5];
    [enc setBytes:params length:sizeof(params) atIndex:6];
    [enc setBytes:cfg    length:sizeof(cfg)    atIndex:7];
    MTLSize tg   = MTLSizeMake(FVIF_BX, FVIF_BY, 1);
    MTLSize grid = MTLSizeMake((width + FVIF_BX - 1u) / FVIF_BX,
                               (height + FVIF_BY - 1u) / FVIF_BY, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

static void encode_decimate(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                            id<MTLBuffer> raw_ref, id<MTLBuffer> raw_dis, id<MTLBuffer> ref_f,
                            id<MTLBuffer> dis_f, id<MTLBuffer> ref_out, id<MTLBuffer> dis_out,
                            int scale, unsigned out_w, unsigned out_h, unsigned in_w,
                            unsigned in_h, unsigned raw_stride, unsigned f_stride,
                            unsigned out_stride, unsigned bpc)
{
    const uint32_t dims[4] = {out_w, out_h, in_w, in_h};
    const uint32_t cfg[4]  = {(uint32_t)scale, raw_stride, f_stride, out_stride};
    const uint32_t bpc4[4] = {bpc, 0u, 0u, 0u};

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:raw_ref offset:0 atIndex:0];
    [enc setBuffer:raw_dis offset:0 atIndex:1];
    [enc setBuffer:ref_f   offset:0 atIndex:2];
    [enc setBuffer:dis_f   offset:0 atIndex:3];
    [enc setBuffer:ref_out offset:0 atIndex:4];
    [enc setBuffer:dis_out offset:0 atIndex:5];
    [enc setBytes:dims length:sizeof(dims) atIndex:6];
    [enc setBytes:cfg  length:sizeof(cfg)  atIndex:7];
    [enc setBytes:bpc4 length:sizeof(bpc4) atIndex:8];
    MTLSize tg   = MTLSizeMake(FVIF_BX, FVIF_BY, 1);
    MTLSize grid = MTLSizeMake((out_w + FVIF_BX - 1u) / FVIF_BX,
                               (out_h + FVIF_BY - 1u) / FVIF_BY, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    FloatVifStateMetal *s = (FloatVifStateMetal *)fex->priv;
    s->index = index;

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;

    id<MTLComputePipelineState> pso_c = (__bridge id<MTLComputePipelineState>)s->pso_compute;
    id<MTLComputePipelineState> pso_d = (__bridge id<MTLComputePipelineState>)s->pso_decimate;
    id<MTLBuffer> raw_ref = (__bridge id<MTLBuffer>)s->raw_ref;
    id<MTLBuffer> raw_dis = (__bridge id<MTLBuffer>)s->raw_dis;

    fill_raw_plane(ref_pic,  raw_ref, s->width, s->height, s->bpc);
    fill_raw_plane(dist_pic, raw_dis, s->width, s->height, s->bpc);

    const unsigned bpp = (s->bpc <= 8u) ? 1u : 2u;
    const unsigned raw_stride = s->width * bpp;

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    /* Scale 0: compute directly from raw. */
    {
        id<MTLBuffer> n0 = (__bridge id<MTLBuffer>)s->num_partials[0];
        id<MTLBuffer> d0 = (__bridge id<MTLBuffer>)s->den_partials[0];
        const unsigned grid_x = (s->scale_w[0] + (unsigned)FVIF_BX - 1u) / (unsigned)FVIF_BX;
        /* float buffers unused at scale 0 but must be bound -> reuse raw. */
        encode_compute(cmd, pso_c, raw_ref, raw_dis,
                       (__bridge id<MTLBuffer>)s->pyr_ref[0],
                       (__bridge id<MTLBuffer>)s->pyr_dis[0],
                       n0, d0, 0, s->scale_w[0], s->scale_h[0],
                       s->scale_w[0], s->bpc, raw_stride, grid_x);
    }

    /* Scales 1..3: decimate (prev dims) then compute (this scale). */
    for (int n = 1; n < FVIF_SCALES; ++n) {
        const int dst_slot = (n - 1) % 2;
        const bool prev_is_raw = (n == 1);
        id<MTLBuffer> ref_in = prev_is_raw
            ? raw_ref
            : (dst_slot == 0 ? (__bridge id<MTLBuffer>)s->pyr_ref[1]
                             : (__bridge id<MTLBuffer>)s->pyr_ref[0]);
        id<MTLBuffer> dis_in = prev_is_raw
            ? raw_dis
            : (dst_slot == 0 ? (__bridge id<MTLBuffer>)s->pyr_dis[1]
                             : (__bridge id<MTLBuffer>)s->pyr_dis[0]);
        const unsigned in_f_stride = prev_is_raw ? 0u : s->scale_w[n - 1];
        id<MTLBuffer> ref_out = (dst_slot == 0) ? (__bridge id<MTLBuffer>)s->pyr_ref[0]
                                                : (__bridge id<MTLBuffer>)s->pyr_ref[1];
        id<MTLBuffer> dis_out = (dst_slot == 0) ? (__bridge id<MTLBuffer>)s->pyr_dis[0]
                                                : (__bridge id<MTLBuffer>)s->pyr_dis[1];
        const unsigned out_stride = s->scale_w[n];

        encode_decimate(cmd, pso_d, raw_ref, raw_dis, ref_in, dis_in, ref_out, dis_out,
                        n, s->scale_w[n], s->scale_h[n], s->scale_w[n - 1], s->scale_h[n - 1],
                        raw_stride, in_f_stride, out_stride, s->bpc);

        id<MTLBuffer> nn = (__bridge id<MTLBuffer>)s->num_partials[n];
        id<MTLBuffer> dn = (__bridge id<MTLBuffer>)s->den_partials[n];
        const unsigned grid_x = (s->scale_w[n] + (unsigned)FVIF_BX - 1u) / (unsigned)FVIF_BX;
        encode_compute(cmd, pso_c, raw_ref, raw_dis, ref_out, dis_out, nn, dn, n,
                       s->scale_w[n], s->scale_h[n], out_stride, s->bpc, raw_stride, grid_x);
    }

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    FloatVifStateMetal *s = (FloatVifStateMetal *)fex->priv;

    /* Sum per-WG partials in double, exactly like the SYCL/CUDA collect. */
    double scores[8];
    for (int i = 0; i < FVIF_SCALES; ++i) {
        const float *np = (const float *)[(__bridge id<MTLBuffer>)s->num_partials[i] contents];
        const float *dp = (const float *)[(__bridge id<MTLBuffer>)s->den_partials[i] contents];
        double n = 0.0, d = 0.0;
        for (unsigned j = 0; j < s->wg_count[i]; ++j) {
            n += (double)np[j];
            d += (double)dp[j];
        }
        scores[2 * i + 0] = n;
        scores[2 * i + 1] = d;
    }

    int err = 0;
    /* vif_skip_scale0: emit 0.0 for scale-0 (host-side suppression),
     * mirroring float_vif.c / float_vif_sycl.cpp. */
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_feature_vif_scale0_score",
        s->vif_skip_scale0 ? 0.0 : scores[0] / scores[1], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_feature_vif_scale1_score",
                                                   scores[2] / scores[3], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_feature_vif_scale2_score",
                                                   scores[4] / scores[5], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_feature_vif_scale3_score",
                                                   scores[6] / scores[7], index);

    if (s->debug && !err) {
        const double score_num =
            (s->vif_skip_scale0 ? 0.0 : scores[0]) + scores[2] + scores[4] + scores[6];
        const double score_den =
            (s->vif_skip_scale0 ? 0.0 : scores[1]) + scores[3] + scores[5] + scores[7];
        const double score = (score_den == 0.0) ? 1.0 : score_num / score_den;
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "vif", score, index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "vif_num", score_num, index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "vif_den", score_den, index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "vif_num_scale0",
                                                       s->vif_skip_scale0 ? 0.0 : scores[0], index);
        err |= vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "vif_den_scale0",
            s->vif_skip_scale0 ? -1.0 : scores[1], index);
        static const char *const names[6] = {"vif_num_scale1", "vif_den_scale1", "vif_num_scale2",
                                             "vif_den_scale2", "vif_num_scale3", "vif_den_scale3"};
        for (int i = 0; i < 6; ++i) {
            err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                           names[i], scores[i + 2], index);
        }
    }
    return err;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatVifStateMetal *s = (FloatVifStateMetal *)fex->priv;

    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_decimate) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_decimate;
        s->pso_decimate = NULL;
    }
    if (s->pso_compute) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_compute;
        s->pso_compute = NULL;
    }

    release_buffers(s);

    if (s->feature_name_dict) {
        int derr = vmaf_dictionary_free(&s->feature_name_dict);
        if (derr != 0 && rc == 0) { rc = derr; }
    }
    if (s->ctx) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = NULL;
    }
    return rc;
}

static const char *provided_features[] = {"VMAF_feature_vif_scale0_score",
                                          "VMAF_feature_vif_scale1_score",
                                          "VMAF_feature_vif_scale2_score",
                                          "VMAF_feature_vif_scale3_score",
                                          "vif",
                                          "vif_num",
                                          "vif_den",
                                          "vif_num_scale0",
                                          "vif_den_scale0",
                                          "vif_num_scale1",
                                          "vif_den_scale1",
                                          "vif_num_scale2",
                                          "vif_den_scale2",
                                          "vif_num_scale3",
                                          "vif_den_scale3",
                                          NULL};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL / Metal feature extractor uses (ADR-0361
 * Metal backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_float_vif_metal = {
    .name              = "float_vif_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = NULL,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(FloatVifStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = FVIF_SCALES + (FVIF_SCALES - 1),
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
