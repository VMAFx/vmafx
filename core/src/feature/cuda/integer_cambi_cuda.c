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

#include "vmaf_nullptr.h"

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

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
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
#define CAMBI_CUDA_MAX_DIFFS 32
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
#define CAMBI_DOUBLE_OPTION(NAME, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM, ALIAS)                   \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(CambiStateCuda, FIELD),                                                 \
        .type = VMAF_OPT_TYPE_DOUBLE,                                                              \
        .default_val.d = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }
#define CAMBI_INT_OPTION(NAME, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM, ALIAS)                      \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(CambiStateCuda, FIELD),                                                 \
        .type = VMAF_OPT_TYPE_INT,                                                                 \
        .default_val.i = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }
#define CAMBI_STRING_OPTION(NAME, HELP, FIELD, DEFAULT, ALIAS)                                     \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(CambiStateCuda, FIELD),                                                 \
        .type = VMAF_OPT_TYPE_STRING,                                                              \
        .default_val.s = (DEFAULT),                                                                \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }

static const VmafOption options[] = {
    CAMBI_DOUBLE_OPTION("cambi_max_val", "maximum value allowed; larger values will be clipped",
                        cambi_max_val, CAMBI_CUDA_DEFAULT_MAX_VAL, 0.0, 1000.0, "cmxv"),
    CAMBI_INT_OPTION("enc_width", "Encoding width", enc_width, 0, 180, 7680, "encw"),
    CAMBI_INT_OPTION("enc_height", "Encoding height", enc_height, 0, 150, 7680, "ench"),
    CAMBI_INT_OPTION("enc_bitdepth", "Encoding bitdepth", enc_bitdepth, 0, 6, 16, "encbd"),
    CAMBI_INT_OPTION("window_size",
                     "Window size to compute CAMBI: 65 corresponds to ~1 degree at 4k", window_size,
                     CAMBI_CUDA_DEFAULT_WINDOW_SIZE, 15, 127, "ws"),
    CAMBI_DOUBLE_OPTION("topk", "Ratio of pixels for the spatial pooling computation", topk,
                        CAMBI_CUDA_DEFAULT_TOPK, 0.0001, 1.0, VMAF_NULLPTR),
    CAMBI_DOUBLE_OPTION("cambi_topk", "Ratio of pixels for the spatial pooling computation",
                        cambi_topk, CAMBI_CUDA_DEFAULT_TOPK, 0.0001, 1.0, "ctpk"),
    CAMBI_DOUBLE_OPTION("tvi_threshold", "Visibility threshold ΔL < tvi_threshold * L_mean",
                        tvi_threshold, CAMBI_CUDA_DEFAULT_TVI, 0.0001, 1.0, "tvit"),
    CAMBI_DOUBLE_OPTION("cambi_vis_lum_threshold",
                        "Luminance value below which banding is assumed invisible",
                        cambi_vis_lum_threshold, CAMBI_CUDA_DEFAULT_VLT, 0.0, 300.0, "vlt"),
    CAMBI_INT_OPTION("max_log_contrast", "Maximum log contrast (0 to 5, default 2)",
                     max_log_contrast, CAMBI_CUDA_DEFAULT_MAX_LOG_CONTRAST, 0, 5, "mlc"),
    CAMBI_STRING_OPTION("eotf", "EOTF for visibility-threshold conversion (bt1886 / pq)", eotf,
                        CAMBI_CUDA_DEFAULT_EOTF, VMAF_NULLPTR),
    CAMBI_STRING_OPTION("cambi_eotf", "EOTF override for cambi (defaults to eotf)", cambi_eotf,
                        CAMBI_CUDA_DEFAULT_EOTF, "ceot"),
    CAMBI_INT_OPTION(
        "cambi_high_res_speedup",
        "Speed up the processing by downsampling post spatial mask for resolutions >= 1080p. Min speed-up resolution possible values: [1080, 1440, 2160, 0]. Default: 0 (not applied)",
        cambi_high_res_speedup, 0, 0, 2160, "hrs"),
    {0},
};

#undef CAMBI_STRING_OPTION
#undef CAMBI_INT_OPTION
#undef CAMBI_DOUBLE_OPTION

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

static int cambi_cuda_init_tvi(CambiStateCuda *s, int num_diffs)
{
    if (!s || num_diffs <= 0 || num_diffs > CAMBI_CUDA_MAX_DIFFS || !s->buffers.tvi_for_diff ||
        !s->buffers.diffs_to_consider)
        return -EINVAL;
    VmafLumaRange luma_range;
    int err = vmaf_luminance_init_luma_range(&luma_range, 10, VMAF_PIXEL_RANGE_LIMITED);
    if (err)
        return err;

    const char *effective_eotf;
    if (s->cambi_eotf && strcmp(s->cambi_eotf, CAMBI_CUDA_DEFAULT_EOTF) != 0) {
        effective_eotf = s->cambi_eotf;
    } else {
        effective_eotf = (s->eotf != VMAF_NULLPTR) ? s->eotf : CAMBI_CUDA_DEFAULT_EOTF;
    }

    VmafEOTF eotf;
    err = vmaf_luminance_init_eotf(&eotf, effective_eotf);
    if (err)
        return err;

    for (int d = 0; d < num_diffs; d++) {
        s->buffers.tvi_for_diff[d] = (uint16_t)cambi_cuda_get_tvi_for_diff(
            s->buffers.diffs_to_consider[d], s->tvi_threshold, 10, luma_range, eotf);
        s->buffers.tvi_for_diff[d] += (uint16_t)num_diffs;
    }

    s->vlt_luma = (uint16_t)cambi_cuda_get_vlt_luma(s->cambi_vis_lum_threshold, luma_range, eotf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* init_fex_cuda */
/* ------------------------------------------------------------------ */
static void cambi_cuda_keep_first_error(int *ret, int error)
{
    if (!*ret && error)
        *ret = error;
}

static int cambi_cuda_free_device_buffers(VmafFeatureExtractor *fex, CambiStateCuda *s)
{
    VmafCudaBuffer **buffers[] = {&s->d_image, &s->d_mask, &s->d_tmp};
    int ret = 0;
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++) {
        if (!*buffers[i])
            continue;
        cambi_cuda_keep_first_error(&ret, vmaf_cuda_buffer_free(fex->cu_state, *buffers[i]));
        free(*buffers[i]);
        *buffers[i] = VMAF_NULLPTR;
    }
    return ret;
}

static int cambi_cuda_free_readbacks(VmafFeatureExtractor *fex, CambiStateCuda *s)
{
    int ret = vmaf_cuda_kernel_readback_free(&s->rb_image, fex->cu_state);
    cambi_cuda_keep_first_error(&ret, vmaf_cuda_kernel_readback_free(&s->rb_mask, fex->cu_state));
    return ret;
}

static void cambi_cuda_free_pictures(CambiStateCuda *s)
{
    (void)vmaf_picture_unref(&s->pics[0]);
    (void)vmaf_picture_unref(&s->pics[1]);
}

static void cambi_cuda_free_host_buffers(CambiStateCuda *s)
{
    free(s->buffers.c_values);
    free(s->buffers.c_values_histograms);
    free(s->buffers.mask_dp);
    free(s->buffers.filter_mode_buffer);
    free(s->buffers.derivative_buffer);
    free(s->buffers.diffs_to_consider);
    free(s->buffers.diff_weights);
    free(s->buffers.all_diffs);
    free(s->buffers.tvi_for_diff);
    s->buffers = (VmafCambiHostBuffers){0};
}

static int cambi_cuda_free_dictionary(CambiStateCuda *s)
{
    return s->feature_name_dict ? vmaf_dictionary_free(&s->feature_name_dict) : 0;
}

static int cambi_cuda_geometry_init(CambiStateCuda *s, unsigned bpc, unsigned width,
                                    unsigned height)
{
    if (s->enc_bitdepth == 0)
        s->enc_bitdepth = (int)bpc;
    if (s->enc_width == 0 || s->enc_height == 0) {
        s->enc_width = (int)width;
        s->enc_height = (int)height;
    }
    if ((unsigned)s->enc_height > height || (unsigned)s->enc_width > width) {
        s->enc_width = (int)width;
        s->enc_height = (int)height;
    }
    if (!cambi_validate_dimensions(s->enc_width, s->enc_height)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "cambi_cuda: encoded resolution %dx%d below minimum %d×%d.\n", s->enc_width,
                 s->enc_height, CAMBI_MIN_WIDTH_HEIGHT, CAMBI_MIN_WIDTH_HEIGHT);
        return -EINVAL;
    }
    const int pixels = s->enc_width * s->enc_height;
    const bool enabled =
        (s->cambi_high_res_speedup == 1080 && pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1080p) ||
        (s->cambi_high_res_speedup == 1440 && pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_1440p) ||
        (s->cambi_high_res_speedup == 2160 && pixels >= CAMBI_HIGH_RES_SPEEDUP_THRESHOLD_2160p);
    if (!enabled)
        s->cambi_high_res_speedup = 0;
    s->src_width = width;
    s->src_height = height;
    s->src_bpc = bpc;
    s->proc_width = (unsigned)s->enc_width;
    s->proc_height = (unsigned)s->enc_height;
    s->adjusted_window = cambi_cuda_adjust_window(s->window_size, s->proc_width, s->proc_height,
                                                  (bool)s->cambi_high_res_speedup);
    return 0;
}

static int cambi_cuda_load_module(CambiStateCuda *s, CudaFunctions *cu_f)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module, cambi_score_ptx));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&s->func_mask, s->module, "cambi_spatial_mask_kernel"));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&s->func_decimate, s->module, "cambi_decimate_kernel"));
    CHECK_CUDA_RETURN(
        cu_f, cuModuleGetFunction(&s->func_filter_mode, s->module, "cambi_filter_mode_kernel"));
    return 0;
}

static int cambi_cuda_runtime_init(VmafFeatureExtractor *fex, CambiStateCuda *s)
{
    int ret = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (ret)
        return ret;
    CudaFunctions *cu_f = fex->cu_state->f;
    const CUresult push = cu_f->cuCtxPushCurrent(fex->cu_state->ctx);
    if (push != CUDA_SUCCESS)
        return vmaf_cuda_result_to_errno((int)push);
    ret = cambi_cuda_load_module(s, cu_f);
    if (ret && s->module) {
        (void)cu_f->cuModuleUnload(s->module);
        s->module = VMAF_NULLPTR;
    }
    const CUresult pop = cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
    if (!ret && pop != CUDA_SUCCESS)
        ret = vmaf_cuda_result_to_errno((int)pop);
    return ret;
}

static int cambi_cuda_module_unload(VmafFeatureExtractor *fex, CambiStateCuda *s)
{
    if (!s->module || !fex->cu_state || !fex->cu_state->f || !fex->cu_state->ctx)
        return 0;
    CudaFunctions *cu_f = fex->cu_state->f;
    const CUresult push = cu_f->cuCtxPushCurrent(fex->cu_state->ctx);
    if (push != CUDA_SUCCESS)
        return vmaf_cuda_result_to_errno((int)push);
    const CUresult unload = cu_f->cuModuleUnload(s->module);
    if (unload == CUDA_SUCCESS)
        s->module = VMAF_NULLPTR;
    const CUresult pop = cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
    if (unload != CUDA_SUCCESS)
        return vmaf_cuda_result_to_errno((int)unload);
    return pop == CUDA_SUCCESS ? 0 : vmaf_cuda_result_to_errno((int)pop);
}

static int cambi_cuda_device_buffers_alloc(VmafFeatureExtractor *fex, CambiStateCuda *s)
{
    const size_t bytes = (size_t)s->proc_width * s->proc_height * sizeof(uint16_t);
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_image, bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_mask, bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->d_tmp, bytes);
    if (!ret)
        ret = vmaf_cuda_kernel_readback_alloc(&s->rb_image, fex->cu_state, bytes);
    if (!ret)
        ret = vmaf_cuda_kernel_readback_alloc(&s->rb_mask, fex->cu_state, bytes);
    return ret;
}

static int cambi_cuda_pictures_alloc(CambiStateCuda *s)
{
    int ret =
        vmaf_picture_alloc(&s->pics[0], VMAF_PIX_FMT_YUV400P, 10, s->proc_width, s->proc_height);
    if (!ret) {
        ret = vmaf_picture_alloc(&s->pics[1], VMAF_PIX_FMT_YUV400P, 10, s->proc_width,
                                 s->proc_height);
    }
    return ret;
}

static int cambi_cuda_diff_buffers_alloc(CambiStateCuda *s, int num_diffs)
{
    if (!s || num_diffs <= 0 || num_diffs > CAMBI_CUDA_MAX_DIFFS)
        return -EINVAL;
    s->buffers.diffs_to_consider = malloc(sizeof(uint16_t) * (size_t)num_diffs);
    s->buffers.diff_weights = malloc(sizeof(int) * (size_t)num_diffs);
    s->buffers.all_diffs = malloc(sizeof(int) * (size_t)(2 * num_diffs + 1));
    s->buffers.tvi_for_diff = malloc(sizeof(uint16_t) * (size_t)num_diffs);
    if (!s->buffers.diffs_to_consider || !s->buffers.diff_weights || !s->buffers.all_diffs ||
        !s->buffers.tvi_for_diff)
        return -ENOMEM;
    static const int contrast_weights[32] = {1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8,
                                             8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    for (int diff = 0; diff < num_diffs; diff++) {
        s->buffers.diffs_to_consider[diff] = (uint16_t)(diff + 1);
        s->buffers.diff_weights[diff] = contrast_weights[diff];
    }
    for (int diff = -num_diffs; diff <= num_diffs; diff++)
        s->buffers.all_diffs[diff + num_diffs] = diff;
    return cambi_cuda_init_tvi(s, num_diffs);
}

static int cambi_cuda_work_buffers_alloc(CambiStateCuda *s, int num_diffs)
{
    if (!s || num_diffs <= 0 || num_diffs > CAMBI_CUDA_MAX_DIFFS || !s->buffers.tvi_for_diff ||
        !s->buffers.all_diffs)
        return -EINVAL;
    const int v_lo = (int)s->vlt_luma - 3 * num_diffs + 1;
    const uint16_t v_band_base = v_lo > 0 ? (uint16_t)v_lo : 0;
    const int v_band_size = (int)s->buffers.tvi_for_diff[num_diffs - 1] + 1 - (int)v_band_base;
    if (v_band_size <= 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "cambi_cuda: v_band_size underflow (tvi_max=%u v_band_base=%u); "
                 "cambi_vis_lum_threshold may be too low\n",
                 (unsigned)s->buffers.tvi_for_diff[num_diffs - 1], (unsigned)v_band_base);
        return -ENOMEM;
    }
    const size_t final_diff = (size_t)num_diffs * 2U;
    const uint16_t num_bins =
        (uint16_t)(1024u + (unsigned)(s->buffers.all_diffs[final_diff] - s->buffers.all_diffs[0]));
    const size_t hist_bins =
        (size_t)v_band_size > (size_t)num_bins ? (size_t)v_band_size : (size_t)num_bins;
    const int padding = CAMBI_CUDA_MASK_FILTER_SIZE / 2;
    const int dp_width = (int)s->proc_width + 2 * padding + 1;
    const int dp_height = 2 * padding + 2;
    s->buffers.c_values = malloc(sizeof(float) * s->proc_width * s->proc_height);
    s->buffers.c_values_histograms = malloc(sizeof(uint16_t) * s->proc_width * hist_bins);
    s->buffers.mask_dp = malloc(sizeof(uint32_t) * (size_t)dp_width * (size_t)dp_height);
    s->buffers.filter_mode_buffer = malloc(sizeof(uint16_t) * 3u * s->proc_width);
    s->buffers.derivative_buffer = malloc(sizeof(uint16_t) * s->proc_width);
    if (!s->buffers.c_values || !s->buffers.c_values_histograms || !s->buffers.mask_dp ||
        !s->buffers.filter_mode_buffer || !s->buffers.derivative_buffer)
        return -ENOMEM;
    return 0;
}

static void cambi_cuda_abort_initialized(VmafFeatureExtractor *fex, CambiStateCuda *s)
{
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    (void)cambi_cuda_free_device_buffers(fex, s);
    (void)cambi_cuda_free_readbacks(fex, s);
    cambi_cuda_free_pictures(s);
    cambi_cuda_free_host_buffers(s);
    (void)cambi_cuda_module_unload(fex, s);
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    CambiStateCuda *s = fex->priv;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;
    const int num_diffs = 1 << s->max_log_contrast;
    int ret = cambi_cuda_geometry_init(s, bpc, w, h);
    if (ret) {
        (void)cambi_cuda_free_dictionary(s);
        return ret;
    }
    ret = cambi_cuda_runtime_init(fex, s);
    if (!ret)
        ret = cambi_cuda_device_buffers_alloc(fex, s);
    if (!ret)
        ret = cambi_cuda_pictures_alloc(s);
    if (!ret)
        ret = cambi_cuda_diff_buffers_alloc(s, num_diffs);
    if (!ret)
        ret = cambi_cuda_work_buffers_alloc(s, num_diffs);
    if (ret) {
        cambi_cuda_abort_initialized(fex, s);
        (void)cambi_cuda_free_dictionary(s);
        return ret;
    }
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
    /* Bug fix (Issue #857): cuLaunchKernel params[i] must point to the VALUE
     * to pass, not to the VmafCudaBuffer struct. Pass &buf->data (address of the
     * CUdeviceptr field) so the driver reads the device pointer, not buf->size. */
    void *params[] = {&s->d_image->data, &s->d_mask->data, &w, &h, &stride_words, &mask_index};
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_mask, grid_x, grid_y, 1u, CAMBI_CUDA_BLOCK_X,
                                     CAMBI_CUDA_BLOCK_Y, 1u, 0u, stream, params, VMAF_NULLPTR));
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
    /* Bug fix (Issue #857): pass device pointer addresses, not struct addresses. */
    void *params[] = {&src->data, &dst->data, &out_w, &out_h, &src_stride_words, &dst_stride_words};
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_decimate, grid_x, grid_y, 1u, CAMBI_CUDA_BLOCK_X,
                                     CAMBI_CUDA_BLOCK_Y, 1u, 0u, stream, params, VMAF_NULLPTR));
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
    /* Bug fix (Issue #857): pass device pointer addresses, not struct addresses. */
    void *params[] = {&in->data, &out->data, &w, &h, &stride_words, &axis};
    CHECK_CUDA_RETURN(cu_f,
                      cuLaunchKernel(s->func_filter_mode, grid_x, grid_y, 1u, CAMBI_CUDA_BLOCK_X,
                                     CAMBI_CUDA_BLOCK_Y, 1u, 0u, stream, params, VMAF_NULLPTR));
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
typedef struct CambiCudaScaleState {
    unsigned width;
    unsigned height;
} CambiCudaScaleState;

static int cambi_cuda_upload_initial(CambiStateCuda *s, CudaFunctions *cu_f,
                                     const VmafPicture *dist, CUstream stream)
{
    VmafPicture host;
    int ret = vmaf_picture_alloc(&host, dist->pix_fmt, dist->bpc, dist->w[0], dist->h[0]);
    if (ret)
        return ret;
    ret = vmaf_cuda_picture_download_async(dist, &host, 0x1);
    if (!ret) {
        const CUresult sync = cu_f->cuStreamSynchronize(vmaf_cuda_picture_get_stream(dist));
        if (sync != CUDA_SUCCESS)
            ret = vmaf_cuda_result_to_errno((int)sync);
    }
    if (!ret) {
        ret = vmaf_cambi_preprocessing(&host, &s->pics[0], (int)s->proc_width, (int)s->proc_height,
                                       s->enc_bitdepth);
    }
    (void)vmaf_picture_unref(&host);
    if (ret)
        return ret;
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(stream, vmaf_cuda_picture_get_ready_event(dist),
                                              CU_EVENT_WAIT_DEFAULT));
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
    const unsigned mask_index = (unsigned)cambi_cuda_get_mask_index(s->proc_width, s->proc_height,
                                                                    CAMBI_CUDA_MASK_FILTER_SIZE);
    return dispatch_mask(s, cu_f, stream, s->proc_width, s->proc_height, s->proc_width, mask_index);
}

static int cambi_cuda_decimate_scale(CambiStateCuda *s, CudaFunctions *cu_f, CUstream stream,
                                     CambiCudaScaleState *scale, int scale_index)
{
    if (scale_index == 0 && !s->cambi_high_res_speedup)
        return 0;
    const unsigned new_width = (scale->width + 1u) >> 1;
    const unsigned new_height = (scale->height + 1u) >> 1;
    int ret = dispatch_decimate(s, cu_f, stream, s->d_image, s->d_tmp, new_width, new_height,
                                scale->width, new_width);
    if (ret)
        return ret;
    VmafCudaBuffer *temporary = s->d_image;
    s->d_image = s->d_tmp;
    s->d_tmp = temporary;
    ret = dispatch_decimate(s, cu_f, stream, s->d_mask, s->d_tmp, new_width, new_height,
                            scale->width, new_width);
    if (ret)
        return ret;
    temporary = s->d_mask;
    s->d_mask = s->d_tmp;
    s->d_tmp = temporary;
    scale->width = new_width;
    scale->height = new_height;
    return 0;
}

static int cambi_cuda_filter_and_download(CambiStateCuda *s, CudaFunctions *cu_f, CUstream stream,
                                          const CambiCudaScaleState *scale)
{
    int ret = dispatch_filter_mode(s, cu_f, stream, s->d_image, s->d_tmp, scale->width,
                                   scale->height, scale->width, 0);
    if (!ret) {
        ret = dispatch_filter_mode(s, cu_f, stream, s->d_tmp, s->d_image, scale->width,
                                   scale->height, scale->width, 1);
    }
    if (ret)
        return ret;
    const size_t row_bytes = scale->width * sizeof(uint16_t);
    CUDA_MEMCPY2D readback = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = s->d_image->data,
        .srcPitch = row_bytes,
        .dstMemoryType = CU_MEMORYTYPE_HOST,
        .dstHost = s->pics[0].data[0],
        .dstPitch = (size_t)s->pics[0].stride[0],
        .WidthInBytes = row_bytes,
        .Height = scale->height,
    };
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&readback, stream));
    readback.srcDevice = s->d_mask->data;
    readback.dstHost = s->pics[1].data[0];
    readback.dstPitch = (size_t)s->pics[1].stride[0];
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&readback, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(stream));
    return 0;
}

static double cambi_cuda_score_scale(CambiStateCuda *s, const CambiCudaScaleState *scale,
                                     int num_diffs, double topk)
{
    vmaf_cambi_calculate_c_values(&s->pics[0], &s->pics[1], s->buffers.c_values,
                                  s->buffers.c_values_histograms, s->adjusted_window,
                                  (uint16_t)num_diffs, s->buffers.tvi_for_diff, s->vlt_luma,
                                  s->buffers.diff_weights, s->buffers.all_diffs, (int)scale->width,
                                  (int)scale->height, s->inc_range_callback, s->dec_range_callback);
    return vmaf_cambi_spatial_pooling(s->buffers.c_values, topk, scale->width, scale->height);
}

static int cambi_cuda_run_scales(CambiStateCuda *s, CudaFunctions *cu_f, CUstream stream,
                                 double scores[CAMBI_CUDA_NUM_SCALES])
{
    CambiCudaScaleState scale = {.width = s->proc_width, .height = s->proc_height};
    const int num_diffs = 1 << s->max_log_contrast;
    const double topk = s->topk != CAMBI_CUDA_DEFAULT_TOPK ? s->topk : s->cambi_topk;
    unsigned stored_width = s->proc_width;
    unsigned stored_height = s->proc_height;
    for (int i = 0; i < CAMBI_CUDA_NUM_SCALES; i++) {
        s->scale_widths[i] = stored_width;
        s->scale_heights[i] = stored_height;
        stored_width = (stored_width + 1u) >> 1;
        stored_height = (stored_height + 1u) >> 1;
        int ret = cambi_cuda_decimate_scale(s, cu_f, stream, &scale, i);
        if (!ret)
            ret = cambi_cuda_filter_and_download(s, cu_f, stream, &scale);
        if (ret)
            return ret;
        scores[i] = cambi_cuda_score_scale(s, &scale, num_diffs, topk);
    }
    return 0;
}

static int cambi_cuda_record_score(VmafFeatureExtractor *fex, CambiStateCuda *s,
                                   CudaFunctions *cu_f, CUstream stream,
                                   const double scores[CAMBI_CUDA_NUM_SCALES])
{
    const uint16_t pixels = vmaf_cambi_get_pixels_in_window(s->adjusted_window);
    double score = vmaf_cambi_weight_scores_per_scale(scores, pixels);
    if (score > s->cambi_max_val)
        score = s->cambi_max_val;
    if (score < 0.0)
        score = 0.0;
    *(double *)s->rb_image.host_pinned = score;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                           const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                           const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    CambiStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    s->index = index;
    const CUresult push = cu_f->cuCtxPushCurrent(fex->cu_state->ctx);
    if (push != CUDA_SUCCESS)
        return vmaf_cuda_result_to_errno((int)push);
    VmafCudaBuffer *const image = s->d_image;
    VmafCudaBuffer *const mask = s->d_mask;
    VmafCudaBuffer *const temporary = s->d_tmp;
    CUstream stream = vmaf_cuda_picture_get_stream(ref_pic);
    int ret = cambi_cuda_upload_initial(s, cu_f, dist_pic, stream);
    double scores[CAMBI_CUDA_NUM_SCALES] = {0.0, 0.0, 0.0, 0.0, 0.0};
    if (!ret)
        ret = cambi_cuda_run_scales(s, cu_f, stream, scores);
    if (!ret)
        ret = cambi_cuda_record_score(fex, s, cu_f, stream, scores);
    s->d_image = image;
    s->d_mask = mask;
    s->d_tmp = temporary;
    const CUresult pop = cu_f->cuCtxPopCurrent(VMAF_NULLPTR);
    if (!ret && pop != CUDA_SUCCESS)
        ret = vmaf_cuda_result_to_errno((int)pop);
    return ret;
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
static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    CambiStateCuda *s = fex->priv;
    int rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    cambi_cuda_keep_first_error(&rc, cambi_cuda_free_device_buffers(fex, s));
    cambi_cuda_keep_first_error(&rc, cambi_cuda_free_readbacks(fex, s));
    cambi_cuda_free_pictures(s);
    cambi_cuda_free_host_buffers(s);
    cambi_cuda_keep_first_error(&rc, cambi_cuda_free_dictionary(s));
    cambi_cuda_keep_first_error(&rc, cambi_cuda_module_unload(fex, s));
    return rc;
}

static const char *provided_features[] = {"Cambi_feature_cambi_score", VMAF_NULLPTR};

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
