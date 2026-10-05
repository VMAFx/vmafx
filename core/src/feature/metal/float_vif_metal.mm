/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_vif feature extractor on the Metal backend (ADR-1498). Objective-C++
 *  wrapper that drives the four MSL kernels of `float_vif.metal`
 *  (float_vif_vertical, float_vif_compute, float_vif_row_sums,
 *  float_vif_decimate). Port of the SYCL twin
 *  `core/src/feature/sycl/float_vif_sycl.cpp` (ADR-1422; the CUDA and HIP
 *  twins are ADR-1412 and ADR-1444): the twin returns the CPU extractor's
 *  values bit for bit.
 *
 *  Per frame:
 *    1. Host copies the ref/dis Y planes into Shared MTLBuffers (raw bytes).
 *       A prescale that changes the frame size (vif_prescale) is done on the
 *       host by the CPU's own picture_copy() and vif_scale_frame_s(), and
 *       scale 0 then reads the prescaled float planes.
 *    2. Scale 0: float_vif_vertical, float_vif_compute (horizontal pass and
 *       the pixel statistic), float_vif_row_sums.
 *    3. Scales 1..3: float_vif_decimate over the previous scale's plane with
 *       this scale's filter, then the same three kernels.
 *    4. One command buffer, one wait. The host adds the row sums of each scale
 *       top to bottom in fp32 (vif_statistic_s()'s outer loop) and widens the
 *       two floats, as compute_vif() does, then emits through
 *       vmaf_vif_emit_scores() with the CPU's per-scale floors.
 *
 *  The four filters come from vif_get_filter() for (float)vif_kernelscale, as
 *  float_vif.c derives them, and are kernel arguments: no kernel holds a tap.
 *  Every kernelscale of the CPU's range runs (a filter has at most 128 taps).
 *
 *  Metallib resolution: embedded-blob pattern shared by every Metal feature
 *  extractor -- reads the __TEXT,__metallib section compiled via xcrun from
 *  float_vif.metal.
 */

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <utility>

/* feature_extractor.h pulled in before the extern "C" block (libc++ on
 * Xcode 16.x emits "templates must have C++ linkage" when <atomic> is
 * dragged into an extern "C" scope -- same workaround as the other .mm). */
#include "feature_extractor.h"

#include "metal/metal_float_vif_math.h"

extern "C" {
#include "dict.h"
#include "feature_collector.h"
#include "feature_name.h"
#include "feature/nonfinite_score.h"
#include "log.h"
#include "mem.h"
#include "picture_copy.h"
#include "vif_options.h"
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
#define FVIF_TG     16
#define FVIF_ROW_TG 64

/* The kernels of float_vif.metal, in the order of FloatVifStateMetal::pso. */
enum {
    FVIF_KERNEL_VERTICAL = 0,
    FVIF_KERNEL_COMPUTE,
    FVIF_KERNEL_ROWS,
    FVIF_KERNEL_DECIMATE,
    FVIF_KERNELS
};

static NSString *const kernel_names[FVIF_KERNELS] = {
    @"float_vif_vertical", @"float_vif_compute", @"float_vif_row_sums", @"float_vif_decimate"};

using FloatVifStateMetal = struct FloatVifStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;
    bool lc_open;

    void *pso[FVIF_KERNELS];

    /* Raw ref/dis Y-plane uploads (Shared storage, packed to width*bpp). */
    void *raw_ref;
    void *raw_dis;
    /* Prescaled scale-0 float planes (only when vif_prescale changes the
     * frame size) and the host float planes they are scaled from. */
    void *scaled_ref;
    void *scaled_dis;
    float *host_ref;
    float *host_dis;

    /* Ping-pong decimated float pyramid (sized to scale 1, the largest
     * decimated scale): [side][slot]. side 0=ref, 1=dis. */
    void *pyr_ref[2];
    void *pyr_dis[2];

    /* Per-pixel scratch of the scale being computed (scale-0 size; one
     * command buffer runs the scales in order): the five vertical moments
     * and the (num, den) terms. */
    void *moments;
    void *terms;
    /* [num rows | den rows] of every scale at row_offset[scale]. */
    void *rows;
    size_t row_offset[FVIF_SCALES];
    size_t row_floats;

    unsigned width;
    unsigned height;
    unsigned bpc;
    bool prescaled;
    size_t float_stride;
    enum vif_scaling_method scaling_method;

    unsigned scale_w[FVIF_SCALES];
    unsigned scale_h[FVIF_SCALES];

    /* vif_get_filter() of the four scales, as float_vif.c::init() takes them. */
    float taps[FVIF_SCALES][128];
    unsigned tap_count[FVIF_SCALES];
    VmafMtlFvifStatisticArgs statistic;

    /* Option table of the CPU float_vif.c, field for field. */
    bool   debug;
    double vif_enhn_gain_limit;
    double vif_kernelscale;
    double vif_prescale;
    double vif_scale1_min_val;
    double vif_scale2_min_val;
    double vif_scale3_min_val;
    char  *vif_prescale_method;
    double vif_sigma_nsq;
    bool   vif_skip_scale0;

    unsigned index;
    VmafDictionary *feature_name_dict;
};

/* The option table of float_vif.c (core/src/feature/float_vif.c), name, alias,
 * type, default, range and flags. Every option runs on the device: the
 * kernelscale through the taps, the prescale on the host. */
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
        .help        = "enhancement gain imposed on vif, must be >= 1.0, "
                       "where 1.0 means the gain is completely disabled",
        .alias       = "egl",
        .offset      = offsetof(FloatVifStateMetal, vif_enhn_gain_limit),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = DEFAULT_VIF_ENHN_GAIN_LIMIT},
        .min         = 1.0,
        .max         = DEFAULT_VIF_ENHN_GAIN_LIMIT,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_kernelscale",
        .help        = "scaling factor for the gaussian kernel (2.0 means "
                       "multiplying the standard deviation by 2 and enlarge "
                       "the kernel size accordingly",
        .alias       = "ks",
        .offset      = offsetof(FloatVifStateMetal, vif_kernelscale),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = DEFAULT_VIF_KERNELSCALE},
        .min         = 0.1,
        .max         = 4.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_prescale",
        .help        = "scaling factor for the frame (2.0 means "
                       "making the image twice as large on each dimension)",
        .alias       = "ps",
        .offset      = offsetof(FloatVifStateMetal, vif_prescale),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = DEFAULT_VIF_PRESCALE},
        .min         = 0.1,
        .max         = 4.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_scale1_min_val",
        .help        = "minimum value allowed; smaller values will be set to this value",
        .alias       = "s1miv",
        .offset      = offsetof(FloatVifStateMetal, vif_scale1_min_val),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 0.0},
        .min         = 0.0,
        .max         = 1.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_scale2_min_val",
        .help        = "minimum value allowed; smaller values will be set to this value",
        .alias       = "s2miv",
        .offset      = offsetof(FloatVifStateMetal, vif_scale2_min_val),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 0.0},
        .min         = 0.0,
        .max         = 1.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_scale3_min_val",
        .help        = "minimum value allowed; smaller values will be set to this value",
        .alias       = "s3miv",
        .offset      = offsetof(FloatVifStateMetal, vif_scale3_min_val),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 0.0},
        .min         = 0.0,
        .max         = 1.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_prescale_method",
        .help        = "scaling method for the frame, supported options: "
                       "[nearest, bilinear, bicubic, lanczos4]",
        .alias       = "pm",
        .offset      = offsetof(FloatVifStateMetal, vif_prescale_method),
        .type        = VMAF_OPT_TYPE_STRING,
        .default_val = {.s = DEFAULT_VIF_PRESCALE_METHOD},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_sigma_nsq",
        .help        = "neural noise variance",
        .alias       = "snsq",
        .offset      = offsetof(FloatVifStateMetal, vif_sigma_nsq),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 2.0},
        .min         = 0.0,
        .max         = 5.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "vif_skip_scale0",
        .help        = "when set, skip scale 0 calculations",
        .alias       = "ssclz",
        .offset      = offsetof(FloatVifStateMetal, vif_skip_scale0),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {.name=nullptr},
};

static id<MTLBuffer> shared_buffer(id<MTLDevice> device, size_t bytes)
{
    return [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
}

static id<MTLBuffer> buffer_of(void *handle)
{
    return (__bridge id<MTLBuffer>)handle;
}

static void release_handle(void **handle)
{
    if (*handle) {
        (void)(__bridge_transfer id)*handle;
        *handle = nullptr;
    }
}

static void release_buffers(FloatVifStateMetal *s)
{
    for (int i = 0; i < 2; ++i) {
        release_handle(&s->pyr_ref[i]);
        release_handle(&s->pyr_dis[i]);
    }
    release_handle(&s->raw_ref);
    release_handle(&s->raw_dis);
    release_handle(&s->scaled_ref);
    release_handle(&s->scaled_dis);
    release_handle(&s->moments);
    release_handle(&s->terms);
    release_handle(&s->rows);
    if (s->host_ref) {
        aligned_free(s->host_ref);
        s->host_ref = nullptr;
    }
    if (s->host_dis) {
        aligned_free(s->host_dis);
        s->host_dis = nullptr;
    }
}

static int build_pipelines(FloatVifStateMetal *s, id<MTLDevice> device)
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

    for (int i = 0; i < FVIF_KERNELS; ++i) {
        id<MTLFunction> fn = [lib newFunctionWithName:kernel_names[i]];
        if (fn == nil) { return -ENODEV; }
        id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:fn
                                                                                error:&err];
        if (pso == nil) { return -ENODEV; }
        s->pso[i] = (__bridge_retained void *)pso;
    }
    return 0;
}

/* The four filters, as float_vif.c::alloc_buffers() derives them, and the
 * statistic's constants. vif_get_min_dim() has bounded the frame, and a
 * filter has at most 128 taps (69 at the largest kernelscale of 4.0). */
static int init_filters(FloatVifStateMetal *s)
{
    for (int scale = 0; scale < FVIF_SCALES; ++scale) {
        const int width = vif_get_filter_size(scale, (float)s->vif_kernelscale);
        if (width < 1 || width > 128) { return -EINVAL; }
        s->tap_count[scale] = (unsigned)width;
        vif_get_filter(s->taps[scale], scale, (float)s->vif_kernelscale);
    }
    s->statistic = vmaf_mtl_fvif_statistic_args(s->vif_sigma_nsq, s->vif_enhn_gain_limit);
    return 0;
}

/* Frame geometry of the four scales and the guards of float_vif.c::init():
 * the raw frame and the prescaled frame must each hold the four-scale ladder
 * (Netflix/vmaf#1582). */
static int init_geometry(FloatVifStateMetal *s, unsigned w, unsigned h)
{
    const int vif_min_dim = vif_get_min_dim((float)s->vif_kernelscale);
    if (std::cmp_less(w ,vif_min_dim) || std::cmp_less(h ,vif_min_dim)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_vif_metal: width and height must be >= %d for the "
                 "four-scale VIF ladder (got %ux%u)\n",
                 vif_min_dim, w, h);
        return -EINVAL;
    }
    if (vif_get_scaling_method(s->vif_prescale_method, &s->scaling_method)) {
        return -EINVAL;
    }
    const size_t scaled_w = (size_t)lround(w * s->vif_prescale);
    const size_t scaled_h = (size_t)lround(h * s->vif_prescale);
    if (std::cmp_less(scaled_w ,vif_min_dim) || std::cmp_less(scaled_h ,vif_min_dim)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "float_vif_metal: scaled width and height must be >= %d for the "
                 "four-scale VIF ladder (got %zux%zu)\n",
                 vif_min_dim, scaled_w, scaled_h);
        return -EINVAL;
    }
    s->prescaled = scaled_w != (size_t)w || scaled_h != (size_t)h;
    s->float_stride = ALIGN_CEIL((size_t)w * sizeof(float));
    /* Per-scale geometry: halve each dimension (no border crop, the CPU's
     * VIF_OPT_HANDLE_BORDERS). */
    s->scale_w[0] = (unsigned)scaled_w;
    s->scale_h[0] = (unsigned)scaled_h;
    for (int i = 1; i < FVIF_SCALES; ++i) {
        s->scale_w[i] = s->scale_w[i - 1] / 2u;
        s->scale_h[i] = s->scale_h[i - 1] / 2u;
    }
    return 0;
}

static int alloc_planes(FloatVifStateMetal *s, id<MTLDevice> device)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const size_t raw_bytes = (size_t)s->width * s->height * bpp;
    id<MTLBuffer> rr = shared_buffer(device, raw_bytes);
    id<MTLBuffer> rd = shared_buffer(device, raw_bytes);
    if (rr == nil || rd == nil) { return -ENOMEM; }
    s->raw_ref = (__bridge_retained void *)rr;
    s->raw_dis = (__bridge_retained void *)rd;

    /* Ping-pong float pyramid sized to scale 1 (largest decimated). */
    const size_t fbytes = (size_t)s->scale_w[1] * s->scale_h[1] * sizeof(float);
    for (int i = 0; i < 2; ++i) {
        id<MTLBuffer> pr = shared_buffer(device, fbytes);
        id<MTLBuffer> pd = shared_buffer(device, fbytes);
        if (pr == nil || pd == nil) { return -ENOMEM; }
        s->pyr_ref[i] = (__bridge_retained void *)pr;
        s->pyr_dis[i] = (__bridge_retained void *)pd;
    }
    if (!s->prescaled) { return 0; }

    const size_t sbytes = (size_t)s->scale_w[0] * s->scale_h[0] * sizeof(float);
    id<MTLBuffer> sr = shared_buffer(device, sbytes);
    id<MTLBuffer> sd = shared_buffer(device, sbytes);
    if (sr == nil || sd == nil) { return -ENOMEM; }
    s->scaled_ref = (__bridge_retained void *)sr;
    s->scaled_dis = (__bridge_retained void *)sd;
    s->host_ref = (float *)aligned_malloc(s->float_stride * s->height, 32);
    s->host_dis = (float *)aligned_malloc(s->float_stride * s->height, 32);
    return (s->host_ref && s->host_dis) ? 0 : -ENOMEM;
}

static int alloc_scratch(FloatVifStateMetal *s, id<MTLDevice> device)
{
    const size_t plane = (size_t)s->scale_w[0] * s->scale_h[0];
    id<MTLBuffer> mo = shared_buffer(device, plane * VMAF_MTL_FVIF_MOMENTS * sizeof(float));
    id<MTLBuffer> te = shared_buffer(device, plane * VMAF_MTL_FVIF_TERM_FLOATS * sizeof(float));
    size_t row_floats = 0;
    for (int i = 0; i < FVIF_SCALES; ++i) {
        s->row_offset[i] = row_floats;
        row_floats += 2u * (size_t)s->scale_h[i];
    }
    s->row_floats = row_floats;
    id<MTLBuffer> ro = shared_buffer(device, row_floats * sizeof(float));
    if (mo == nil || te == nil || ro == nil) { return -ENOMEM; }
    s->moments = (__bridge_retained void *)mo;
    s->terms = (__bridge_retained void *)te;
    s->rows = (__bridge_retained void *)ro;
    return 0;
}

static int alloc_device_state(FloatVifStateMetal *s)
{
    void *const dh = vmaf_metal_context_device_handle(s->ctx);
    if (dh == nullptr) { return -ENODEV; }
    id<MTLDevice> device = (__bridge id<MTLDevice>)dh;
    int err = alloc_planes(s, device);
    if (err != 0) { return err; }
    err = alloc_scratch(s, device);
    if (err != 0) { return err; }
    return build_pipelines(s, device);
}

static void release_device_state(FloatVifStateMetal *s)
{
    for (auto & i : s->pso) {
        release_handle(&i);
    }
    release_buffers(s);
    if (s->lc_open) {
        (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
        s->lc_open = false;
    }
    if (s->ctx) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = nullptr;
    }
}

static int open_device(FloatVifStateMetal *s)
{
    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }
    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { return err; }
    s->lc_open = true;
    return alloc_device_state(s);
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatVifStateMetal *s = (FloatVifStateMetal *)fex->priv;

    s->width  = w;
    s->height = h;
    s->bpc    = bpc;

    int err = init_geometry(s, w, h);
    if (err != 0) { return err; }
    err = init_filters(s);
    if (err != 0) { return err; }
    err = open_device(s);
    if (err != 0) {
        release_device_state(s);
        return err;
    }
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == nullptr) {
        release_device_state(s);
        return -ENOMEM;
    }
    return 0;
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

/* float_vif.c::extract()'s prescale: picture_copy() to float, then
 * vif_scale_frame_s() to the scaled size, written into the scale-0 plane. */
static void prescale_plane(const FloatVifStateMetal *s, VmafPicture *pic, float *host,
                           id<MTLBuffer> dst)
{
    picture_copy(host, (ptrdiff_t)s->float_stride, pic, -128, pic->bpc, 0);
    vif_scale_frame_s(s->scaling_method, host, (float *)[dst contents], (int)pic->w[0],
                      (int)pic->h[0], (int)(s->float_stride / sizeof(float)),
                      (int)s->scale_w[0], (int)s->scale_h[0], (int)s->scale_w[0]);
}

static void bind_buffers(id<MTLComputeCommandEncoder> enc, void *const *buffers, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        [enc setBuffer:buffer_of(buffers[i]) offset:0 atIndex:i];
    }
}

static void dispatch_grid(id<MTLComputeCommandEncoder> enc, unsigned w, unsigned h)
{
    const MTLSize tg   = MTLSizeMake(FVIF_TG, FVIF_TG, 1);
    const MTLSize grid = MTLSizeMake((w + FVIF_TG - 1u) / FVIF_TG, (h + FVIF_TG - 1u) / FVIF_TG, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
}

/* The plane a kernel of `scale` reads: the raw frame, or float planes. */
using FvifInput = struct FvifInput {
    void *ref;
    void *dis;
    bool raw;
};

static VmafMtlFvifInputArgs input_args(const FloatVifStateMetal *s, bool raw)
{
    VmafMtlFvifInputArgs input;
    input.raw        = raw ? 1u : 0u;
    input.raw_stride = s->width * ((s->bpc <= 8u) ? 1u : 2u);
    input.bpc        = s->bpc;
    return input;
}

/* float_vif_vertical for `scale` over `in`. */
static void encode_vertical(const FloatVifStateMetal *s, id<MTLCommandBuffer> cmd, int scale,
                            FvifInput in)
{
    const VmafMtlFvifFilterArgs filter = {.width=s->scale_w[scale], .height=s->scale_h[scale],
                                          .taps=s->tap_count[scale],
                                          .plane=s->scale_w[scale] * s->scale_h[scale]};
    const VmafMtlFvifInputArgs input = input_args(s, in.raw);
    void *const buffers[5] = {s->raw_ref, s->raw_dis, in.ref, in.dis, s->moments};

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso[FVIF_KERNEL_VERTICAL]];
    /* A float plane of the raw path is unused: bind the raw plane there. */
    for (unsigned i = 0; i < 5u; ++i) {
        void *handle = buffers[i] ? buffers[i] : s->raw_ref;
        [enc setBuffer:buffer_of(handle) offset:0 atIndex:i];
    }
    [enc setBytes:s->taps[scale] length:sizeof(s->taps[scale]) atIndex:5];
    [enc setBytes:&filter length:sizeof(filter) atIndex:6];
    [enc setBytes:&input length:sizeof(input) atIndex:7];
    dispatch_grid(enc, filter.width, filter.height);
    [enc endEncoding];
}

/* float_vif_compute for `scale`. */
static void encode_compute(const FloatVifStateMetal *s, id<MTLCommandBuffer> cmd, int scale)
{
    const VmafMtlFvifFilterArgs filter = {.width=s->scale_w[scale], .height=s->scale_h[scale],
                                          .taps=s->tap_count[scale],
                                          .plane=s->scale_w[scale] * s->scale_h[scale]};
    void *const buffers[2] = {s->moments, s->terms};

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso[FVIF_KERNEL_COMPUTE]];
    bind_buffers(enc, buffers, 2);
    [enc setBytes:s->taps[scale] length:sizeof(s->taps[scale]) atIndex:2];
    [enc setBytes:&filter length:sizeof(filter) atIndex:3];
    [enc setBytes:&s->statistic length:sizeof(s->statistic) atIndex:4];
    dispatch_grid(enc, filter.width, filter.height);
    [enc endEncoding];
}

/* float_vif_row_sums for `scale`: one thread per row. */
static void encode_rows(const FloatVifStateMetal *s, id<MTLCommandBuffer> cmd, int scale)
{
    const VmafMtlFvifRowArgs row = {.width=s->scale_w[scale], .height=s->scale_h[scale]};

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso[FVIF_KERNEL_ROWS]];
    [enc setBuffer:buffer_of(s->terms) offset:0 atIndex:0];
    [enc setBuffer:buffer_of(s->rows) offset:s->row_offset[scale] * sizeof(float) atIndex:1];
    [enc setBytes:&row length:sizeof(row) atIndex:2];
    const MTLSize tg   = MTLSizeMake(FVIF_ROW_TG, 1, 1);
    const MTLSize grid = MTLSizeMake((row.height + FVIF_ROW_TG - 1u) / FVIF_ROW_TG, 1, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

/* float_vif_decimate: this scale's filter over `in` (the previous scale's
 * plane), written to ping-pong slot `out_slot`. */
static void encode_decimate(const FloatVifStateMetal *s, id<MTLCommandBuffer> cmd, int scale,
                            FvifInput in, int out_slot)
{
    const VmafMtlFvifDecimateArgs dims = {.in_width=s->scale_w[scale - 1], .in_height=s->scale_h[scale - 1],
                                          .out_width=s->scale_w[scale], .out_height=s->scale_h[scale],
                                          .taps=s->tap_count[scale]};
    const VmafMtlFvifInputArgs input = input_args(s, in.raw);
    void *const buffers[6] = {s->raw_ref, s->raw_dis, in.ref, in.dis, s->pyr_ref[out_slot],
                              s->pyr_dis[out_slot]};

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso[FVIF_KERNEL_DECIMATE]];
    for (unsigned i = 0; i < 6u; ++i) {
        void *handle = buffers[i] ? buffers[i] : s->raw_ref;
        [enc setBuffer:buffer_of(handle) offset:0 atIndex:i];
    }
    [enc setBytes:s->taps[scale] length:sizeof(s->taps[scale]) atIndex:6];
    [enc setBytes:&dims length:sizeof(dims) atIndex:7];
    [enc setBytes:&input length:sizeof(input) atIndex:8];
    dispatch_grid(enc, dims.out_width, dims.out_height);
    [enc endEncoding];
}

/* The plane scale 0 reads: raw, or the prescaled float planes. */
static FvifInput scale0_input(const FloatVifStateMetal *s)
{
    FvifInput in = {.ref=s->scaled_ref, .dis=s->scaled_dis, .raw=!s->prescaled};
    return in;
}

/* Everything of one frame in one command buffer. */
static void encode_frame(const FloatVifStateMetal *s, id<MTLCommandBuffer> cmd)
{
    FvifInput in = scale0_input(s);
    encode_vertical(s, cmd, 0, in);
    encode_compute(s, cmd, 0);
    encode_rows(s, cmd, 0);
    for (int n = 1; n < FVIF_SCALES; ++n) {
        const int out_slot = (n - 1) % 2;
        encode_decimate(s, cmd, n, in, out_slot);
        in.ref = s->pyr_ref[out_slot];
        in.dis = s->pyr_dis[out_slot];
        in.raw = false;
        encode_vertical(s, cmd, n, in);
        encode_compute(s, cmd, n);
        encode_rows(s, cmd, n);
    }
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    FloatVifStateMetal *s = (FloatVifStateMetal *)fex->priv;
    s->index = index;

    void *const qh = vmaf_metal_context_queue_handle(s->ctx);
    if (qh == nullptr) { return -ENODEV; }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;

    fill_raw_plane(ref_pic,  buffer_of(s->raw_ref), s->width, s->height, s->bpc);
    fill_raw_plane(dist_pic, buffer_of(s->raw_dis), s->width, s->height, s->bpc);
    if (s->prescaled) {
        prescale_plane(s, ref_pic, s->host_ref, buffer_of(s->scaled_ref));
        prescale_plane(s, dist_pic, s->host_dis, buffer_of(s->scaled_dis));
    }

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }
    encode_frame(s, cmd);
    [cmd commit];
    [cmd waitUntilCompleted];
    return cmd.status == MTLCommandBufferStatusCompleted ? 0 : -EIO;
}

/* vif_statistic_s()'s outer loop: the row sums added top to bottom into one
 * fp32 accumulator per output; compute_vif() widens the two floats. */
static void sum_rows(const FloatVifStateMetal *s, double scores[8])
{
    const float *rows = (const float *)[buffer_of(s->rows) contents];
    for (int scale = 0; scale < FVIF_SCALES; ++scale) {
        const float *num_rows = rows + s->row_offset[scale];
        const float *den_rows = num_rows + s->scale_h[scale];
        float num = 0.0f;
        float den = 0.0f;
        for (unsigned y = 0; y < s->scale_h[scale]; ++y) {
            num += num_rows[y];
            den += den_rows[y];
        }
        scores[2 * scale + 0] = (double)num;
        scores[2 * scale + 1] = (double)den;
    }
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    FloatVifStateMetal *s = (FloatVifStateMetal *)fex->priv;
    double scores[8];
    sum_rows(s, scores);

    VmafVifScoreSet output = {
        .minimum = {s->vif_scale1_min_val, s->vif_scale2_min_val, s->vif_scale3_min_val},
        .use_minimums = true,
        .skip_scale0 = s->vif_skip_scale0,
        .debug = s->debug,
    };
    const size_t start = s->vif_skip_scale0 ? 2u : 0u;
    for (size_t i = 0u; i < 8u; ++i) {
        output.scale[i] = scores[i];
        if (i >= start) {
            output.score_num += i % 2u == 0u ? scores[i] : 0.0;
            output.score_den += i % 2u != 0u ? scores[i] : 0.0;
        }
    }
    output.score = output.score_den > 0.0 ? output.score_num / output.score_den : NAN;
    return vmaf_vif_emit_scores(feature_collector, s->feature_name_dict, "float_vif_metal",
                                &output, VMAF_VIF_FLOAT_NAMES, index);
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatVifStateMetal *s = (FloatVifStateMetal *)fex->priv;
    int rc = 0;
    if (s->lc_open) {
        rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
        s->lc_open = false;
    }
    release_device_state(s);
    if (s->feature_name_dict) {
        int const derr = vmaf_dictionary_free(&s->feature_name_dict);
        if (derr != 0 && rc == 0) { rc = derr; }
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
                                          nullptr};

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
    .flush             = nullptr,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(FloatVifStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 3 * FVIF_SCALES + (FVIF_SCALES - 1),
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
