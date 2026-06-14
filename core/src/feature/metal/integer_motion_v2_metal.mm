/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  motion_v2 feature extractor on the Metal backend — first
 *  real kernel (T8-1c / ADR-0421). Replaces the T8-1 scaffold's
 *  `-ENOSYS` returns with real `MTLComputePipelineState` dispatch
 *  of `integer_motion_v2.metal`'s `motion_v2_kernel_{8,16}bpc`.
 *
 *  Algorithm: SAD( vertical-blur( prev - cur ) horizontal-blurred )
 *  exploiting convolution linearity so a single kernel dispatch over
 *  (prev_ref - cur_ref) emits the score. Result on host:
 *  `motion_v2_sad_score = SAD / 256.0 / (W*H)`.
 *  `motion2_v2_score = min(score[i], score[i+1])` (host-side flush).
 *  `motion3_v2_score` (per-frame blend + clip + optional moving-average)
 *  is emitted host-side in flush() with the same formula and option
 *  surface as the CPU integer_motion_v2.c::flush and the CUDA twin,
 *  bit-exact at default options. The motion3_v2 post-process and its
 *  four-option surface were added in ADR-1108 (closing the GPU-twin
 *  deferral ADR-0337 left open).
 *
 *  Metallib resolution: the build embeds the compiled metallib into
 *  the libvmaf binary's __TEXT,__metallib section via the meson
 *  custom_target in `libvmaf/src/feature/metal/meson.build`. At init
 *  time we wrap the byte range in `dispatch_data_create` and hand it
 *  to `[device newLibraryWithData:]` — no filesystem path dependency.
 *  Same embedded-blob pattern the CUDA backend uses for cubin
 *  (`vmaf_cuda_compiled_kernels_start`).
 */

#include <errno.h>
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
#include "libvmaf/picture.h"
#include "motion_blend_tools.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
}

/* Default maximum value allowed for motion — mirrors
 * DEFAULT_MOTION_MAX_VAL in integer_motion_v2.c (the CPU reference). */
#define MOTION_V2_METAL_DEFAULT_MAX_VAL (10000.0)

/* Linker-defined symbols bracketing the embedded metallib byte range.
 * See `libvmaf/src/feature/metal/meson.build` for the embed mechanism
 * (objcopy / ld --format=binary equivalent on macOS:
 * `-sectcreate __TEXT __metallib path/to/default.metallib`). */
extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

typedef struct MotionV2StateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalKernelBuffer rb;
    VmafMetalContext *ctx;

    /* Pipeline states (one per bpc variant). Bridge-retained `void *`
     * so the C struct doesn't need to be Obj-C++ in the header. */
    void *pso_8bpc;
    void *pso_16bpc;

    /* Previous-frame ref Y buffer (MTLBuffer w/ Shared storage).
     * Unified-memory collapse of the HIP twin's pix[2] ping-pong. */
    void *prev_ref_buf;
    void *prev_ref_contents;

    /* Last SAD value collected; emit motion2_v2 = min(cur, prev). */
    double last_score;
    bool   have_last;

    size_t plane_bytes;
    size_t partials_count;   /* number of threadgroups (grid_w * grid_h) */
    unsigned index;
    unsigned frame_w;
    unsigned frame_h;
    unsigned bpc;
    double motion_fps_weight;

    /* motion3_v2 post-process options — mirror the CPU reference
     * (integer_motion_v2.c) option table byte-for-byte so a model
     * file carrying `motion_v2_metal=motion_blend_factor=…` loads
     * and scores identically to the CPU path (ADR-1108). */
    double motion_blend_factor;
    double motion_blend_offset;
    double motion_max_val;
    bool motion_moving_average;

    VmafDictionary *feature_name_dict;
} MotionV2StateMetal;

/* Option table mirrors integer_motion_v2.c (CPU reference) for the
 * subset of options the Metal twin's host-side motion3_v2 post-process
 * consumes: name / alias / type / default / min / max / flags match
 * byte-for-byte so co-scheduled CPU+Metal runs name features identically
 * and model files load on either path (ADR-1108). motion_force_zero and
 * motion_five_frame_window are CPU-only knobs (the Metal kernel always
 * computes the SAD, and the 5-frame window is unsupported per ADR-0337);
 * they are intentionally omitted from this twin's surface. */
static const VmafOption options[] = {
    {
        .name = "motion_fps_weight",
        .alias = "mfw",
        .help = "fps-aware multiplicative weight/correction",
        .offset = offsetof(MotionV2StateMetal, motion_fps_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 5.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_factor",
        .alias = "mbf",
        .help = "blend motion score given an offset",
        .offset = offsetof(MotionV2StateMetal, motion_blend_factor),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 1.0,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_blend_offset",
        .alias = "mbo",
        .help = "blend motion score starting from this offset",
        .offset = offsetof(MotionV2StateMetal, motion_blend_offset),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 40.0,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_max_val",
        .alias = "mmxv",
        .help = "maximum value allowed; larger values will be clipped to this value",
        .offset = offsetof(MotionV2StateMetal, motion_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = MOTION_V2_METAL_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 10000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "motion_moving_average",
        .alias = "mma",
        .help = "smooth motion3 with a 2-frame moving average",
        .offset = offsetof(MotionV2StateMetal, motion_moving_average),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0}};

static int build_pipelines(MotionV2StateMetal *s, id<MTLDevice> device)
{
    const size_t blob_size = (size_t)(libvmaf_metallib_end - libvmaf_metallib_start);
    if (blob_size == 0) {
        return -ENODEV;
    }

    dispatch_data_t data = dispatch_data_create(
        libvmaf_metallib_start, blob_size,
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (data == NULL) {
        return -ENOMEM;
    }

    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithData:data error:&err];
    if (lib == nil) {
        return -ENODEV;
    }

    id<MTLFunction> fn8  = [lib newFunctionWithName:@"motion_v2_kernel_8bpc"];
    id<MTLFunction> fn16 = [lib newFunctionWithName:@"motion_v2_kernel_16bpc"];
    if (fn8 == nil || fn16 == nil) {
        return -ENODEV;
    }

    id<MTLComputePipelineState> pso8 =
        [device newComputePipelineStateWithFunction:fn8 error:&err];
    if (pso8 == nil) {
        return -ENODEV;
    }
    id<MTLComputePipelineState> pso16 =
        [device newComputePipelineStateWithFunction:fn16 error:&err];
    if (pso16 == nil) {
        return -ENODEV;
    }

    s->pso_8bpc  = (__bridge_retained void *)pso8;
    s->pso_16bpc = (__bridge_retained void *)pso16;
    return 0;
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                          unsigned w, unsigned h)
{
    (void)pix_fmt;
    MotionV2StateMetal *s = (MotionV2StateMetal *)fex->priv;

    s->frame_w = w;
    s->frame_h = h;
    s->bpc = bpc;
    s->plane_bytes = (size_t)w * h * (bpc <= 8u ? 1u : 2u);
    s->have_last = false;
    s->last_score = 0.0;

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) {
        return err;
    }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) {
        goto fail_after_ctx;
    }

    /* Partials buffer: one uint per threadgroup. Grid is
     * ceil(w/16) × ceil(h/16); pre-allocate max-size based on
     * frame dimensions. Host reduces in double precision after
     * each kernel run. */
    {
        const size_t grid_w = (s->frame_w + 15) / 16;
        const size_t grid_h = (s->frame_h + 15) / 16;
        s->partials_count   = grid_w * grid_h;
        err = vmaf_metal_kernel_buffer_alloc(&s->rb, s->ctx,
                                             s->partials_count * sizeof(uint32_t));
    }
    if (err != 0) {
        goto fail_after_lc;
    }

    {
        void *device_handle = vmaf_metal_context_device_handle(s->ctx);
        if (device_handle == NULL) {
            err = -ENODEV;
            goto fail_after_rb;
        }
        id<MTLDevice> device = (__bridge id<MTLDevice>)device_handle;

        id<MTLBuffer> prev = [device newBufferWithLength:s->plane_bytes
                                                 options:MTLResourceStorageModeShared];
        if (prev == nil) {
            err = -ENOMEM;
            goto fail_after_rb;
        }
        s->prev_ref_buf      = (__bridge_retained void *)prev;
        s->prev_ref_contents = [prev contents];

        err = build_pipelines(s, device);
        if (err != 0) {
            goto fail_after_prev;
        }
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) {
        err = -ENOMEM;
        goto fail_after_pso;
    }

    return 0;

fail_after_pso:
    if (s->pso_8bpc != NULL) {
        id<MTLComputePipelineState> p __attribute__((unused)) =
            (__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;
        s->pso_8bpc = NULL;
    }
    if (s->pso_16bpc != NULL) {
        id<MTLComputePipelineState> p __attribute__((unused)) =
            (__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc;
        s->pso_16bpc = NULL;
    }
fail_after_prev:
    if (s->prev_ref_buf != NULL) {
        id<MTLBuffer> b __attribute__((unused)) =
            (__bridge_transfer id<MTLBuffer>)s->prev_ref_buf;
        s->prev_ref_buf = NULL;
        s->prev_ref_contents = NULL;
    }
fail_after_rb:
    (void)vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
fail_after_lc:
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
fail_after_ctx:
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

static void copy_y_plane(VmafPicture *pic, void *dst, size_t row_bytes)
{
    const uint8_t *src = (const uint8_t *)pic->data[0];
    const size_t src_stride = pic->stride[0];
    uint8_t *out = (uint8_t *)dst;
    for (unsigned y = 0; y < pic->h[0]; y++) {
        memcpy(out + y * row_bytes, src + y * src_stride, row_bytes);
    }
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)dist_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;
    MotionV2StateMetal *s = (MotionV2StateMetal *)fex->priv;

    s->index = index;
    s->frame_w = ref_pic->w[0];
    s->frame_h = ref_pic->h[0];

    const size_t row_bytes = (size_t)ref_pic->w[0] * (s->bpc <= 8u ? 1u : 2u);
    if (index == 0) {
        /* First frame has no "prev" — copy cur to prev_ref and emit 0 in collect. */
        copy_y_plane(ref_pic, s->prev_ref_contents, row_bytes);
        return 0;
    }

    void *device_handle = vmaf_metal_context_device_handle(s->ctx);
    void *queue_handle  = vmaf_metal_context_queue_handle(s->ctx);
    if (device_handle == NULL || queue_handle == NULL) {
        return -ENODEV;
    }
    id<MTLDevice> device       = (__bridge id<MTLDevice>)device_handle;
    id<MTLCommandQueue> queue  = (__bridge id<MTLCommandQueue>)queue_handle;
    id<MTLBuffer> prev_ref     = (__bridge id<MTLBuffer>)s->prev_ref_buf;
    id<MTLBuffer> sad_buf      = (__bridge id<MTLBuffer>)(void *)s->rb.buffer;
    id<MTLComputePipelineState> pso = (s->bpc <= 8u)
        ? (__bridge id<MTLComputePipelineState>)s->pso_8bpc
        : (__bridge id<MTLComputePipelineState>)s->pso_16bpc;

    /* Per-frame cur buffer. Could be cached on the state for fewer
     * allocs; correctness-first shape for the first kernel. */
    id<MTLBuffer> cur_buf = [device newBufferWithLength:s->plane_bytes
                                                options:MTLResourceStorageModeShared];
    if (cur_buf == nil) {
        return -ENOMEM;
    }
    copy_y_plane(ref_pic, [cur_buf contents], row_bytes);

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) {
        return -ENOMEM;
    }

    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit fillBuffer:sad_buf
               range:NSMakeRange(0, s->partials_count * sizeof(uint32_t))
               value:0];
    [blit endEncoding];

    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:prev_ref offset:0 atIndex:0];
    [enc setBuffer:cur_buf  offset:0 atIndex:1];
    [enc setBuffer:sad_buf  offset:0 atIndex:2];

    if (s->bpc <= 8u) {
        uint32_t strides[2] = {(uint32_t)row_bytes, (uint32_t)row_bytes};
        [enc setBytes:strides length:sizeof(strides) atIndex:3];
    } else {
        uint32_t strides[4] = {(uint32_t)row_bytes, (uint32_t)row_bytes, (uint32_t)s->bpc, 0};
        [enc setBytes:strides length:sizeof(strides) atIndex:3];
    }

    uint32_t dim[2] = {(uint32_t)s->frame_w, (uint32_t)s->frame_h};
    [enc setBytes:dim length:sizeof(dim) atIndex:4];

    const size_t tile_int_count = 20 * 21; /* MV2_TILE_H * MV2_TILE_PITCH */
    [enc setThreadgroupMemoryLength:(tile_int_count * sizeof(int32_t)) atIndex:0];

    MTLSize tg_size   = MTLSizeMake(16, 16, 1);
    MTLSize grid_size = MTLSizeMake((s->frame_w + 15) / 16,
                                    (s->frame_h + 15) / 16, 1);
    [enc dispatchThreadgroups:grid_size threadsPerThreadgroup:tg_size];
    [enc endEncoding];

    [cmd commit];
    [cmd waitUntilCompleted];

    /* After dispatch, copy cur into prev for the next frame. */
    copy_y_plane(ref_pic, s->prev_ref_contents, row_bytes);
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    MotionV2StateMetal *s = (MotionV2StateMetal *)fex->priv;

    double score = 0.0;
    if (index > 0) {
        /* Reduce per-threadgroup partials in double precision —
         * sidesteps Apple MSL's lack of 64-bit atomic_fetch_add
         * (CI run 25685703780 / job 75408804495). */
        const uint32_t *partials = (const uint32_t *)s->rb.host_view;
        if (partials != NULL) {
            double sad_d = 0.0;
            for (size_t i = 0; i < s->partials_count; ++i) {
                sad_d += (double)partials[i];
            }
            const double denom = (double)s->frame_w * (double)s->frame_h * 256.0;
            score = (denom > 0.0) ? (sad_d / denom) : 0.0;
        }
    }

    int err = vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict,
        "VMAF_integer_feature_motion_v2_sad_score", score, index);
    if (err != 0) {
        return err;
    }

    if (s->have_last) {
        /* Apply fps weight — mirrors CPU integer_motion_v2.c flush logic.
         * Bit-exact when motion_fps_weight = 1.0 (default). */
        const double w_score      = score * s->motion_fps_weight;
        const double w_last_score = s->last_score * s->motion_fps_weight;
        const double m2 = (w_last_score < w_score) ? w_last_score : w_score;
        err = vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict,
            "VMAF_integer_feature_motion2_v2_score", m2, index - 1);
        if (err != 0) {
            return err;
        }
    }
    s->last_score = score;
    s->have_last  = true;
    return 0;
}

static int flush_fex_metal(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    MotionV2StateMetal *s = (MotionV2StateMetal *)fex->priv;
    /* Final motion2_v2 frame: apply fps weight then emit.
     * Bit-exact when motion_fps_weight = 1.0 (default). */
    if (s->have_last) {
        const double weighted = s->last_score * s->motion_fps_weight;
        (void)vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict,
            "VMAF_integer_feature_motion2_v2_score", weighted, s->index);
    }

    /* motion3_v2 post-process — mirrors integer_motion_v2.c::flush and
     * the CUDA twin (flush_fex_cuda) byte-for-byte. The per-frame
     * motion2 input is recomputed from the raw SAD scores (collected
     * for every frame), not read back from motion2_v2, so the blend
     * input matches the CPU reference regardless of dict renaming.
     * Emitted via the shared motion_blend_tools.h helper (ADR-1108). */

    /* Resolve the (possibly renamed, for sfr/hfr co-schedule) SAD feature
     * name from the dict — mirrors integer_motion_v2.c::flush. */
    VmafDictionaryEntry *e_sad =
        vmaf_dictionary_get(&s->feature_name_dict, "VMAF_integer_feature_motion_v2_sad_score", 0);
    const char *sad_name = e_sad ? e_sad->val : "VMAF_integer_feature_motion_v2_sad_score";

    unsigned n_frames = 0;
    double dummy;
    while (!vmaf_feature_collector_get_score(feature_collector, sad_name, &dummy, n_frames))
        n_frames++;

    if (n_frames < 2)
        return 1;

    /* motion3_v2 seeding — mirrors integer_motion_v2.c::flush exactly.
     * 3-frame mode only (min_idx = 1; the 5-frame window is unsupported
     * on motion_v2, ADR-0337). stamp_value blends the *raw SAD* at
     * min_idx, clipped to motion_max_val; it is emitted for all indices
     * i < min_idx. */
    const unsigned min_idx = 1;
    double stamp_value = 0.;
    if (n_frames > min_idx) {
        double sad_at_min_idx;
        if (!vmaf_feature_collector_get_score(feature_collector, sad_name, &sad_at_min_idx,
                                              min_idx)) {
            stamp_value =
                MIN(motion_blend(sad_at_min_idx, s->motion_blend_factor, s->motion_blend_offset),
                    s->motion_max_val);
        }
    }

    double prev_processed = 0.;
    for (unsigned i = 0; i < n_frames; i++) {
        double score_cur;
        double score_next;
        vmaf_feature_collector_get_score(feature_collector, sad_name, &score_cur, i);
        /* Apply fps weight — mirrors CPU integer_motion_v2.c flush logic.
         * Bit-exact when motion_fps_weight = 1.0 (default). */
        score_cur *= s->motion_fps_weight;

        double motion2;
        if (i + 1 < n_frames) {
            vmaf_feature_collector_get_score(feature_collector, sad_name, &score_next, i + 1);
            score_next *= s->motion_fps_weight;
            motion2 = score_cur < score_next ? score_cur : score_next;
        } else {
            motion2 = score_cur;
        }

        /* motion3_v2_score: per-frame blend + clip + optional moving-average.
         * Mirrors integer_motion_v2.c::flush lines 466-481 byte-for-byte. */
        double motion3;
        if (i < min_idx) {
            motion3 = stamp_value;
            prev_processed = stamp_value;
        } else {
            double processed =
                MIN(motion_blend(motion2, s->motion_blend_factor, s->motion_blend_offset),
                    s->motion_max_val);
            motion3 = s->motion_moving_average ? (processed + prev_processed) / 2.0 : processed;
            prev_processed = processed;
        }

        int append_err = vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, "VMAF_integer_feature_motion3_v2_score",
            motion3, i);
        if (append_err)
            return append_err;
    }

    return 1;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    MotionV2StateMetal *s = (MotionV2StateMetal *)fex->priv;

    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_16bpc != NULL) {
        id<MTLComputePipelineState> p __attribute__((unused)) =
            (__bridge_transfer id<MTLComputePipelineState>)s->pso_16bpc;
        s->pso_16bpc = NULL;
    }
    if (s->pso_8bpc != NULL) {
        id<MTLComputePipelineState> p __attribute__((unused)) =
            (__bridge_transfer id<MTLComputePipelineState>)s->pso_8bpc;
        s->pso_8bpc = NULL;
    }
    if (s->prev_ref_buf != NULL) {
        id<MTLBuffer> b __attribute__((unused)) =
            (__bridge_transfer id<MTLBuffer>)s->prev_ref_buf;
        s->prev_ref_buf = NULL;
        s->prev_ref_contents = NULL;
    }

    int err = vmaf_metal_kernel_buffer_free(&s->rb, s->ctx);
    if (err != 0 && rc == 0) {
        rc = err;
    }
    if (s->feature_name_dict != NULL) {
        err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0) {
            rc = err;
        }
    }
    if (s->ctx != NULL) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = NULL;
    }
    return rc;
}

static const char *provided_features[] = {"VMAF_integer_feature_motion_v2_sad_score",
                                          "VMAF_integer_feature_motion2_v2_score",
                                          "VMAF_integer_feature_motion3_v2_score", NULL};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0421 Metal
 * first-kernel motion_v2; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0421 / ADR-0278
VmafFeatureExtractor vmaf_fex_integer_motion_v2_metal = {
    .name = "motion_v2_metal",
    .init = init_fex_metal,
    .submit = submit_fex_metal,
    .collect = collect_fex_metal,
    .flush = flush_fex_metal,
    .close = close_fex_metal,
    .options = options,
    .priv_size = sizeof(MotionV2StateMetal),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_TEMPORAL,
    .chars =
        {
            .n_dispatches_per_frame = 1,
            .is_reduction_only = true,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};
} /* extern "C" */
