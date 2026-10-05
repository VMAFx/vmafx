/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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
 *    4. Host (collect): sum per-WG int64 partials per scale, round each
 *       scale's (num, den) to float as integer_vif.c::vif_store_residuals()
 *       does, and build the score set as integer_vif.c::write_scores() does:
 *       the frame sums add the rounded values and the emitter divides each
 *       scale in single precision (single_precision_ratio) ->
 *       VMAF_integer_feature_vif_scale{0..3}_score (+ optional debug
 *       aggregates).
 *
 *  log2 LUT: integer_vif.c fills a VIF_LOG2_TABLE_SIZE (32768) entry uint16
 *  table host-side with vif_log2_table_generate() (feature/vif_log2_table.h).
 *  init() fills a Shared device buffer with the same call, which the compute
 *  kernels read through, so the GPU log2_32 / log2_64 accessors are bit-exact
 *  to the CPU's.
 *
 *  Multi-scale buffer strategy: like float_vif_metal, two ping-pong uint16
 *  buffers per side (ref/dis), sized to scale 1 (the largest decimated
 *  scale); decimate writes scale n into the slot read by scale n+1's
 *  decimate. Per-scale int64 (num_log, den_log, num_non_log, den_non_log)
 *  partials are separate Shared buffers sized to that scale's WG count. All
 *  buffers are allocated once in init() (zero per-frame heap traffic).
 *
 *  Param contract: vif_enhn_gain_limit reaches the kernels as a
 *  VmafMtlGainLimit (metal_integer_vif_gain.h), its fp64 parts formed once per
 *  frame on the host: the CPU truncates two integers from an fp64 gain, and
 *  the kernels return those integers (ADR-1432, ported by ADR-1498). debug
 *  and vif_skip_scale0 are host-side only.
 *
 *  Minimum frame size (T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29, the HIP
 *  guard of ADR-1381 on ADR-1324, ported by ADR-1498): every scale reflects
 *  its filter taps once, which stays inside the plane only while
 *  floor(dim / 2^s) exceeds the tap half-width, so frames below
 *  vif_metal_min_dim() = 16 go to the CPU `vif` under model dispatch
 *  (check_context_metal()) and a direct request for the twin fails init()
 *  with -EINVAL before any device work.
 */

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* feature_extractor.h pulled in before the extern "C" block (libc++ on
 * Xcode 16.x emits "templates must have C++ linkage" when <atomic> is
 * dragged into an extern "C" scope — same workaround as the other .mm). */
#include "feature_extractor.h"
/* Plain C with <math.h> / <stdint.h> only, both included above. */
#include "feature/vif_log2_table.h"

extern "C" {
#include "dict.h"
#include "feature_collector.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "integer_vif.h"
#include "libvmaf/picture.h"
#include "log.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

#include "../../metal/objc_handle.h"

/* The gain terms' arithmetic and the VmafMtlGainLimit layout the kernels read. */
#include "metal_integer_vif_gain.h"


#define IVIF_SCALES 4
#define IVIF_BX     16
#define IVIF_BY     16

namespace {

/* Host-side mirror of the int64 per-WG accumulator written by the kernel
 * (struct VifWgAccum in integer_vif.metal — same field order / 64-bit
 * widths so the Shared-buffer layout matches byte-for-byte). */
using VifWgAccumHost = struct VifWgAccumHost {
    int64_t num_log;
    int64_t den_log;
    int64_t num_non_log;
    int64_t den_non_log;
};

using IntegerVifStateMetal = struct IntegerVifStateMetal {
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
};
} // namespace

namespace {

/* Options mirror the CPU integer_vif.c table EXACTLY (debug,
 * vif_enhn_gain_limit/egl, vif_skip_scale0/ssclz). The integer path has no
 * vif_kernelscale / vif_sigma_nsq option (those are float_vif-only). */
const VmafOption options[] = {
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
    {.name=nullptr},
};

int build_pipelines(IntegerVifStateMetal *s, id<MTLDevice> device)
{
    int load_rc = 0;
    id<MTLLibrary> const lib = vmaf_metal_library_load(device, &load_rc);
    if (lib == nil) { return load_rc; }
    NSError *err = nil;

    id<MTLFunction> const fn_c8  = [lib newFunctionWithName:@"integer_vif_compute_8"];
    id<MTLFunction> const fn_c16 = [lib newFunctionWithName:@"integer_vif_compute_16"];
    id<MTLFunction> const fn_d8  = [lib newFunctionWithName:@"integer_vif_decimate_8"];
    id<MTLFunction> const fn_d16 = [lib newFunctionWithName:@"integer_vif_decimate_16"];
    if (fn_c8 == nil || fn_c16 == nil || fn_d8 == nil || fn_d16 == nil) { return -ENODEV; }

    id<MTLComputePipelineState> const pso_c8  =
        [device newComputePipelineStateWithFunction:fn_c8  error:&err];
    id<MTLComputePipelineState> const pso_c16 =
        [device newComputePipelineStateWithFunction:fn_c16 error:&err];
    id<MTLComputePipelineState> const pso_d8  =
        [device newComputePipelineStateWithFunction:fn_d8  error:&err];
    id<MTLComputePipelineState> const pso_d16 =
        [device newComputePipelineStateWithFunction:fn_d16 error:&err];
    if (pso_c8 == nil || pso_c16 == nil || pso_d8 == nil || pso_d16 == nil) { return -ENODEV; }

    s->pso_compute_8   = (__bridge_retained void *)pso_c8;
    s->pso_compute_16  = (__bridge_retained void *)pso_c16;
    s->pso_decimate_8  = (__bridge_retained void *)pso_d8;
    s->pso_decimate_16 = (__bridge_retained void *)pso_d16;
    return 0;
}

void release_buffers(IntegerVifStateMetal *s)
{
    for (auto & i : s->wg_accum) {
        if (i) {
            (void)(__bridge_transfer id<MTLBuffer>)i;
            i = nullptr;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (s->pyr_ref[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->pyr_ref[i];
            s->pyr_ref[i] = nullptr;
        }
        if (s->pyr_dis[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->pyr_dis[i];
            s->pyr_dis[i] = nullptr;
        }
    }
    if (s->log2_buf) {
        (void)(__bridge_transfer id<MTLBuffer>)s->log2_buf;
        s->log2_buf = nullptr;
    }
    if (s->raw_ref) {
        (void)(__bridge_transfer id<MTLBuffer>)s->raw_ref;
        s->raw_ref = nullptr;
    }
    if (s->raw_dis) {
        (void)(__bridge_transfer id<MTLBuffer>)s->raw_dis;
        s->raw_dis = nullptr;
    }
}

/*
 * Smallest frame dimension every scale can filter, from the CPU's filter
 * widths (integer_vif.h::vif_filter1d_width), as vif_hip_min_dim() derives it
 * (ADR-1381). Scale s works on floor(dim / 2^s) samples and the CPU reflects
 * each filter tap once; a tap half-width of k stays inside the plane only
 * while floor(dim / 2^s) >= k + 1, i.e. dim >= (k + 1) << s. The scale
 * filters {17, 9, 5, 3} need {9, 10, 12, 16} and the decimation filters (the
 * next scale's, {9, 5, 3}) need {5, 6, 8}, so the bound is 16, as for
 * vif_sycl, vif_cuda and vif_hip. Below it the kernels would read other
 * samples than the CPU does.
 */
unsigned vif_metal_min_dim()
{
    unsigned min_dim = 1u;
    for (unsigned scale = 0u; scale < (unsigned)IVIF_SCALES; scale++) {
        const unsigned need = (((unsigned)vif_filter1d_width[scale] / 2u) + 1u) << scale;
        const unsigned rd_need =
            (scale + 1u < (unsigned)IVIF_SCALES)
                ? (((unsigned)vif_filter1d_width[scale + 1u] / 2u) + 1u) << scale
                : 1u;
        min_dim = (need > min_dim) ? need : min_dim;
        min_dim = (rd_need > min_dim) ? rd_need : min_dim;
    }
    return min_dim;
}

/* ADR-1324 first-picture gate: model dispatch computes frames below
 * vif_metal_min_dim() with the CPU `vif` extractor instead of this twin. */
int check_context_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                               unsigned bpc, unsigned w, unsigned h)
{
    (void)fex;
    (void)pix_fmt;
    (void)bpc;
    const unsigned min_dim = vif_metal_min_dim();
    return (w < min_dim || h < min_dim) ? -ENOTSUP : 0;
}

/* Per-scale geometry: halve each dimension (no border crop); every scale is
 * at least two samples wide and high from the minimum on. */
void vif_metal_init_geometry(IntegerVifStateMetal *s, unsigned bpc, unsigned w, unsigned h)
{
    s->width  = w;
    s->height = h;
    s->bpc    = bpc;
    s->scale_w[0] = w;
    s->scale_h[0] = h;
    for (int i = 1; i < IVIF_SCALES; ++i) {
        s->scale_w[i] = s->scale_w[i - 1] / 2u;
        s->scale_h[i] = s->scale_h[i - 1] / 2u;
    }
    for (int i = 0; i < IVIF_SCALES; ++i) {
        const unsigned gx = (s->scale_w[i] + (unsigned)IVIF_BX - 1u) / (unsigned)IVIF_BX;
        const unsigned gy = (s->scale_h[i] + (unsigned)IVIF_BY - 1u) / (unsigned)IVIF_BY;
        s->wg_count[i] = gx * gy;
    }
}

void *vif_metal_new_buffer(id<MTLDevice> device, size_t bytes)
{
    id<MTLBuffer> const buf = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    return (buf == nil) ? nullptr : (__bridge_retained void *)buf;
}

/* Raw uploads, the CPU's log2 table, the ping-pong pyramid and the per-scale
 * partials, all Shared, once (zero per-frame heap traffic). */
int vif_metal_alloc_buffers(IntegerVifStateMetal *s, id<MTLDevice> device)
{
    const size_t raw_bytes = (size_t)s->width * s->height * ((s->bpc <= 8u) ? 1u : 2u);
    s->raw_ref = vif_metal_new_buffer(device, raw_bytes);
    s->raw_dis = vif_metal_new_buffer(device, raw_bytes);
    s->log2_buf = vif_metal_new_buffer(device, VIF_LOG2_TABLE_SIZE * sizeof(uint16_t));
    if (s->raw_ref == nullptr || s->raw_dis == nullptr || s->log2_buf == nullptr) { return -ENOMEM; }
    /* The CPU's table: integer_vif.c fills its state with the same call. */
    vif_log2_table_generate((uint16_t *)[(__bridge id<MTLBuffer>)s->log2_buf contents]);

    /* Ping-pong uint16 pyramid sized to scale 1 (largest decimated). */
    const size_t pbytes = (size_t)s->scale_w[1] * s->scale_h[1] * sizeof(uint16_t);
    for (int i = 0; i < 2; ++i) {
        s->pyr_ref[i] = vif_metal_new_buffer(device, pbytes);
        s->pyr_dis[i] = vif_metal_new_buffer(device, pbytes);
        if (s->pyr_ref[i] == nullptr || s->pyr_dis[i] == nullptr) { return -ENOMEM; }
    }
    for (int i = 0; i < IVIF_SCALES; ++i) {
        const size_t ab = (size_t)s->wg_count[i] * sizeof(VifWgAccumHost);
        s->wg_accum[i] = vif_metal_new_buffer(device, ab);
        if (s->wg_accum[i] == nullptr) { return -ENOMEM; }
    }
    return 0;
}

/* Tear down everything init() may have set up; every step tolerates a handle
 * that was never created, so this serves a failed init() and close(). */
int vif_metal_release(IntegerVifStateMetal *s)
{
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
    void **const psos[] = {&s->pso_decimate_16, &s->pso_decimate_8, &s->pso_compute_16,
                     &s->pso_compute_8};
    for (auto & pso : psos) {
        if (*pso != nullptr) {
            (void)(__bridge_transfer id<MTLComputePipelineState>)*pso;
            *pso = nullptr;
        }
    }
    release_buffers(s);
    if (s->feature_name_dict != nullptr) {
        const int derr = vmaf_dictionary_free(&s->feature_name_dict);
        if (derr != 0 && rc == 0) { rc = derr; }
    }
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = nullptr;
    return rc;
}

int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    IntegerVifStateMetal *s = (IntegerVifStateMetal *)fex->priv;

    /* Direct `integer_vif_metal` requests get no fallback (ADR-1324): refuse
     * before any device work, so there is nothing to free. */
    const unsigned min_dim = vif_metal_min_dim();
    if (w < min_dim || h < min_dim) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "integer_vif_metal requires width >= %u and height >= %u (got %ux%u); the "
                 "CPU extractor `vif` computes smaller frames\n",
                 min_dim, min_dim, w, h);
        return -EINVAL;
    }
    vif_metal_init_geometry(s, bpc, w, h);

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err == 0) {
        err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    }
    void *const dh = (err == 0) ? vmaf_metal_context_device_handle(s->ctx) : nullptr;
    if (err == 0 && dh == nullptr) {
        err = -ENODEV;
    }
    if (err == 0) {
        err = vif_metal_alloc_buffers(s, (__bridge id<MTLDevice>)dh);
    }
    if (err == 0) {
        err = build_pipelines(s, (__bridge id<MTLDevice>)dh);
    }
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == nullptr) { err = -ENOMEM; }
    }
    if (err != 0) {
        (void)vif_metal_release(s);
    }
    return err;
}

/* Copy the ref/dis Y-plane raw bytes (packed, stride = width*bpp) into the
 * shared upload buffer. */
void fill_raw_plane(VmafPicture *pic, id<MTLBuffer> dst, unsigned w, unsigned h,
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

void encode_compute_8(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                             id<MTLBuffer> raw_ref, id<MTLBuffer> raw_dis, id<MTLBuffer> log2_buf,
                             id<MTLBuffer> wg, unsigned width, unsigned height, unsigned raw_stride,
                             unsigned grid_x, VmafMtlGainLimit egl)
{
    const uint32_t params[4] = {width, height, raw_stride, grid_x};

    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:raw_ref  offset:0 atIndex:0];
    [enc setBuffer:raw_dis  offset:0 atIndex:1];
    [enc setBuffer:log2_buf offset:0 atIndex:2];
    [enc setBuffer:wg       offset:0 atIndex:3];
    [enc setBytes:params length:sizeof(params) atIndex:4];
    [enc setBytes:&egl   length:sizeof(egl)    atIndex:5];
    MTLSize const tg   = MTLSizeMake(IVIF_BX, IVIF_BY, 1);
    MTLSize const grid = MTLSizeMake((width + IVIF_BX - 1u) / IVIF_BX,
                               (height + IVIF_BY - 1u) / IVIF_BY, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

void encode_compute_16(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                              id<MTLBuffer> ref_f, id<MTLBuffer> dis_f, id<MTLBuffer> log2_buf,
                              id<MTLBuffer> wg, int scale, unsigned width, unsigned height,
                              unsigned f_stride, unsigned grid_x, unsigned bpc, VmafMtlGainLimit egl)
{
    const uint32_t params[4] = {width, height, f_stride, grid_x};
    const uint32_t cfg2[4]   = {(uint32_t)scale, bpc, 0u, 0u};

    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_f    offset:0 atIndex:0];
    [enc setBuffer:dis_f    offset:0 atIndex:1];
    [enc setBuffer:log2_buf offset:0 atIndex:2];
    [enc setBuffer:wg       offset:0 atIndex:3];
    [enc setBytes:params length:sizeof(params) atIndex:4];
    [enc setBytes:cfg2   length:sizeof(cfg2)   atIndex:5];
    [enc setBytes:&egl   length:sizeof(egl)    atIndex:6];
    MTLSize const tg   = MTLSizeMake(IVIF_BX, IVIF_BY, 1);
    MTLSize const grid = MTLSizeMake((width + IVIF_BX - 1u) / IVIF_BX,
                               (height + IVIF_BY - 1u) / IVIF_BY, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

void encode_decimate_8(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                              id<MTLBuffer> raw_ref, id<MTLBuffer> raw_dis, id<MTLBuffer> ref_out,
                              id<MTLBuffer> dis_out, unsigned out_w, unsigned out_h, unsigned in_w,
                              unsigned in_h, unsigned raw_stride, unsigned out_stride)
{
    const uint32_t dims[4] = {out_w, out_h, in_w, in_h};
    const uint32_t cfg[4]  = {raw_stride, out_stride, 0u, 0u};

    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:raw_ref offset:0 atIndex:0];
    [enc setBuffer:raw_dis offset:0 atIndex:1];
    [enc setBuffer:ref_out offset:0 atIndex:2];
    [enc setBuffer:dis_out offset:0 atIndex:3];
    [enc setBytes:dims length:sizeof(dims) atIndex:4];
    [enc setBytes:cfg  length:sizeof(cfg)  atIndex:5];
    MTLSize const tg   = MTLSizeMake(IVIF_BX, IVIF_BY, 1);
    MTLSize const grid = MTLSizeMake((out_w + IVIF_BX - 1u) / IVIF_BX,
                               (out_h + IVIF_BY - 1u) / IVIF_BY, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

void encode_decimate_16(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                               id<MTLBuffer> ref_in, id<MTLBuffer> dis_in, id<MTLBuffer> ref_out,
                               id<MTLBuffer> dis_out, unsigned out_w, unsigned out_h, unsigned in_w,
                               unsigned in_h, unsigned in_stride, unsigned out_stride,
                               int filt_scale, unsigned bpc)
{
    const uint32_t dims[4] = {out_w, out_h, in_w, in_h};
    const uint32_t cfg[4]  = {in_stride, out_stride, (uint32_t)filt_scale, bpc};

    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_in  offset:0 atIndex:0];
    [enc setBuffer:dis_in  offset:0 atIndex:1];
    [enc setBuffer:ref_out offset:0 atIndex:2];
    [enc setBuffer:dis_out offset:0 atIndex:3];
    [enc setBytes:dims length:sizeof(dims) atIndex:4];
    [enc setBytes:cfg  length:sizeof(cfg)  atIndex:5];
    MTLSize const tg   = MTLSizeMake(IVIF_BX, IVIF_BY, 1);
    MTLSize const grid = MTLSizeMake((out_w + IVIF_BX - 1u) / IVIF_BX,
                               (out_h + IVIF_BY - 1u) / IVIF_BY, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

/* Scale 0: compute directly from raw (skipped at host level if
 * vif_skip_scale0, but cheap to always run; collect suppresses it). */
void vif_metal_encode_scale0(const IntegerVifStateMetal *s, id<MTLCommandBuffer> cmd,
                                    VmafMtlGainLimit egl)
{
    id<MTLBuffer> const raw_ref  = (__bridge id<MTLBuffer>)s->raw_ref;
    id<MTLBuffer> const raw_dis  = (__bridge id<MTLBuffer>)s->raw_dis;
    id<MTLBuffer> const log2_buf = (__bridge id<MTLBuffer>)s->log2_buf;
    id<MTLBuffer> const wg0 = (__bridge id<MTLBuffer>)s->wg_accum[0];
    const unsigned grid_x = (s->scale_w[0] + (unsigned)IVIF_BX - 1u) / (unsigned)IVIF_BX;
    if (s->bpc <= 8u) {
        encode_compute_8(cmd, (__bridge id<MTLComputePipelineState>)s->pso_compute_8, raw_ref,
                         raw_dis, log2_buf, wg0, s->scale_w[0], s->scale_h[0], s->width, grid_x,
                         egl);
    } else {
        /* HBD scale 0 reads the raw uint16 plane via the _16 compute
         * kernel; raw_stride is in bytes, but the _16 kernel indexes by
         * ushort stride, so pass width (ushorts) as the f_stride. */
        encode_compute_16(cmd, (__bridge id<MTLComputePipelineState>)s->pso_compute_16, raw_ref,
                          raw_dis, log2_buf, wg0, 0, s->scale_w[0], s->scale_h[0], s->width,
                          grid_x, s->bpc, egl);
    }
}

/* Scale n of 1..3: decimate (previous scale's dims) into ping-pong slot
 * (n - 1) % 2, then compute this scale's statistic from it. */
void vif_metal_encode_scale(const IntegerVifStateMetal *s, id<MTLCommandBuffer> cmd, int n,
                                   VmafMtlGainLimit egl)
{
    id<MTLComputePipelineState> const pso_d16 = (__bridge id<MTLComputePipelineState>)s->pso_decimate_16;
    id<MTLBuffer> const raw_ref = (__bridge id<MTLBuffer>)s->raw_ref;
    id<MTLBuffer> const raw_dis = (__bridge id<MTLBuffer>)s->raw_dis;
    const int dst_slot = (n - 1) % 2;
    id<MTLBuffer> const ref_out = (__bridge id<MTLBuffer>)s->pyr_ref[dst_slot];
    id<MTLBuffer> const dis_out = (__bridge id<MTLBuffer>)s->pyr_dis[dst_slot];
    const unsigned out_stride = s->scale_w[n];

    if (n == 1 && s->bpc <= 8u) {
        encode_decimate_8(cmd, (__bridge id<MTLComputePipelineState>)s->pso_decimate_8, raw_ref,
                          raw_dis, ref_out, dis_out, s->scale_w[1], s->scale_h[1], s->scale_w[0],
                          s->scale_h[0], s->width, out_stride);
    } else if (n == 1) {
        /* HBD scale 0->1 reads the raw uint16 plane via the _16
         * decimate kernel (filt_scale = 1, in_stride = width). */
        encode_decimate_16(cmd, pso_d16, raw_ref, raw_dis, ref_out, dis_out, s->scale_w[1],
                           s->scale_h[1], s->scale_w[0], s->scale_h[0], s->width, out_stride, 1,
                           s->bpc);
    } else {
        id<MTLBuffer> const ref_in = (__bridge id<MTLBuffer>)s->pyr_ref[1 - dst_slot];
        id<MTLBuffer> const dis_in = (__bridge id<MTLBuffer>)s->pyr_dis[1 - dst_slot];
        encode_decimate_16(cmd, pso_d16, ref_in, dis_in, ref_out, dis_out, s->scale_w[n],
                           s->scale_h[n], s->scale_w[n - 1], s->scale_h[n - 1], s->scale_w[n - 1],
                           out_stride, n, s->bpc);
    }

    id<MTLBuffer> const wgn = (__bridge id<MTLBuffer>)s->wg_accum[n];
    const unsigned grid_x = (s->scale_w[n] + (unsigned)IVIF_BX - 1u) / (unsigned)IVIF_BX;
    encode_compute_16(cmd, (__bridge id<MTLComputePipelineState>)s->pso_compute_16, ref_out,
                      dis_out, (__bridge id<MTLBuffer>)s->log2_buf, wgn, n, s->scale_w[n],
                      s->scale_h[n], out_stride, grid_x, s->bpc, egl);
}

int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    IntegerVifStateMetal *s = (IntegerVifStateMetal *)fex->priv;
    s->index = index;

    void *const qh = vmaf_metal_context_queue_handle(s->ctx);
    if (qh == nullptr) { return -ENODEV; }
    id<MTLCommandQueue> const queue = (__bridge id<MTLCommandQueue>)qh;

    fill_raw_plane(ref_pic,  (__bridge id<MTLBuffer>)s->raw_ref, s->width, s->height, s->bpc);
    fill_raw_plane(dist_pic, (__bridge id<MTLBuffer>)s->raw_dis, s->width, s->height, s->bpc);

    const VmafMtlGainLimit egl = vmaf_mtl_ivif_make_gain_limit(s->vif_enhn_gain_limit);
    id<MTLCommandBuffer> const cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    vif_metal_encode_scale0(s, cmd, egl);
    for (int n = 1; n < IVIF_SCALES; ++n) {
        vif_metal_encode_scale(s, cmd, n, egl);
    }

    [cmd commit];
    [cmd waitUntilCompleted];
    return ([cmd status] == MTLCommandBufferStatusCompleted) ? 0 : -EIO;
}

/* Sum per-WG int64 partials per scale and apply the exact CPU final formula:
 *   num = accum_num_log/2048.0 +
 *         (accum_den_non_log - (accum_num_non_log/16384.0)/65025.0)
 *   den = accum_den_log/2048.0 + accum_den_non_log
 * Each result is rounded to float, as integer_vif.c::vif_store_residuals()
 * stores it; the double the caller receives holds that float exactly.
 * collect_fex_metal() then divides in single precision, as the CPU does. */
void scale_num_den(const IntegerVifStateMetal *s, int scale, double *num, double *den)
{
    const VifWgAccumHost *p = (const VifWgAccumHost *)[(__bridge id<MTLBuffer>)s->wg_accum[scale]
                                                          contents];
    int64_t num_log = 0;
    int64_t den_log = 0;
    int64_t num_non_log = 0;
    int64_t den_non_log = 0;
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

int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    IntegerVifStateMetal *const s = (IntegerVifStateMetal *)fex->priv;

    double num[IVIF_SCALES];
    double den[IVIF_SCALES];
    for (int i = 0; i < IVIF_SCALES; ++i) {
        scale_num_den(s, i, &num[i], &den[i]);
    }

    /* integer_vif.c::write_scores(): the emitter divides each scale's
     * float-rounded sums in single precision. A double division gives a
     * score up to half an fp32 step away from the CPU's on every frame. */
    VmafVifScoreSet output = {
        .single_precision_ratio = true,
        .skip_scale0 = s->vif_skip_scale0,
        .debug = s->debug,
    };
    const unsigned scale_start = s->vif_skip_scale0 ? 1u : 0u;
    for (unsigned scale = 0u; scale < IVIF_SCALES; ++scale) {
        output.scale[(size_t)scale * 2u] = num[scale];
        output.scale[((size_t)scale * 2u) + 1u] = den[scale];
        if (scale >= scale_start) {
            output.score_num += num[scale];
            output.score_den += den[scale];
        }
    }
    output.score = output.score_den > 0.0 ? output.score_num / output.score_den : NAN;
    return vmaf_vif_emit_scores(feature_collector, s->feature_name_dict, "integer_vif_metal",
                                &output, VMAF_VIF_INTEGER_NAMES, index);
}

int close_fex_metal(VmafFeatureExtractor *fex)
{
    return vif_metal_release((IntegerVifStateMetal *)fex->priv);
}

const char *provided_features[] = {"VMAF_integer_feature_vif_scale0_score",
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
                                          nullptr};
} // namespace

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
    .flush             = nullptr,
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
    /* ADR-1381 / ADR-1498: frames below the 16-pixel filter footprint run on
     * the CPU `vif` under model dispatch (ADR-1324 gate). */
    .context_check         = check_context_metal,
    .context_fallback_name = "vif",
};
} /* extern "C" */
