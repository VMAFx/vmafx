/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CAMBI banding-detection feature extractor on the Metal backend
 *  (feature name "cambi"; Metal twin "integer_cambi_metal").
 *
 *  Metal port of core/src/feature/cuda/integer_cambi_cuda.c (T3-15 /
 *  ADR-0360). Same Strategy II hybrid (ADR-0205 precedent):
 *
 *    GPU stages (three Metal kernels in integer_cambi.metal):
 *      - cambi_mask_kernel:    derivative + 7x7 box sum + threshold
 *        -> uint16 mask buffer (1 = flat, 0 = edge).
 *      - cambi_decimate_kernel: strict 2x stride-2 subsample.
 *      - cambi_filter_mode_kernel: separable 3-tap mode filter (H + V).
 *
 *    Host CPU stages (exact CPU code via cambi_internal.h wrappers):
 *      - vmaf_cambi_preprocessing:  decimate/upcast to 10-bit.
 *      - vmaf_cambi_calculate_c_values: sliding-histogram c-value pass.
 *      - vmaf_cambi_spatial_pooling:    top-K pooling -> per-scale score.
 *      - vmaf_cambi_weight_scores_per_scale: inner-product scale weights.
 *
 *  Per-frame flow (synchronous, mirrors the CUDA per-scale posture):
 *    1. Host preprocessing (CPU): resize/upcast dist_pic -> pics[0].
 *       Metal pictures are host-resident (unified memory), so no DtoH
 *       download is needed before the host preprocessing reads the plane
 *       (this is the one place the CUDA twin had to download first).
 *    2. Host->buffer upload of pics[0] luma plane -> d_image (memcpy into
 *       the Shared-storage MTLBuffer's [contents]).
 *    3. Scale 0: GPU cambi_mask_kernel over d_image -> d_mask.
 *    4. For scale = 0 .. NUM_SCALES-1:
 *         a. (scale > 0) GPU cambi_decimate_kernel on d_image -> d_tmp,
 *            swap; same for d_mask.
 *         b. GPU cambi_filter_mode_kernel H: d_image -> d_tmp.
 *         c. GPU cambi_filter_mode_kernel V: d_tmp   -> d_image.
 *         d. Buffer->pic copy: d_image -> pics[0], d_mask -> pics[1].
 *         e. Host vmaf_cambi_calculate_c_values + vmaf_cambi_spatial_pooling.
 *    5. Host vmaf_cambi_weight_scores_per_scale -> final score (clamped).
 *    6. submit() stores the score; collect() emits
 *       "Cambi_feature_cambi_score" into the feature collector.
 *
 *  Precision contract: places=4 (ULP=0 on the emitted score). All GPU
 *  phases are integer + bit-exact. The host residual runs the exact CPU
 *  code via cambi_internal.h, so the emitted score is bit-for-bit
 *  identical to vmaf_fex_cambi. The GPU mask kernel reproduces the CPU
 *  zero-pad SAT box-sum semantics exactly (out-of-frame derivative taps
 *  contribute 0), so the mask is bit-identical to the CPU mask too.
 *
 *  Out of scope for v1 (matches the CUDA twin's v1 scope, ADR-0360):
 *    - full_ref / FR-CAMBI mode (no GPU twin for the source pyramid).
 *    - heatmap dump via heatmaps_path.
 *    - high_res_speedup.
 *    - EOTF variants other than bt1886 (host TVI table is already correct).
 *
 *  Feature name: cambi (provided feature "Cambi_feature_cambi_score").
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
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
#include "luminance_tools.h"
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
#include "../cambi_internal.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

/* --- Constants matching cambi.c / integer_cambi_cuda.c --- */
#define CAMBI_METAL_NUM_SCALES        VMAF_CAMBI_NUM_SCALES
#define CAMBI_METAL_MIN_WIDTH_HEIGHT  216
#define CAMBI_METAL_MASK_FILTER_SIZE  VMAF_CAMBI_MASK_FILTER_SIZE
#define CAMBI_METAL_DEFAULT_MAX_VAL   1000.0
#define CAMBI_METAL_DEFAULT_WINDOW_SIZE 65
#define CAMBI_METAL_DEFAULT_TOPK      0.6
#define CAMBI_METAL_DEFAULT_TVI       0.019
#define CAMBI_METAL_DEFAULT_VLT       0.0
#define CAMBI_METAL_DEFAULT_MAX_LOG_CONTRAST 2
#define CAMBI_METAL_DEFAULT_EOTF      "bt1886"
#define CAMBI_METAL_BLOCK_X           16u
#define CAMBI_METAL_BLOCK_Y           16u

typedef struct IntegerCambiStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;

    /* Pipeline states for the three GPU kernels. */
    void *pso_mask;
    void *pso_decimate;
    void *pso_filter_mode;

    /* Device (Shared-storage) buffers — flat uint16 arrays sized for
     * proc_width x proc_height at scale 0. */
    void *d_image;
    void *d_mask;
    void *d_tmp;

    /* Host VmafPicture pair for the CPU residual (image + mask). */
    VmafPicture pics[2];

    /* Host scratch buffers for the CPU residual. */
    VmafCambiHostBuffers buffers;

    /* Callbacks (scalar; GPU has done the parallel work). */
    VmafCambiRangeUpdater inc_range_callback;
    VmafCambiRangeUpdater dec_range_callback;
    VmafCambiDerivativeCalculator derivative_callback;

    /* Configuration options (mirrors CambiState option subset). */
    int    enc_width;
    int    enc_height;
    int    enc_bitdepth;
    int    max_log_contrast;
    int    window_size;
    double topk;
    double cambi_topk;
    double tvi_threshold;
    double cambi_max_val;
    double cambi_vis_lum_threshold;
    char  *eotf;
    char  *cambi_eotf;

    /* Resolved per-frame geometry. */
    unsigned src_bpc;
    unsigned proc_width;
    unsigned proc_height;

    uint16_t adjusted_window;
    uint16_t vlt_luma;

    /* Score computed in submit(), emitted in collect(). */
    double score;

    VmafDictionary *feature_name_dict;
} IntegerCambiStateMetal;

/* --- Options (EXACT subset mirror of integer_cambi_cuda.c / cambi.c
 *     v1 GPU scope: full_ref / heatmaps / high_res_speedup excluded). --- */
static const VmafOption options[] = {
    {
        .name        = "cambi_max_val",
        .help        = "maximum value allowed; larger values will be clipped",
        .offset      = offsetof(IntegerCambiStateMetal, cambi_max_val),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = CAMBI_METAL_DEFAULT_MAX_VAL},
        .min         = 0.0,
        .max         = 1000.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias       = "cmxv",
    },
    {
        .name        = "enc_width",
        .help        = "Encoding width",
        .offset      = offsetof(IntegerCambiStateMetal, enc_width),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min         = 180,
        .max         = 7680,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias       = "encw",
    },
    {
        .name        = "enc_height",
        .help        = "Encoding height",
        .offset      = offsetof(IntegerCambiStateMetal, enc_height),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min         = 150,
        .max         = 7680,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias       = "ench",
    },
    {
        .name        = "enc_bitdepth",
        .help        = "Encoding bitdepth",
        .offset      = offsetof(IntegerCambiStateMetal, enc_bitdepth),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min         = 6,
        .max         = 16,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias       = "encbd",
    },
    {
        .name        = "window_size",
        .help        = "Window size to compute CAMBI: 65 corresponds to ~1 degree at 4k",
        .offset      = offsetof(IntegerCambiStateMetal, window_size),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = CAMBI_METAL_DEFAULT_WINDOW_SIZE},
        .min         = 15,
        .max         = 127,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias       = "ws",
    },
    {
        .name        = "topk",
        .help        = "Ratio of pixels for the spatial pooling computation",
        .offset      = offsetof(IntegerCambiStateMetal, topk),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = CAMBI_METAL_DEFAULT_TOPK},
        .min         = 0.0001,
        .max         = 1.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "cambi_topk",
        .help        = "Ratio of pixels for the spatial pooling computation",
        .offset      = offsetof(IntegerCambiStateMetal, cambi_topk),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = CAMBI_METAL_DEFAULT_TOPK},
        .min         = 0.0001,
        .max         = 1.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias       = "ctpk",
    },
    {
        .name        = "tvi_threshold",
        .help        = "Visibility threshold dL < tvi_threshold * L_mean",
        .offset      = offsetof(IntegerCambiStateMetal, tvi_threshold),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = CAMBI_METAL_DEFAULT_TVI},
        .min         = 0.0001,
        .max         = 1.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias       = "tvit",
    },
    {
        .name        = "cambi_vis_lum_threshold",
        .help        = "Luminance value below which banding is assumed invisible",
        .offset      = offsetof(IntegerCambiStateMetal, cambi_vis_lum_threshold),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = CAMBI_METAL_DEFAULT_VLT},
        .min         = 0.0,
        .max         = 300.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias       = "vlt",
    },
    {
        .name        = "max_log_contrast",
        .help        = "Maximum log contrast (0 to 5, default 2)",
        .offset      = offsetof(IntegerCambiStateMetal, max_log_contrast),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = CAMBI_METAL_DEFAULT_MAX_LOG_CONTRAST},
        .min         = 0,
        .max         = 5,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias       = "mlc",
    },
    {
        .name        = "eotf",
        .help        = "EOTF for visibility-threshold conversion (bt1886 / pq)",
        .offset      = offsetof(IntegerCambiStateMetal, eotf),
        .type        = VMAF_OPT_TYPE_STRING,
        .default_val = {.s = CAMBI_METAL_DEFAULT_EOTF},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "cambi_eotf",
        .help        = "EOTF override for cambi (defaults to eotf)",
        .offset      = offsetof(IntegerCambiStateMetal, cambi_eotf),
        .type        = VMAF_OPT_TYPE_STRING,
        .default_val = {.s = CAMBI_METAL_DEFAULT_EOTF},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias       = "ceot",
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Helpers (mirror integer_cambi_cuda.c — same integer arithmetic).    */
/* ------------------------------------------------------------------ */
static uint16_t cambi_metal_adjust_window(int window_size, unsigned w, unsigned h)
{
    unsigned adjusted = (unsigned)(window_size) * (w + h) / 375u;
    adjusted >>= 4;
    if (adjusted < 1u) { adjusted = 1u; }
    if ((adjusted & 1u) == 0u) { adjusted++; }
    return (uint16_t)adjusted;
}

static uint16_t cambi_metal_ceil_log2(uint32_t num)
{
    if (num == 0u) { return 0u; }
    uint32_t tmp = num - 1u;
    uint16_t shift = 0;
    while (tmp > 0u) {
        tmp >>= 1;
        shift++;
    }
    return shift;
}

static uint16_t cambi_metal_get_mask_index(unsigned w, unsigned h, uint16_t filter_size)
{
    uint32_t shifted_wh = (w >> 6) * (h >> 6);
    return (
        uint16_t)((filter_size * filter_size + 3 * (cambi_metal_ceil_log2(shifted_wh) - 11) - 1) >>
                  1);
}

static int cambi_metal_init_tvi(IntegerCambiStateMetal *s)
{
    VmafLumaRange luma_range;
    int err = vmaf_luminance_init_luma_range(&luma_range, 10, VMAF_PIXEL_RANGE_LIMITED);
    if (err) { return err; }

    const char *effective_eotf;
    if (s->cambi_eotf && strcmp(s->cambi_eotf, CAMBI_METAL_DEFAULT_EOTF) != 0) {
        effective_eotf = s->cambi_eotf;
    } else {
        effective_eotf = (s->eotf != NULL) ? s->eotf : CAMBI_METAL_DEFAULT_EOTF;
    }

    VmafEOTF eotf;
    err = vmaf_luminance_init_eotf(&eotf, effective_eotf);
    if (err) { return err; }

    const int num_diffs = 1 << s->max_log_contrast;
    for (int d = 0; d < num_diffs; d++) {
        const int diff = (int)s->buffers.diffs_to_consider[d];
        int lo = 0;
        int hi = (1 << 10) - 1 - diff;
        int found = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            double sample_lum = vmaf_luminance_get_luminance(mid, luma_range, eotf);
            double diff_lum =
                vmaf_luminance_get_luminance(mid + diff, luma_range, eotf) - sample_lum;
            if (diff_lum < s->tvi_threshold * sample_lum) {
                found = mid;
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        if (found < 0) { found = 0; }
        s->buffers.tvi_for_diff[d] = (uint16_t)(found + num_diffs);
    }

    int vlt = 0;
    for (int v = 0; v < (1 << 10); v++) {
        double L = vmaf_luminance_get_luminance(v, luma_range, eotf);
        if (L < s->cambi_vis_lum_threshold) { vlt = v; }
    }
    s->vlt_luma = (uint16_t)vlt;
    return 0;
}

/* ------------------------------------------------------------------ */
/* build_pipelines: load the three CAMBI kernels from the embedded     */
/* metallib (same blob pattern as every other Metal feature kernel).   */
/* ------------------------------------------------------------------ */
static int build_pipelines(IntegerCambiStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn_mask = [lib newFunctionWithName:@"cambi_mask_kernel"];
    id<MTLFunction> fn_dec  = [lib newFunctionWithName:@"cambi_decimate_kernel"];
    id<MTLFunction> fn_fm   = [lib newFunctionWithName:@"cambi_filter_mode_kernel"];
    if (fn_mask == nil || fn_dec == nil || fn_fm == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso_mask = [device newComputePipelineStateWithFunction:fn_mask error:&err];
    id<MTLComputePipelineState> pso_dec  = [device newComputePipelineStateWithFunction:fn_dec  error:&err];
    id<MTLComputePipelineState> pso_fm   = [device newComputePipelineStateWithFunction:fn_fm   error:&err];
    if (pso_mask == nil || pso_dec == nil || pso_fm == nil) { return -ENODEV; }

    s->pso_mask        = (__bridge_retained void *)pso_mask;
    s->pso_decimate    = (__bridge_retained void *)pso_dec;
    s->pso_filter_mode = (__bridge_retained void *)pso_fm;
    return 0;
}

static void release_host_buffers(IntegerCambiStateMetal *s)
{
    free(s->buffers.diffs_to_consider);   s->buffers.diffs_to_consider   = NULL;
    free(s->buffers.diff_weights);        s->buffers.diff_weights        = NULL;
    free(s->buffers.all_diffs);           s->buffers.all_diffs           = NULL;
    free(s->buffers.tvi_for_diff);        s->buffers.tvi_for_diff        = NULL;
    free(s->buffers.c_values);            s->buffers.c_values            = NULL;
    free(s->buffers.c_values_histograms); s->buffers.c_values_histograms = NULL;
    free(s->buffers.mask_dp);             s->buffers.mask_dp             = NULL;
    free(s->buffers.filter_mode_buffer);  s->buffers.filter_mode_buffer  = NULL;
    free(s->buffers.derivative_buffer);   s->buffers.derivative_buffer   = NULL;
}

/* ------------------------------------------------------------------ */
/* init                                                                 */
/* ------------------------------------------------------------------ */
static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                          unsigned w, unsigned h)
{
    (void)pix_fmt;
    IntegerCambiStateMetal *s = (IntegerCambiStateMetal *)fex->priv;

    /* Resolve enc geometry (matches cambi.c::init / integer_cambi_cuda.c). */
    if (s->enc_bitdepth == 0) { s->enc_bitdepth = (int)bpc; }
    if (s->enc_width == 0 || s->enc_height == 0) {
        s->enc_width  = (int)w;
        s->enc_height = (int)h;
    }
    if ((unsigned)s->enc_height > h || (unsigned)s->enc_width > w) {
        s->enc_width  = (int)w;
        s->enc_height = (int)h;
    }
    if (s->enc_width < CAMBI_METAL_MIN_WIDTH_HEIGHT &&
        s->enc_height < CAMBI_METAL_MIN_WIDTH_HEIGHT) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "integer_cambi_metal: encoded resolution %dx%d below minimum %dx%d.\n",
                 s->enc_width, s->enc_height, CAMBI_METAL_MIN_WIDTH_HEIGHT,
                 CAMBI_METAL_MIN_WIDTH_HEIGHT);
        return -EINVAL;
    }

    s->src_bpc      = bpc;
    s->proc_width   = (unsigned)s->enc_width;
    s->proc_height  = (unsigned)s->enc_height;
    s->adjusted_window =
        cambi_metal_adjust_window(s->window_size, s->proc_width, s->proc_height);

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    /* Host VmafPictures for the CPU residual (10-bit planar). */
    err = vmaf_picture_alloc(&s->pics[0], VMAF_PIX_FMT_YUV400P, 10, s->proc_width, s->proc_height);
    if (err) { goto fail_lc; }
    err = vmaf_picture_alloc(&s->pics[1], VMAF_PIX_FMT_YUV400P, 10, s->proc_width, s->proc_height);
    if (err) { goto fail_pic0; }

    {
        /* Host scratch buffers (mirror cambi.c::CambiBuffers via
         * cambi_internal.h::VmafCambiHostBuffers). */
        const int num_diffs = 1 << s->max_log_contrast;
        s->buffers.diffs_to_consider = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)num_diffs);
        s->buffers.diff_weights      = (int *)malloc(sizeof(int) * (size_t)num_diffs);
        s->buffers.all_diffs         = (int *)malloc(sizeof(int) * (size_t)(2 * num_diffs + 1));
        s->buffers.tvi_for_diff      = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)num_diffs);
        if (!s->buffers.diffs_to_consider || !s->buffers.diff_weights ||
            !s->buffers.all_diffs || !s->buffers.tvi_for_diff) {
            err = -ENOMEM;
            goto fail_hostbuf;
        }

        /* Suprathreshold contrast weights (g_contrast_weights from cambi.c). */
        static const int contrast_weights[32] = {1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8,
                                                  8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
        for (int d = 0; d < num_diffs; d++) {
            s->buffers.diffs_to_consider[d] = (uint16_t)(d + 1);
            s->buffers.diff_weights[d]      = contrast_weights[d];
        }
        for (int d = -num_diffs; d <= num_diffs; d++) {
            s->buffers.all_diffs[d + num_diffs] = d;
        }

        err = cambi_metal_init_tvi(s);
        if (err) { goto fail_hostbuf; }

        s->buffers.c_values =
            (float *)malloc(sizeof(float) * (size_t)s->proc_width * (size_t)s->proc_height);
        if (!s->buffers.c_values) { err = -ENOMEM; goto fail_hostbuf; }

        const uint16_t num_bins =
            (uint16_t)(1024u + (unsigned)(s->buffers.all_diffs[2 * num_diffs] -
                                          s->buffers.all_diffs[0]));
        s->buffers.c_values_histograms =
            (uint16_t *)malloc(sizeof(uint16_t) * (size_t)s->proc_width * (size_t)num_bins);
        if (!s->buffers.c_values_histograms) { err = -ENOMEM; goto fail_hostbuf; }

        const int pad_size  = CAMBI_METAL_MASK_FILTER_SIZE / 2;
        const int dp_width  = (int)s->proc_width + 2 * pad_size + 1;
        const int dp_height = 2 * pad_size + 2;
        s->buffers.mask_dp =
            (uint32_t *)malloc(sizeof(uint32_t) * (size_t)dp_width * (size_t)dp_height);
        s->buffers.filter_mode_buffer =
            (uint16_t *)malloc(sizeof(uint16_t) * 3u * (size_t)s->proc_width);
        s->buffers.derivative_buffer =
            (uint16_t *)malloc(sizeof(uint16_t) * (size_t)s->proc_width);
        if (!s->buffers.mask_dp || !s->buffers.filter_mode_buffer ||
            !s->buffers.derivative_buffer) {
            err = -ENOMEM;
            goto fail_hostbuf;
        }
    }

    vmaf_cambi_default_callbacks(&s->inc_range_callback, &s->dec_range_callback,
                                 &s->derivative_callback);

    {
        void *dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == NULL) { err = -ENODEV; goto fail_hostbuf; }
        id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

        /* Device (Shared-storage) buffers, one flat uint16 plane each. */
        const size_t buf_bytes = (size_t)s->proc_width * (size_t)s->proc_height * sizeof(uint16_t);
        id<MTLBuffer> b_image = [device newBufferWithLength:buf_bytes
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_mask  = [device newBufferWithLength:buf_bytes
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> b_tmp   = [device newBufferWithLength:buf_bytes
                                                   options:MTLResourceStorageModeShared];
        if (b_image == nil || b_mask == nil || b_tmp == nil) { err = -ENOMEM; goto fail_hostbuf; }
        s->d_image = (__bridge_retained void *)b_image;
        s->d_mask  = (__bridge_retained void *)b_mask;
        s->d_tmp   = (__bridge_retained void *)b_tmp;

        err = build_pipelines(s, device);
    }
    if (err != 0) { goto fail_bufs; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) { err = -ENOMEM; goto fail_pso; }

    return 0;

fail_pso:
    if (s->pso_filter_mode) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_filter_mode; s->pso_filter_mode = NULL; }
    if (s->pso_decimate)    { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_decimate;    s->pso_decimate    = NULL; }
    if (s->pso_mask)        { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_mask;        s->pso_mask        = NULL; }
fail_bufs:
    if (s->d_tmp)   { (void)(__bridge_transfer id<MTLBuffer>)s->d_tmp;   s->d_tmp   = NULL; }
    if (s->d_mask)  { (void)(__bridge_transfer id<MTLBuffer>)s->d_mask;  s->d_mask  = NULL; }
    if (s->d_image) { (void)(__bridge_transfer id<MTLBuffer>)s->d_image; s->d_image = NULL; }
fail_hostbuf:
    release_host_buffers(s);
    (void)vmaf_picture_unref(&s->pics[1]);
fail_pic0:
    (void)vmaf_picture_unref(&s->pics[0]);
fail_lc:
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
fail_ctx:
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

/* ------------------------------------------------------------------ */
/* encode_grid: dispatch one of the three GPU kernels over (w x h).     */
/* ------------------------------------------------------------------ */
static void encode_kernel(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pso,
                          id<MTLBuffer> in_buf, id<MTLBuffer> out_buf, const uint32_t params[4],
                          unsigned w, unsigned h)
{
    id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    [enc setBuffer:in_buf  offset:0 atIndex:0];
    [enc setBuffer:out_buf offset:0 atIndex:1];
    [enc setBytes:params length:sizeof(uint32_t) * 4u atIndex:2];
    const MTLSize tg   = MTLSizeMake(CAMBI_METAL_BLOCK_X, CAMBI_METAL_BLOCK_Y, 1);
    const MTLSize grid = MTLSizeMake((w + CAMBI_METAL_BLOCK_X - 1u) / CAMBI_METAL_BLOCK_X,
                                     (h + CAMBI_METAL_BLOCK_Y - 1u) / CAMBI_METAL_BLOCK_Y, 1);
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

/* Copy a flat (scaled_w x scaled_h, stride scaled_w) uint16 device
 * buffer into a stride-aware VmafPicture plane. */
static void copy_buf_to_pic(id<MTLBuffer> buf, VmafPicture *pic, unsigned scaled_w,
                            unsigned scaled_h)
{
    const uint16_t *src = (const uint16_t *)[buf contents];
    uint16_t *dst       = (uint16_t *)pic->data[0];
    const ptrdiff_t dst_stride_words = pic->stride[0] >> 1;
    for (unsigned y = 0; y < scaled_h; ++y) {
        memcpy(dst + (size_t)y * (size_t)dst_stride_words, src + (size_t)y * scaled_w,
               (size_t)scaled_w * sizeof(uint16_t));
    }
}

/* ------------------------------------------------------------------ */
/* submit: full synchronous CAMBI pipeline (GPU stages + host residual).*/
/* ------------------------------------------------------------------ */
static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic; (void)ref_pic_90; (void)dist_pic_90; (void)index;
    IntegerCambiStateMetal *s = (IntegerCambiStateMetal *)fex->priv;

    /* Step 1: host preprocessing -> pics[0] (10-bit planar). Metal
     * pictures are host-resident, so dist_pic->data[0] is directly
     * readable (no DtoH download needed). */
    int err = vmaf_cambi_preprocessing(dist_pic, &s->pics[0], (int)s->proc_width,
                                       (int)s->proc_height, s->enc_bitdepth);
    if (err) { return err; }

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }
    id<MTLCommandQueue> queue   = (__bridge id<MTLCommandQueue>)qh;
    id<MTLBuffer>       d_image = (__bridge id<MTLBuffer>)s->d_image;
    id<MTLBuffer>       d_mask  = (__bridge id<MTLBuffer>)s->d_mask;
    id<MTLBuffer>       d_tmp   = (__bridge id<MTLBuffer>)s->d_tmp;

    id<MTLComputePipelineState> pso_mask = (__bridge id<MTLComputePipelineState>)s->pso_mask;
    id<MTLComputePipelineState> pso_dec  = (__bridge id<MTLComputePipelineState>)s->pso_decimate;
    id<MTLComputePipelineState> pso_fm   = (__bridge id<MTLComputePipelineState>)s->pso_filter_mode;

    /* Step 2: host->buffer upload pics[0] luma plane -> d_image
     * (tightly packed, stride == proc_width). */
    {
        uint16_t *img = (uint16_t *)[d_image contents];
        const uint16_t *src = (const uint16_t *)s->pics[0].data[0];
        const ptrdiff_t src_stride_words = s->pics[0].stride[0] >> 1;
        for (unsigned y = 0; y < s->proc_height; ++y) {
            memcpy(img + (size_t)y * s->proc_width, src + (size_t)y * (size_t)src_stride_words,
                   (size_t)s->proc_width * sizeof(uint16_t));
        }
    }

    const int num_diffs = 1 << s->max_log_contrast;
    const double topk = (s->topk != CAMBI_METAL_DEFAULT_TOPK) ? s->topk : s->cambi_topk;
    double scores_per_scale[CAMBI_METAL_NUM_SCALES] = {0.0, 0.0, 0.0, 0.0, 0.0};

    /* Step 3: scale-0 spatial mask over the full plane. */
    {
        const unsigned mask_index =
            (unsigned)cambi_metal_get_mask_index(s->proc_width, s->proc_height,
                                                 CAMBI_METAL_MASK_FILTER_SIZE);
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        if (cmd == nil) { return -ENOMEM; }
        const uint32_t p[4] = {s->proc_width, s->proc_height, s->proc_width, mask_index};
        encode_kernel(cmd, pso_mask, d_image, d_mask, p, s->proc_width, s->proc_height);
        [cmd commit];
        [cmd waitUntilCompleted];
    }

    /* Step 4: per-scale GPU pipeline + host residual. */
    unsigned scaled_w = s->proc_width;
    unsigned scaled_h = s->proc_height;
    for (int scale = 0; scale < CAMBI_METAL_NUM_SCALES; ++scale) {
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        if (cmd == nil) { return -ENOMEM; }

        if (scale > 0) {
            const unsigned new_w = (scaled_w + 1u) >> 1;
            const unsigned new_h = (scaled_h + 1u) >> 1;
            const uint32_t pd[4] = {new_w, new_h, scaled_w, new_w};
            /* decimate d_image -> d_tmp, then swap. */
            encode_kernel(cmd, pso_dec, d_image, d_tmp, pd, new_w, new_h);
            { id<MTLBuffer> t = d_image; d_image = d_tmp; d_tmp = t;
              void *tp = s->d_image; s->d_image = s->d_tmp; s->d_tmp = tp; }
            /* decimate d_mask -> d_tmp, then swap. */
            encode_kernel(cmd, pso_dec, d_mask, d_tmp, pd, new_w, new_h);
            { id<MTLBuffer> t = d_mask; d_mask = d_tmp; d_tmp = t;
              void *tp = s->d_mask; s->d_mask = s->d_tmp; s->d_tmp = tp; }
            scaled_w = new_w;
            scaled_h = new_h;
        }

        /* filter_mode H: d_image -> d_tmp; V: d_tmp -> d_image. */
        const uint32_t ph[4] = {scaled_w, scaled_h, scaled_w, 0u};
        encode_kernel(cmd, pso_fm, d_image, d_tmp, ph, scaled_w, scaled_h);
        const uint32_t pv[4] = {scaled_w, scaled_h, scaled_w, 1u};
        encode_kernel(cmd, pso_fm, d_tmp, d_image, pv, scaled_w, scaled_h);

        [cmd commit];
        [cmd waitUntilCompleted];

        /* Buffer -> VmafPicture for the host residual. */
        copy_buf_to_pic(d_image, &s->pics[0], scaled_w, scaled_h);
        copy_buf_to_pic(d_mask,  &s->pics[1], scaled_w, scaled_h);

        /* Host residual: sliding-histogram c-values + top-K pooling. */
        vmaf_cambi_calculate_c_values(&s->pics[0], &s->pics[1], s->buffers.c_values,
                                      s->buffers.c_values_histograms, s->adjusted_window,
                                      (uint16_t)num_diffs, s->buffers.tvi_for_diff, s->vlt_luma,
                                      s->buffers.diff_weights, s->buffers.all_diffs, (int)scaled_w,
                                      (int)scaled_h, s->inc_range_callback, s->dec_range_callback);
        scores_per_scale[scale] =
            vmaf_cambi_spatial_pooling(s->buffers.c_values, topk, scaled_w, scaled_h);
    }

    /* Step 5: inner-product scale weighting + clamp. */
    const uint16_t pixels_in_window = vmaf_cambi_get_pixels_in_window(s->adjusted_window);
    double score = vmaf_cambi_weight_scores_per_scale(scores_per_scale, pixels_in_window);
    if (score > s->cambi_max_val) { score = s->cambi_max_val; }
    if (score < 0.0) { score = 0.0; }
    s->score = score;
    return 0;
}

/* ------------------------------------------------------------------ */
/* collect: emit the score computed in submit().                        */
/* ------------------------------------------------------------------ */
static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    IntegerCambiStateMetal *s = (IntegerCambiStateMetal *)fex->priv;
    return vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "Cambi_feature_cambi_score", s->score, index);
}

/* ------------------------------------------------------------------ */
/* close                                                                */
/* ------------------------------------------------------------------ */
static int close_fex_metal(VmafFeatureExtractor *fex)
{
    IntegerCambiStateMetal *s = (IntegerCambiStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    if (s->pso_filter_mode) { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_filter_mode; s->pso_filter_mode = NULL; }
    if (s->pso_decimate)    { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_decimate;    s->pso_decimate    = NULL; }
    if (s->pso_mask)        { (void)(__bridge_transfer id<MTLComputePipelineState>)s->pso_mask;        s->pso_mask        = NULL; }
    if (s->d_tmp)   { (void)(__bridge_transfer id<MTLBuffer>)s->d_tmp;   s->d_tmp   = NULL; }
    if (s->d_mask)  { (void)(__bridge_transfer id<MTLBuffer>)s->d_mask;  s->d_mask  = NULL; }
    if (s->d_image) { (void)(__bridge_transfer id<MTLBuffer>)s->d_image; s->d_image = NULL; }

    (void)vmaf_picture_unref(&s->pics[0]);
    (void)vmaf_picture_unref(&s->pics[1]);
    release_host_buffers(s);

    if (s->feature_name_dict) { (void)vmaf_dictionary_free(&s->feature_name_dict); }
    if (s->ctx) { vmaf_metal_context_destroy(s->ctx); s->ctx = NULL; }
    return rc;
}

static const char *provided_features[] = {
    "Cambi_feature_cambi_score", NULL
};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_integer_cambi_metal = {
    .name              = "integer_cambi_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = NULL,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(IntegerCambiStateMetal),
    .provided_features = provided_features,
    .flags             = 0,
    .chars = {
        .n_dispatches_per_frame = 15, /* 5 scales x (mask once + filter_H + filter_V) + decimate */
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
