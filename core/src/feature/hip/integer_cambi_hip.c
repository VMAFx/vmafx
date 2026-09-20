/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CAMBI banding-detection feature extractor on the HIP backend.
 *  Direct port of `libvmaf/src/feature/cuda/integer_cambi_cuda.c`
 *  (T3-15 / ADR-0360) to the HIP backend.
 *
 *  Strategy II hybrid (matches the CUDA twin):
 *    GPU stages (three HIP kernels in cambi_score.hip):
 *      - cambi_spatial_mask_kernel: 7×7 box derivative + threshold.
 *      - cambi_decimate_kernel: strict 2× stride-2 subsample.
 *      - cambi_filter_mode_kernel: separable 3-tap mode filter (H + V).
 *
 *    Host CPU stages (via cambi_internal.h wrappers — bit-exact):
 *      - vmaf_cambi_preprocessing: decimate/upcast to 10-bit.
 *      - vmaf_cambi_calculate_c_values: sliding-histogram c-value pass.
 *      - vmaf_cambi_spatial_pooling: top-K pooling → per-scale score.
 *      - vmaf_cambi_weight_scores_per_scale: inner-product scale weights.
 *
 *  HIP adaptation notes vs. CUDA twin:
 *    - `CUmodule` / `cuModuleLoadData` → `hipModule_t` / `hipModuleLoadData`.
 *    - `CUfunction` / `cuModuleGetFunction` → `hipFunction_t` / `hipModuleGetFunction`.
 *    - `cuLaunchKernel` → `hipModuleLaunchKernel`.
 *    - `CUdeviceptr` / `cuMemcpyHtoDAsync` / `cuMemcpyDtoH` →
 *      `hipDeviceptr_t` / `hipMemcpyHtoDAsync` / `hipMemcpyDtoH`.
 *    - `cuStreamSynchronize` → `hipStreamSynchronize`.
 *    - `cuCtxPushCurrent` / `cuCtxPopCurrent` — not needed; HIP uses the
 *      default device context selected by `hipSetDevice`.
 *    - `CuStreamWaitEvent` → `hipStreamWaitEvent`.
 *    - `cuEventRecord` → `hipEventRecord`.
 *    - `VmafCudaKernelLifecycle` / `VmafCudaBuffer` / `VmafCudaKernelReadback`
 *      → `VmafHipKernelLifecycle` / dedicated `hipDeviceptr_t` device
 *      allocations via `hipMalloc` / `hipFree` / host pinned via
 *      `VmafHipKernelReadback`.
 *    - `vmaf_cuda_*` → `vmaf_hip_*` equivalents.
 *
 *  Precision contract: `places=4` (ULP=0 on the emitted score). GPU
 *  phases are integer + bit-exact. The host residual runs the exact CPU
 *  code via cambi_internal.h. Cross-backend gate target: ULP=0.
 *
 *  Lifecycle: mirrors the CUDA twin's synchronous-per-scale approach.
 *  submit() runs all GPU + CPU work per scale synchronously; collect()
 *  emits the pre-computed score. This is correct and matches the
 *  per-scale synchronous posture of the CUDA twin.
 */

#include "vmaf_nullptr.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "common.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"
#include "mem.h"
#include "picture.h"

/* HIP common.h must come before <hip/hip_runtime_api.h>: common.h provides
 * the typedef stubs (hipError_t = int) used in the non-HAVE_HIPCC path.
 * Including the real HIP header first causes a type-redefinition error. */
#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "integer_cambi_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>
#endif /* HAVE_HIPCC */

#include "feature/cambi_internal.h"

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* --- Constants matching cambi.c --- */
/* CAMBI_MIN_WIDTH_HEIGHT and CAMBI_WINDOW_DIVISOR come from cambi_internal.h */
#define CAMBI_HIP_NUM_SCALES 5
#define CAMBI_HIP_MASK_FILTER_SIZE 7
#define CAMBI_HIP_DEFAULT_MAX_VAL 1000.0
#define CAMBI_HIP_DEFAULT_WINDOW_SIZE 65
#define CAMBI_HIP_DEFAULT_TOPK 0.6
#define CAMBI_HIP_DEFAULT_TVI 0.019
#define CAMBI_HIP_DEFAULT_VLT 0.0
#define CAMBI_HIP_DEFAULT_MAX_LOG_CONTRAST 2
#define CAMBI_HIP_DEFAULT_EOTF "bt1886"
#define CAMBI_HIP_BLOCK_X 16u
#define CAMBI_HIP_BLOCK_Y 16u

/* ------------------------------------------------------------------ */
/* Private state                                                       */
/* ------------------------------------------------------------------ */
typedef struct CambiStateHip {
    /* HIP lifecycle (stream + events). */
    VmafHipKernelLifecycle lc;
    VmafHipContext *ctx;

#ifdef HAVE_HIPCC
    /* HIP module + kernel function handles (require real HIP types). */
    hipModule_t module;
    hipFunction_t func_mask;
    hipFunction_t func_decimate;
    hipFunction_t func_filter_mode;

    /* Device buffers (flat uint16 arrays, proc_width × proc_height). */
    hipDeviceptr_t d_image; /* current scale image on device */
    hipDeviceptr_t d_mask;  /* spatial mask on device */
    hipDeviceptr_t d_tmp;   /* scratch: filter_mode H output, decimate output */
    size_t d_buf_bytes;     /* allocation size of each device buffer (scale 0) */
#endif                      /* HAVE_HIPCC */

    /* Host VmafPicture pair for CPU residual. */
    VmafPicture pics[2]; /* pics[0] = image, pics[1] = mask */

    /* Pinned host readback: just a double for the pre-computed score. */
    VmafHipKernelReadback rb_score;

    /* Host scratch buffers for the CPU residual. */
    VmafCambiHostBuffers buffers;

    /* Callbacks (scalar; GPU has done the heavy lifting). */
    VmafCambiRangeUpdater inc_range_callback;
    VmafCambiRangeUpdater dec_range_callback;
    VmafCambiDerivativeCalculator derivative_callback;

    /* Configuration options (mirrors CambiStateCuda). */
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
    int cambi_high_res_speedup; /* reserved; v1 ignores it */

    /* Resolved per-frame geometry. */
    unsigned src_width;
    unsigned src_height;
    unsigned src_bpc;
    unsigned proc_width;
    unsigned proc_height;

    /* Adjusted window and vlt_luma threshold. */
    uint16_t adjusted_window;
    uint16_t vlt_luma;

    /* Per-frame index stored by submit() for collect(). */
    unsigned index;

    VmafDictionary *feature_name_dict;
} CambiStateHip;

/* --- Options --- */
#define CAMBI_DOUBLE_OPTION(NAME, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM, ALIAS)                   \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .help = (HELP),                                                                            \
        .offset = offsetof(CambiStateHip, FIELD),                                                  \
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
        .offset = offsetof(CambiStateHip, FIELD),                                                  \
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
        .offset = offsetof(CambiStateHip, FIELD),                                                  \
        .type = VMAF_OPT_TYPE_STRING,                                                              \
        .default_val.s = (DEFAULT),                                                                \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
        .alias = (ALIAS),                                                                          \
    }

static const VmafOption options[] = {
    CAMBI_DOUBLE_OPTION("cambi_max_val", "maximum value allowed; larger values will be clipped",
                        cambi_max_val, CAMBI_HIP_DEFAULT_MAX_VAL, 0.0, 1000.0, "cmxv"),
    CAMBI_INT_OPTION("enc_width", "Encoding width", enc_width, 0, 180, 7680, "encw"),
    CAMBI_INT_OPTION("enc_height", "Encoding height", enc_height, 0, 150, 7680, "ench"),
    CAMBI_INT_OPTION("enc_bitdepth", "Encoding bitdepth", enc_bitdepth, 0, 6, 16, "encbd"),
    CAMBI_INT_OPTION("window_size",
                     "Window size to compute CAMBI: 65 corresponds to ~1 degree at 4k", window_size,
                     CAMBI_HIP_DEFAULT_WINDOW_SIZE, 15, 127, "ws"),
    CAMBI_DOUBLE_OPTION("topk", "Ratio of pixels for the spatial pooling computation", topk,
                        CAMBI_HIP_DEFAULT_TOPK, 0.0001, 1.0, VMAF_NULLPTR),
    CAMBI_DOUBLE_OPTION("cambi_topk", "Ratio of pixels for the spatial pooling computation",
                        cambi_topk, CAMBI_HIP_DEFAULT_TOPK, 0.0001, 1.0, "ctpk"),
    CAMBI_DOUBLE_OPTION("tvi_threshold", "Visibility threshold delta-L < tvi_threshold * L_mean",
                        tvi_threshold, CAMBI_HIP_DEFAULT_TVI, 0.0001, 1.0, "tvit"),
    CAMBI_DOUBLE_OPTION("cambi_vis_lum_threshold",
                        "Luminance value below which banding is assumed invisible",
                        cambi_vis_lum_threshold, CAMBI_HIP_DEFAULT_VLT, 0.0, 300.0, "vlt"),
    CAMBI_INT_OPTION("max_log_contrast", "Maximum log contrast (0 to 5, default 2)",
                     max_log_contrast, CAMBI_HIP_DEFAULT_MAX_LOG_CONTRAST, 0, 5, "mlc"),
    CAMBI_STRING_OPTION("eotf", "EOTF for visibility-threshold conversion (bt1886 / pq)", eotf,
                        CAMBI_HIP_DEFAULT_EOTF, VMAF_NULLPTR),
    CAMBI_STRING_OPTION("cambi_eotf", "EOTF override for cambi (defaults to eotf)", cambi_eotf,
                        CAMBI_HIP_DEFAULT_EOTF, "ceot"),
    {0},
};

#undef CAMBI_STRING_OPTION
#undef CAMBI_INT_OPTION
#undef CAMBI_DOUBLE_OPTION

/* ------------------------------------------------------------------ */
/* Helper: compute adjusted window size (mirrors cambi.c). */
/* ------------------------------------------------------------------ */
static uint16_t cambi_hip_adjust_window(int window_size, unsigned w, unsigned h)
{
    unsigned adjusted = (unsigned)(window_size) * (w + h) / (unsigned)CAMBI_WINDOW_DIVISOR;
    adjusted >>= 4;
    if (adjusted < 1u)
        adjusted = 1u;
    if ((adjusted & 1u) == 0u)
        adjusted++;
    return (uint16_t)adjusted;
}

#ifdef HAVE_HIPCC

/* ------------------------------------------------------------------ */
/* HIP error → errno translation (only needed with real HIP runtime). */
/* ------------------------------------------------------------------ */
static int hip_err(hipError_t rc)
{
    if (rc == hipSuccess)
        return 0;
    switch (rc) {
    case hipErrorInvalidValue:
    case hipErrorInvalidHandle:
        return -EINVAL;
    case hipErrorOutOfMemory:
        return -ENOMEM;
    case hipErrorNoDevice:
    case hipErrorInvalidDevice:
        return -ENODEV;
    case hipErrorNotSupported:
        return -ENOSYS;
    default:
        return -EIO;
    }
}

/* ------------------------------------------------------------------ */
/* Helper: ceil_log2 for mask_index (mirrors cambi.c). */
/* ------------------------------------------------------------------ */
static uint16_t cambi_hip_ceil_log2(uint32_t num)
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

static uint16_t cambi_hip_get_mask_index(unsigned w, unsigned h, uint16_t filter_size)
{
    uint32_t shifted_wh = (w >> 6) * (h >> 6);
    return (
        uint16_t)((filter_size * filter_size + 3 * (cambi_hip_ceil_log2(shifted_wh) - 11) - 1) >>
                  1);
}

/* ------------------------------------------------------------------ */
/* TVI table initialisation. */
/* ------------------------------------------------------------------ */
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
static int cambi_hip_init_tvi(CambiStateHip *s)
{
    const int num_diffs = 1 << s->max_log_contrast;
    return vmaf_cambi_init_tvi_and_vlt(num_diffs, s->buffers.diffs_to_consider, s->tvi_threshold,
                                       s->cambi_vis_lum_threshold, s->cambi_eotf, s->eotf,
                                       s->buffers.tvi_for_diff, &s->vlt_luma, VMAF_NULLPTR, VMAF_NULLPTR);
}

#endif /* HAVE_HIPCC — closes opener at line 305 */

/* ------------------------------------------------------------------ */
/* Module load helper (extracted to keep init under 60 lines). */
/* ------------------------------------------------------------------ */
#ifdef HAVE_HIPCC
static int cambi_hip_module_load(CambiStateHip *s)
{
    hipError_t hip_rc = hipModuleLoadData(&s->module, cambi_score_hsaco);
    if (hip_rc != hipSuccess)
        return hip_err(hip_rc);

    hip_rc = hipModuleGetFunction(&s->func_mask, s->module, "cambi_spatial_mask_kernel");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = VMAF_NULLPTR;
        return hip_err(hip_rc);
    }
    hip_rc = hipModuleGetFunction(&s->func_decimate, s->module, "cambi_decimate_kernel");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = VMAF_NULLPTR;
        return hip_err(hip_rc);
    }
    hip_rc = hipModuleGetFunction(&s->func_filter_mode, s->module, "cambi_filter_mode_kernel");
    if (hip_rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = VMAF_NULLPTR;
        return hip_err(hip_rc);
    }
    return 0;
}
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Device buffer free helper. */
/* ------------------------------------------------------------------ */
static void cambi_hip_free_device_buffers(CambiStateHip *s)
{
#ifdef HAVE_HIPCC
    if (s->d_image) {
        (void)hipFree((void *)s->d_image);
        s->d_image = (hipDeviceptr_t)0;
    }
    if (s->d_mask) {
        (void)hipFree((void *)s->d_mask);
        s->d_mask = (hipDeviceptr_t)0;
    }
    if (s->d_tmp) {
        (void)hipFree((void *)s->d_tmp);
        s->d_tmp = (hipDeviceptr_t)0;
    }
#else
    (void)s;
#endif /* HAVE_HIPCC */
}

static void cambi_hip_free_host_buffers(CambiStateHip *s)
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
    s->buffers.c_values = VMAF_NULLPTR;
    s->buffers.c_values_histograms = VMAF_NULLPTR;
    s->buffers.mask_dp = VMAF_NULLPTR;
    s->buffers.filter_mode_buffer = VMAF_NULLPTR;
    s->buffers.derivative_buffer = VMAF_NULLPTR;
    s->buffers.diffs_to_consider = VMAF_NULLPTR;
    s->buffers.diff_weights = VMAF_NULLPTR;
    s->buffers.all_diffs = VMAF_NULLPTR;
    s->buffers.tvi_for_diff = VMAF_NULLPTR;
}

static void cambi_hip_free_pictures(CambiStateHip *s)
{
    (void)vmaf_picture_unref(&s->pics[0]);
    (void)vmaf_picture_unref(&s->pics[1]);
}

static int cambi_hip_free_dictionary(CambiStateHip *s)
{
    if (s->feature_name_dict == VMAF_NULLPTR)
        return 0;
    return vmaf_dictionary_free(&s->feature_name_dict);
}

static int cambi_hip_geometry_init(CambiStateHip *s, unsigned bpc, unsigned w, unsigned h)
{
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
    if (!cambi_validate_dimensions((unsigned)s->enc_width, (unsigned)s->enc_height)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "cambi_hip: encoded resolution %dx%d below minimum %d×%d.\n",
                 s->enc_width, s->enc_height, CAMBI_MIN_WIDTH_HEIGHT, CAMBI_MIN_WIDTH_HEIGHT);
        return -EINVAL;
    }

    s->src_width = w;
    s->src_height = h;
    s->src_bpc = bpc;
    s->proc_width = (unsigned)s->enc_width;
    s->proc_height = (unsigned)s->enc_height;
    s->adjusted_window = cambi_hip_adjust_window(s->window_size, s->proc_width, s->proc_height);
    return 0;
}

#ifdef HAVE_HIPCC
static int cambi_hip_module_unload(CambiStateHip *s)
{
    int err = 0;
    if (s->module != VMAF_NULLPTR)
        err = hip_err(hipModuleUnload(s->module));
    s->module = VMAF_NULLPTR;
    s->func_mask = VMAF_NULLPTR;
    s->func_decimate = VMAF_NULLPTR;
    s->func_filter_mode = VMAF_NULLPTR;
    return err;
}

static void cambi_hip_context_destroy(CambiStateHip *s)
{
    if (s->ctx != VMAF_NULLPTR) {
        vmaf_hip_context_destroy(s->ctx);
        s->ctx = VMAF_NULLPTR;
    }
}

static int cambi_hip_runtime_base_init(CambiStateHip *s)
{
    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err != 0)
        return err;

    err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) {
        cambi_hip_context_destroy(s);
        return err;
    }
    err = vmaf_hip_kernel_readback_alloc(&s->rb_score, s->ctx, sizeof(double));
    if (err != 0) {
        (void)vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
        cambi_hip_context_destroy(s);
        return err;
    }
    err = cambi_hip_module_load(s);
    if (err != 0) {
        (void)vmaf_hip_kernel_readback_free(&s->rb_score, s->ctx);
        (void)vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
        cambi_hip_context_destroy(s);
    }
    return err;
}

static int cambi_hip_device_buffers_alloc(CambiStateHip *s)
{
    s->d_buf_bytes = (size_t)s->proc_width * s->proc_height * sizeof(uint16_t);
    hipError_t rc = hipMalloc((void **)&s->d_image, s->d_buf_bytes);
    if (rc != hipSuccess)
        return hip_err(rc);
    rc = hipMalloc((void **)&s->d_mask, s->d_buf_bytes);
    if (rc != hipSuccess)
        return hip_err(rc);
    rc = hipMalloc((void **)&s->d_tmp, s->d_buf_bytes);
    return hip_err(rc);
}

static int cambi_hip_pictures_alloc(CambiStateHip *s)
{
    int err =
        vmaf_picture_alloc(&s->pics[0], VMAF_PIX_FMT_YUV400P, 10, s->proc_width, s->proc_height);
    if (err != 0)
        return err;
    return vmaf_picture_alloc(&s->pics[1], VMAF_PIX_FMT_YUV400P, 10, s->proc_width, s->proc_height);
}

static int cambi_hip_diff_buffers_alloc(CambiStateHip *s, int num_diffs)
{
    s->buffers.diffs_to_consider = malloc(sizeof(uint16_t) * (size_t)num_diffs);
    s->buffers.diff_weights = malloc(sizeof(int) * (size_t)num_diffs);
    s->buffers.all_diffs = malloc(sizeof(int) * (size_t)(2 * num_diffs + 1));
    s->buffers.tvi_for_diff = malloc(sizeof(uint16_t) * (size_t)num_diffs);
    if (s->buffers.diffs_to_consider == VMAF_NULLPTR || s->buffers.diff_weights == VMAF_NULLPTR ||
        s->buffers.all_diffs == VMAF_NULLPTR || s->buffers.tvi_for_diff == VMAF_NULLPTR)
        return -ENOMEM;

    static const int contrast_weights[32] = {1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8,
                                             8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    for (int d = 0; d < num_diffs; d++) {
        s->buffers.diffs_to_consider[d] = (uint16_t)(d + 1);
        s->buffers.diff_weights[d] = contrast_weights[d];
    }
    for (int d = -num_diffs; d <= num_diffs; d++)
        s->buffers.all_diffs[d + num_diffs] = d;
    return cambi_hip_init_tvi(s);
}

static int cambi_hip_work_buffers_alloc(CambiStateHip *s, int num_diffs)
{
    const uint16_t num_bins = (uint16_t)(1024u + (unsigned)(s->buffers.all_diffs[2 * num_diffs] -
                                                            s->buffers.all_diffs[0]));
    const int pad_size = CAMBI_HIP_MASK_FILTER_SIZE / 2;
    const int dp_width = (int)s->proc_width + 2 * pad_size + 1;
    const int dp_height = 2 * pad_size + 2;

    s->buffers.c_values = malloc(sizeof(float) * s->proc_width * s->proc_height);
    s->buffers.c_values_histograms = malloc(sizeof(uint16_t) * s->proc_width * (size_t)num_bins);
    s->buffers.mask_dp = malloc(sizeof(uint32_t) * (size_t)dp_width * (size_t)dp_height);
    s->buffers.filter_mode_buffer = malloc(sizeof(uint16_t) * 3u * s->proc_width);
    s->buffers.derivative_buffer = malloc(sizeof(uint16_t) * s->proc_width);
    if (s->buffers.c_values == VMAF_NULLPTR || s->buffers.c_values_histograms == VMAF_NULLPTR ||
        s->buffers.mask_dp == VMAF_NULLPTR || s->buffers.filter_mode_buffer == VMAF_NULLPTR ||
        s->buffers.derivative_buffer == VMAF_NULLPTR)
        return -ENOMEM;
    return 0;
}

static void cambi_hip_abort_initialized(CambiStateHip *s)
{
    (void)cambi_hip_module_unload(s);
    cambi_hip_free_device_buffers(s);
    cambi_hip_free_pictures(s);
    cambi_hip_free_host_buffers(s);
    (void)vmaf_hip_kernel_readback_free(&s->rb_score, s->ctx);
    (void)vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    cambi_hip_context_destroy(s);
}
#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* Kernel dispatch helpers. */
/* ------------------------------------------------------------------ */
#ifdef HAVE_HIPCC

static int dispatch_mask_hip(CambiStateHip *s, hipStream_t stream, unsigned w, unsigned h,
                             unsigned stride_words, unsigned mask_index)
{
    const unsigned grid_x = (w + CAMBI_HIP_BLOCK_X - 1u) / CAMBI_HIP_BLOCK_X;
    const unsigned grid_y = (h + CAMBI_HIP_BLOCK_Y - 1u) / CAMBI_HIP_BLOCK_Y;
    void *params[] = {&s->d_image, &s->d_mask, &w, &h, &stride_words, &mask_index};
    hipError_t rc = hipModuleLaunchKernel(s->func_mask, grid_x, grid_y, 1u, CAMBI_HIP_BLOCK_X,
                                          CAMBI_HIP_BLOCK_Y, 1u, 0u, stream, params, VMAF_NULLPTR);
    return hip_err(rc);
}

static int dispatch_decimate_hip(CambiStateHip *s, hipStream_t stream, hipDeviceptr_t src,
                                 hipDeviceptr_t dst, unsigned out_w, unsigned out_h,
                                 unsigned src_stride_words, unsigned dst_stride_words)
{
    const unsigned grid_x = (out_w + CAMBI_HIP_BLOCK_X - 1u) / CAMBI_HIP_BLOCK_X;
    const unsigned grid_y = (out_h + CAMBI_HIP_BLOCK_Y - 1u) / CAMBI_HIP_BLOCK_Y;
    void *params[] = {&src, &dst, &out_w, &out_h, &src_stride_words, &dst_stride_words};
    hipError_t rc = hipModuleLaunchKernel(s->func_decimate, grid_x, grid_y, 1u, CAMBI_HIP_BLOCK_X,
                                          CAMBI_HIP_BLOCK_Y, 1u, 0u, stream, params, VMAF_NULLPTR);
    return hip_err(rc);
}

static int dispatch_filter_mode_hip(CambiStateHip *s, hipStream_t stream, hipDeviceptr_t in,
                                    hipDeviceptr_t out, unsigned w, unsigned h,
                                    unsigned stride_words, int axis)
{
    const unsigned grid_x = (w + CAMBI_HIP_BLOCK_X - 1u) / CAMBI_HIP_BLOCK_X;
    const unsigned grid_y = (h + CAMBI_HIP_BLOCK_Y - 1u) / CAMBI_HIP_BLOCK_Y;
    void *params[] = {&in, &out, &w, &h, &stride_words, &axis};
    hipError_t rc =
        hipModuleLaunchKernel(s->func_filter_mode, grid_x, grid_y, 1u, CAMBI_HIP_BLOCK_X,
                              CAMBI_HIP_BLOCK_Y, 1u, 0u, stream, params, VMAF_NULLPTR);
    return hip_err(rc);
}

#endif /* HAVE_HIPCC */

/* ------------------------------------------------------------------ */
/* init_fex_hip                                                        */
/* ------------------------------------------------------------------ */
static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)pix_fmt;
    (void)bpc;
    (void)w;
    (void)h;
    return -ENOSYS;
#else
    (void)pix_fmt;
    CambiStateHip *s = fex->priv;
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == VMAF_NULLPTR)
        return -ENOMEM;

    int err = cambi_hip_geometry_init(s, bpc, w, h);
    if (err != 0) {
        (void)cambi_hip_free_dictionary(s);
        return err;
    }
    err = cambi_hip_runtime_base_init(s);
    if (err != 0) {
        (void)cambi_hip_free_dictionary(s);
        return err;
    }
    err = cambi_hip_device_buffers_alloc(s);
    if (err == 0)
        err = cambi_hip_pictures_alloc(s);
    const int num_diffs = 1 << s->max_log_contrast;
    if (err == 0)
        err = cambi_hip_diff_buffers_alloc(s, num_diffs);
    if (err == 0)
        err = cambi_hip_work_buffers_alloc(s, num_diffs);
    if (err != 0) {
        cambi_hip_abort_initialized(s);
        (void)cambi_hip_free_dictionary(s);
        return err;
    }

    vmaf_cambi_default_callbacks(&s->inc_range_callback, &s->dec_range_callback,
                                 &s->derivative_callback);
    return 0;
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* submit_fex_hip                                                      */
/* ------------------------------------------------------------------ */
#ifdef HAVE_HIPCC
typedef struct CambiHipScaleState {
    unsigned width;
    unsigned height;
    hipDeviceptr_t image;
    hipDeviceptr_t mask;
    hipDeviceptr_t temporary;
} CambiHipScaleState;

static int cambi_hip_upload_initial(CambiStateHip *s, const VmafPicture *dist_pic, hipStream_t stream)
{
    int err = vmaf_cambi_preprocessing(dist_pic, &s->pics[0], (int)s->proc_width,
                                       (int)s->proc_height, s->enc_bitdepth);
    if (err != 0)
        return err;

    const size_t row_bytes = s->proc_width * sizeof(uint16_t);
    hipError_t rc =
        hipMemcpy2DAsync(s->d_image, row_bytes, s->pics[0].data[0], (size_t)s->pics[0].stride[0],
                         row_bytes, s->proc_height, hipMemcpyHostToDevice, stream);
    if (rc != hipSuccess)
        return hip_err(rc);

    const unsigned mask_index = (unsigned)cambi_hip_get_mask_index(s->proc_width, s->proc_height,
                                                                   CAMBI_HIP_MASK_FILTER_SIZE);
    return dispatch_mask_hip(s, stream, s->proc_width, s->proc_height, s->proc_width, mask_index);
}

static int cambi_hip_decimate_scale(CambiStateHip *s, hipStream_t stream, CambiHipScaleState *scale)
{
    const unsigned new_width = (scale->width + 1u) >> 1;
    const unsigned new_height = (scale->height + 1u) >> 1;
    int err = dispatch_decimate_hip(s, stream, scale->image, scale->temporary, new_width,
                                    new_height, scale->width, new_width);
    if (err != 0)
        return err;
    hipDeviceptr_t swap = scale->image;
    scale->image = scale->temporary;
    scale->temporary = swap;

    err = dispatch_decimate_hip(s, stream, scale->mask, scale->temporary, new_width, new_height,
                                scale->width, new_width);
    if (err != 0)
        return err;
    swap = scale->mask;
    scale->mask = scale->temporary;
    scale->temporary = swap;
    scale->width = new_width;
    scale->height = new_height;
    return 0;
}

static int cambi_hip_filter_and_download(CambiStateHip *s, hipStream_t stream,
                                         const CambiHipScaleState *scale)
{
    int err = dispatch_filter_mode_hip(s, stream, scale->image, scale->temporary, scale->width,
                                       scale->height, scale->width, 0);
    if (err != 0)
        return err;
    err = dispatch_filter_mode_hip(s, stream, scale->temporary, scale->image, scale->width,
                                   scale->height, scale->width, 1);
    if (err != 0)
        return err;

    const size_t row_bytes = scale->width * sizeof(uint16_t);
    hipError_t rc =
        hipMemcpy2DAsync(s->pics[0].data[0], (size_t)s->pics[0].stride[0], scale->image, row_bytes,
                         row_bytes, scale->height, hipMemcpyDeviceToHost, stream);
    if (rc != hipSuccess)
        return hip_err(rc);
    rc = hipMemcpy2DAsync(s->pics[1].data[0], (size_t)s->pics[1].stride[0], scale->mask, row_bytes,
                          row_bytes, scale->height, hipMemcpyDeviceToHost, stream);
    if (rc != hipSuccess)
        return hip_err(rc);
    return hip_err(hipStreamSynchronize(stream));
}

static double cambi_hip_score_scale(CambiStateHip *s, const CambiHipScaleState *scale,
                                    int num_diffs, double topk)
{
    vmaf_cambi_calculate_c_values(&s->pics[0], &s->pics[1], s->buffers.c_values,
                                  s->buffers.c_values_histograms, s->adjusted_window,
                                  (uint16_t)num_diffs, s->buffers.tvi_for_diff, s->vlt_luma,
                                  s->buffers.diff_weights, s->buffers.all_diffs, (int)scale->width,
                                  (int)scale->height, s->inc_range_callback, s->dec_range_callback);
    return vmaf_cambi_spatial_pooling(s->buffers.c_values, topk, scale->width, scale->height);
}

static int cambi_hip_run_scales(CambiStateHip *s, hipStream_t stream, double scores_per_scale[])
{
    CambiHipScaleState scale = {
        .width = s->proc_width,
        .height = s->proc_height,
        .image = s->d_image,
        .mask = s->d_mask,
        .temporary = s->d_tmp,
    };
    const int num_diffs = 1 << s->max_log_contrast;
    const double topk = s->topk != CAMBI_HIP_DEFAULT_TOPK ? s->topk : s->cambi_topk;

    for (int i = 0; i < CAMBI_HIP_NUM_SCALES; i++) {
        int err = i == 0 ? 0 : cambi_hip_decimate_scale(s, stream, &scale);
        if (err != 0)
            return err;
        err = cambi_hip_filter_and_download(s, stream, &scale);
        if (err != 0)
            return err;
        scores_per_scale[i] = cambi_hip_score_scale(s, &scale, num_diffs, topk);
    }
    return 0;
}

static int cambi_hip_record_score(CambiStateHip *s, hipStream_t stream,
                                  const double scores_per_scale[])
{
    const uint16_t pixels_in_window = vmaf_cambi_get_pixels_in_window(s->adjusted_window);
    double score = vmaf_cambi_weight_scores_per_scale(scores_per_scale, pixels_in_window);
    if (score > s->cambi_max_val)
        score = s->cambi_max_val;
    if (score < 0.0)
        score = 0.0;
    *(double *)s->rb_score.host_pinned = score;

    hipError_t rc = hipEventRecord((hipEvent_t)s->lc.submit, stream);
    if (rc != hipSuccess)
        return hip_err(rc);
    rc = hipStreamWaitEvent(stream, (hipEvent_t)s->lc.submit, 0u);
    if (rc != hipSuccess)
        return hip_err(rc);
    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}
#endif /* HAVE_HIPCC */

static int submit_fex_hip(VmafFeatureExtractor *fex, const VmafPicture *ref_pic, const VmafPicture *ref_pic_90,
                          const VmafPicture *dist_pic, const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic_90;

#ifndef HAVE_HIPCC
    (void)fex;
    (void)dist_pic;
    (void)index;
    return -ENOSYS;
#else
    CambiStateHip *s = fex->priv;
    s->index = index;
    const hipStream_t stream = (hipStream_t)s->lc.str;
    int err = cambi_hip_upload_initial(s, dist_pic, stream);
    if (err != 0)
        return err;

    double scores_per_scale[CAMBI_HIP_NUM_SCALES] = {0.0, 0.0, 0.0, 0.0, 0.0};
    err = cambi_hip_run_scales(s, stream, scores_per_scale);
    if (err != 0)
        return err;
    return cambi_hip_record_score(s, stream, scores_per_scale);
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* collect_fex_hip — emit the pre-computed score. */
/* ------------------------------------------------------------------ */
static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index,
                           VmafFeatureCollector *feature_collector)
{
#ifndef HAVE_HIPCC
    (void)fex;
    (void)index;
    (void)feature_collector;
    return -ENOSYS;
#else
    CambiStateHip *s = fex->priv;

    int err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (err)
        return err;

    const double score = *(double *)s->rb_score.host_pinned;
    return vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "Cambi_feature_cambi_score", score, index);
#endif /* HAVE_HIPCC */
}

/* ------------------------------------------------------------------ */
/* close_fex_hip */
/* ------------------------------------------------------------------ */
static int close_fex_hip(VmafFeatureExtractor *fex)
{
    CambiStateHip *s = fex->priv;
    int rc = 0;

#ifdef HAVE_HIPCC
    int err = cambi_hip_module_unload(s);
    if (rc == 0)
        rc = err;
    cambi_hip_free_device_buffers(s);

    err = vmaf_hip_kernel_readback_free(&s->rb_score, s->ctx);
    if (rc == 0)
        rc = err;
    err = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
    if (rc == 0)
        rc = err;
#endif /* HAVE_HIPCC */

    cambi_hip_free_pictures(s);
    cambi_hip_free_host_buffers(s);
    const int dict_err = cambi_hip_free_dictionary(s);
    if (rc == 0)
        rc = dict_err;
#ifdef HAVE_HIPCC
    cambi_hip_context_destroy(s);
#endif /* HAVE_HIPCC */
    return rc;
}

static const char *provided_features[] = {"Cambi_feature_cambi_score", VMAF_NULLPTR};

/* Load-bearing: declared `extern` in feature_extractor.c's
 * `feature_extractor_list[]` under `#if HAVE_HIP`. Making this static
 * would unlink the extractor from the registry. Same pattern as every
 * HIP extractor (see vmaf_fex_float_psnr_hip). */
VmafFeatureExtractor vmaf_fex_cambi_hip = {
    .name = "cambi_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(CambiStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 15, /* 5 scales × 3 kernels */
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_DIRECT,
        },
};
