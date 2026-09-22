/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_adm feature extractor on the HIP backend — ninth consumer
 *  of `core/src/hip/kernel_template.h` (T7-10b batch-2 / ADR-0468).
 *
 *  This TU mirrors `core/src/feature/cuda/float_adm_cuda.c`
 *  call-graph-for-call-graph. When `HAVE_HIPCC` is defined (i.e.,
 *  `enable_hipcc=true` at configure time), the `init`, `submit`, and
 *  `collect` functions use real HIP Module API calls following the
 *  canonical pattern established by PR #612 / ADR-0254.
 *
 *  Without `HAVE_HIPCC` (CPU-only or HIP-scaffold builds), every
 *  lifecycle helper returns -ENOSYS (scaffold posture preserved).
 *
 *  Algorithm: 16-launch (4 stages × 4 scales) DWT+CSF+CM pipeline.
 *  Same four pipeline stages as the CUDA twin, same `-1` mirror form,
 *  same fused stage 3 with cross-band CM threshold.
 *  Host reduction in double precision (places=4 contract).
 *
 *  Key HIP adaptation:
 *  - Module API: `hipModuleLoadData` / `hipModuleGetFunction` /
 *    `hipModuleLaunchKernel` replace CUDA driver-API equivalents.
 *  - Buffer alloc: `hipMalloc` / `hipMemsetAsync` / `hipMemcpyAsync`
 *    replace `vmaf_cuda_buffer_alloc` + `cuMemsetD8Async`.
 *  - Stream/event: `hipStream_t` / `hipEvent_t` from `lc`; same
 *    submit/finished event-fence pattern as every HIP consumer.
 *  - Warp size 64 on GCN/RDNA: shared-memory partial arrays sized
 *    at FADM_WARPS_PER_BLOCK = 4.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "dict.h"
#include "feature/adm_options.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "float_adm_hip.h"

#ifdef HAVE_HIPCC
#include <hip/hip_runtime_api.h>

#include "../../hip/hip_handle.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

extern const unsigned char float_adm_score_hsaco[];
extern const unsigned int float_adm_score_hsaco_len;
#endif /* HAVE_HIPCC */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FADM_NUM_SCALES 4
#define FADM_NUM_BANDS 3
#define FADM_BX 16
#define FADM_BY 16
#define FADM_BORDER_FACTOR 0.1
/* ADR-0574: slots 0..5 = adm2 csf+cm per band; slots 6..8 = aim_cm per band. */
#define FADM_ACCUM_SLOTS 9

typedef struct FloatAdmStateHip {
    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    /* ADR-0574: AIM / ADM3 options. */
    int adm_adm3_apply_hm;
    double adm_p_norm;
    double adm_dlm_weight;
    double adm_min_val;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned buf_stride;

    float rfactor[12];

    VmafHipKernelLifecycle lc;
    VmafHipContext *ctx;

#ifdef HAVE_HIPCC
    hipModule_t module;
    hipFunction_t func_dwt_vert;
    hipFunction_t func_dwt_hori;
    hipFunction_t func_decouple_csf;
    hipFunction_t func_csf_cm;
    hipFunction_t func_csf_r;
    hipFunction_t func_aim_cm;

    void *src_ref;
    void *src_dis;
    void *dwt_tmp_ref;
    void *dwt_tmp_dis;
    void *ref_band[FADM_NUM_SCALES];
    void *dis_band[FADM_NUM_SCALES];
    void *csf_a;
    void *csf_f;
    void *csf_a_aim;
    void *csf_f_aim;
    void *accum[FADM_NUM_SCALES];
    float *accum_host[FADM_NUM_SCALES];
#endif /* HAVE_HIPCC */

    unsigned wg_count[FADM_NUM_SCALES];
    unsigned scale_w[FADM_NUM_SCALES];
    unsigned scale_h[FADM_NUM_SCALES];
    unsigned scale_half_w[FADM_NUM_SCALES];
    unsigned scale_half_h[FADM_NUM_SCALES];

    VmafDictionary *feature_name_dict;
} FloatAdmStateHip;

/* Compact layout: clang-format would put every field on its own line and push
 * the table past the 60-line HISS-04 function-size limit. */
// clang-format off
static const VmafOption options[] = {
    {.name = "debug", .help = "debug mode: enable additional output",
     .offset = offsetof(FloatAdmStateHip, debug), .type = VMAF_OPT_TYPE_BOOL,
     .default_val.b = false},
    {.name = "adm_enhn_gain_limit", .alias = "egl",
     .help = "enhancement gain imposed on adm, must be >= 1.0",
     .offset = offsetof(FloatAdmStateHip, adm_enhn_gain_limit), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 100.0, .min = 1.0, .max = 100.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_norm_view_dist", .alias = "nvd", .help = "normalized viewing distance",
     .offset = offsetof(FloatAdmStateHip, adm_norm_view_dist), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 3.0, .min = 0.75, .max = 24.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_ref_display_height", .alias = "rdf", .help = "reference display height in pixels",
     .offset = offsetof(FloatAdmStateHip, adm_ref_display_height), .type = VMAF_OPT_TYPE_INT,
     .default_val.i = 1080, .min = 1, .max = 4320, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_mode", .alias = "csf",
     .help = "contrast sensitivity function (mode 0 only on HIP v1)",
     .offset = offsetof(FloatAdmStateHip, adm_csf_mode), .type = VMAF_OPT_TYPE_INT,
     .default_val.i = 0, .min = 0, .max = 9, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_scale", .alias = "scf",
     .help = "CSF band-scale multiplier for h/v bands (default 1.0 = no scaling)",
     .offset = offsetof(FloatAdmStateHip, adm_csf_scale), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = DEFAULT_ADM_CSF_SCALE, .min = 0.0, .max = 50.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_diag_scale", .alias = "scfd",
     .help = "CSF band-scale multiplier for diagonal bands (default 1.0 = no scaling)",
     .offset = offsetof(FloatAdmStateHip, adm_csf_diag_scale), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = DEFAULT_ADM_CSF_DIAG_SCALE, .min = 0.0, .max = 50.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_noise_weight", .alias = "nw",
     .help = "noise floor weight for CM numerator (default 0.03125 = 1/32)",
     .offset = offsetof(FloatAdmStateHip, adm_noise_weight), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = DEFAULT_ADM_NOISE_WEIGHT, .min = 0.0, .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    /* ADR-0574: AIM / ADM3 options — mirrors CUDA twin. */
    {.name = "adm_adm3_apply_hm", .alias = "aah",
     .help = "apply harmonic mean for adm3 score (false = linear blend)",
     .offset = offsetof(FloatAdmStateHip, adm_adm3_apply_hm), .type = VMAF_OPT_TYPE_BOOL,
     .default_val.b = false, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_p_norm", .alias = "apn",
     .help = "p-norm exponent for AIM/ADM3 score (default 3.0)",
     .offset = offsetof(FloatAdmStateHip, adm_p_norm), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 3.0, .min = 1.0, .max = 20.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_dlm_weight", .alias = "dlmw",
     .help = "DLM weight for linear-blend adm3 score (default 0.5)",
     .offset = offsetof(FloatAdmStateHip, adm_dlm_weight), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 0.5, .min = 0.0, .max = 1.0, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_min_val", .alias = "min", .help = "minimum clamp for adm3 score (default 0.0)",
     .offset = offsetof(FloatAdmStateHip, adm_min_val), .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = DEFAULT_ADM_MIN_VAL, .min = 0.0, .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {0}};
// clang-format on

/* DB2/CDF-9-7 wavelet noise model — identical to the CUDA twin. */
static const float fadm_dwt_basis_amp[6][4] = {
    {0.62171f, 0.67234f, 0.72709f, 0.67234f},     {0.34537f, 0.41317f, 0.49428f, 0.41317f},
    {0.18004f, 0.22727f, 0.28688f, 0.22727f},     {0.091401f, 0.11792f, 0.15214f, 0.11792f},
    {0.045943f, 0.059758f, 0.077727f, 0.059758f}, {0.023013f, 0.030018f, 0.039156f, 0.030018f},
};
static const float fadm_dwt_a_Y = 0.495f;
static const float fadm_dwt_k_Y = 0.466f;
static const float fadm_dwt_f0_Y = 0.401f;
static const float fadm_dwt_g_Y[4] = {1.501f, 1.0f, 0.534f, 1.0f};

static float fadm_dwt_quant_step(int lambda, int theta, double view_dist, int display_h)
{
    const float r = (float)(view_dist * (double)display_h * M_PI / 180.0);
    const float temp = (float)log10(pow(2.0, (double)(lambda + 1)) * (double)fadm_dwt_f0_Y *
                                    (double)fadm_dwt_g_Y[theta] / (double)r);
    const float Q = (float)(2.0 * (double)fadm_dwt_a_Y *
                            pow(10.0, (double)fadm_dwt_k_Y * (double)temp * (double)temp) /
                            (double)fadm_dwt_basis_amp[lambda][theta]);
    return Q;
}

static void compute_per_scale_dims(FloatAdmStateHip *s)
{
    unsigned cw = s->width;
    unsigned ch = s->height;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const unsigned hw = (cw + 1u) / 2u;
        const unsigned hh = (ch + 1u) / 2u;
        s->scale_w[scale] = cw;
        s->scale_h[scale] = ch;
        s->scale_half_w[scale] = hw;
        s->scale_half_h[scale] = hh;
        cw = hw;
        ch = hh;
    }
    s->buf_stride = (s->scale_half_w[0] + 3u) & ~3u;
}

/* Active range [*lo, *hi) of a half-resolution dimension once the ADM border
 * (FADM_BORDER_FACTOR of the dimension on each side) is removed. */
static void fadm_active_range(int half, int *lo, int *hi)
{
    int border = (int)((double)half * FADM_BORDER_FACTOR - 0.5);
    if (border < 0)
        border = 0;
    *lo = border;
    *hi = half - border;
}

#ifdef HAVE_HIPCC
static int fadm_hip_rc(hipError_t rc)
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

typedef struct FadmHipKernelSlot {
    hipFunction_t *slot;
    const char *name;
} FadmHipKernelSlot;

/* Load the kernel blob and resolve the six kernels by name. On failure the
 * module is unloaded again and `s->module` is NULL. */
static int fadm_hip_module_load(FloatAdmStateHip *s)
{
    hipError_t rc = hipModuleLoadData(&s->module, float_adm_score_hsaco);
    if (rc != hipSuccess)
        return fadm_hip_rc(rc);

    const FadmHipKernelSlot kernels[] = {
        {&s->func_dwt_vert, "float_adm_dwt_vert"},
        {&s->func_dwt_hori, "float_adm_dwt_hori"},
        {&s->func_decouple_csf, "float_adm_decouple_csf"},
        {&s->func_csf_cm, "float_adm_csf_cm"},
        {&s->func_csf_r, "float_adm_csf_r"},
        {&s->func_aim_cm, "float_adm_aim_cm"},
    };
    const unsigned n_kernels = (unsigned)(sizeof(kernels) / sizeof(kernels[0]));
    for (unsigned i = 0; i < n_kernels && rc == hipSuccess; i++)
        rc = hipModuleGetFunction(kernels[i].slot, s->module, kernels[i].name);
    if (rc != hipSuccess) {
        (void)hipModuleUnload(s->module);
        s->module = NULL;
    }
    return fadm_hip_rc(rc);
}

/* What the six stages of one scale share. The fields are kernel arguments,
 * passed by address, so the struct is not const. */
typedef struct FadmScaleGeom {
    int scale;
    int cur_w;
    int cur_h;
    int half_w;
    int half_h;
    int buf_stride;
    /* Active window of the CM stages, border removed. */
    int left;
    int top;
    int right;
    int bottom;
    float rfh;
    float rfv;
    float rfd;
    float gain_limit;
    float pnorm;
    float *ref_band;
    float *dis_band;
} FadmScaleGeom;

static void fadm_hip_scale_geom(const FloatAdmStateHip *s, int scale, FadmScaleGeom *g)
{
    g->scale = scale;
    g->cur_w = (int)s->scale_w[scale];
    g->cur_h = (int)s->scale_h[scale];
    g->half_w = (int)s->scale_half_w[scale];
    g->half_h = (int)s->scale_half_h[scale];
    g->buf_stride = (int)s->buf_stride;
    fadm_active_range(g->half_h, &g->top, &g->bottom);
    fadm_active_range(g->half_w, &g->left, &g->right);
    g->rfh = s->rfactor[scale * 3 + 0];
    g->rfv = s->rfactor[scale * 3 + 1];
    g->rfd = s->rfactor[scale * 3 + 2];
    g->gain_limit = (float)s->adm_enhn_gain_limit;
    /* adm_p_norm is a VMAF_OPT_FLAG_FEATURE_PARAM the twin advertises; until
     * ADR-1220 the kernels hardcoded p = 3 and it moved only the AIM exponent. */
    g->pnorm = (float)s->adm_p_norm;
    g->ref_band = (float *)s->ref_band[scale];
    g->dis_band = (float *)s->dis_band[scale];
}

/* Stage 0 — DWT vertical (z=2 fuses ref+dis). Scale 0 reads the staged raw
 * planes; scales 1..3 read the parent scale's band buffer. */
static int fadm_launch_dwt_vert(FloatAdmStateHip *s, FadmScaleGeom *g, hipStream_t pstr)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    ptrdiff_t raw_stride = (ptrdiff_t)(s->width * bpp);
    float scaler = 1.0f;
    if (s->bpc == 10u) {
        scaler = 4.0f;
    } else if (s->bpc == 12u) {
        scaler = 16.0f;
    } else if (s->bpc == 16u) {
        scaler = 256.0f;
    }
    float pixel_offset = -128.0f;

    const bool has_parent = (g->scale > 0);
    const uint8_t *ref_raw_d = (const uint8_t *)s->src_ref;
    const uint8_t *dis_raw_d = (const uint8_t *)s->src_dis;
    const float *parent_ref = has_parent ? (const float *)s->ref_band[g->scale - 1] : NULL;
    const float *parent_dis = has_parent ? (const float *)s->dis_band[g->scale - 1] : NULL;
    int par_w = has_parent ? g->cur_w : 0;
    int par_h = has_parent ? g->cur_h : 0;
    int par_half_h = has_parent ? (int)s->scale_half_h[g->scale - 1] : 0;
    float *dwt_ref_d = (float *)s->dwt_tmp_ref;
    float *dwt_dis_d = (float *)s->dwt_tmp_dis;
    unsigned bpc = s->bpc;

    const unsigned gx = ((unsigned)g->cur_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)g->half_h + FADM_BY - 1u) / FADM_BY;
    void *args[] = {(void *)&g->scale,      (void *)&ref_raw_d,  (void *)&dis_raw_d,
                    (void *)&raw_stride,    (void *)&parent_ref, (void *)&parent_dis,
                    (void *)&g->buf_stride, (void *)&par_half_h, (void *)&par_w,
                    (void *)&par_h,         (void *)&dwt_ref_d,  (void *)&dwt_dis_d,
                    (void *)&g->cur_w,      (void *)&g->cur_h,   (void *)&g->half_h,
                    (void *)&bpc,           (void *)&scaler,     (void *)&pixel_offset};
    return fadm_hip_rc(hipModuleLaunchKernel(s->func_dwt_vert, gx, gy, 2u, FADM_BX, FADM_BY, 1u, 0u,
                                             pstr, args, NULL));
}

/* Stage 1 — DWT horizontal. */
static int fadm_launch_dwt_hori(FloatAdmStateHip *s, FadmScaleGeom *g, hipStream_t pstr)
{
    float *dwt_ref_d = (float *)s->dwt_tmp_ref;
    float *dwt_dis_d = (float *)s->dwt_tmp_dis;
    const unsigned gx = ((unsigned)g->half_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)g->half_h + FADM_BY - 1u) / FADM_BY;
    void *args[] = {(void *)&g->scale,    (void *)&dwt_ref_d,   (void *)&dwt_dis_d,
                    (void *)&g->ref_band, (void *)&g->dis_band, (void *)&g->cur_w,
                    (void *)&g->half_w,   (void *)&g->half_h,   (void *)&g->buf_stride};
    return fadm_hip_rc(hipModuleLaunchKernel(s->func_dwt_hori, gx, gy, 2u, FADM_BX, FADM_BY, 1u, 0u,
                                             pstr, args, NULL));
}

/* Stages 2 and 2b — decouple + CSF into `csf_a` / `csf_f`. Stage 2 is
 * `func_decouple_csf`; stage 2b is `func_csf_r`, which works on decouple_r
 * and writes the AIM buffers (ADR-0574). Same argument list. */
static int fadm_launch_csf(hipFunction_t func, FadmScaleGeom *g, void *csf_a, void *csf_f,
                           hipStream_t pstr)
{
    float *csf_a_d = (float *)csf_a;
    float *csf_f_d = (float *)csf_f;
    const unsigned gx = ((unsigned)g->half_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)g->half_h + FADM_BY - 1u) / FADM_BY;
    void *args[] = {(void *)&g->ref_band,   (void *)&g->dis_band,  (void *)&csf_a_d,
                    (void *)&csf_f_d,       (void *)&g->half_w,    (void *)&g->half_h,
                    (void *)&g->buf_stride, (void *)&g->rfh,       (void *)&g->rfv,
                    (void *)&g->rfd,        (void *)&g->gain_limit};
    return fadm_hip_rc(
        hipModuleLaunchKernel(func, gx, gy, 1u, FADM_BX, FADM_BY, 1u, 0u, pstr, args, NULL));
}

/* Stages 3 and 3b — contrast masking, 1D over 3 bands x active rows, into the
 * scale's accumulator. Stage 3 is `func_csf_cm` (CSF denominator + CM fused);
 * stage 3b is `func_aim_cm` on the AIM buffers (noise_weight = 0, ADR-0574).
 * Same argument list. */
static int fadm_launch_cm(FloatAdmStateHip *s, hipFunction_t func, FadmScaleGeom *g, void *csf_a,
                          void *csf_f, hipStream_t pstr)
{
    float *csf_a_d = (float *)csf_a;
    float *csf_f_d = (float *)csf_f;
    float *accum_d = (float *)s->accum[g->scale];
    const int active_h = g->bottom - g->top;
    const unsigned num_rows = (unsigned)(active_h > 0 ? active_h : 1);
    const unsigned gx = 3u * num_rows;
    void *args[] = {(void *)&g->ref_band,   (void *)&g->dis_band,   (void *)&csf_a_d,
                    (void *)&csf_f_d,       (void *)&accum_d,       (void *)&g->half_w,
                    (void *)&g->half_h,     (void *)&g->buf_stride, (void *)&g->left,
                    (void *)&g->top,        (void *)&g->right,      (void *)&g->bottom,
                    (void *)&g->rfh,        (void *)&g->rfv,        (void *)&g->rfd,
                    (void *)&g->gain_limit, (void *)&g->pnorm};
    return fadm_hip_rc(
        hipModuleLaunchKernel(func, gx, 1u, 1u, FADM_BX, FADM_BY, 1u, 0u, pstr, args, NULL));
}

/* The six stages of one scale, in the CUDA twin's order. */
static int fadm_hip_launch_scale(FloatAdmStateHip *s, int scale, hipStream_t pstr)
{
    FadmScaleGeom g;
    fadm_hip_scale_geom(s, scale, &g);

    int err = fadm_launch_dwt_vert(s, &g, pstr);
    if (err == 0)
        err = fadm_launch_dwt_hori(s, &g, pstr);
    if (err == 0)
        err = fadm_launch_csf(s->func_decouple_csf, &g, s->csf_a, s->csf_f, pstr);
    if (err == 0)
        err = fadm_launch_cm(s, s->func_csf_cm, &g, s->csf_a, s->csf_f, pstr);
    if (err == 0)
        err = fadm_launch_csf(s->func_csf_r, &g, s->csf_a_aim, s->csf_f_aim, pstr);
    if (err == 0)
        err = fadm_launch_cm(s, s->func_aim_cm, &g, s->csf_a_aim, s->csf_f_aim, pstr);
    return err;
}

static int fadm_hip_launch(FloatAdmStateHip *s, uintptr_t pic_stream_handle)
{
    hipStream_t pstr = vmaf_hip_stream_of(pic_stream_handle);
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    hipEvent_t submit_ev = vmaf_hip_event_of(s->lc.submit);

    /* Zero all accumulator buffers — stage 3 writes only 2 out of 6
     * slots per WG; the others must be zero for the host reduction. */
    hipError_t rc = hipSuccess;
    for (int scale = 0; scale < FADM_NUM_SCALES && rc == hipSuccess; scale++) {
        rc = hipMemsetAsync(s->accum[scale], 0,
                            (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float), pstr);
    }
    if (rc != hipSuccess)
        return fadm_hip_rc(rc);

    int err = 0;
    for (int scale = 0; scale < FADM_NUM_SCALES && err == 0; scale++)
        err = fadm_hip_launch_scale(s, scale, pstr);
    if (err != 0)
        return err;

    /* Event fence → secondary stream → D2H copy partials. */
    rc = hipEventRecord(submit_ev, pstr);
    if (rc == hipSuccess)
        rc = hipStreamWaitEvent(str, submit_ev, 0);
    for (int scale = 0; scale < FADM_NUM_SCALES && rc == hipSuccess; scale++) {
        const size_t n_bytes = (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float);
        rc = hipMemcpyAsync(s->accum_host[scale], s->accum[scale], n_bytes, hipMemcpyDeviceToHost,
                            str);
    }
    if (rc != hipSuccess)
        return fadm_hip_rc(rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}

/* Allocate every device buffer and the pinned accumulator readbacks. On
 * failure the buffers already allocated stay set; fadm_hip_release() frees
 * them through fadm_hip_bufs_free(). */
static int fadm_hip_bufs_alloc(FloatAdmStateHip *s)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const size_t raw_bytes = (size_t)s->width * s->height * bpp;
    const size_t dwt_bytes = (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(float);
    const size_t csf_bytes =
        (size_t)FADM_NUM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(float);

    void **flat[] = {&s->src_ref, &s->src_dis, &s->dwt_tmp_ref, &s->dwt_tmp_dis,
                     &s->csf_a,   &s->csf_f,   &s->csf_a_aim,   &s->csf_f_aim};
    const size_t flat_bytes[] = {raw_bytes, raw_bytes, dwt_bytes, dwt_bytes,
                                 csf_bytes, csf_bytes, csf_bytes, csf_bytes};
    hipError_t rc = hipSuccess;
    for (unsigned i = 0; i < 8u && rc == hipSuccess; i++)
        rc = hipMalloc(flat[i], flat_bytes[i]);

    for (int scale = 0; scale < FADM_NUM_SCALES && rc == hipSuccess; scale++) {
        const size_t band_bytes =
            (size_t)4u * s->buf_stride * s->scale_half_h[scale] * sizeof(float);
        const size_t accum_bytes = (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float);
        rc = hipMalloc(&s->ref_band[scale], band_bytes);
        if (rc == hipSuccess)
            rc = hipMalloc(&s->dis_band[scale], band_bytes);
        if (rc == hipSuccess)
            rc = hipMalloc(&s->accum[scale], accum_bytes);
        if (rc == hipSuccess) {
            rc = hipHostMalloc((void **)&s->accum_host[scale], accum_bytes, hipHostMallocDefault);
        }
    }
    return (rc == hipSuccess) ? 0 : -ENOMEM;
}

/* Free every buffer fadm_hip_bufs_alloc() may have allocated and unload the
 * module. Safe on a partially set up state. Returns -EIO when the module
 * fails to unload; freeing the buffers is best-effort. */
static int fadm_hip_bufs_free(FloatAdmStateHip *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        if (s->accum_host[scale] != NULL)
            (void)hipHostFree(s->accum_host[scale]);
        s->accum_host[scale] = NULL;
        void **per_scale[] = {&s->accum[scale], &s->dis_band[scale], &s->ref_band[scale]};
        for (unsigned i = 0; i < 3u; i++) {
            if (*per_scale[i] != NULL)
                (void)hipFree(*per_scale[i]);
            *per_scale[i] = NULL;
        }
    }
    void **flat[] = {&s->csf_f_aim,   &s->csf_a_aim,   &s->csf_f,   &s->csf_a,
                     &s->dwt_tmp_dis, &s->dwt_tmp_ref, &s->src_dis, &s->src_ref};
    for (unsigned i = 0; i < 8u; i++) {
        if (*flat[i] != NULL)
            (void)hipFree(*flat[i]);
        *flat[i] = NULL;
    }
    int rc = 0;
    if (s->module != NULL) {
        if (hipModuleUnload(s->module) != hipSuccess)
            rc = -EIO;
        s->module = NULL;
    }
    return rc;
}
#endif /* HAVE_HIPCC */

/* rfactor = 1 / dwt_quant_step per scale and band (h, v, d). */
static void fadm_hip_init_rfactor(FloatAdmStateHip *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float f1 =
            fadm_dwt_quant_step(scale, 1, s->adm_norm_view_dist, s->adm_ref_display_height);
        const float f2 =
            fadm_dwt_quant_step(scale, 2, s->adm_norm_view_dist, s->adm_ref_display_height);
        /* ADR-1214: match the CPU reference exactly. In the Watson-97 mode this
         * twin supports (adm_csf_mode == 0) `adm_tools.c::adm_csf_rfactor_s`
         * sets rfactor = 1 / dwt_quant_step(...) and does NOT consult
         * adm_csf_scale / adm_csf_diag_scale — those two options only enter the
         * Barten branch (mode 1). Multiplying them in here made a non-default
         * scale change the GPU score while the CPU ignored it, and the comment
         * that used to sit here claimed the opposite of what adm_tools.c does. */
        s->rfactor[scale * 3 + 0] = 1.0f / f1;
        s->rfactor[scale * 3 + 1] = 1.0f / f1;
        s->rfactor[scale * 3 + 2] = 1.0f / f2;
    }
}

/* One work group per band and active row of each scale's CM stages. */
static void fadm_hip_init_wg_counts(FloatAdmStateHip *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        int top = 0;
        int bottom = 0;
        fadm_active_range((int)s->scale_half_h[scale], &top, &bottom);
        const unsigned num_rows = (bottom > top) ? (unsigned)(bottom - top) : 1u;
        s->wg_count[scale] = 3u * num_rows;
    }
}

/* Tear down everything init() may have set up. Every step tolerates a handle
 * that was never created, so this serves both a failed init() and close().
 * The stream is drained first, so no kernel still uses a buffer. Returns the
 * first error. */
static int fadm_hip_release(FloatAdmStateHip *s)
{
    int rc = vmaf_hip_kernel_lifecycle_close(&s->lc, s->ctx);
#ifdef HAVE_HIPCC
    const int e = fadm_hip_bufs_free(s);
    if (e != 0 && rc == 0)
        rc = e;
#endif /* HAVE_HIPCC */
    if (s->feature_name_dict != NULL) {
        const int err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0)
            rc = err;
    }
    vmaf_hip_context_destroy(s->ctx);
    s->ctx = NULL;
    return rc;
}

static int init_fex_hip(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                        unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatAdmStateHip *s = fex->priv;

    if (s->adm_csf_mode != 0)
        return -EINVAL;

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);
    fadm_hip_init_rfactor(s);
    fadm_hip_init_wg_counts(s);

    int err = vmaf_hip_context_new(&s->ctx, 0);
    if (err == 0)
        err = vmaf_hip_kernel_lifecycle_init(&s->lc, s->ctx);
#ifdef HAVE_HIPCC
    if (err == 0)
        err = fadm_hip_module_load(s);
    if (err == 0)
        err = fadm_hip_bufs_alloc(s);
#endif /* HAVE_HIPCC */
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == NULL)
            err = -ENOMEM;
    }
    if (err != 0)
        (void)fadm_hip_release(s);
    return err;
}

static int submit_fex_hip(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                          VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    FloatAdmStateHip *s = fex->priv;

#ifdef HAVE_HIPCC
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    const ptrdiff_t raw_stride = (ptrdiff_t)(s->width * bpp);
    const uintptr_t pic_stream_handle = 0;

    /* Returns once both pictures are read: the caller recycles them when
     * submit() returns (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). */
    const VmafHipPlaneUpload planes[] = {
        {.dst = s->src_ref,
         .dst_pitch = (size_t)raw_stride,
         .pic = ref_pic,
         .plane = 0u,
         .row_bytes = (size_t)raw_stride,
         .rows = s->height},
        {.dst = s->src_dis,
         .dst_pitch = (size_t)raw_stride,
         .pic = dist_pic,
         .plane = 0u,
         .row_bytes = (size_t)raw_stride,
         .rows = s->height},
    };
    /* Upload on the private stream, not the null stream the kernels use: a
     * null-stream copy would queue behind every other extractor's kernels of
     * this frame, and the wait would block the host on all of them. The
     * copies are complete before the kernels are enqueued, and collect() of
     * the previous frame has already drained the kernels that read these
     * buffers. */
    const int err = vmaf_hip_picture_upload(planes, 2u, s->lc.str);
    if (err != 0)
        return err;

    return fadm_hip_launch(s, pic_stream_handle);
#else
    (void)dist_pic;
    (void)ref_pic;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

#ifdef HAVE_HIPCC
/* Per-scale, per-band sums of the work-group partials. */
typedef struct FadmTotals {
    double cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double csf[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double aim_cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
} FadmTotals;

/* Pooled numerators / denominators: `scores` is {num, den} per scale. */
typedef struct FadmPooled {
    double scores[2 * FADM_NUM_SCALES];
    double score_num;
    double score_den;
    double aim_num;
    double aim_den;
} FadmPooled;

/* Per-scale double accumulation across WGs.
 * Slots 0..2: csf_den per band; 3..5: cm_num per band;
 * 6..8: aim_cm per band (ADR-0574). */
static void fadm_hip_reduce(const FloatAdmStateHip *s, FadmTotals *t)
{
    memset(t, 0, sizeof(*t));
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float *slots = s->accum_host[scale];
        const unsigned wg_count = s->wg_count[scale];
        for (unsigned wg = 0u; wg < wg_count; wg++) {
            const float *p = slots + (size_t)wg * FADM_ACCUM_SLOTS;
            for (int b = 0; b < FADM_NUM_BANDS; b++) {
                t->csf[scale][b] += (double)p[b];
                t->cm[scale][b] += (double)p[3 + b];
                t->aim_cm[scale][b] += (double)p[6 + b];
            }
        }
    }
}

/* Pool one scale into `p`. The band sums are pooled in float, as
 * adm_tools.c pools them. */
static void fadm_hip_pool_scale(const FloatAdmStateHip *s, const FadmTotals *t, int scale,
                                FadmPooled *p)
{
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
    fadm_active_range((int)s->scale_half_w[scale], &left, &right);
    fadm_active_range((int)s->scale_half_h[scale], &top, &bottom);
    /* The pooling root and the noise constant are 1/adm_p_norm, not a
     * hardcoded 1/3: adm_tools.c uses powf(accum, 1.0f / adm_p_norm) and
     * get_noise_constant(..., adm_p_norm). ADR-1220. */
    const float inv_p = 1.0f / (float)s->adm_p_norm;
    const float area_cbrt =
        powf((float)((bottom - top) * (right - left)) * (float)s->adm_noise_weight, inv_p);
    float num_scale = 0.0f;
    float den_scale = 0.0f;
    /* ADR-0574: AIM accumulation — same CSF denominator as adm2 (den_scale). */
    float aim_num_scale = 0.0f;
    for (int b = 0; b < FADM_NUM_BANDS; b++) {
        num_scale += powf((float)t->cm[scale][b], inv_p) + area_cbrt;
        den_scale += powf((float)t->csf[scale][b], inv_p) + area_cbrt;
        aim_num_scale += powf((float)t->aim_cm[scale][b], inv_p);
    }
    p->scores[2 * scale + 0] = num_scale;
    p->scores[2 * scale + 1] = den_scale;
    p->score_num += num_scale;
    p->score_den += den_scale;
    p->aim_den += den_scale;
    p->aim_num += aim_num_scale;
}

/* Debug-mode outputs: the aggregate adm and every per-scale num / den. */
static int fadm_hip_emit_debug(FloatAdmStateHip *s, VmafFeatureCollector *fc, const FadmPooled *p,
                               double score, unsigned index)
{
    int err =
        vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm", score, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_num",
                                                   p->score_num, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_den",
                                                   p->score_den, index);
    const char *names[8] = {"adm_num_scale0", "adm_den_scale0", "adm_num_scale1", "adm_den_scale1",
                            "adm_num_scale2", "adm_den_scale2", "adm_num_scale3", "adm_den_scale3"};
    for (int i = 0; i < 8 && !err; i++) {
        err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, names[i],
                                                       p->scores[i], index);
    }
    return err;
}

/* adm2, the per-scale scores, AIM and ADM3 (ADR-0574) from the pooled sums. */
static int fadm_hip_emit(FloatAdmStateHip *s, VmafFeatureCollector *fc, FadmPooled *p,
                         unsigned index)
{
    const int w = (int)s->scale_w[0];
    const int h = (int)s->scale_h[0];
    const double numden_limit = 1e-2 * (double)(w * h) / (1920.0 * 1080.0);
    if (p->score_num < numden_limit)
        p->score_num = 0.0;
    if (p->score_den < numden_limit)
        p->score_den = 0.0;
    const double score = (p->score_den == 0.0) ? 1.0 : p->score_num / p->score_den;

    const double score_aim = (p->aim_den == 0.0) ? 1.0 : fmin(p->aim_num / p->aim_den, 1.0);
    double score_adm3;
    if (s->adm_adm3_apply_hm) {
        const double hm_denom = score + score_aim;
        score_adm3 = (hm_denom > 0.0) ? (2.0 * score * score_aim / hm_denom) : 0.0;
    } else {
        score_adm3 = score * s->adm_dlm_weight + (1.0 - score_aim) * (1.0 - s->adm_dlm_weight);
    }
    if (score_adm3 < s->adm_min_val)
        score_adm3 = s->adm_min_val;

    int err = vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                      "VMAF_feature_adm2_score", score, index);
    const char *scale_names[FADM_NUM_SCALES] = {
        "VMAF_feature_adm_scale0_score", "VMAF_feature_adm_scale1_score",
        "VMAF_feature_adm_scale2_score", "VMAF_feature_adm_scale3_score"};
    for (size_t scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const size_t num_idx = scale * 2u;
        err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, scale_names[scale],
                                                       p->scores[num_idx] / p->scores[num_idx + 1u],
                                                       index);
    }
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_aim_score", score_aim, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_adm3_score", score_adm3, index);
    if (s->debug && !err)
        err |= fadm_hip_emit_debug(s, fc, p, score, index);
    return err;
}
#endif /* HAVE_HIPCC */

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    FloatAdmStateHip *s = fex->priv;

    int sync_err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (sync_err != 0)
        return sync_err;

#ifdef HAVE_HIPCC
    FadmTotals totals;
    fadm_hip_reduce(s, &totals);

    FadmPooled pooled = {0};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++)
        fadm_hip_pool_scale(s, &totals, scale, &pooled);

    return fadm_hip_emit(s, fc, &pooled, index);
#else
    (void)fc;
    (void)index;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

static int close_fex_hip(VmafFeatureExtractor *fex)
{
    return fadm_hip_release(fex->priv);
}

static const char *provided_features[] = {"VMAF_feature_adm2_score",
                                          "VMAF_feature_adm_scale0_score",
                                          "VMAF_feature_adm_scale1_score",
                                          "VMAF_feature_adm_scale2_score",
                                          "VMAF_feature_adm_scale3_score",
                                          "VMAF_feature_aim_score",
                                          "VMAF_feature_adm3_score",
                                          "adm",
                                          "adm_num",
                                          "adm_den",
                                          "adm_num_scale0",
                                          "adm_den_scale0",
                                          "adm_num_scale1",
                                          "adm_den_scale1",
                                          "adm_num_scale2",
                                          "adm_den_scale2",
                                          "adm_num_scale3",
                                          "adm_den_scale3",
                                          NULL};

/* Load-bearing: registered via `extern VmafFeatureExtractor vmaf_fex_float_adm_hip;`
 * in `core/src/feature/feature_extractor.cpp`'s `feature_extractor_list[]`.
 * Ninth HIP kernel-template consumer (ADR-0468). Same pattern as
 * every CUDA / SYCL / Vulkan / HIP feature extractor. */
// NOLINTNEXTLINE(misc-use-internal-linkage): ADR-0468 — registration symbol must have external linkage
VmafFeatureExtractor vmaf_fex_float_adm_hip = {
    .name = "float_adm_hip",
    .init = init_fex_hip,
    .submit = submit_fex_hip,
    .collect = collect_fex_hip,
    .close = close_fex_hip,
    .options = options,
    .priv_size = sizeof(FloatAdmStateHip),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 24, /* 6 stages × 4 scales (ADR-0574) */
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
