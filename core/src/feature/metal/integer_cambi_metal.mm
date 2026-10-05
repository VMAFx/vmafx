/**
 *  Copyright 2016-2026 Netflix, Inc.
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
 *  Per picture (synchronous, mirrors the CUDA per-scale posture):
 *    1. Host preprocessing (CPU): resize/upcast dist_pic -> pics[0].
 *       Metal pictures are host-resident (unified memory), so no DtoH
 *       download is needed before the host preprocessing reads the plane
 *       (this is the one place the CUDA twin had to download first).
 *    2. Host->buffer upload of pics[0] luma plane -> d_image (memcpy into
 *       the Shared-storage MTLBuffer's [contents]).
 *    3. Scale 0: GPU cambi_mask_kernel over d_image -> d_mask.
 *    4. For scale = 0 .. NUM_SCALES-1:
 *         a. (scale > 0, or any scale when cambi_high_res_speedup is active
 *            for this resolution) GPU cambi_decimate_kernel on d_image ->
 *            d_tmp, swap; same for d_mask. Mirrors cambi.c::cambi_score.
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
 *  Options (T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26, ADR-1498): the
 *  CPU cambi table. src_width / src_height take the CPU's defaults and checks
 *  (cambi.c::validate_and_setup_dimensions: unset means the picture size,
 *  both sizes validated, an encode and a source that scale in opposite
 *  directions refused) and size the source window, which the CPU's
 *  vmaf_cambi_check_window_fits_lut() bounds together with the encode window.
 *  full_ref runs the same pipeline on the reference picture at the source
 *  size and window and emits `cambi_source` and `cambi_full_reference` =
 *  MIN(MAX(0, dist - src), cambi_max_val), as cambi.c::extract does.
 *  heatmaps_path writes the distorted picture's c-values of every scale with
 *  cambi.c's own writers (vmaf_cambi_open_heatmaps() / _dump_c_values() /
 *  _close_heatmaps()), before the pooling reorders them, so the files are the
 *  CPU's (T-METAL-CAMBI-SCORE-NAME-SUFFIXED-2026-10-05).
 *
 *  Feature names: init() builds the name dictionary from the options as the
 *  caller set them, before cambi_metal_resolve_dimensions() writes the
 *  resolved encode and source sizes into their option slots, as cambi.c::init
 *  does. Built afterwards, every name carried `_encbd_8_ench_..._srcw_...`
 *  and nothing read the score (T-METAL-CAMBI-SCORE-NAME-SUFFIXED-2026-10-05).
 *
 *  Feature name: cambi (provided feature "Cambi_feature_cambi_score").
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <utility>

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
#define CAMBI_METAL_MIN_WIDTH_HEIGHT  CAMBI_MIN_WIDTH_HEIGHT
#define CAMBI_METAL_MASK_FILTER_SIZE  VMAF_CAMBI_MASK_FILTER_SIZE
#define CAMBI_METAL_DEFAULT_MAX_VAL   1000.0
#define CAMBI_METAL_DEFAULT_WINDOW_SIZE 65
#define CAMBI_METAL_DEFAULT_TOPK      0.6
#define CAMBI_METAL_DEFAULT_TVI       0.019
#define CAMBI_METAL_DEFAULT_VLT       0.0
#define CAMBI_METAL_DEFAULT_MAX_LOG_CONTRAST 2
#define CAMBI_METAL_DEFAULT_EOTF      "bt1886"
#define CAMBI_METAL_DEFAULT_HIGH_RES_SPEEDUP 0
/* The >= 1080p / 1440p / 2160p pixel-count thresholds are the shared
 * CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_* macros from ../cambi_internal.h; do not
 * re-declare them here — a local copy is exactly how the twins drift. */
#define CAMBI_METAL_BLOCK_X           16u
#define CAMBI_METAL_BLOCK_Y           16u

using IntegerCambiStateMetal = struct IntegerCambiStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;

    /* Pipeline states for the three GPU kernels. */
    void *pso_mask;
    void *pso_decimate;
    void *pso_filter_mode;

    /* Device (Shared-storage) buffers — flat uint16 arrays sized for the
     * larger of the encode and (with full_ref) the source picture. */
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

    /* The CPU cambi.c options. */
    int    enc_width;
    int    enc_height;
    int    enc_bitdepth;
    int    src_width;
    int    src_height;
    int    max_log_contrast;
    int    window_size;
    double topk;
    double cambi_topk;
    double tvi_threshold;
    double cambi_max_val;
    double cambi_vis_lum_threshold;
    bool   full_ref;
    char  *eotf;
    char  *cambi_eotf;
    char  *heatmaps_path;
    int    cambi_high_res_speedup;
    bool   high_res_speedup;

    /* One heatmap file per scale when heatmaps_path is set. */
    FILE  *heatmaps_files[CAMBI_METAL_NUM_SCALES];

    /* Largest picture the pipeline runs on (cambi.c::init's alloc_w/h). */
    unsigned alloc_width;
    unsigned alloc_height;

    /* Adjusted windows of the encode and the source picture. */
    uint16_t adjusted_window;
    uint16_t src_window;
    uint16_t vlt_luma;

    /* Scores computed in submit(), before the cambi_max_val cap. */
    double dist_score;
    double src_score;

    VmafDictionary *feature_name_dict;
};

/* --- Options: the CPU cambi.c table (names, aliases, defaults, ranges,
 * flags). --- */
static const VmafOption options[] = {
    {
        .name        = "cambi_max_val",
        .help        = "maximum value allowed; larger values will be clipped to this value",
        .alias       = "cmxv",
        .offset      = offsetof(IntegerCambiStateMetal, cambi_max_val),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = CAMBI_METAL_DEFAULT_MAX_VAL},
        .min         = 0.0,
        .max         = 1000.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "enc_width",
        .help        = "Encoding width",
        .alias       = "encw",
        .offset      = offsetof(IntegerCambiStateMetal, enc_width),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min         = 180,
        .max         = 7680,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "enc_height",
        .help        = "Encoding height",
        .alias       = "ench",
        .offset      = offsetof(IntegerCambiStateMetal, enc_height),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min         = 150,
        .max         = 7680,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "enc_bitdepth",
        .help        = "Encoding bitdepth",
        .alias       = "encbd",
        .offset      = offsetof(IntegerCambiStateMetal, enc_bitdepth),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min         = 6,
        .max         = 16,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "src_width",
        .help        = "Source width. Only used when full_ref=true.",
        .alias       = "srcw",
        .offset      = offsetof(IntegerCambiStateMetal, src_width),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min         = 320,
        .max         = 7680,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "src_height",
        .help        = "Source height. Only used when full_ref=true.",
        .alias       = "srch",
        .offset      = offsetof(IntegerCambiStateMetal, src_height),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = 0},
        .min         = 200,
        .max         = 4320,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "window_size",
        .help        = "Window size to compute CAMBI: 65 corresponds to ~1 degree at 4k",
        .alias       = "ws",
        .offset      = offsetof(IntegerCambiStateMetal, window_size),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = CAMBI_METAL_DEFAULT_WINDOW_SIZE},
        .min         = 15,
        .max         = 127,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "topk",
        .help        = "Ratio of pixels for the spatial pooling computation, "
                       "must be 0 < topk <= 1.0",
        .offset      = offsetof(IntegerCambiStateMetal, topk),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = CAMBI_METAL_DEFAULT_TOPK},
        .min         = 0.0001,
        .max         = 1.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "cambi_topk",
        .help        = "Ratio of pixels for the spatial pooling computation, "
                       "must be 0 < cambi_topk <= 1.0",
        .alias       = "ctpk",
        .offset      = offsetof(IntegerCambiStateMetal, cambi_topk),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = CAMBI_METAL_DEFAULT_TOPK},
        .min         = 0.0001,
        .max         = 1.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "tvi_threshold",
        .help        = "Visibility threshold for luminance dL < tvi_threshold*L_mean",
        .alias       = "tvit",
        .offset      = offsetof(IntegerCambiStateMetal, tvi_threshold),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = CAMBI_METAL_DEFAULT_TVI},
        .min         = 0.0001,
        .max         = 1.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "cambi_vis_lum_threshold",
        .help        = "Luminance value below which we assume any banding is not visible",
        .alias       = "vlt",
        .offset      = offsetof(IntegerCambiStateMetal, cambi_vis_lum_threshold),
        .type        = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = CAMBI_METAL_DEFAULT_VLT},
        .min         = 0.0,
        .max         = 300.0,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "max_log_contrast",
        .help        = "Maximum contrast in log luma level (2^max_log_contrast) at 10-bits; "
                       "from 0 to 5, default 2",
        .alias       = "mlc",
        .offset      = offsetof(IntegerCambiStateMetal, max_log_contrast),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = CAMBI_METAL_DEFAULT_MAX_LOG_CONTRAST},
        .min         = 0,
        .max         = 5,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "heatmaps_path",
        .help        = "Path where heatmaps will be dumped.",
        .offset      = offsetof(IntegerCambiStateMetal, heatmaps_path),
        .type        = VMAF_OPT_TYPE_STRING,
        .default_val = {.s = NULL},
    },
    {
        .name        = "full_ref",
        .help        = "If true, CAMBI will be run in full-reference mode and will be computed "
                       "on both the reference and distorted inputs",
        .offset      = offsetof(IntegerCambiStateMetal, full_ref),
        .type        = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name        = "eotf",
        .help        = "Determines the EOTF used to compute the visibility thresholds. "
                       "Possible values: ['bt1886', 'pq']. Default: 'bt1886'",
        .offset      = offsetof(IntegerCambiStateMetal, eotf),
        .type        = VMAF_OPT_TYPE_STRING,
        .default_val = {.s = CAMBI_METAL_DEFAULT_EOTF},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "cambi_eotf",
        .help        = "Determines the EOTF used to compute the visibility thresholds. "
                       "Possible values: ['bt1886', 'pq']. Default: 'bt1886'. If both eotf and "
                       "cambi_eotf are set, cambi_eotf takes precedence.",
        .alias       = "ceot",
        .offset      = offsetof(IntegerCambiStateMetal, cambi_eotf),
        .type        = VMAF_OPT_TYPE_STRING,
        .default_val = {.s = CAMBI_METAL_DEFAULT_EOTF},
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name        = "cambi_high_res_speedup",
        .help        = "Speed up the processing by downsampling post spatial mask for "
                       "resolutions >= 1080p. Min speed-up resolution possible values: "
                       "[1080, 1440, 2160, 0]. Default: 0 (not applied)",
        .alias       = "hrs",
        .offset      = offsetof(IntegerCambiStateMetal, cambi_high_res_speedup),
        .type        = VMAF_OPT_TYPE_INT,
        .default_val = {.i = CAMBI_METAL_DEFAULT_HIGH_RES_SPEEDUP},
        .min         = 0,
        .max         = 2160,
        .flags       = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {.name=nullptr},
};

/* ADR-1219 — delegate to the shared CPU helper rather than re-deriving the
 * TVI table.
 *
 * This used to hand-roll a binary search whose predicate was the NEGATION of
 * the CPU's `tvi_hard_threshold_condition`, seeded from luma 0 instead of
 * `luma_range.foot`, and it derived `vlt_luma` as the LARGEST luma below the
 * visibility threshold where the CPU takes the SMALLEST luma at or above it.
 * At the default `max_log_contrast = 2` that produced
 * `tvi_for_diff = [1026, 1025, 1024, 64]` against the CPU's
 * `[182, 309, 436, 563]`, which collapses the derived luma band from 564
 * entries to 65 and drops almost every pixel as out-of-band.
 *
 * `vmaf_cambi_init_tvi_and_vlt()` is the same helper the SYCL twin calls; it
 * runs the CPU's own bisection, so the twins cannot drift again. */
static int cambi_metal_init_tvi(IntegerCambiStateMetal *s)
{
    const int num_diffs = 1 << s->max_log_contrast;
    return vmaf_cambi_init_tvi_and_vlt(num_diffs, s->buffers.diffs_to_consider, s->tvi_threshold,
                                       s->cambi_vis_lum_threshold, s->cambi_eotf, s->eotf,
                                       s->buffers.tvi_for_diff, &s->vlt_luma, nullptr, nullptr);
}

/* ------------------------------------------------------------------ */
/* build_pipelines: load the three CAMBI kernels from the embedded     */
/* metallib (same blob pattern as every other Metal feature kernel).   */
/* ------------------------------------------------------------------ */
static int build_pipelines(IntegerCambiStateMetal *s, id<MTLDevice> device)
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

    id<MTLFunction> fn_mask = [lib newFunctionWithName:@"cambi_mask_kernel"];
    id<MTLFunction> fn_dec  = [lib newFunctionWithName:@"cambi_decimate_kernel"];
    id<MTLFunction> fn_fm   = [lib newFunctionWithName:@"cambi_filter_mode_kernel"];
    if (fn_mask == nil || fn_dec == nil || fn_fm == nil) { return -ENODEV; }

    id<MTLComputePipelineState> pso_mask =
        [device newComputePipelineStateWithFunction:fn_mask error:&err];
    id<MTLComputePipelineState> pso_dec =
        [device newComputePipelineStateWithFunction:fn_dec error:&err];
    id<MTLComputePipelineState> pso_fm =
        [device newComputePipelineStateWithFunction:fn_fm error:&err];
    if (pso_mask == nil || pso_dec == nil || pso_fm == nil) { return -ENODEV; }

    s->pso_mask        = (__bridge_retained void *)pso_mask;
    s->pso_decimate    = (__bridge_retained void *)pso_dec;
    s->pso_filter_mode = (__bridge_retained void *)pso_fm;
    return 0;
}

static void release_host_buffers(IntegerCambiStateMetal *s)
{
    free(s->buffers.diffs_to_consider);   s->buffers.diffs_to_consider   = nullptr;
    free(s->buffers.diff_weights);        s->buffers.diff_weights        = nullptr;
    free(s->buffers.all_diffs);           s->buffers.all_diffs           = nullptr;
    free(s->buffers.tvi_for_diff);        s->buffers.tvi_for_diff        = nullptr;
    free(s->buffers.c_values);            s->buffers.c_values            = nullptr;
    free(s->buffers.c_values_histograms); s->buffers.c_values_histograms = nullptr;
    free(s->buffers.mask_dp);             s->buffers.mask_dp             = nullptr;
    free(s->buffers.filter_mode_buffer);  s->buffers.filter_mode_buffer  = nullptr;
    free(s->buffers.derivative_buffer);   s->buffers.derivative_buffer   = nullptr;
}

/* ------------------------------------------------------------------ */
/* init                                                                 */
/* ------------------------------------------------------------------ */

/* Encode and source sizes, as cambi.c::validate_and_setup_dimensions()
 * resolves and checks them: an unset size is the picture's, an encode larger
 * than the picture is not upscaled back, both sizes must reach the CAMBI
 * minimum, and an encode and a source that scale in opposite directions have
 * no defined CAMBI interpretation. */
static int cambi_metal_resolve_dimensions(IntegerCambiStateMetal *s, unsigned bpc, unsigned w,
                                          unsigned h)
{
    if (s->enc_bitdepth == 0) { s->enc_bitdepth = (int)bpc; }
    if (s->enc_width == 0 || s->enc_height == 0) {
        s->enc_width  = (int)w;
        s->enc_height = (int)h;
    }
    if (s->src_width == 0 || s->src_height == 0) {
        s->src_width  = (int)w;
        s->src_height = (int)h;
    }
    if (std::cmp_greater(s->enc_height, h) || std::cmp_greater(s->enc_width, w)) {
        s->enc_width  = (int)w;
        s->enc_height = (int)h;
    }
    if (!cambi_validate_dimensions((unsigned)s->enc_width, (unsigned)s->enc_height) ||
        !cambi_validate_dimensions((unsigned)s->src_width, (unsigned)s->src_height)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "integer_cambi_metal: encode %dx%d or source %dx%d below minimum %dx%d.\n",
                 s->enc_width, s->enc_height, s->src_width, s->src_height,
                 CAMBI_METAL_MIN_WIDTH_HEIGHT, CAMBI_METAL_MIN_WIDTH_HEIGHT);
        return -EINVAL;
    }
    if ((s->src_width > s->enc_width && s->src_height < s->enc_height) ||
        (s->src_width < s->enc_width && s->src_height > s->enc_height)) {
        return -EINVAL;
    }
    return 0;
}

/* The high-res speed-up for this encode size (cambi.c clears the option below
 * its tier), the adjusted encode and source windows (cambi.c's
 * adjust_window_size() through vmaf_cambi_adjust_window()) and the CPU's
 * reciprocal-table bound on the larger of the two. */
static int cambi_metal_resolve_windows(IntegerCambiStateMetal *s)
{
    const int enc_pix = s->enc_width * s->enc_height;
    switch (s->cambi_high_res_speedup) {
    case 1080:
        s->high_res_speedup = enc_pix >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1080p;
        break;
    case 1440:
        s->high_res_speedup = enc_pix >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1440p;
        break;
    case 2160:
        s->high_res_speedup = enc_pix >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_2160p;
        break;
    default:
        s->high_res_speedup = false;
        break;
    }
    s->adjusted_window = vmaf_cambi_adjust_window(s->window_size, (unsigned)s->enc_width,
                                                  (unsigned)s->enc_height, s->high_res_speedup);
    s->src_window = vmaf_cambi_adjust_window(s->window_size, (unsigned)s->src_width,
                                             (unsigned)s->src_height, s->high_res_speedup);
    return vmaf_cambi_check_window_fits_lut(s->adjusted_window, s->src_window);
}

/* Contrast arrays (the CPU's contrast weights), the TVI table and the host
 * scratch of the CPU residual, sized for alloc_width. */
static int cambi_metal_alloc_host_buffers(IntegerCambiStateMetal *s)
{
    const int num_diffs = 1 << s->max_log_contrast;
    s->buffers.diffs_to_consider = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)num_diffs);
    s->buffers.diff_weights      = (int *)malloc(sizeof(int) * (size_t)num_diffs);
    s->buffers.all_diffs         = (int *)malloc(sizeof(int) * (size_t)(2 * num_diffs + 1));
    s->buffers.tvi_for_diff      = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)num_diffs);
    if (!s->buffers.diffs_to_consider || !s->buffers.diff_weights || !s->buffers.all_diffs ||
        !s->buffers.tvi_for_diff) {
        return -ENOMEM;
    }
    const int *contrast_weights = vmaf_cambi_contrast_weights(nullptr);
    for (int d = 0; d < num_diffs; d++) {
        s->buffers.diffs_to_consider[d] = (uint16_t)(d + 1);
        s->buffers.diff_weights[d]      = contrast_weights[d];
    }
    for (int d = -num_diffs; d <= num_diffs; d++) {
        s->buffers.all_diffs[d + num_diffs] = d;
    }
    const int err = cambi_metal_init_tvi(s);
    if (err) { return err; }

    const size_t w = s->alloc_width;
    const uint16_t num_bins = (uint16_t)(1024u + (unsigned)(s->buffers.all_diffs[2 * num_diffs] -
                                                            s->buffers.all_diffs[0]));
    const size_t pad_size  = CAMBI_METAL_MASK_FILTER_SIZE / 2;
    const size_t dp_width  = w + 2u * pad_size + 1u;
    const size_t dp_height = 2u * pad_size + 2u;
    s->buffers.c_values = (float *)malloc(sizeof(float) * w * (size_t)s->alloc_height);
    s->buffers.c_values_histograms = (uint16_t *)malloc(sizeof(uint16_t) * w * (size_t)num_bins);
    s->buffers.mask_dp = (uint32_t *)malloc(sizeof(uint32_t) * dp_width * dp_height);
    s->buffers.filter_mode_buffer = (uint16_t *)malloc(sizeof(uint16_t) * 3u * w);
    s->buffers.derivative_buffer = (uint16_t *)malloc(sizeof(uint16_t) * w);
    if (!s->buffers.c_values || !s->buffers.c_values_histograms || !s->buffers.mask_dp ||
        !s->buffers.filter_mode_buffer || !s->buffers.derivative_buffer) {
        return -ENOMEM;
    }
    return 0;
}

/* The device planes and the pipelines. */
static int cambi_metal_alloc_device(IntegerCambiStateMetal *s)
{
    void  const*dh = vmaf_metal_context_device_handle(s->ctx);
    if (dh == nullptr) { return -ENODEV; }
    id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

    const size_t buf_bytes = (size_t)s->alloc_width * (size_t)s->alloc_height * sizeof(uint16_t);
    id<MTLBuffer> b_image = [device newBufferWithLength:buf_bytes
                                               options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_mask  = [device newBufferWithLength:buf_bytes
                                               options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_tmp   = [device newBufferWithLength:buf_bytes
                                               options:MTLResourceStorageModeShared];
    if (b_image == nil || b_mask == nil || b_tmp == nil) { return -ENOMEM; }
    s->d_image = (__bridge_retained void *)b_image;
    s->d_mask  = (__bridge_retained void *)b_mask;
    s->d_tmp   = (__bridge_retained void *)b_tmp;
    return build_pipelines(s, device);
}

/* Tear down everything init() may have set up; every step tolerates a handle
 * that was never created, so this serves a failed init() and close(). */
static int cambi_metal_release(IntegerCambiStateMetal *s)
{
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
    void * const*handles[] = {&s->pso_filter_mode, &s->pso_decimate, &s->pso_mask,
                        &s->d_tmp,           &s->d_mask,       &s->d_image};
    for (auto & handle : handles) {
        if (*handle != nullptr) {
            (void)(__bridge_transfer id)*handle;
            *handle = nullptr;
        }
    }
    for (auto & pic : s->pics) {
        /* A slot init() never allocated has no ref; unref would fail on it. */
        if (pic.ref != nullptr) {
            const int e = vmaf_picture_unref(&pic);
            if (e != 0 && rc == 0) { rc = e; }
        }
    }
    release_host_buffers(s);
    const int heatmaps = vmaf_cambi_close_heatmaps(s->heatmaps_files);
    if (heatmaps != 0 && rc == 0) { rc = heatmaps; }
    if (s->feature_name_dict != NULL) {
        const int e = vmaf_dictionary_free(&s->feature_name_dict);
        if (e != 0 && rc == 0) { rc = e; }
    }
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = nullptr;
    return rc;
}

/* Everything init() allocates once the sizes are resolved: the context, the
 * host pictures and scratch of the CPU residual, the device planes and
 * pipelines, and the heatmap files of the encode size (cambi.c::init's
 * open_heatmaps()). */
static int cambi_metal_init_resources(IntegerCambiStateMetal *s)
{
    /* cambi.c::init's alloc_w / alloc_h: the source size counts only under
     * full_ref, where the reference picture runs at it. */
    s->alloc_width  = (unsigned)s->enc_width;
    s->alloc_height = (unsigned)s->enc_height;
    if (s->full_ref) {
        s->alloc_width  = (unsigned)((s->src_width > s->enc_width) ? s->src_width : s->enc_width);
        s->alloc_height =
            (unsigned)((s->src_height > s->enc_height) ? s->src_height : s->enc_height);
    }

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err == 0) {
        err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    }
    for (unsigned i = 0; i < 2u && err == 0; ++i) {
        err = vmaf_picture_alloc(&s->pics[i], VMAF_PIX_FMT_YUV400P, 10, s->alloc_width,
                                 s->alloc_height);
    }
    if (err == 0) {
        err = cambi_metal_alloc_host_buffers(s);
    }
    if (err == 0) {
        vmaf_cambi_default_callbacks(&s->inc_range_callback, &s->dec_range_callback,
                                     &s->derivative_callback);
        err = cambi_metal_alloc_device(s);
    }
    if (err == 0) {
        err = vmaf_cambi_open_heatmaps(s->heatmaps_path, (unsigned)s->enc_width,
                                       (unsigned)s->enc_height, s->heatmaps_files);
    }
    return err;
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                          unsigned w, unsigned h)
{
    (void)pix_fmt;
    IntegerCambiStateMetal *s = (IntegerCambiStateMetal *)fex->priv;

    /* cambi.c::init's order: the feature names follow the options as the
     * caller set them. cambi_metal_resolve_dimensions() writes the resolved
     * encode and source sizes into their option slots (FEATURE_PARAM, default
     * 0), so names built after it carry `_encbd_8_ench_..._srcw_...`
     * (T-METAL-CAMBI-SCORE-NAME-SUFFIXED-2026-10-05). */
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) { return -ENOMEM; }

    int err = cambi_metal_resolve_dimensions(s, bpc, w, h);
    if (err == 0) {
        err = cambi_metal_resolve_windows(s);
    }
    if (err == 0) {
        err = cambi_metal_init_resources(s);
    }
    if (err != 0 && cambi_metal_release(s) != 0) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "integer_cambi_metal: release after a failed init failed\n");
    }
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

static int cambi_metal_commit(id<MTLCommandBuffer> cmd)
{
    [cmd commit];
    [cmd waitUntilCompleted];
    return ([cmd status] == MTLCommandBufferStatusCompleted) ? 0 : -EIO;
}

/* The three device planes of one picture's pipeline. They rotate through
 * local handles only: s->d_image / d_mask / d_tmp keep pointing at the
 * buffers init() made. */
using CambiMetalPlanes = struct CambiMetalPlanes {
    void *image;
    void *mask;
    void *tmp;
    unsigned width;
    unsigned height;
};

/* pics[0] <- `pic` decimated or resized to width x height and shifted to 10
 * bits (the CPU's vmaf_cambi_preprocessing()), uploaded packed to d_image,
 * then the scale-0 spatial mask (d_image -> d_mask). Metal pictures are
 * host-resident (unified memory), so the host reads the plane directly. */
static int cambi_metal_prepare(IntegerCambiStateMetal *s, id<MTLCommandQueue> queue,
                               const VmafPicture *pic, unsigned width, unsigned height)
{
    const int err = vmaf_cambi_preprocessing(pic, &s->pics[0], (int)width, (int)height,
                                             s->enc_bitdepth);
    if (err) { return err; }
    uint16_t *img = (uint16_t *)[(__bridge id<MTLBuffer>)s->d_image contents];
    const uint16_t *src = (const uint16_t *)s->pics[0].data[0];
    const ptrdiff_t src_stride_words = s->pics[0].stride[0] >> 1;
    for (unsigned y = 0; y < height; ++y) {
        memcpy(img + (size_t)y * width, src + (size_t)y * (size_t)src_stride_words,
               (size_t)width * sizeof(uint16_t));
    }

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }
    const uint32_t p[4] = {width, height, width, vmaf_cambi_mask_index(width, height)};
    encode_kernel(cmd, (__bridge id<MTLComputePipelineState>)s->pso_mask,
                  (__bridge id<MTLBuffer>)s->d_image, (__bridge id<MTLBuffer>)s->d_mask, p,
                  width, height);
    return cambi_metal_commit(cmd);
}

static void cambi_metal_swap(void **a, void **b)
{
    void *t = *a;
    *a = *b;
    *b = t;
}

/* The GPU part of one scale (cambi.c::cambi_score): the strict 2x
 * decimation of image and mask from scale 1 on, or at every scale under the
 * high-res speed-up, then the mode filter H (image -> tmp) and V (tmp ->
 * image). */
static int cambi_metal_scale_gpu(IntegerCambiStateMetal *s, id<MTLCommandQueue> queue, int scale,
                                 CambiMetalPlanes *pl)
{
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }
    id<MTLComputePipelineState> pso_dec = (__bridge id<MTLComputePipelineState>)s->pso_decimate;
    id<MTLComputePipelineState> pso_fm  = (__bridge id<MTLComputePipelineState>)s->pso_filter_mode;

    if (scale > 0 || s->high_res_speedup) {
        const unsigned new_w = (pl->width + 1u) >> 1;
        const unsigned new_h = (pl->height + 1u) >> 1;
        const uint32_t pd[4] = {new_w, new_h, pl->width, new_w};
        encode_kernel(cmd, pso_dec, (__bridge id<MTLBuffer>)pl->image,
                      (__bridge id<MTLBuffer>)pl->tmp, pd, new_w, new_h);
        cambi_metal_swap(&pl->image, &pl->tmp);
        encode_kernel(cmd, pso_dec, (__bridge id<MTLBuffer>)pl->mask,
                      (__bridge id<MTLBuffer>)pl->tmp, pd, new_w, new_h);
        cambi_metal_swap(&pl->mask, &pl->tmp);
        pl->width  = new_w;
        pl->height = new_h;
    }

    const uint32_t ph[4] = {pl->width, pl->height, pl->width, 0u};
    encode_kernel(cmd, pso_fm, (__bridge id<MTLBuffer>)pl->image, (__bridge id<MTLBuffer>)pl->tmp,
                  ph, pl->width, pl->height);
    const uint32_t pv[4] = {pl->width, pl->height, pl->width, 1u};
    encode_kernel(cmd, pso_fm, (__bridge id<MTLBuffer>)pl->tmp, (__bridge id<MTLBuffer>)pl->image,
                  pv, pl->width, pl->height);
    return cambi_metal_commit(cmd);
}

/* The host part of one scale: the CPU's sliding-histogram c-values, written
 * to the scale's heatmap as frame `*heatmap_frame` when that is not NULL
 * (before the pooling's quick-select reorders them, as cambi.c::cambi_score
 * does), then the top-K pooling into `*score`. */
static int cambi_metal_scale_host(IntegerCambiStateMetal *s, const CambiMetalPlanes *pl,
                                  uint16_t window, double topk, int scale,
                                  const unsigned *heatmap_frame, double *score)
{
    copy_buf_to_pic((__bridge id<MTLBuffer>)pl->image, &s->pics[0], pl->width, pl->height);
    copy_buf_to_pic((__bridge id<MTLBuffer>)pl->mask, &s->pics[1], pl->width, pl->height);
    const int num_diffs = 1 << s->max_log_contrast;
    vmaf_cambi_calculate_c_values(&s->pics[0], &s->pics[1], s->buffers.c_values,
                                  s->buffers.c_values_histograms, window, (uint16_t)num_diffs,
                                  s->buffers.tvi_for_diff, s->vlt_luma, s->buffers.diff_weights,
                                  s->buffers.all_diffs, (int)pl->width, (int)pl->height,
                                  s->inc_range_callback, s->dec_range_callback);
    if (heatmap_frame != NULL) {
        const int err = vmaf_cambi_dump_c_values(s->heatmaps_files, s->buffers.c_values,
                                                 (int)pl->width, (int)pl->height, scale,
                                                 (int)window, (uint16_t)num_diffs,
                                                 s->buffers.diff_weights, (int)*heatmap_frame);
        if (err != 0) { return err; }
    }
    *score = vmaf_cambi_spatial_pooling(s->buffers.c_values, topk, pl->width, pl->height);
    return 0;
}

/* cambi.c::preprocess_and_extract_cambi() for one picture at width x height
 * with the adjusted window `window`: the weighted score of the five scales,
 * before the cambi_max_val cap; the c-values go to the heatmaps as frame
 * `*heatmap_frame` when that is not NULL. */
static int cambi_metal_score(IntegerCambiStateMetal *s, const VmafPicture *pic, unsigned width,
                             unsigned height, uint16_t window, const unsigned *heatmap_frame,
                             double *score)
{
    void  const*qh = vmaf_metal_context_queue_handle(s->ctx);
    if (qh == nullptr) { return -ENODEV; }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;

    int err = cambi_metal_prepare(s, queue, pic, width, height);
    /* The original `topk` when it was set to a non-default value, else
     * `cambi_topk` (cambi.c). */
    const double topk = (s->topk != CAMBI_METAL_DEFAULT_TOPK) ? s->topk : s->cambi_topk;
    double scores_per_scale[CAMBI_METAL_NUM_SCALES] = {0.0, 0.0, 0.0, 0.0, 0.0};
    CambiMetalPlanes pl = {.image=s->d_image, .mask=s->d_mask, .tmp=s->d_tmp, .width=width, .height=height};
    for (int scale = 0; scale < CAMBI_METAL_NUM_SCALES && err == 0; ++scale) {
        err = cambi_metal_scale_gpu(s, queue, scale, &pl);
        if (err == 0) {
            err = cambi_metal_scale_host(s, &pl, window, topk, scale, heatmap_frame,
                                         &scores_per_scale[scale]);
        }
    }
    if (err == 0) {
        *score = vmaf_cambi_weight_scores_per_scale(scores_per_scale,
                                                    vmaf_cambi_get_pixels_in_window(window));
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* submit: the distorted picture at the encode size and, with full_ref, */
/* the reference at the source size (cambi.c::extract).                */
/* ------------------------------------------------------------------ */
static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    IntegerCambiStateMetal *s = (IntegerCambiStateMetal *)fex->priv;

    /* Heatmaps of the distorted picture only, as cambi.c writes them. */
    const unsigned *heatmap_frame = (s->heatmaps_path != NULL) ? &index : NULL;
    int err = cambi_metal_score(s, dist_pic, (unsigned)s->enc_width, (unsigned)s->enc_height,
                                s->adjusted_window, heatmap_frame, &s->dist_score);
    if (err == 0 && s->full_ref) {
        err = cambi_metal_score(s, ref_pic, (unsigned)s->src_width, (unsigned)s->src_height,
                                s->src_window, NULL, &s->src_score);
    }
    return err;
}

/* MIN(score, cambi_max_val), as cambi.c's MIN() spells it. */
static double cambi_metal_cap(const IntegerCambiStateMetal *s, double score)
{
    return (score < s->cambi_max_val) ? score : s->cambi_max_val;
}

/* ------------------------------------------------------------------ */
/* collect: emit the scores computed in submit(), as cambi.c::extract.  */
/* ------------------------------------------------------------------ */
static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index,
                             VmafFeatureCollector *feature_collector)
{
    IntegerCambiStateMetal  const*s = (IntegerCambiStateMetal *)fex->priv;
    int err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                      "Cambi_feature_cambi_score",
                                                      cambi_metal_cap(s, s->dist_score), index);
    if (err != 0 || !s->full_ref) { return err; }

    err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                  "cambi_source", cambi_metal_cap(s, s->src_score),
                                                  index);
    if (err != 0) { return err; }
    /* combine_dist_src_scores(): MAX(0, dist - src). */
    const double diff = s->dist_score - s->src_score;
    const double combined = (0 > diff) ? 0 : diff;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "cambi_full_reference",
                                                   cambi_metal_cap(s, combined), index);
}

/* ------------------------------------------------------------------ */
/* close                                                                */
/* ------------------------------------------------------------------ */
static int close_fex_metal(VmafFeatureExtractor *fex)
{
    return cambi_metal_release((IntegerCambiStateMetal *)fex->priv);
}

static const char *provided_features[] = {
    "Cambi_feature_cambi_score", nullptr
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
    .flush             = nullptr,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(IntegerCambiStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 15, /* 5 scales x (mask once + filter_H + filter_V) + decimate */
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
