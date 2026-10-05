/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_moment feature extractor on the Metal backend (T8-1e / ADR-0421).
 *  Dispatches `float_moment_kernel_{8,16}bpc` from float_moment.metal.
 *
 *  The kernel emits four exact uint64 workgroup sums as eight uint32 lo/hi
 *  buffers. The host reconstructs and adds them in uint64, converts each total
 *  once, then applies the high-bit-depth power-of-two scaler used by the CPU
 *  picture-copy path (ADR-1498; the CUDA host's operations, ADR-1453).
 *
 *  A frame whose second-moment sum can pass 2^53 units of 1 / scaler^2
 *  (vmaf_mtl_msum_may_round(): 16-bit frames of more than 2^21 pixels) needs
 *  the CPU's rounded double sum rather than the exact one (ADR-1497). The
 *  frame's encoder then also runs the five kernels of float_moment.metal that
 *  form it (metal_float_moment_sum.h), and collect() takes the two
 *  second-moment sums from the walk's result. Other frames run the frame
 *  kernel alone.
 *  Feature names: float_moment_ref1st, float_moment_dis1st,
 *                 float_moment_ref2nd, float_moment_dis2nd.
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
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

#include "../../metal/objc_handle.h"

#include "metal_float_moment_sum.h"


#define FM_PARTIAL_BUFFER_COUNT 8u

/* The rounded-sum pass (ADR-1497): five kernels, four buffers. */
#define FM_SUM_KERNEL_COUNT 5u
#define FM_SUM_BUFFER_COUNT 4u
#define FM_SUM_ROW_TOTALS 0u /* uint64 [plane][row] */
#define FM_SUM_ROW_PLANS 1u  /* int32 [plane][row] */
#define FM_SUM_ROW_UNITS 2u  /* int64 [plane][row][2] */
#define FM_SUM_FRAME 3u      /* uint64 [4]: ref1, dis1, ref2, dis2 */

namespace {

const char *const fm_sum_kernel_names[FM_SUM_KERNEL_COUNT] = {
    "float_moment_plane_sums",  "float_moment_row_totals", "float_moment_row_plans",
    "float_moment_row_units",   "float_moment_ordered_totals",
};
} // namespace

using FloatMomentStateMetal = struct FloatMomentStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb[FM_PARTIAL_BUFFER_COUNT]; /* 4 uint64 lo/hi pairs */
    VmafMetalContext *ctx;
    void *pso_8bpc;
    void *pso_16bpc;

    bool rounds; /* vmaf_mtl_msum_may_round(): the rounded-sum pass runs */
    VmafMetalKernelBuffer sum_buf[FM_SUM_BUFFER_COUNT];
    void *pso_sum[FM_SUM_KERNEL_COUNT];

    size_t plane_bytes;
    size_t partials_count; /* grid_w × grid_h */
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;

    VmafDictionary *feature_name_dict;
};

namespace {

const VmafOption options[] = {{.name=nullptr}};

void release_sum_pipelines(FloatMomentStateMetal *s)
{
    for (auto & k : s->pso_sum) {
        if (k) {
            (void)(__bridge_transfer id<MTLComputePipelineState>)k;
            k = nullptr;
        }
    }
}

/* The five kernels of the rounded-sum pass. Each runs one threadgroup of
 * VMAF_MTL_MSUM_LANES lanes, so a pipeline that cannot is refused (fail
 * closed, never a smaller group). */
int build_sum_pipelines(FloatMomentStateMetal *s, id<MTLDevice> device, id<MTLLibrary> lib)
{
    NSError *err = nil;
    for (unsigned k = 0u; k < FM_SUM_KERNEL_COUNT; ++k) {
        NSString *name = [NSString stringWithUTF8String:fm_sum_kernel_names[k]];
        id<MTLFunction> fn = [lib newFunctionWithName:name];
        if (fn == nil) {
            release_sum_pipelines(s);
            return -ENODEV;
        }
        id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:fn
                                                                                error:&err];
        if (pso == nil || [pso maxTotalThreadsPerThreadgroup] < VMAF_MTL_MSUM_LANES) {
            release_sum_pipelines(s);
            return -ENODEV;
        }
        s->pso_sum[k] = (__bridge_retained void *)pso;
    }
    return 0;
}

int build_pipelines(FloatMomentStateMetal *s, id<MTLDevice> device)
{
    int load_rc = 0;
    id<MTLLibrary> lib = vmaf_metal_library_load(device, &load_rc);
    if (lib == nil) { return load_rc; }
    NSError *err = nil;

    id<MTLFunction> fn8 = [lib newFunctionWithName:@"float_moment_kernel_8bpc"];
    id<MTLFunction> fn16 = [lib newFunctionWithName:@"float_moment_kernel_16bpc"];
    if (fn8 == nil || fn16 == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso8 =
        [device newComputePipelineStateWithFunction:fn8 error:&err];
    id<MTLComputePipelineState> pso16 =
        [device newComputePipelineStateWithFunction:fn16 error:&err];
    if (pso8 == nil || pso16 == nil) { return -ENODEV; }

    if (s->rounds) {
        const int sum_err = build_sum_pipelines(s, device, lib);
        if (sum_err != 0) { return sum_err; }
    }

    s->pso_8bpc = (__bridge_retained void *)pso8;
    s->pso_16bpc = (__bridge_retained void *)pso16;
    return 0;
}

/* Row arrays and the frame sums of the rounded-sum pass, for a frame that
 * can pass 2^53 units; none otherwise. */
void free_sum_buffers(FloatMomentStateMetal *s)
{
    for (auto & b : s->sum_buf) {
        (void)vmaf_metal_kernel_buffer_free(&b, s->ctx);
    }
}

int alloc_sum_buffers(FloatMomentStateMetal *s)
{
    if (!s->rounds) { return 0; }
    const size_t rows = (size_t)VMAF_MTL_MSUM_PLANES * s->frame_h;
    const size_t bytes[FM_SUM_BUFFER_COUNT] = {
        rows * sizeof(uint64_t),
        rows * sizeof(int32_t),
        rows * 2u * sizeof(int64_t),
        4u * sizeof(uint64_t),
    };
    for (unsigned b = 0u; b < FM_SUM_BUFFER_COUNT; ++b) {
        const int err = vmaf_metal_kernel_buffer_alloc(&s->sum_buf[b], s->ctx, bytes[b]);
        if (err != 0) {
            for (unsigned q = 0u; q < b; ++q) {
                (void)vmaf_metal_kernel_buffer_free(&s->sum_buf[q], s->ctx);
            }
            return err;
        }
    }
    return 0;
}

int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                          unsigned bpc, unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatMomentStateMetal *s = (FloatMomentStateMetal *)fex->priv;

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->plane_bytes = (size_t)w * h * (bpc <= 8u ? 1u : 2u);
    s->rounds = vmaf_mtl_msum_may_round(w, h, bpc) != 0;

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    {
        const size_t grid_w = (w + 15) / 16;
        const size_t grid_h = (h + 15) / 16;
        s->partials_count = grid_w * grid_h;
        const size_t par_size = s->partials_count * sizeof(uint32_t);
        for (unsigned b = 0u; b < FM_PARTIAL_BUFFER_COUNT; ++b) {
            err = vmaf_metal_kernel_buffer_alloc(&s->rb[b], s->ctx, par_size);
            if (err != 0) {
                for (unsigned q = 0u; q < b; ++q) {
                    (void)vmaf_metal_kernel_buffer_free(&s->rb[q], s->ctx);
                }
                goto fail_lc;
            }
        }
    }

    err = alloc_sum_buffers(s);
    if (err != 0) { goto fail_rb; }

    {
        void *const dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == nullptr) {
            err = -ENODEV;
            goto fail_sum;
        }
        err = build_pipelines(s, (__bridge id<MTLDevice>)dh);
    }
    if (err != 0) { goto fail_sum; }

    s->feature_name_dict = vmaf_feature_name_dict_from_provided_features(
        fex->provided_features, fex->options, s);
    if (s->feature_name_dict == nullptr) {
        err = -ENOMEM;
        goto fail_pso;
    }
    return 0;

fail_pso:
    if (s->pso_8bpc) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;
        s->pso_8bpc = nullptr;
    }
    if (s->pso_16bpc) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc;
        s->pso_16bpc = nullptr;
    }
    release_sum_pipelines(s);
fail_sum:
    if (s->rounds) { free_sum_buffers(s); }
fail_rb:
    for (auto & b : s->rb) {
        (void)vmaf_metal_kernel_buffer_free(&b, s->ctx);
    }
fail_lc:
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
fail_ctx:
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = nullptr;
    return err;
}

void bind_sum_buffer(const FloatMomentStateMetal *s, id<MTLComputeCommandEncoder> enc,
                            unsigned which, NSUInteger index)
{
    id<MTLBuffer> buf = vmaf_metal::borrow<id<MTLBuffer>>(s->sum_buf[which].buffer);
    [enc setBuffer:buf offset:0 atIndex:index];
}

/* Selects the pipeline of pass `kernel`; the caller then sets the pass's
 * bindings, in the order the kernel declares them, and dispatches it. */
void select_sum_pass(const FloatMomentStateMetal *s, id<MTLComputeCommandEncoder> enc,
                            unsigned kernel)
{
    id<MTLComputePipelineState> pso = (__bridge id<MTLComputePipelineState>)s->pso_sum[kernel];
    [enc setComputePipelineState:pso];
}

/* The rounded-sum pass of ADR-1497, on the frame kernel's encoder, after it
 * (serial dispatch: each kernel sees the writes of the one before). The
 * kernels of float_moment.metal return at once on a plane whose exact sum is
 * at most 2^53 units. */
void encode_sum_passes(const FloatMomentStateMetal *s, id<MTLComputeCommandEncoder> enc,
                              id<MTLBuffer> ref_buf, id<MTLBuffer> dis_buf, size_t row_bytes)
{
    const uint32_t dim[4] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h, (uint32_t)row_bytes, 0u};
    const MTLSize lanes = MTLSizeMake(VMAF_MTL_MSUM_LANES, 1, 1);
    const MTLSize by_row = MTLSizeMake(s->frame_h, VMAF_MTL_MSUM_PLANES, 1);
    const MTLSize by_plane = MTLSizeMake(VMAF_MTL_MSUM_PLANES, 1, 1);

    /* 0: the four exact frame sums from the workgroup partials. */
    select_sum_pass(s, enc, 0u);
    for (unsigned b = 0u; b < FM_PARTIAL_BUFFER_COUNT; ++b) {
        id<MTLBuffer> buf = vmaf_metal::borrow<id<MTLBuffer>>(s->rb[b].buffer);
        [enc setBuffer:buf offset:0 atIndex:(NSUInteger)b];
    }
    bind_sum_buffer(s, enc, FM_SUM_FRAME, 8);
    const uint32_t count = (uint32_t)s->partials_count;
    [enc setBytes:&count length:sizeof(count) atIndex:9];
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:lanes];

    /* 1: each row's exact sum. */
    select_sum_pass(s, enc, 1u);
    [enc setBuffer:ref_buf offset:0 atIndex:0];
    [enc setBuffer:dis_buf offset:0 atIndex:1];
    bind_sum_buffer(s, enc, FM_SUM_FRAME, 2);
    bind_sum_buffer(s, enc, FM_SUM_ROW_TOTALS, 3);
    [enc setBytes:dim length:sizeof(dim) atIndex:4];
    [enc dispatchThreadgroups:by_row threadsPerThreadgroup:lanes];

    /* 2: a plan per row. */
    select_sum_pass(s, enc, 2u);
    bind_sum_buffer(s, enc, FM_SUM_FRAME, 0);
    bind_sum_buffer(s, enc, FM_SUM_ROW_TOTALS, 1);
    bind_sum_buffer(s, enc, FM_SUM_ROW_PLANS, 2);
    [enc setBytes:dim length:sizeof(dim) atIndex:3];
    [enc dispatchThreadgroups:by_plane threadsPerThreadgroup:lanes];

    /* 3: each planned row's increments. */
    select_sum_pass(s, enc, 3u);
    [enc setBuffer:ref_buf offset:0 atIndex:0];
    [enc setBuffer:dis_buf offset:0 atIndex:1];
    bind_sum_buffer(s, enc, FM_SUM_FRAME, 2);
    bind_sum_buffer(s, enc, FM_SUM_ROW_PLANS, 3);
    bind_sum_buffer(s, enc, FM_SUM_ROW_UNITS, 4);
    [enc setBytes:dim length:sizeof(dim) atIndex:5];
    [enc dispatchThreadgroups:by_row threadsPerThreadgroup:lanes];

    /* 4: one walk per plane; stores the CPU's sums in sums[2 + plane]. */
    select_sum_pass(s, enc, 4u);
    [enc setBuffer:ref_buf offset:0 atIndex:0];
    [enc setBuffer:dis_buf offset:0 atIndex:1];
    bind_sum_buffer(s, enc, FM_SUM_FRAME, 2);
    bind_sum_buffer(s, enc, FM_SUM_ROW_TOTALS, 3);
    bind_sum_buffer(s, enc, FM_SUM_ROW_PLANS, 4);
    bind_sum_buffer(s, enc, FM_SUM_ROW_UNITS, 5);
    [enc setBytes:dim length:sizeof(dim) atIndex:6];
    [enc dispatchThreadgroups:by_plane threadsPerThreadgroup:lanes];
}

int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    FloatMomentStateMetal *s = (FloatMomentStateMetal *)fex->priv;

    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];
    const size_t row_bytes = (size_t)s->frame_w * (s->bpc <= 8u ? 1u : 2u);

    void *const dh = vmaf_metal_context_device_handle(s->ctx);
    void *const qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == nullptr || qh == nullptr) { return -ENODEV; }

    id<MTLDevice> device = (__bridge id<MTLDevice>)dh;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;
    id<MTLComputePipelineState> pso = (s->bpc <= 8u)
        ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
        : (__bridge id<MTLComputePipelineState>)s->pso_16bpc;

    id<MTLBuffer> ref_buf =
        [device newBufferWithLength:s->plane_bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> dis_buf =
        [device newBufferWithLength:s->plane_bytes options:MTLResourceStorageModeShared];
    if (ref_buf == nil || dis_buf == nil) { return -ENOMEM; }
    {
        uint8_t *rd = (uint8_t *)[ref_buf contents];
        uint8_t *dd = (uint8_t *)[dis_buf contents];
        for (unsigned y = 0; y < s->frame_h; y++) {
            memcpy(rd + y * row_bytes,
                   (uint8_t *)ref_pic->data[0] + y * ref_pic->stride[0], row_bytes);
            memcpy(dd + y * row_bytes,
                   (uint8_t *)dist_pic->data[0] + y * dist_pic->stride[0], row_bytes);
        }
    }

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    const size_t par_size = s->partials_count * sizeof(uint32_t);
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    for (auto & b : s->rb) {
        id<MTLBuffer> buf = vmaf_metal::borrow<id<MTLBuffer>>(b.buffer);
        [blit fillBuffer:buf range:NSMakeRange(0, par_size) value:0];
    }
    [blit endEncoding];

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:ref_buf offset:0 atIndex:0];
    [enc setBuffer:dis_buf offset:0 atIndex:1];
    for (unsigned b = 0u; b < FM_PARTIAL_BUFFER_COUNT; ++b) {
        id<MTLBuffer> buf = vmaf_metal::borrow<id<MTLBuffer>>(s->rb[b].buffer);
        [enc setBuffer:buf offset:0 atIndex:(NSUInteger)(b + 2u)];
    }
    if (s->bpc <= 8u) {
        uint32_t st[2] = {(uint32_t)row_bytes, (uint32_t)row_bytes};
        [enc setBytes:st length:sizeof(st) atIndex:10];
    } else {
        uint32_t st[4] = {
            (uint32_t)row_bytes,
            (uint32_t)row_bytes,
            (uint32_t)s->bpc,
            0,
        };
        [enc setBytes:st length:sizeof(st) atIndex:10];
    }
    uint32_t dim[2] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h};
    [enc setBytes:dim length:sizeof(dim) atIndex:11];

    MTLSize const tg = MTLSizeMake(16, 16, 1);
    MTLSize const grid = MTLSizeMake((s->frame_w + 15) / 16, (s->frame_h + 15) / 16, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    if (s->rounds) { encode_sum_passes(s, enc, ref_buf, dis_buf, row_bytes); }
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

uint64_t reconstruct_partial(const uint32_t *lo, const uint32_t *hi, size_t i)
{
    return ((uint64_t)hi[i] << 32u) | (uint64_t)lo[i];
}

/* The four frame sums, exact: the workgroup sums are integers of the CPU's
 * own terms (the samples, and the float squares moment.c forms) in units of
 * 1 / scaler and 1 / scaler^2, so their uint64 total is the exact sum of the
 * terms, which is what the CPU's running double holds while it is at most 2^53
 * units (every frame of up to 2^21 pixels). Past it the CPU rounds as it adds:
 * apply_rounded_sums() replaces the two second-moment totals (ADR-1497). */
void accumulate_partials(const FloatMomentStateMetal *s, uint64_t sum[4])
{
    const uint32_t *parts[FM_PARTIAL_BUFFER_COUNT];
    for (unsigned b = 0u; b < FM_PARTIAL_BUFFER_COUNT; ++b) {
        parts[b] = (const uint32_t *)s->rb[b].host_view;
        if (parts[b] == nullptr) { return; }
    }
    for (size_t i = 0; i < s->partials_count; ++i) {
        sum[0] += reconstruct_partial(parts[0], parts[1], i);
        sum[1] += reconstruct_partial(parts[2], parts[3], i);
        sum[2] += reconstruct_partial(parts[4], parts[5], i);
        sum[3] += reconstruct_partial(parts[6], parts[7], i);
    }
}

/* On a frame that can pass 2^53 units the walk has stored the CPU's rounded
 * second-moment sums, each a double the conversion below holds exactly, in
 * entries 2 and 3 of the frame sums (ADR-1497). Fails closed: no sums, no
 * score. */
int apply_rounded_sums(const FloatMomentStateMetal *s, uint64_t sum[4])
{
    if (!s->rounds) { return 0; }
    const uint64_t *frame = (const uint64_t *)s->sum_buf[FM_SUM_FRAME].host_view;
    if (frame == nullptr) { return -ENODEV; }
    sum[2] = frame[2];
    sum[3] = frame[3];
    return 0;
}

int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    FloatMomentStateMetal *const s = (FloatMomentStateMetal *)fex->priv;
    uint64_t sum[4] = {0u, 0u, 0u, 0u};
    accumulate_partials(s, sum);
    const int sum_err = apply_rounded_sums(s, sum);
    if (sum_err != 0) { return sum_err; }

    /* moment.c divides its sum of terms in units of 1 / scaler (or
     * 1 / scaler^2), an exact double, by w * h. The products below are
     * exact (a power of two times the pixel count), so each quotient is that
     * one correctly rounded division of the same exact value. */
    const double n_pix = (double)s->frame_w * (double)s->frame_h;
    const double scaler = s->bpc > 8u ? (double)(1u << (s->bpc - 8u)) : 1.0;
    const double denom1 = n_pix * scaler;
    const double denom2 = denom1 * scaler;
    const double ref1 = denom1 > 0.0 ? (double)sum[0] / denom1 : 0.0;
    const double dis1 = denom1 > 0.0 ? (double)sum[1] / denom1 : 0.0;
    const double ref2 = denom2 > 0.0 ? (double)sum[2] / denom2 : 0.0;
    const double dis2 = denom2 > 0.0 ? (double)sum[3] / denom2 : 0.0;

    int err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "float_moment_ref1st", ref1, index);
    if (err != 0) { return err; }
    err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "float_moment_dis1st", dis1, index);
    if (err != 0) { return err; }
    err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "float_moment_ref2nd", ref2, index);
    if (err != 0) { return err; }
    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "float_moment_dis2nd", dis2, index);
}

int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatMomentStateMetal *s = (FloatMomentStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_16bpc) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc;
        s->pso_16bpc = nullptr;
    }
    if (s->pso_8bpc) {
        (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;
        s->pso_8bpc = nullptr;
    }

    release_sum_pipelines(s);
    for (auto & b : s->rb) {
        int const err = vmaf_metal_kernel_buffer_free(&b, s->ctx);
        if (err != 0 && rc == 0) { rc = err; }
    }
    if (s->rounds) { free_sum_buffers(s); }
    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = nullptr;
    }
    return rc;
}

const char *provided_features[] = {
    "float_moment_ref1st",
    "float_moment_dis1st",
    "float_moment_ref2nd",
    "float_moment_dis2nd",
    nullptr,
};
} // namespace

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_float_moment_metal = {
    .name = "float_moment_metal",
    .init = init_fex_metal,
    .submit = submit_fex_metal,
    .collect = collect_fex_metal,
    .flush = nullptr,
    .close = close_fex_metal,
    .options = options,
    .priv_size = sizeof(FloatMomentStateMetal),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 1,
        .is_reduction_only = true,
        .min_useful_frame_area = 1920U * 1080U,
        .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
