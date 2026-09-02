/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  integer_vif feature extractor on the Metal backend (feature name "vif").
 *  ObjC++ wrapper that drives the four MSL kernels in `integer_vif.metal`
 *  (integer_vif_compute_8 / _16 and integer_vif_decimate_8 / _16). Fixed-
 *  point twin of float_vif_metal.mm — mirrors its 4-scale pyramid dispatch
 *  scaffold (lifecycle, buffer layout, per-scale division in collect) and
 *  swaps the float arithmetic for the int64 fixed-point arithmetic of the
 *  CPU reference core/src/feature/integer_vif.c.
 *
 *  Algorithm summary (per frame), mirroring integer_vif.c::extract:
 *    1. Host copies ref/dis Y-plane raw bytes into Shared MTLBuffers.
 *    2. Scale 0: integer_vif_compute_8 (8 bpc) or _16 (HBD) reads the raw
 *       plane, applies the 17-tap fixed-point Gaussian (V then H), computes
 *       the per-pixel integer VIF statistic, and reduces four int64
 *       accumulators (num_log, den_log, num_non_log, den_non_log) per WG.
 *    3. Scales 1..3: integer_vif_decimate_{8,16} (subsample_rd_{8,16} twin)
 *       builds the decimated uint16 pyramid, then integer_vif_compute_16
 *       runs the statistic at that scale.
 *    4. Host (collect): sum per-WG int64 partials per scale, apply the exact
 *       CPU final formula to get (num, den) per scale, then divide ->
 *       VMAF_integer_feature_vif_scale{0..3}_score (+ optional debug
 *       aggregates), identical to integer_vif.c::write_scores.
 *
 *  log2 LUT: integer_vif.c generates a VIF_LOG2_TABLE_SIZE (32768) entry
 *  uint16 table host-side (log_generate). We regenerate the identical table
 *  in init() and upload it as a Shared device buffer that the compute kernels
 *  read through, so the GPU log2_32 / log2_64 accessors are bit-exact to the
 *  CPU's.
 *
 *  Multi-scale buffer strategy: like float_vif_metal, two ping-pong uint16
 *  buffers per side (ref/dis), sized to scale 1 (the largest decimated
 *  scale); decimate writes scale n into the slot read by scale n+1's
 *  decimate. Per-scale int64 (num_log, den_log, num_non_log, den_non_log)
 *  partials are separate Shared buffers sized to that scale's WG count. All
 *  buffers are allocated once in init() (zero per-frame heap traffic).
 *
 *  Param contract: vif_enhn_gain_limit is forwarded to the kernel as a float
 *  (the CPU uses it as a double inside the statistic; the only float-touched
 *  values are small relative to the 1e-4 gate). debug and vif_skip_scale0 are
 *  host-side only.
 *
 *  Parity: places=4 (1e-4, ADR-0214) — the same gate the CUDA/SYCL
 *  integer_vif parity tests assert.
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
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

#define IVIF_SCALES 4
#define IVIF_BX     16
#define IVIF_BY     16
#define VIF_LOG2_TABLE_SIZE 32768u

/* Host-side mirror of the int64 per-WG accumulator written by the kernel
 * (struct VifWgAccum in integer_vif.metal — same field order / 64-bit
 * widths so the Shared-buffer layout matches byte-for-byte). */
typedef struct VifWgAccumHost {
    int64_t num_log;
    int64_t den_log;
    int64_t num_non_log;
    int64_t den_non_log;
} VifWgAccumHost;

typedef struct IntegerVifStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;

    /* Pipeline states for the four VIF kernels. */
    void *pso_compute_8;
    void *pso_compute_16;
    void *pso_decimate_8;
    void *pso_decimate_16;

    /* Raw ref/dis Y-plane uploads (Shared storage, packed to width*bpp). */
    void *raw_ref;
    void *raw_dis;

    /* log2 LUT (Shared storage, VIF_LOG2_TABLE_SIZE uint16). */
    void *log2_buf;

    /* Ping-pong decimated uint16 pyramid (sized to scale 1): [side][slot]. */
    void *pyr_ref[2];
    void *pyr_dis[2];

    /* Per-scale VifWgAccum partials (Shared storage). */
    void *wg_accum[IVIF_SCALES];

    unsigned width;
    unsigned height;
    unsigned bpc;

    unsigned scale_w[IVIF_SCALES];
    unsigned scale_h[IVIF_SCALES];
    unsigned wg_count[IVIF_SCALES];

    /* Host-side option mirror of the CPU integer_vif.c twin. */
    bool   debug;
    double vif_enhn_gain_limit;
    bool   vif_skip_scale0;

    unsigned index;
    VmafDictionary *feature_name_dict;
} IntegerVifStateMetal;

/* Options mirror the CPU integer_vif.c table EXACTLY (debug,
 * vif_enhn_gain_limit/egl, vif_skip_scale0/ssclz). The integer path has no
 * vif_kernelscale / vif_sigma_nsq option (those are float_vif-only). */
static const VmafOption options[] = {
    {
        .name        = "debug",
        .help        = "debug mode: enable additional output",
        .offset      = offsetof(IntegerVifStateMetal, debug),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name        = "vif_enhn_gain_limit",
        .alias       = "egl",
        .help        = "enhancement gain imposed on vif, must be >= 1.0, "
                       "where 1.0 means the gain is completely disabled",
        .offset      = offsetof(IntegerVifStateMetal, vif_enhn_gain_limit),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 100.0},
        .min         = 1.0,
        .max         = 100.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_skip_scale0",
        .alias       = "ssclz",
        .help        = "when set, skip scale 0 calculations",
        .offset      = offsetof(IntegerVifStateMetal, vif_skip_scale0),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0},
};

static int build_pipelines(IntegerVifStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn_c8  = [lib newFunctionWithName:@"integer_vif_compute_8"];
    id<MTLFunction> fn_c16 = [lib newFunctionWithName:@"integer_vif_compute_16"];
    id<MTLFunction> fn_d8  = [lib newFunctionWithName:@"integer_vif_decimate_8"];
    id<MTLFunction> fn_d16 = [lib newFunctionWithName:@"integer_vif_decimate_16"];
    if (fn_c8 == nil || fn_c16 == nil || fn_d8 == nil || fn_d16 == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso_c8  =
        [device newComputePipelineStateWithFunction:fn_c8  error:&err];
    id<MTLComputePipelineState> pso_c16 =
        [device newComputePipelineStateWithFunction:fn_c16 error:&err];
    id<MTLComputePipelineState> pso_d8  =
        [device newComputePipelineStateWithFunction:fn_d8  error:&err];
    id<MTLComputePipelineState> pso_d16 =
        [device newComputePipelineStateWithFunction:fn_d16 error:&err];
    if (pso_c8 == nil || pso_c16 == nil || pso_d8 == nil || pso_d16 == nil) { return -ENODEV; }

    s->pso_compute_8   = (__bridge_retained void *)pso_c8;
    s->pso_compute_16  = (__bridge_retained void *)pso_c16;
    s->pso_decimate_8  = (__bridge_retained void *)pso_d8;
    s->pso_decimate_16 = (__bridge_retained void *)pso_d16;
    return 0;
}

static void release_buffers(IntegerVifStateMetal *s)
{
    for (int i = 0; i < IVIF_SCALES; ++i) {
        if (s->wg_accum[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->wg_accum[i];
            s->wg_accum[i] = NULL;
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
    if (s->log2_buf) {
        (void)(__bridge_transfer id<MTLBuffer>)s->log2_buf;
        s->log2_buf = NULL;
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

/* Regenerate the VIF log2 LUT identically to integer_vif.c::log_generate:
 * entry i = round(log2f(VIF_LOG2_TABLE_OFFSET + i) * 2048), where
 * VIF_LOG2_TABLE_OFFSET = 0x8000. */
static void fill_log2_table(uint16_t *t)
{
    for (unsigned i = 0; i < VIF_LOG2_TABLE_SIZE; ++i) {
        t[i] = (uint16_t)lround(log2f((float)(0x8000u + i)) * 2048.0f);
    }
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    IntegerVifStateMetal *s = (IntegerVifStateMetal *)fex->priv;

    s->width  = w;
    s->height = h;
    s->bpc    = bpc;

    /* Per-scale geometry: halve each dimension (no border crop). */
    s->scale_w[0] = w;
    s->scale_h[0] = h;
    for (int i = 1; i < IVIF_SCALES; ++i) {
        s->scale_w[i] = s->scale_w[i - 1] / 2u;
        s->scale_h[i] = s->scale_h[i - 1] / 2u;
    }
    if (s->scale_w[IVIF_SCALES - 1] == 0u || s->scale_h[IVIF_SCALES - 1] == 0u) {
        return -EINVAL;
    }

    for (int i = 0; i < IVIF_SCALES; ++i) {
        const unsigned gx = (s->scale_w[i] + (unsigned)IVIF_BX - 1u) / (unsigned)IVIF_BX;
        const unsigned gy = (s->scale_h[i] + (unsigned)IVIF_BY - 1u) / (unsigned)IVIF_BY;
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

        /* log2 LUT upload. */
        id<MTLBuffer> lb = [device newBufferWithLength:VIF_LOG2_TABLE_SIZE * sizeof(uint16_t)
                                              options:MTLResourceStorageModeShared];
        if (lb == nil) { err = -ENOMEM; goto fail_bufs; }
        fill_log2_table((uint16_t *)[lb contents]);
        s->log2_buf = (__bridge_retained void *)lb;

        /* Ping-pong uint16 pyramid sized to scale 1 (largest decimated). */
        const size_t pbytes = (size_t)s->scale_w[1] * s->scale_h[1] * sizeof(uint16_t);
        for (int i = 0; i < 2; ++i) {
            id<MTLBuffer> pr = [device newBufferWithLength:pbytes
                                                  options:MTLResourceStorageModeShared];
            id<MTLBuffer> pd = [device newBufferWithLength:pbytes
                                                  options:MTLResourceStorageModeShared];
            if (pr == nil || pd == nil) { err = -ENOMEM; goto fail_bufs; }
            s->pyr_ref[i] = (__bridge_retained void *)pr;
            s->pyr_dis[i] = (__bridge_retained void *)pd;
        }

        for (int i = 0; i < IVIF_SCALES; ++i) {
            const size_t ab = (size_t)s->wg_count[i] * sizeof(VifWgAccumHost);
            id<MTLBuffer> ba = [device newBufferWithLength:ab
                                                  options:MTLResourceStorageModeShared];
            if (ba == nil) { err = -ENOMEM; goto fail_bufs; }
            s->wg_accum[i] = (__bridge_retained void *)ba;
        }

        err = build_pipelines(s, device);
    }
    if (err != 0) { goto fail_pso; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    if (s->pso_decimate_16) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_decimate_16;
        s->pso_decimate_16 = NULL;
    }
    if (s->pso_decimate_8) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_decimate_8;
        s->pso_decimate_8 = NULL;
    }
    if (s->pso_compute_16) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_compute_16;
        s->pso_compute_16 = NULL;
    }
    if (s->pso_compute_8) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_compute_8;
        s->pso_compute_8 = NULL;
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
 * shared upload buffer. */
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

static void encode_compute_8(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                             id<MTLBuffer> raw_ref, id<MTLBuffer> raw_dis, id<MTLBuffer> log2_buf,
                             id<MTLBuffer> wg, unsigned width, unsigned height, unsigned raw_stride,
                             unsigned grid_x, float egl)
{
    const uint32_t params[4] = {width, height, raw_stride, grid_x};
    const float cfgf[4]      = {egl, 0.0f, 0.0f, 0.0f};

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:raw_ref  offset:0 atIndex:0];
    [enc setBuffer:raw_dis  offset:0 atIndex:1];
    [enc setBuffer:log2_buf offset:0 atIndex:2];
    [enc setBuffer:wg       offset:0 atIndex:3];
    [enc setBytes:params length:sizeof(params) atIndex:4];
    [enc setBytes:cfgf   length:sizeof(cfgf)   atIndex:5];
    MTLSize tg   = MTLSizeMake(IVIF_BX, IVIF_BY, 1);
    MTLSize grid = MTLSizeMake((width + IVIF_BX - 1u) / IVIF_BX,
                               (height + IVIF_BY - 1u) / IVIF_BY, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

static void encode_compute_16(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                              id<MTLBuffer> ref_f, id<MTLBuffer> dis_f, id<MTLBuffer> log2_buf,
                              id<MTLBuffer> wg, int scale, unsigned width, unsigned height,
                              unsigned f_stride, unsigned grid_x, unsigned bpc, float egl)
{
    const uint32_t params[4] = {width, height, f_stride, grid_x};
    const uint32_t cfg2[4]   = {(uint32_t)scale, bpc, 0u, 0u};
    const float cfgf[4]      = {egl, 0.0f, 0.0f, 0.0f};

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_f    offset:0 atIndex:0];
    [enc setBuffer:dis_f    offset:0 atIndex:1];
    [enc setBuffer:log2_buf offset:0 atIndex:2];
    [enc setBuffer:wg       offset:0 atIndex:3];
    [enc setBytes:params length:sizeof(params) atIndex:4];
    [enc setBytes:cfg2   length:sizeof(cfg2)   atIndex:5];
    [enc setBytes:cfgf   length:sizeof(cfgf)   atIndex:6];
    MTLSize tg   = MTLSizeMake(IVIF_BX, IVIF_BY, 1);
    MTLSize grid = MTLSizeMake((width + IVIF_BX - 1u) / IVIF_BX,
                               (height + IVIF_BY - 1u) / IVIF_BY, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

static void encode_decimate_8(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                              id<MTLBuffer> raw_ref, id<MTLBuffer> raw_dis, id<MTLBuffer> ref_out,
                              id<MTLBuffer> dis_out, unsigned out_w, unsigned out_h, unsigned in_w,
                              unsigned in_h, unsigned raw_stride, unsigned out_stride)
{
    const uint32_t dims[4] = {out_w, out_h, in_w, in_h};
    const uint32_t cfg[4]  = {raw_stride, out_stride, 0u, 0u};

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:raw_ref offset:0 atIndex:0];
    [enc setBuffer:raw_dis offset:0 atIndex:1];
    [enc setBuffer:ref_out offset:0 atIndex:2];
    [enc setBuffer:dis_out offset:0 atIndex:3];
    [enc setBytes:dims length:sizeof(dims) atIndex:4];
    [enc setBytes:cfg  length:sizeof(cfg)  atIndex:5];
    MTLSize tg   = MTLSizeMake(IVIF_BX, IVIF_BY, 1);
    MTLSize grid = MTLSizeMake((out_w + IVIF_BX - 1u) / IVIF_BX,
                               (out_h + IVIF_BY - 1u) / IVIF_BY, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

static void encode_decimate_16(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                               id<MTLBuffer> ref_in, id<MTLBuffer> dis_in, id<MTLBuffer> ref_out,
                               id<MTLBuffer> dis_out, unsigned out_w, unsigned out_h, unsigned in_w,
                               unsigned in_h, unsigned in_stride, unsigned out_stride,
                               int filt_scale, unsigned bpc)
{
    const uint32_t dims[4] = {out_w, out_h, in_w, in_h};
    const uint32_t cfg[4]  = {in_stride, out_stride, (uint32_t)filt_scale, bpc};

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_in  offset:0 atIndex:0];
    [enc setBuffer:dis_in  offset:0 atIndex:1];
    [enc setBuffer:ref_out offset:0 atIndex:2];
    [enc setBuffer:dis_out offset:0 atIndex:3];
    [enc setBytes:dims length:sizeof(dims) atIndex:4];
    [enc setBytes:cfg  length:sizeof(cfg)  atIndex:5];
    MTLSize tg   = MTLSizeMake(IVIF_BX, IVIF_BY, 1);
    MTLSize grid = MTLSizeMake((out_w + IVIF_BX - 1u) / IVIF_BX,
                               (out_h + IVIF_BY - 1u) / IVIF_BY, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    IntegerVifStateMetal *s = (IntegerVifStateMetal *)fex->priv;
    s->index = index;

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;

    id<MTLComputePipelineState> pso_c8  = (__bridge id<MTLComputePipelineState>)s->pso_compute_8;
    id<MTLComputePipelineState> pso_c16 = (__bridge id<MTLComputePipelineState>)s->pso_compute_16;
    id<MTLComputePipelineState> pso_d8  = (__bridge id<MTLComputePipelineState>)s->pso_decimate_8;
    id<MTLComputePipelineState> pso_d16 = (__bridge id<MTLComputePipelineState>)s->pso_decimate_16;
    id<MTLBuffer> raw_ref  = (__bridge id<MTLBuffer>)s->raw_ref;
    id<MTLBuffer> raw_dis  = (__bridge id<MTLBuffer>)s->raw_dis;
    id<MTLBuffer> log2_buf = (__bridge id<MTLBuffer>)s->log2_buf;

    fill_raw_plane(ref_pic,  raw_ref, s->width, s->height, s->bpc);
    fill_raw_plane(dist_pic, raw_dis, s->width, s->height, s->bpc);

    const unsigned bpp = (s->bpc <= 8u) ? 1u : 2u;
    const unsigned raw_stride = s->width * bpp;
    const float egl = (float)s->vif_enhn_gain_limit;
    const bool is8 = (s->bpc <= 8u);

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    /* Scale 0: compute directly from raw (skipped at host level if
     * vif_skip_scale0, but cheap to always run; collect suppresses it). */
    {
        id<MTLBuffer> wg0 = (__bridge id<MTLBuffer>)s->wg_accum[0];
        const unsigned grid_x = (s->scale_w[0] + (unsigned)IVIF_BX - 1u) / (unsigned)IVIF_BX;
        if (is8) {
            encode_compute_8(cmd, pso_c8, raw_ref, raw_dis, log2_buf, wg0, s->scale_w[0],
                             s->scale_h[0], raw_stride, grid_x, egl);
        } else {
            /* HBD scale 0 reads the raw uint16 plane via the _16 compute
             * kernel; raw_stride is in bytes, but the _16 kernel indexes by
             * ushort stride, so pass width (ushorts) as the f_stride. */
            encode_compute_16(cmd, pso_c16, raw_ref, raw_dis, log2_buf, wg0, 0, s->scale_w[0],
                              s->scale_h[0], s->width, grid_x, s->bpc, egl);
        }
    }

    /* Scales 1..3: decimate (prev dims) then compute (this scale). */
    for (int n = 1; n < IVIF_SCALES; ++n) {
        const int dst_slot = (n - 1) % 2;
        const bool prev_is_raw = (n == 1);
        id<MTLBuffer> ref_out = (dst_slot == 0) ? (__bridge id<MTLBuffer>)s->pyr_ref[0]
                                                : (__bridge id<MTLBuffer>)s->pyr_ref[1];
        id<MTLBuffer> dis_out = (dst_slot == 0) ? (__bridge id<MTLBuffer>)s->pyr_dis[0]
                                                : (__bridge id<MTLBuffer>)s->pyr_dis[1];
        const unsigned out_stride = s->scale_w[n];

        if (prev_is_raw) {
            if (is8) {
                encode_decimate_8(cmd, pso_d8, raw_ref, raw_dis, ref_out, dis_out, s->scale_w[1],
                                  s->scale_h[1], s->scale_w[0], s->scale_h[0], raw_stride,
                                  out_stride);
            } else {
                /* HBD scale 0->1 reads the raw uint16 plane via the _16
                 * decimate kernel (filt_scale = 1, in_stride = width). */
                encode_decimate_16(cmd, pso_d16, raw_ref, raw_dis, ref_out, dis_out, s->scale_w[1],
                                   s->scale_h[1], s->scale_w[0], s->scale_h[0], s->width, out_stride,
                                   1, s->bpc);
            }
        } else {
            id<MTLBuffer> ref_in = (dst_slot == 0) ? (__bridge id<MTLBuffer>)s->pyr_ref[1]
                                                   : (__bridge id<MTLBuffer>)s->pyr_ref[0];
            id<MTLBuffer> dis_in = (dst_slot == 0) ? (__bridge id<MTLBuffer>)s->pyr_dis[1]
                                                   : (__bridge id<MTLBuffer>)s->pyr_dis[0];
            const unsigned in_stride = s->scale_w[n - 1];
            encode_decimate_16(cmd, pso_d16, ref_in, dis_in, ref_out, dis_out, s->scale_w[n],
                               s->scale_h[n], s->scale_w[n - 1], s->scale_h[n - 1], in_stride,
                               out_stride, n, s->bpc);
        }

        id<MTLBuffer> wgn = (__bridge id<MTLBuffer>)s->wg_accum[n];
        const unsigned grid_x = (s->scale_w[n] + (unsigned)IVIF_BX - 1u) / (unsigned)IVIF_BX;
        encode_compute_16(cmd, pso_c16, ref_out, dis_out, log2_buf, wgn, n, s->scale_w[n],
                          s->scale_h[n], out_stride, grid_x, s->bpc, egl);
    }

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

/* Sum per-WG int64 partials per scale and apply the exact CPU final formula:
 *   num = accum_num_log/2048.0 +
 *         (accum_den_non_log - (accum_num_non_log/16384.0)/65025.0)
 *   den = accum_den_log/2048.0 + accum_den_non_log
 * (float casts mirror integer_vif.c's `num[0]` / `den[0]` writes, which are
 *  float; we keep double here and cast at the score division.) */
static void scale_num_den(const IntegerVifStateMetal *s, int scale, double *num, double *den)
{
    const VifWgAccumHost *p = (const VifWgAccumHost *)[(__bridge id<MTLBuffer>)s->wg_accum[scale]
                                                          contents];
    int64_t num_log = 0, den_log = 0, num_non_log = 0, den_non_log = 0;
    for (unsigned j = 0; j < s->wg_count[scale]; ++j) {
        num_log     += p[j].num_log;
        den_log     += p[j].den_log;
        num_non_log += p[j].num_non_log;
        den_non_log += p[j].den_non_log;
    }
    /* CPU writes num[0]/den[0] as float; reproduce that single-precision
     * rounding so the per-scale ratio matches the CPU bit-for-bit. */
    const float fnum = (float)(num_log / 2048.0 +
                               (den_non_log - ((double)num_non_log / 16384.0) / 65025.0));
    const float fden = (float)(den_log / 2048.0 + (double)den_non_log);
    *num = (double)fnum;
    *den = (double)fden;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    IntegerVifStateMetal *s = (IntegerVifStateMetal *)fex->priv;

    double num[IVIF_SCALES], den[IVIF_SCALES];
    for (int i = 0; i < IVIF_SCALES; ++i) {
        scale_num_den(s, i, &num[i], &den[i]);
    }

    int err = 0;
    /* vif_skip_scale0: emit 0.0 for scale-0 score, mirroring
     * integer_vif.c::write_scores. */
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_vif_scale0_score",
        s->vif_skip_scale0 ? 0.0 : num[0] / den[0], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_vif_scale1_score",
                                                   num[1] / den[1], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_vif_scale2_score",
                                                   num[2] / den[2], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_vif_scale3_score",
                                                   num[3] / den[3], index);

    if (!s->debug) {
        return err;
    }

    double score_num, score_den;
    if (s->vif_skip_scale0) {
        score_num = num[1] + num[2] + num[3];
        score_den = den[1] + den[2] + den[3];
    } else {
        score_num = num[0] + num[1] + num[2] + num[3];
        score_den = den[0] + den[1] + den[2] + den[3];
    }
    const double score = (score_den == 0.0) ? 1.0 : score_num / score_den;

    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif", score, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_num", score_num, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_den", score_den, index);
    if (s->vif_skip_scale0) {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "integer_vif_num_scale0", 0.0, index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "integer_vif_den_scale0", -1.0, index);
    } else {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "integer_vif_num_scale0", num[0], index);
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       "integer_vif_den_scale0", den[0], index);
    }
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_num_scale1", num[1], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_den_scale1", den[1], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_num_scale2", num[2], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_den_scale2", den[2], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_num_scale3", num[3], index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_vif_den_scale3", den[3], index);
    return err;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    IntegerVifStateMetal *s = (IntegerVifStateMetal *)fex->priv;

    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_decimate_16) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_decimate_16;
        s->pso_decimate_16 = NULL;
    }
    if (s->pso_decimate_8) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_decimate_8;
        s->pso_decimate_8 = NULL;
    }
    if (s->pso_compute_16) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_compute_16;
        s->pso_compute_16 = NULL;
    }
    if (s->pso_compute_8) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_compute_8;
        s->pso_compute_8 = NULL;
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

static const char *provided_features[] = {"VMAF_integer_feature_vif_scale0_score",
                                          "VMAF_integer_feature_vif_scale1_score",
                                          "VMAF_integer_feature_vif_scale2_score",
                                          "VMAF_integer_feature_vif_scale3_score",
                                          "integer_vif",
                                          "integer_vif_num",
                                          "integer_vif_den",
                                          "integer_vif_num_scale0",
                                          "integer_vif_den_scale0",
                                          "integer_vif_num_scale1",
                                          "integer_vif_den_scale1",
                                          "integer_vif_num_scale2",
                                          "integer_vif_den_scale2",
                                          "integer_vif_num_scale3",
                                          "integer_vif_den_scale3",
                                          NULL};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL / Metal feature extractor uses (ADR-0361
 * Metal backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_integer_vif_metal = {
    .name              = "integer_vif_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = NULL,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(IntegerVifStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = IVIF_SCALES + (IVIF_SCALES - 1),
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
