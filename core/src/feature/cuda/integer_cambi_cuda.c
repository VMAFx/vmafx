/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CAMBI banding-detection feature extractor on the CUDA backend
 *  (T3-15 / ADR-0360). The Vulkan twin (cambi_vulkan.c, ADR-0210)
 *  was removed per ADR-0726 (Vulkan backend dropped).
 *
 *  Strategy II hybrid (ADR-0205 / ADR-0210 precedent):
 *
 *    GPU stages (three CUDA kernels in cambi_score.cu):
 *      - cambi_spatial_mask_kernel: derivative + 7×7 box sum + threshold
 *        → produces a uint16 mask buffer (0 = flat, 1 = edge).
 *      - cambi_decimate_kernel: strict 2× stride-2 subsample.
 *      - cambi_filter_mode_kernel: separable 3-tap mode filter (H + V).
 *
 *    Host CPU stages (exact CPU code via cambi_internal.h wrappers):
 *      - vmaf_cambi_preprocessing: decimate/upcast to 10-bit.
 *      - vmaf_cambi_calculate_c_values: sliding-histogram c-value pass.
 *      - vmaf_cambi_spatial_pooling: top-K pooling → per-scale score.
 *      - vmaf_cambi_weight_scores_per_scale: inner-product scale weights.
 *
 *  Per-frame flow:
 *    1. Host preprocessing (CPU): resize/upcast dist_pic → pics[0].
 *    2. HtoD upload of pics[0] luma plane → d_image.
 *    3. Scale 0: GPU cambi_spatial_mask_kernel over d_image → d_mask.
 *    4. For scale = 0 .. NUM_SCALES-1:
 *         a. (scale > 0) GPU cambi_decimate_kernel on d_image → d_tmp,
 *            swap; same for d_mask.
 *         b. GPU cambi_filter_mode_kernel H: d_image → d_tmp.
 *         c. GPU cambi_filter_mode_kernel V: d_tmp → d_image.
 *         d. DtoH readback: d_image → pics[0], d_mask → pics[1].
 *         e. Host vmaf_cambi_calculate_c_values + vmaf_cambi_spatial_pooling.
 *    5. Host vmaf_cambi_weight_scores_per_scale → final score.
 *    6. Emit "Cambi_feature_cambi_score" into the feature collector.
 *
 *  Precision contract: `places=4` (ULP=0 on the emitted score). All GPU
 *  phases are integer + bit-exact. The host residual runs the exact CPU
 *  code via cambi_internal.h, so the emitted score is bit-for-bit
 *  identical to `vmaf_fex_cambi`. Cross-backend gate target: ULP=0.
 *
 *  Out of scope for v1 (future work):
 *    - full_ref / FR-CAMBI mode (no GPU twin for the source pyramid).
 *    - heatmap dump via heatmaps_path.
 *    - high_res_speedup (GPU decimate path makes it cheap but v1 omits it).
 *    - EOTF variants other than bt1886 (host TVI table is already correct).
 *
 *  CUDA async lifecycle mirrors integer_psnr_cuda.c:
 *    submit() enqueues all GPU work on the picture's stream, records
 *    submit event, DtoH-copies on the private stream, records finished.
 *    collect() drains the private stream and runs the host residual.
 *    This keeps the pipeline asynchronous with respect to motion_cuda et al.
 *
 *  IMPORTANT: Because the host residual in collect() does significant
 *  CPU work (calculate_c_values is O(W*H*num_diffs)), this extractor
 *  is NOT marked is_reduction_only. The dispatch_hint is set to
 *  VMAF_FEATURE_DISPATCH_SEQUENTIAL so the engine does not pipeline
 *  this extractor with itself across frames.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "common/alignment.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "cuda/integer_cambi_cuda.h"
#include "cuda/kernel_template.h"
#include "log.h"
#include "luminance_tools.h"
#include "mem.h"
#include "picture.h"
#include "picture_cuda.h"
#include "cuda_helper.cuh"

#include "feature/cambi_internal.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* --- Constants matching cambi.c --- */
/* CAMBI_MIN_WIDTH_HEIGHT and CAMBI_WINDOW_DIVISOR come from cambi_internal.h */
#define CAMBI_CUDA_NUM_SCALES 5
#define CAMBI_CUDA_MASK_FILTER_SIZE 7
#define CAMBI_CUDA_DEFAULT_MAX_VAL 1000.0
#define CAMBI_CUDA_DEFAULT_WINDOW_SIZE 65
#define CAMBI_CUDA_DEFAULT_TOPK 0.6
#define CAMBI_CUDA_DEFAULT_TVI 0.019
#define CAMBI_CUDA_DEFAULT_VLT 0.0
#define CAMBI_CUDA_DEFAULT_MAX_LOG_CONTRAST 2
#define CAMBI_CUDA_DEFAULT_EOTF "bt1886"
#define CAMBI_CUDA_BLOCK_X 16u
#define CAMBI_CUDA_BLOCK_Y 16u

typedef struct CambiStateCuda {
    /* CUDA lifecycle (stream + events). */
    VmafCudaKernelLifecycle lc;

    /* CUDA kernel function handles (loaded from cambi_score.cu). */
    CUfunction func_mask;
    CUfunction func_decimate;
    CUfunction func_filter_mode;

    /* Device buffers (flat uint16 arrays sized for proc_width × proc_height). */
    VmafCudaBuffer *d_image; /* current scale image on device */
    VmafCudaBuffer *d_mask;  /* spatial mask on device */
    VmafCudaBuffer *d_tmp;   /* scratch: filter_mode H output, decimate output */

    /* Host VmafPicture pair for DtoH readback (same role as Vulkan's pics[]). */
    VmafPicture pics[2]; /* pics[0] = image, pics[1] = mask */

    /* Host scratch buffers for the CPU residual (mirrors CambiVkState::buffers). */
    VmafCambiHostBuffers buffers;

    /* Callbacks (always scalar scalar; GPU has done the heavy lifting). */
    VmafCambiRangeUpdater inc_range_callback;
    VmafCambiRangeUpdater dec_range_callback;
    VmafCambiDerivativeCalculator derivative_callback;

    /* Configuration options (mirrors CambiState). */
    int enc_width;
    int enc_height;
    int enc_bitdepth;
    int max_log_contrast;
    int window_size;
    double topk;
    double cambi_topk;
    double tvi_threshold;
    double cambi_max_val;
    double cambi_vis_lum_threshold;
    char *eotf;
    char *cambi_eotf;
    /* Resolved in init_fex_cuda() against the encode pixel count, exactly as
     * cambi.c does: a value of 1080 / 1440 / 2160 below its own threshold is
     * reset to 0.  Non-zero means "decimate once before scale 0 and halve the
     * adjusted window" (cambi.c::cambi_score / adjust_window_size). */
    int cambi_high_res_speedup;

    /* Resolved per-frame geometry. */
    unsigned src_width;
    unsigned src_height;
    unsigned src_bpc;
    unsigned proc_width;
    unsigned proc_height;

    /* Adjusted window (cambi_vk::adjusted_window equivalent). */
    uint16_t adjusted_window;
    uint16_t vlt_luma;

    /* Per-frame index stored by submit() for collect(). */
    unsigned index;

    /* Per-scale geometry stored by submit() for collect(). */
    unsigned scale_widths[CAMBI_CUDA_NUM_SCALES];
    unsigned scale_heights[CAMBI_CUDA_NUM_SCALES];

    /* DtoH readback buffers — one flat slot per scale for image + mask.
     * We read back after every scale (as in Vulkan v1), so we need only
     * the current scale's readback; reuse the same pinned buffer. */
    VmafCudaKernelReadback rb_image;
    VmafCudaKernelReadback rb_mask;

    /* PTX module backing the CAMBI kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;

    VmafDictionary *feature_name_dict;
} CambiStateCuda;

/* --- Options --- */
static const VmafOption options[] = {
    {
        .name = "cambi_max_val",
        .help = "maximum value allowed; larger values will be clipped",
        .offset = offsetof(CambiStateCuda, cambi_max_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_CUDA_DEFAULT_MAX_VAL,
        .min = 0.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "cmxv",
    },
    {
        .name = "enc_width",
        .help = "Encoding width",
        .offset = offsetof(CambiStateCuda, enc_width),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 180,
        .max = 7680,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "encw",
    },
    {
        .name = "enc_height",
        .help = "Encoding height",
        .offset = offsetof(CambiStateCuda, enc_height),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 150,
        .max = 7680,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ench",
    },
    {
        .name = "enc_bitdepth",
        .help = "Encoding bitdepth",
        .offset = offsetof(CambiStateCuda, enc_bitdepth),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 6,
        .max = 16,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "encbd",
    },
    {
        .name = "window_size",
        .help = "Window size to compute CAMBI: 65 corresponds to ~1 degree at 4k",
        .offset = offsetof(CambiStateCuda, window_size),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = CAMBI_CUDA_DEFAULT_WINDOW_SIZE,
        .min = 15,
        .max = 127,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ws",
    },
    {
        .name = "topk",
        .help = "Ratio of pixels for the spatial pooling computation",
        .offset = offsetof(CambiStateCuda, topk),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_CUDA_DEFAULT_TOPK,
        .min = 0.0001,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "cambi_topk",
        .help = "Ratio of pixels for the spatial pooling computation",
        .offset = offsetof(CambiStateCuda, cambi_topk),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_CUDA_DEFAULT_TOPK,
        .min = 0.0001,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ctpk",
    },
    {
        .name = "tvi_threshold",
        .help = "Visibility threshold ΔL < tvi_threshold * L_mean",
        .offset = offsetof(CambiStateCuda, tvi_threshold),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_CUDA_DEFAULT_TVI,
        .min = 0.0001,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "tvit",
    },
    {
        .name = "cambi_vis_lum_threshold",
        .help = "Luminance value below which banding is assumed invisible",
        .offset = offsetof(CambiStateCuda, cambi_vis_lum_threshold),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = CAMBI_CUDA_DEFAULT_VLT,
        .min = 0.0,
        .max = 300.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "vlt",
    },
    {
        .name = "max_log_contrast",
        .help = "Maximum log contrast (0 to 5, default 2)",
        .offset = offsetof(CambiStateCuda, max_log_contrast),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = CAMBI_CUDA_DEFAULT_MAX_LOG_CONTRAST,
        .min = 0,
        .max = 5,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "mlc",
    },
    {
        .name = "eotf",
        .help = "EOTF for visibility-threshold conversion (bt1886 / pq)",
        .offset = offsetof(CambiStateCuda, eotf),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = CAMBI_CUDA_DEFAULT_EOTF,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "cambi_eotf",
        .help = "EOTF override for cambi (defaults to eotf)",
        .offset = offsetof(CambiStateCuda, cambi_eotf),
        .type = VMAF_OPT_TYPE_STRING,
        .default_val.s = CAMBI_CUDA_DEFAULT_EOTF,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "ceot",
    },
    {
        .name = "cambi_high_res_speedup",
        .help =
            "Speed up the processing by downsampling post spatial mask for resolutions >= 1080p. "
            "Min speed-up resolution possible values: [1080, 1440, 2160, 0]. Default: 0 (not applied)",
        .offset = offsetof(CambiStateCuda, cambi_high_res_speedup),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = 0,
        .min = 0,
        .max = 2160,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
        .alias = "hrs",
    },
    {0},
};

/* ------------------------------------------------------------------ */
/* Helper: compute adjusted window size (mirrors cambi.c). */
/* ------------------------------------------------------------------ */
static uint16_t cambi_cuda_adjust_window(int window_size, unsigned w, unsigned h,
                                         bool cambi_high_res_speedup)
{
    /* cambi.c::adjust_window_size formula:
     *   window = window * (w + h) / (4K_W + 4K_H) / 16, rounded up to odd. */
    unsigned adjusted = (unsigned)(window_size) * (w + h) / (unsigned)CAMBI_WINDOW_DIVISOR;
    adjusted >>= 4;
    if (cambi_high_res_speedup) {
        adjusted = (adjusted + 1u) >> 1;
    }
    if (adjusted < 1u)
        adjusted = 1u;
    if ((adjusted & 1u) == 0u)
        adjusted++;
    return (uint16_t)adjusted;
}

/* ------------------------------------------------------------------ */
/* Helper: ceil_log2 for mask_index (mirrors cambi.c). */
/* ------------------------------------------------------------------ */
static uint16_t cambi_cuda_ceil_log2(uint32_t num)
{
    if (num == 0u)
        return 0u;
    uint32_t tmp = num - 1u;
    uint16_t shift = 0;
    while (tmp > 0u) {
        tmp >>= 1;
        shift++;
    }
    return shift;
}

static uint16_t cambi_cuda_get_mask_index(unsigned w, unsigned h, uint16_t filter_size)
{
    uint32_t shifted_wh = (w >> 6) * (h >> 6);
    return (
        uint16_t)((filter_size * filter_size + 3 * (cambi_cuda_ceil_log2(shifted_wh) - 11) - 1) >>
                  1);
}

/* ------------------------------------------------------------------ */
/* TVI table initialisation (mirrors cambi.c).                         */
/* ------------------------------------------------------------------ */
enum CambiCudaTVIBisectFlag {
    CAMBI_CUDA_TVI_BISECT_TOO_SMALL,
    CAMBI_CUDA_TVI_BISECT_CORRECT,
    CAMBI_CUDA_TVI_BISECT_TOO_BIG,
};

static bool cambi_cuda_tvi_condition(int sample, int diff, double tvi_threshold,
                                     VmafLumaRange luma_range, VmafEOTF eotf)
{
    double mean_luminance = vmaf_luminance_get_luminance(sample, luma_range, eotf);
    double diff_luminance = vmaf_luminance_get_luminance(sample + diff, luma_range, eotf);
    double delta_luminance = diff_luminance - mean_luminance;
    return (delta_luminance > tvi_threshold * mean_luminance);
}

static enum CambiCudaTVIBisectFlag cambi_cuda_tvi_hard_threshold_condition(int sample, int diff,
                                                                           double tvi_threshold,
                                                                           VmafLumaRange luma_range,
                                                                           VmafEOTF eotf)
{
    if (!cambi_cuda_tvi_condition(sample, diff, tvi_threshold, luma_range, eotf))
        return CAMBI_CUDA_TVI_BISECT_TOO_BIG;

    if (cambi_cuda_tvi_condition(sample + 1, diff, tvi_threshold, luma_range, eotf))
        return CAMBI_CUDA_TVI_BISECT_TOO_SMALL;

    return CAMBI_CUDA_TVI_BISECT_CORRECT;
}

static int cambi_cuda_get_tvi_for_diff(int diff, double tvi_threshold, int bitdepth,
                                       VmafLumaRange luma_range, VmafEOTF eotf)
{
    const int max_val = (1 << bitdepth) - 1;
    int foot = luma_range.foot;
    int head = luma_range.head - diff - 1;

    enum CambiCudaTVIBisectFlag tvi_bisect =
        cambi_cuda_tvi_hard_threshold_condition(foot, diff, tvi_threshold, luma_range, eotf);
    if (tvi_bisect == CAMBI_CUDA_TVI_BISECT_TOO_BIG)
        return 0;
    if (tvi_bisect == CAMBI_CUDA_TVI_BISECT_CORRECT)
        return foot;

    tvi_bisect =
        cambi_cuda_tvi_hard_threshold_condition(head, diff, tvi_threshold, luma_range, eotf);
    if (tvi_bisect == CAMBI_CUDA_TVI_BISECT_TOO_SMALL)
        return max_val;
    if (tvi_bisect == CAMBI_CUDA_TVI_BISECT_CORRECT)
        return head;

    while (head - foot > 1) {
        int mid = foot + (head - foot) / 2;
        tvi_bisect =
            cambi_cuda_tvi_hard_threshold_condition(mid, diff, tvi_threshold, luma_range, eotf);
        if (tvi_bisect == CAMBI_CUDA_TVI_BISECT_TOO_BIG) {
            head = mid;
        } else if (tvi_bisect == CAMBI_CUDA_TVI_BISECT_TOO_SMALL) {
            foot = mid;
        } else if (tvi_bisect == CAMBI_CUDA_TVI_BISECT_CORRECT) {
            return mid;
        }
    }
    return foot;
}

static int cambi_cuda_get_vlt_luma(double visibility_luminance_threshold, VmafLumaRange luma_range,
                                   VmafEOTF eotf)
{
    uint16_t sample = (uint16_t)luma_range.foot;
    while (vmaf_luminance_get_luminance(sample, luma_range, eotf) <
           visibility_luminance_threshold) {
        sample++;
    }
    if (sample == (uint16_t)luma_range.foot)
        return 0;
    return sample;
}

static int cambi_cuda_init_tvi(CambiStateCuda *s)
{
    VmafLumaRange luma_range;
    int err = vmaf_luminance_init_luma_range(&luma_range, 10, VMAF_PIXEL_RANGE_LIMITED);
    if (err)
        return err;

    const char *effective_eotf;
    if (s->cambi_eotf && strcmp(s->cambi_eotf, CAMBI_CUDA_DEFAULT_EOTF) != 0) {
        effective_eotf = s->cambi_eotf;
    } else {
        effective_eotf = (s->eotf != NULL) ? s->eotf : CAMBI_CUDA_DEFAULT_EOTF;
    }

    VmafEOTF eotf;
    err = vmaf_luminance_init_eotf(&eotf, effective_eotf);
    if (err)
        return err;

    const int num_diffs = 1 << s->max_log_contrast;
    for (int d = 0; d < num_diffs; d++) {
        s->buffers.tvi_for_diff[d] = (uint16_t)cambi_cuda_get_tvi_for_diff(
            s->buffers.diffs_to_consider[d], s->tvi_threshold, 10, luma_range, eotf);
        s->buffers.tvi_for_diff[d] += (uint16_t)num_diffs;
    }

    s->vlt_luma = (uint16_t)cambi_cuda_get_vlt_luma(s->cambi_vis_lum_threshold, luma_range, eotf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* cambi_init_unwind - the single teardown path for init_fex_cuda.
 *
 * HISS-01: this is the former `free_ref` label block, moved verbatim
 * and in the same statement order. Every early-exit site passes its
 * live `err`, so the code returned here is bit-identical to the
 * label's `(err != 0) ? err : -ENOMEM`.
 */
static int cambi_init_unwind(VmafFeatureExtractor *fex, CambiStateCuda *s, int err)
{
    /* Best-effort teardown; close_fex_cuda handles null checks. */
    (void)vmaf_picture_unref(&s->pics[0]);
    (void)vmaf_picture_unref(&s->pics[1]);
    if (s->d_image)
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->d_image);
    if (s->d_mask)
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->d_mask);
    if (s->d_tmp)
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->d_tmp);
    (void)vmaf_cuda_kernel_readback_free(&s->rb_image, fex->cu_state);
    (void)vmaf_cuda_kernel_readback_free(&s->rb_mask, fex->cu_state);
    free(s->buffers.diffs_to_consider);
    free(s->buffers.diff_weights);
    free(s->buffers.all_diffs);
    free(s->buffers.tvi_for_diff);
    free(s->buffers.c_values);
    free(s->buffers.c_values_histograms);
    free(s->buffers.mask_dp);
    free(s->buffers.filter_mode_buffer);
    free(s->buffers.derivative_buffer);
    if (s->feature_name_dict)
        (void)vmaf_dictionary_free(&s->feature_name_dict);
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    return (err != 0) ? err : -ENOMEM;
}

/* cambi_resolve_geometry - encoded geometry, high-res speedup and window.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda; the resolution fallbacks,
 * the speedup switch and the window adjustment are unchanged.
 */
static int cambi_resolve_geometry(CambiStateCuda *s, unsigned bpc, unsigned w, unsigned h)
{
    /* Resolve enc geometry (matches cambi.c::init logic). */
    if (s->enc_bitdepth == 0)
        s->enc_bitdepth = (int)bpc;
    if (s->enc_width == 0 || s->enc_height == 0) {
        s->enc_width = (int)w;
        s->enc_height = (int)h;
    }
    if ((unsigned)s->enc_height > h || (unsigned)s->enc_width > w) {
        s->enc_width = (int)w;
        s->enc_height = (int)h;
    }
    if (!cambi_validate_dimensions(s->enc_width, s->enc_height)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "cambi_cuda: encoded resolution %dx%d below minimum %d×%d.\n", s->enc_width,
                 s->enc_height, CAMBI_MIN_WIDTH_HEIGHT, CAMBI_MIN_WIDTH_HEIGHT);
        return -EINVAL;
    }

    const int enc_pix = s->enc_width * s->enc_height;
    switch (s->cambi_high_res_speedup) {
    case 1080:
        if (enc_pix < CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1080p)
            s->cambi_high_res_speedup = 0;
        break;
    case 1440:
        if (enc_pix < CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1440p)
            s->cambi_high_res_speedup = 0;
        break;
    case 2160:
        if (enc_pix < CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_2160p)
            s->cambi_high_res_speedup = 0;
        break;
    default:
        s->cambi_high_res_speedup = 0;
        break;
    }

    s->src_width = w;
    s->src_height = h;
    s->src_bpc = bpc;
    s->proc_width = (unsigned)s->enc_width;
    s->proc_height = (unsigned)s->enc_height;

    s->adjusted_window = cambi_cuda_adjust_window(s->window_size, s->proc_width, s->proc_height,
                                                  (bool)s->cambi_high_res_speedup);
    return 0;
}

/* cambi_load_kernels - module load plus the three kernel handles.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda. CHECK_CUDA_GOTO and the two
 * labels it targets move with it, so the context is still popped exactly once
 * on every exit path and the lifecycle is closed in the same order.
 */
static int cambi_load_kernels(VmafFeatureExtractor *fex, CambiStateCuda *s, CudaFunctions *cu_f)
{
    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail_cuda);
    ctx_pushed = 1;

    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, cambi_score_ptx), fail_cuda);
    CHECK_CUDA_GOTO(cu_f,
                    cuModuleGetFunction(&s->func_mask, s->module, "cambi_spatial_mask_kernel"),
                    fail_cuda);
    CHECK_CUDA_GOTO(cu_f,
                    cuModuleGetFunction(&s->func_decimate, s->module, "cambi_decimate_kernel"),
                    fail_cuda);
    CHECK_CUDA_GOTO(
        cu_f, cuModuleGetFunction(&s->func_filter_mode, s->module, "cambi_filter_mode_kernel"),
        fail_cuda);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    ctx_pushed = 0;
    return 0;

fail_cuda:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    return _cuda_err;
}

/* cambi_alloc_device - device buffers, pinned readbacks and the two host
 * VmafPictures the CPU residual works on.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda; each failure still unwinds
 * through cambi_init_unwind() with the same error value.
 */
static int cambi_alloc_device(VmafFeatureExtractor *fex, CambiStateCuda *s)
{
    /* Device buffers: one flat uint16 array per logical buffer, sized for the
     * full (proc_width × proc_height) luma plane at scale 0. Per-scale
     * dispatches read only the leading (scaled_w × scaled_h) prefix. */
    const size_t buf_bytes = (size_t)s->proc_width * s->proc_height * sizeof(uint16_t);
    int err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_image, buf_bytes);
    if (err)
        return cambi_init_unwind(fex, s, err);
    err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_mask, buf_bytes);
    if (err)
        return cambi_init_unwind(fex, s, err);
    err = vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_tmp, buf_bytes);
    if (err)
        return cambi_init_unwind(fex, s, err);

    /* Pinned readback buffers: sized for scale-0 (worst case). */
    err = vmaf_cuda_kernel_readback_alloc(&s->rb_image, fex->cu_state, buf_bytes);
    if (err)
        return cambi_init_unwind(fex, s, err);
    err = vmaf_cuda_kernel_readback_alloc(&s->rb_mask, fex->cu_state, buf_bytes);
    if (err)
        return cambi_init_unwind(fex, s, err);

    /* Host VmafPictures for the CPU residual. */
    err = vmaf_picture_alloc(&s->pics[0], VMAF_PIX_FMT_YUV400P, 10, s->proc_width, s->proc_height);
    if (err)
        return cambi_init_unwind(fex, s, err);
    err = vmaf_picture_alloc(&s->pics[1], VMAF_PIX_FMT_YUV400P, 10, s->proc_width, s->proc_height);
    if (err)
        return cambi_init_unwind(fex, s, err);
    return 0;
}

/* cambi_alloc_diff_tables - the contrast difference tables and the TVI LUT.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda. `err` starts at 0 here just
 * as it did at this point in the original function, so a failed malloc still
 * unwinds with -ENOMEM.
 */
static int cambi_alloc_diff_tables(VmafFeatureExtractor *fex, CambiStateCuda *s, int num_diffs)
{
    int err = 0;
    /* Host scratch buffers for the CPU residual. */
    s->buffers.diffs_to_consider = malloc(sizeof(uint16_t) * (size_t)num_diffs);
    if (!s->buffers.diffs_to_consider)
        return cambi_init_unwind(fex, s, err);
    s->buffers.diff_weights = malloc(sizeof(int) * (size_t)num_diffs);
    if (!s->buffers.diff_weights)
        return cambi_init_unwind(fex, s, err);
    s->buffers.all_diffs = malloc(sizeof(int) * (size_t)(2 * num_diffs + 1));
    if (!s->buffers.all_diffs)
        return cambi_init_unwind(fex, s, err);

    /* Suprathreshold contrast weights (g_contrast_weights from cambi.c). */
    static const int contrast_weights[32] = {1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8,
                                             8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    for (int d = 0; d < num_diffs; d++) {
        s->buffers.diffs_to_consider[d] = (uint16_t)(d + 1);
        s->buffers.diff_weights[d] = contrast_weights[d];
    }
    for (int d = -num_diffs; d <= num_diffs; d++)
        s->buffers.all_diffs[d + num_diffs] = d;

    s->buffers.tvi_for_diff = malloc(sizeof(uint16_t) * (size_t)num_diffs);
    if (!s->buffers.tvi_for_diff)
        return cambi_init_unwind(fex, s, err);

    err = cambi_cuda_init_tvi(s);
    if (err)
        return cambi_init_unwind(fex, s, err);
    return 0;
}

/* cambi_alloc_histograms - c_values, the histogram bins and the mask scratch.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda; the bin-count arithmetic and
 * the underflow guard are unchanged.
 */
static int cambi_alloc_histograms(VmafFeatureExtractor *fex, CambiStateCuda *s, int num_diffs)
{
    const int err = 0;
    s->buffers.c_values = malloc(sizeof(float) * s->proc_width * s->proc_height);
    if (!s->buffers.c_values)
        return cambi_init_unwind(fex, s, err);

    int v_lo_signed = (int)s->vlt_luma - 3 * (int)num_diffs + 1;
    uint16_t v_band_base = v_lo_signed > 0 ? (uint16_t)v_lo_signed : 0;
    int v_band_size_signed = (int)s->buffers.tvi_for_diff[num_diffs - 1] + 1 - (int)v_band_base;
    if (v_band_size_signed <= 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "cambi_cuda: v_band_size underflow (tvi_max=%u v_band_base=%u); "
                 "cambi_vis_lum_threshold may be too low\n",
                 (unsigned)s->buffers.tvi_for_diff[num_diffs - 1], (unsigned)v_band_base);
        return cambi_init_unwind(fex, s, err);
    }

    const uint16_t num_bins = (uint16_t)(1024u + (unsigned)(s->buffers.all_diffs[2 * num_diffs] -
                                                            s->buffers.all_diffs[0]));
    const size_t hist_bins = (size_t)v_band_size_signed > (size_t)num_bins ?
                                 (size_t)v_band_size_signed :
                                 (size_t)num_bins;
    s->buffers.c_values_histograms = malloc(sizeof(uint16_t) * s->proc_width * hist_bins);
    if (!s->buffers.c_values_histograms)
        return cambi_init_unwind(fex, s, err);

    /* mask_dp scratch (not used for GPU; kept for cambi_internal API). */
    const int pad_size = CAMBI_CUDA_MASK_FILTER_SIZE / 2;
    const int dp_width = (int)s->proc_width + 2 * pad_size + 1;
    const int dp_height = 2 * pad_size + 2;
    s->buffers.mask_dp = malloc(sizeof(uint32_t) * (size_t)dp_width * (size_t)dp_height);
    if (!s->buffers.mask_dp)
        return cambi_init_unwind(fex, s, err);

    s->buffers.filter_mode_buffer = malloc(sizeof(uint16_t) * 3u * s->proc_width);
    if (!s->buffers.filter_mode_buffer)
        return cambi_init_unwind(fex, s, err);
    s->buffers.derivative_buffer = malloc(sizeof(uint16_t) * s->proc_width);
    if (!s->buffers.derivative_buffer)
        return cambi_init_unwind(fex, s, err);
    return 0;
}

/* ------------------------------------------------------------------ */
/* init_fex_cuda */
/* ------------------------------------------------------------------ */
static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    CambiStateCuda *s = fex->priv;

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;

    int err = cambi_resolve_geometry(s, bpc, w, h);
    if (err)
        return err;

    /* CUDA lifecycle. */
    err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;

    CudaFunctions *cu_f = fex->cu_state->f;
    err = cambi_load_kernels(fex, s, cu_f);
    if (err)
        return err;

    err = cambi_alloc_device(fex, s);
    if (err)
        return err;

    const int num_diffs = 1 << s->max_log_contrast;
    err = cambi_alloc_diff_tables(fex, s, num_diffs);
    if (err)
        return err;

    err = cambi_alloc_histograms(fex, s, num_diffs);
    if (err)
        return err;

    vmaf_cambi_default_callbacks(&s->inc_range_callback, &s->dec_range_callback,
                                 &s->derivative_callback);

    return 0;
}

/* ------------------------------------------------------------------ */
/* dispatch_mask — GPU spatial-mask kernel over (w × h) of d_image. */
/* ------------------------------------------------------------------ */
static int dispatch_mask(CambiStateCuda *s, CudaFunctions *cu_f, CUstream stream, unsigned w,
                         unsigned h, unsigned stride_words, unsigned mask_index)
{
    const unsigned grid_x = (w + CAMBI_CUDA_BLOCK_X - 1u) / CAMBI_CUDA_BLOCK_X;
    const unsigned grid_y = (h + CAMBI_CUDA_BLOCK_Y - 1u) / CAMBI_CUDA_BLOCK_Y;
    /* Bug fix (Issue lusoris/vmaf#857): cuLaunchKernel params[i] must point to the VALUE
     * to pass, not to the VmafCudaBuffer struct. Pass &buf->data (address of the
     * CUdeviceptr field) so the driver reads the device pointer, not buf->size. */
    void *params[] = {&s->d_image->data, &s->d_mask->data, &w, &h, &stride_words, &mask_index};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_mask, grid_x, grid_y, 1u, CAMBI_CUDA_BLOCK_X,
                                           CAMBI_CUDA_BLOCK_Y, 1u, 0u, stream, params, NULL));
    return 0;
}

/* ------------------------------------------------------------------ */
/* dispatch_decimate — GPU 2× decimate of src → dst. */
/* ------------------------------------------------------------------ */
static int dispatch_decimate(CambiStateCuda *s, CudaFunctions *cu_f, CUstream stream,
                             VmafCudaBuffer *src, VmafCudaBuffer *dst, unsigned out_w,
                             unsigned out_h, unsigned src_stride_words, unsigned dst_stride_words)
{
    const unsigned grid_x = (out_w + CAMBI_CUDA_BLOCK_X - 1u) / CAMBI_CUDA_BLOCK_X;
    const unsigned grid_y = (out_h + CAMBI_CUDA_BLOCK_Y - 1u) / CAMBI_CUDA_BLOCK_Y;
    /* Bug fix (Issue lusoris/vmaf#857): pass device pointer addresses, not struct addresses. */
    void *params[] = {&src->data, &dst->data, &out_w, &out_h, &src_stride_words, &dst_stride_words};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_decimate, grid_x, grid_y, 1u, CAMBI_CUDA_BLOCK_X,
                                           CAMBI_CUDA_BLOCK_Y, 1u, 0u, stream, params, NULL));
    return 0;
}

/* ------------------------------------------------------------------ */
/* dispatch_filter_mode — GPU 3-tap mode filter (axis=0 H, axis=1 V). */
/* ------------------------------------------------------------------ */
static int dispatch_filter_mode(CambiStateCuda *s, CudaFunctions *cu_f, CUstream stream,
                                VmafCudaBuffer *in, VmafCudaBuffer *out, unsigned w, unsigned h,
                                unsigned stride_words, int axis)
{
    const unsigned grid_x = (w + CAMBI_CUDA_BLOCK_X - 1u) / CAMBI_CUDA_BLOCK_X;
    const unsigned grid_y = (h + CAMBI_CUDA_BLOCK_Y - 1u) / CAMBI_CUDA_BLOCK_Y;
    /* Bug fix (Issue lusoris/vmaf#857): pass device pointer addresses, not struct addresses. */
    void *params[] = {&in->data, &out->data, &w, &h, &stride_words, &axis};
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_filter_mode, grid_x, grid_y, 1u, CAMBI_CUDA_BLOCK_X,
                                     CAMBI_CUDA_BLOCK_Y, 1u, 0u, stream, params, NULL));
    return 0;
}

/* ------------------------------------------------------------------ */
/* submit_fex_cuda                                                     */
/*                                                                     */
/* Pipeline:                                                           */
/*   1. Host preprocess → pics[0] (CPU, luma only).                   */
/*   2. HtoD upload pics[0].data[0] → d_image.                        */
/*   3. GPU spatial mask → d_mask.                                     */
/*   4. For each scale:                                                */
/*        a. (scale>0) GPU decimate d_image → d_tmp, swap;            */
/*                     GPU decimate d_mask  → d_tmp, swap.            */
/*        b. GPU filter_mode H: d_image → d_tmp.                      */
/*           GPU filter_mode V: d_tmp   → d_image.                    */
/*        c. DtoH: d_image → rb_image.host_pinned (async).            */
/*           DtoH: d_mask  → rb_mask.host_pinned  (async).            */
/*   5. Record submit + enqueue finished on private stream.            */
/*                                                                     */
/* Note: the DtoH copies at step 4c overwrite the same pinned buffers */
/* each scale — this is safe because collect() processes one scale at */
/* a time after the full GPU pipeline has drained. Alternatively, we  */
/* could use per-scale readback slots; the current approach saves      */
/* memory at the cost of requiring a sync between scales. Since the   */
/* host residual (calculate_c_values) is the bottleneck anyway, this  */
/* is the right tradeoff.                                              */
/* ------------------------------------------------------------------ */
/* Everything submit_fex_cuda has to give back when it bails out. The three
 * orig_d_* pointers are the device buffers as they were on entry: the
 * per-scale pipeline swaps s->d_image / s->d_mask / s->d_tmp around, so an
 * error mid-swap must put the original assignment back before returning. */
typedef struct {
    CambiStateCuda *s;
    CudaFunctions *cu_f;
    VmafPicture *dist_host; /* NULL unless the host staging picture is live */
    VmafCudaBuffer *orig_d_image;
    VmafCudaBuffer *orig_d_mask;
    VmafCudaBuffer *orig_d_tmp;
} CambiSubmitUnwind;

/* cambi_submit_unwind - the single teardown path for submit_fex_cuda.
 *
 * HISS-01: the body of the former `fail_cuda` label, unchanged and in the
 * same order. `fail_cuda` is still a CHECK_CUDA_GOTO target so the label
 * stays and now defers here; `fail_after_pop` keeps its own bare return,
 * because reaching it means the success-path pop failed and the buffer
 * pointers were never swapped away from their originals.
 */
static int cambi_submit_unwind(const CambiSubmitUnwind *u, int ctx_pushed, int err, int cuda_err)
{
    u->s->d_image = u->orig_d_image;
    u->s->d_mask = u->orig_d_mask;
    u->s->d_tmp = u->orig_d_tmp;
    if (u->dist_host)
        (void)vmaf_picture_unref(u->dist_host);
    if (ctx_pushed)
        (void)u->cu_f->cuCtxPopCurrent(NULL);
    return (err != 0) ? err : (cuda_err != 0 ? cuda_err : -EIO);
}

/* cambi_precompute_scale_geometry - per-scale widths and heights.
 *
 * HISS-04: lifted verbatim out of submit_fex_cuda's "pre-compute per-scale
 * geometry" block. */
static void cambi_precompute_scale_geometry(CambiStateCuda *s)
{
    unsigned sw = s->proc_width;
    unsigned sh = s->proc_height;
    for (int scale = 0; scale < CAMBI_CUDA_NUM_SCALES; scale++) {
        s->scale_widths[scale] = sw;
        s->scale_heights[scale] = sh;
        sw = (sw + 1u) >> 1;
        sh = (sh + 1u) >> 1;
    }
}

/* cambi_download_and_preprocess - step 0 and step 1.
 *
 * HISS-04: lifted verbatim out of submit_fex_cuda. Inside a helper the stream
 * sync uses CHECK_CUDA_RETURN instead of CHECK_CUDA_GOTO; the caller routes a
 * non-zero return into cambi_submit_unwind() with the same ctx_pushed, which
 * is what the `fail_cuda` label did, so the same resources are released in
 * the same order and the returned errno is unchanged. `u->dist_host` is
 * cleared at exactly the point the original cleared it, so a later failure
 * still unrefs the staging picture once and only once.
 */
static int cambi_download_and_preprocess(CambiStateCuda *s, CudaFunctions *cu_f,
                                         VmafPicture *dist_pic, VmafPicture *dist_host,
                                         CambiSubmitUnwind *u)
{
    int err = vmaf_cuda_picture_download_async(dist_pic, dist_host, 0x1);
    if (err)
        return err;

    /* Sync the dist_pic private stream: vmaf_cuda_picture_download_async
     * enqueues the DtoH copy on that stream, so we must drain it before
     * the host preprocessing reads dist_host.data[0]. */
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(vmaf_cuda_picture_get_stream(dist_pic)));

    /* Step 1: host preprocessing → pics[0] (10-bit planar, proc_w × proc_h). */
    err = vmaf_cambi_preprocessing(dist_host, &s->pics[0], (int)s->proc_width, (int)s->proc_height,
                                   s->enc_bitdepth);
    (void)vmaf_picture_unref(dist_host);
    u->dist_host = NULL;
    return err;
}

/* cambi_upload_and_mask - step 2 and step 3.
 *
 * HISS-04: lifted verbatim out of submit_fex_cuda.
 */
static int cambi_upload_and_mask(CambiStateCuda *s, CudaFunctions *cu_f, CUstream stream,
                                 VmafPicture *dist_pic)
{
    /* Wait for dist upload to complete before starting GPU work. */
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                              CU_EVENT_WAIT_DEFAULT));

    /* Step 2: HtoD upload pics[0].data[0] → d_image.
     *
     * One strided 2D copy, not one call per row. The host picture's stride and
     * the packed device buffer's differ, which is exactly what the pitch fields
     * are for; issuing a call per row cost `proc_height` driver round trips a
     * frame for a copy the driver does in one. */
    CUDA_MEMCPY2D upload = {
        .srcMemoryType = CU_MEMORYTYPE_HOST,
        .srcHost = s->pics[0].data[0],
        .srcPitch = (size_t)s->pics[0].stride[0],
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = s->d_image->data,
        .dstPitch = s->proc_width * sizeof(uint16_t),
        .WidthInBytes = s->proc_width * sizeof(uint16_t),
        .Height = s->proc_height,
    };
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&upload, stream));

    /* Step 3: spatial mask at full scale. */
    const unsigned mask_index_0 = (unsigned)cambi_cuda_get_mask_index(s->proc_width, s->proc_height,
                                                                      CAMBI_CUDA_MASK_FILTER_SIZE);
    return dispatch_mask(s, cu_f, stream, s->proc_width, s->proc_height, s->proc_width,
                         mask_index_0);
}

/* cambi_decimate_scale - halve d_image and d_mask into d_tmp and swap.
 *
 * HISS-04: lifted verbatim out of submit_fex_cuda's scale loop; the two
 * dispatches and the two pointer swaps keep their original order, and the
 * caller's CambiSubmitUnwind still holds the original allocations.
 */
static int cambi_decimate_scale(CambiStateCuda *s, CudaFunctions *cu_f, CUstream stream,
                                unsigned *scaled_w, unsigned *scaled_h)
{
    /* GPU decimate d_image → d_tmp (out = half resolution). */
    const unsigned new_w = (*scaled_w + 1u) >> 1;
    const unsigned new_h = (*scaled_h + 1u) >> 1;
    int err =
        dispatch_decimate(s, cu_f, stream, s->d_image, s->d_tmp, new_w, new_h, *scaled_w, new_w);
    if (err)
        return err;
    /* Swap d_image ↔ d_tmp. */
    VmafCudaBuffer *tmp = s->d_image;
    s->d_image = s->d_tmp;
    s->d_tmp = tmp;

    /* GPU decimate d_mask → d_tmp. */
    err = dispatch_decimate(s, cu_f, stream, s->d_mask, s->d_tmp, new_w, new_h, *scaled_w, new_w);
    if (err)
        return err;
    tmp = s->d_mask;
    s->d_mask = s->d_tmp;
    s->d_tmp = tmp;

    *scaled_w = new_w;
    *scaled_h = new_h;
    return 0;
}

/* cambi_filter_and_readback - the two filter_mode passes plus the DtoH pair.
 *
 * HISS-04: lifted verbatim out of submit_fex_cuda's scale loop.
 */
static int cambi_filter_and_readback(CambiStateCuda *s, CudaFunctions *cu_f, CUstream stream,
                                     unsigned scaled_w, unsigned scaled_h)
{
    /* GPU filter_mode H: d_image → d_tmp. */
    int err = dispatch_filter_mode(s, cu_f, stream, s->d_image, s->d_tmp, scaled_w, scaled_h,
                                   scaled_w, 0);
    if (err)
        return err;
    /* GPU filter_mode V: d_tmp → d_image. */
    err = dispatch_filter_mode(s, cu_f, stream, s->d_tmp, s->d_image, scaled_w, scaled_h, scaled_w,
                               1);
    if (err)
        return err;

    /* DtoH: d_image → pics[0].data[0], d_mask → pics[1].data[0].
     *
     * Two strided 2D copies enqueued on the stream, then one stall that
     * waits for both. This used to stall first and then issue two BLOCKING
     * copies per row: at 1080p that is 2,160 driver round trips for scale 0
     * alone, about 4,200 a frame across the five scales, and it dominated
     * the whole CUDA pipeline — 0.60 s of a 1.03 s run over 48 frames,
     * more than every other extractor combined. */
    const size_t scaled_row_bytes = scaled_w * sizeof(uint16_t);
    CUDA_MEMCPY2D readback = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = s->d_image->data,
        .srcPitch = scaled_row_bytes,
        .dstMemoryType = CU_MEMORYTYPE_HOST,
        .dstHost = s->pics[0].data[0],
        .dstPitch = (size_t)s->pics[0].stride[0],
        .WidthInBytes = scaled_row_bytes,
        .Height = scaled_h,
    };
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&readback, stream));
    readback.srcDevice = s->d_mask->data;
    readback.dstHost = s->pics[1].data[0];
    readback.dstPitch = (size_t)s->pics[1].stride[0];
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&readback, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(stream));
    return 0;
}

/* cambi_submit_scale - one scale of the synchronous GPU + CPU pipeline.
 *
 * HISS-04: lifted verbatim out of submit_fex_cuda's scale loop. The design
 * notes that used to sit inline above the loop follow, unchanged.
 */
/* We need to store per-scale readback data for the host residual in
 * collect(). Since we overwrite the same pinned buffers, we process
 * all scales in submit() on the GPU and record a finished event after
 * each scale's DtoH. collect() then runs the CPU residual per-scale.
 *
 * Practical approach: record a submit event after all GPU work, then
 * DtoH-copy each scale's result into per-scale pinned regions.
 * For v1, we simplify by doing a synchronous host-side approach:
 * all GPU work for all scales streams asynchronously, but the DtoH
 * copies and CPU residual run in collect() after a single sync.
 *
 * To avoid needing per-scale device buffers, we execute the GPU
 * pipeline for all 5 scales in submit() (streaming), DtoH each
 * scale into its dedicated section of the readback buffer, then
 * collect() reads from those sections.
 *
 * Buffer layout: rb_image.host_pinned and rb_mask.host_pinned are
 * each (proc_width × proc_height) — enough for scale 0. Each scale
 * occupies a prefix of size scaled_w × scaled_h within the stride-
 * proc_width layout. We copy each scale's data from device to the
 * top-left corner of the readback buffer sequentially (each DtoH
 * overwrites the previous scale), which is fine because collect()
 * first awaits the GPU sync, then runs the CPU residual for each
 * scale in order — so by the time scale k is processed on the host,
 * the DtoH for scale k+1 has not yet started on the device.
 *
 * The actual approach: record all scale GPU kernels in one go, then
 * for each scale do a separate DtoH + CPU residual in collect(). We
 * cannot mix GPU pipeline and DtoH in a single submit() without
 * blocking between scales, so we use a two-pass design:
 *   submit(): GPU pipeline for scale 0 only (mask + filter_mode).
 *             Record per-scale results for scales 1..4 by running
 *             all 5 scales on the GPU + recording events.
 *   collect(): For each scale, wait for the corresponding event,
 *              DtoH the readback, run CPU residual.
 *
 * For v1 simplicity, we run the GPU pipeline for all scales here
 * in submit() and enqueue DtoH copies for all scales into separate
 * regions of the readback buffer (laid out as a 5-element array).
 * collect() reads each region without a per-scale sync.
 *
 * Readback buffer size must accommodate 5 scales. Scale k has area:
 *   proc_width × proc_height / 4^k (approximately). Total ≤ 2×.
 * For clarity, we allocate one buffer per scale:
 *   scale 0: proc_w × proc_h
 *   scale 1: ceil(proc_w/2) × ceil(proc_h/2)
 *   ...
 * These are sub-regions of the pre-allocated rb_image/rb_mask (sized
 * for scale 0). We reuse the same device buffers (d_image, d_mask)
 * across scales, packing the DtoH copies into the pinned buffer
 * using stride-proc_width layout (row-major, same stride for all
 * scales — upper-left corner read back).
 *
 * After careful analysis, the cleanest approach that preserves the
 * async model and avoids per-scale host sync is:
 *   - Run GPU pipeline for all 5 scales: decimate/filter_mode each.
 *   - After each scale's filter_mode pass, record a CUevent and
 *     enqueue DtoH into a per-scale region of the pinned buffer.
 *   - collect() streams all 5 scales' host residuals after one drain.
 *
 * Implementation below uses per-scale pitched DtoH into the single
 * large pinned allocation (rb_image.host_pinned stores all 5 scales
 * sequentially, separated by scaled_w × scaled_h uint16 elements).
 */

/* Total needed: offset bytes in each readback buffer. */
/* rb_image and rb_mask were allocated for proc_w × proc_h — sufficient
 * only for scale 0. We need to ensure they are large enough.
 * Solution: rb_image / rb_mask allocated in init() for proc_w × proc_h
 * (scale 0 area). Scales 1..4 each have a smaller area, so their
 * cumulative total is < scale_0_area * 2. We therefore need 2×.
 *
 * Since this violates the allocation size, fall back to the
 * per-scale synchronous approach: after each scale's GPU filter_mode
 * pass, do a synchronous DtoH into the scale-0-sized buffer top-left,
 * run the CPU residual, then proceed to the next scale.
 *
 * This means submit() cannot be fully asynchronous for CAMBI v1 —
 * the per-scale CPU residual is integrated into submit(). collect()
 * only emits the already-computed score. This mirrors how the Vulkan
 * path works (extract() is fully synchronous).
 *
 * To fit the async submit/collect model:
 *   submit() runs all GPU + CPU work synchronously (no frame pipelining).
 *   collect() just emits the pre-computed score.
 *
 * A future optimisation (v2) could decouple per-frame GPU work
 * (all 5 scales) from the CPU residual by serialising the CPU
 * work in collect(). This requires per-scale pinned readback buffers.
 */

/* ----  Synchronous per-scale loop  ---- */
/* We run all GPU work + DtoH + CPU residual per scale in submit().
 * The CUstream is the picture stream (ref_pic's stream); we wait for
 * each scale's GPU pass to finish before doing the CPU residual.
 * This deviates from the pure-async model but is correct and matches
 * the Vulkan precedent (cambi_vk_extract is synchronous w.r.t. each
 * scale). The drain-batch optimisation (ADR-0242) does not apply here
 * because there is no DtoH to pipeline. */

static int cambi_submit_scale(CambiStateCuda *s, CudaFunctions *cu_f, CUstream stream, int scale,
                              unsigned *scaled_w, unsigned *scaled_h, int num_diffs, double topk,
                              double *score_out)
{
    if (scale > 0 || s->cambi_high_res_speedup) {
        const int dec_err = cambi_decimate_scale(s, cu_f, stream, scaled_w, scaled_h);
        if (dec_err)
            return dec_err;
    }

    const int err = cambi_filter_and_readback(s, cu_f, stream, *scaled_w, *scaled_h);
    if (err)
        return err;

    /* CPU residual: calculate_c_values + spatial pooling. */
    vmaf_cambi_calculate_c_values(&s->pics[0], &s->pics[1], s->buffers.c_values,
                                  s->buffers.c_values_histograms, s->adjusted_window,
                                  (uint16_t)num_diffs, s->buffers.tvi_for_diff, s->vlt_luma,
                                  s->buffers.diff_weights, s->buffers.all_diffs, (int)*scaled_w,
                                  (int)*scaled_h, s->inc_range_callback, s->dec_range_callback);

    *score_out = vmaf_cambi_spatial_pooling(s->buffers.c_values, topk, *scaled_w, *scaled_h);
    return 0;
}

/* cambi_finalize_score - weight the per-scale scores and park the result.
 *
 * HISS-04: lifted verbatim out of submit_fex_cuda; the clamps and the
 * pinned-buffer store are unchanged.
 */
static void cambi_finalize_score(CambiStateCuda *s, double *scores_per_scale)
{
    /* Compute the final score. */
    const uint16_t pixels_in_window = vmaf_cambi_get_pixels_in_window(s->adjusted_window);
    double score = vmaf_cambi_weight_scores_per_scale(scores_per_scale, pixels_in_window);
    if (score > s->cambi_max_val)
        score = s->cambi_max_val;
    if (score < 0.0)
        score = 0.0;

    /* Store in rb_image.host_pinned (single double — misuse of the
     * readback slot, but safe: host_pinned is large enough and the
     * collect() is the only reader). */
    *(double *)s->rb_image.host_pinned = score;
}

/* cambi_submit_pipeline - everything between the context push and the pop.
 *
 * HISS-04: lifted out of submit_fex_cuda. Every failure returns the errno
 * the caller then hands to cambi_submit_unwind() with the same ctx_pushed,
 * which is exactly what the `fail_cuda` label did.
 */
static int cambi_submit_pipeline(VmafFeatureExtractor *fex, CambiStateCuda *s, CudaFunctions *cu_f,
                                 VmafPicture *ref_pic, VmafPicture *dist_pic,
                                 VmafPicture *dist_host, CambiSubmitUnwind *u)
{
    int err = cambi_download_and_preprocess(s, cu_f, dist_pic, dist_host, u);
    if (err)
        return err;

    CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);
    err = cambi_upload_and_mask(s, cu_f, stream, dist_pic);
    if (err)
        return err;

    /* Step 4: per-scale GPU pipeline. */
    cambi_precompute_scale_geometry(s);

    unsigned scaled_w = s->proc_width;
    unsigned scaled_h = s->proc_height;
    const int num_diffs = 1 << s->max_log_contrast;
    double scores_per_scale[CAMBI_CUDA_NUM_SCALES] = {0.0, 0.0, 0.0, 0.0, 0.0};

    /* The topk ratio: prefer topk if non-default, else cambi_topk. */
    const double topk = (s->topk != CAMBI_CUDA_DEFAULT_TOPK) ? s->topk : s->cambi_topk;

    for (int scale = 0; scale < CAMBI_CUDA_NUM_SCALES; scale++) {
        err = cambi_submit_scale(s, cu_f, stream, scale, &scaled_w, &scaled_h, num_diffs, topk,
                                 &scores_per_scale[scale]);
        if (err)
            return err;
    }

    /* Restore d_image / d_mask / d_tmp to point at the original allocations. */
    s->d_image = u->orig_d_image;
    s->d_mask = u->orig_d_mask;
    s->d_tmp = u->orig_d_tmp;

    cambi_finalize_score(s, scores_per_scale);

    /* Emit a dummy event on the private stream so collect() can drain
     * without blocking (the score is already computed). */
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    CambiStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    s->index = index;

    int _cuda_err = 0;
    int ctx_pushed = 0;
    int err = 0;
    CambiSubmitUnwind u = {
        .s = s,
        .cu_f = cu_f,
        .dist_host = NULL,
        .orig_d_image = s->d_image,
        .orig_d_mask = s->d_mask,
        .orig_d_tmp = s->d_tmp,
    };

    /* Step 0: download dist_pic GPU→host so vmaf_cambi_preprocessing (host
     * code) can read it.  Pictures delivered to a CUDA extractor's submit()
     * have device pointers in data[]; dereferencing them on the host causes
     * a segfault (Issue lusoris/vmaf#857).  Other CUDA extractors avoid this because
     * they keep all preprocessing on the GPU; CAMBI is unique in needing a
     * host-side decimate-and-10b-upcast before its GPU pipeline. */
    VmafPicture dist_host;
    err = vmaf_picture_alloc(&dist_host, dist_pic->pix_fmt, dist_pic->bpc, dist_pic->w[0],
                             dist_pic->h[0]);
    if (err)
        return err;
    u.dist_host = &dist_host;

    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail_cuda);
    ctx_pushed = 1;

    err = cambi_submit_pipeline(fex, s, cu_f, ref_pic, dist_pic, &dist_host, &u);
    if (err)
        return cambi_submit_unwind(&u, ctx_pushed, err, _cuda_err);

    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    ctx_pushed = 0;
    return 0;

fail_cuda:
    return cambi_submit_unwind(&u, ctx_pushed, err, _cuda_err);
fail_after_pop:
    return (err != 0) ? err : (_cuda_err != 0 ? _cuda_err : -EIO);
}

/* ------------------------------------------------------------------ */
/* collect_fex_cuda — emit the pre-computed score. */
/* ------------------------------------------------------------------ */
static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    CambiStateCuda *s = fex->priv;

    /* Drain the private stream (no-op if drain_batch already handled it). */
    int err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (err)
        return err;

    const double score = *(double *)s->rb_image.host_pinned;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Cambi_feature_cambi_score", score, index);
}

/* ------------------------------------------------------------------ */
/* close_fex_cuda */
/* ------------------------------------------------------------------ */
/* cambi_unload_module - push the fex context, unload the PTX module, pop.
 *
 * HISS-01: lifted out of close_fex_cuda so the former `goto unload_done`
 * (which only skipped the failure block) becomes an ordinary return. The
 * CHECK_CUDA_GOTO error labels are unchanged, so the pop still runs exactly
 * when the push succeeded and the same _cuda_err reaches the caller.
 */
static int cambi_unload_module(VmafFeatureExtractor *fex, CambiStateCuda *s,
                               const CudaFunctions *cu_f)
{
    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail_unload);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuModuleUnload(s->module), fail_unload);
    s->module = NULL;
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop_unload);
    return 0;

fail_unload:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop_unload:
    return _cuda_err;
}

/* cambi_close_device_buffers - release the device buffers and the two
 * readback staging areas.
 *
 * HISS-04: lifted verbatim out of close_fex_cuda. The same resources are
 * released in the same order and the first non-zero status still wins.
 */
static int cambi_close_device_buffers(VmafFeatureExtractor *fex, CambiStateCuda *s, int rc)
{
    if (s->d_image) {
        const int e = vmaf_cuda_buffer_free(fex->cu_state, s->d_image);
        if (e && rc == 0)
            rc = e;
        free(s->d_image);
        s->d_image = NULL;
    }
    if (s->d_mask) {
        const int e = vmaf_cuda_buffer_free(fex->cu_state, s->d_mask);
        if (e && rc == 0)
            rc = e;
        free(s->d_mask);
        s->d_mask = NULL;
    }
    if (s->d_tmp) {
        const int e = vmaf_cuda_buffer_free(fex->cu_state, s->d_tmp);
        if (e && rc == 0)
            rc = e;
        free(s->d_tmp);
        s->d_tmp = NULL;
    }

    {
        const int e = vmaf_cuda_kernel_readback_free(&s->rb_image, fex->cu_state);
        if (e && rc == 0)
            rc = e;
    }
    {
        const int e = vmaf_cuda_kernel_readback_free(&s->rb_mask, fex->cu_state);
        if (e && rc == 0)
            rc = e;
    }
    return rc;
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    CambiStateCuda *s = fex->priv;
    int rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);

    rc = cambi_close_device_buffers(fex, s, rc);

    (void)vmaf_picture_unref(&s->pics[0]);
    (void)vmaf_picture_unref(&s->pics[1]);

    free(s->buffers.c_values);
    free(s->buffers.c_values_histograms);
    free(s->buffers.mask_dp);
    free(s->buffers.filter_mode_buffer);
    free(s->buffers.derivative_buffer);
    free(s->buffers.diffs_to_consider);
    free(s->buffers.diff_weights);
    free(s->buffers.all_diffs);
    free(s->buffers.tvi_for_diff);

    if (s->feature_name_dict) {
        const int e = vmaf_dictionary_free(&s->feature_name_dict);
        if (e && rc == 0)
            rc = e;
    }
    const CudaFunctions *cu_f = fex->cu_state ? fex->cu_state->f : NULL;
    if (cu_f && fex->cu_state && fex->cu_state->ctx && s->module) {
        const int unload_err = cambi_unload_module(fex, s, cu_f);
        if (unload_err && rc == 0)
            rc = unload_err;
    }
    return rc;
}

static const char *provided_features[] = {"Cambi_feature_cambi_score", NULL};

VmafFeatureExtractor vmaf_fex_cambi_cuda = {
    .name = "cambi_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(CambiStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
    /* CAMBI has a non-trivial CPU residual (calculate_c_values) in submit().
     * is_reduction_only = false (meaningful pixel processing on both GPU +
     * CPU), dispatch_hint = VMAF_FEATURE_DISPATCH_DIRECT — run one frame
     * at a time without batching; the per-frame CPU residual serialises
     * frames already. DIRECT matches the per-scale synchronous posture
     * previously established in the (now-removed) Vulkan twin. */
    .chars =
        {
            .n_dispatches_per_frame = 15, /* 5 scales × 3 kernels (mask + filter_H + filter_V) */
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_DIRECT,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
