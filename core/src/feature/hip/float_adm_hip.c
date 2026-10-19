/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_adm feature extractor on the HIP backend — ninth consumer
 *  of `core/src/hip/kernel_template.h` (T7-10b batch-2 / ADR-0468; the CPU's
 *  arithmetic since ADR-1458).
 *
 *  This TU follows `core/src/feature/cuda/float_adm_cuda.c` (ADR-1420): five
 *  kernel launches per scale (DWT vertical, DWT horizontal, decouple + CSF,
 *  per-sample terms, per-row sums), one readback of nine fp32 row sums per
 *  row of the reduced region per scale, and the reference's own routines for
 *  everything the host concludes.
 *
 *  Numerical contract: float_adm_hip returns the CPU extractor's values bit
 *  for bit. The per-sample arithmetic is `feature/float_adm_gpu_common.h`,
 *  the header the CUDA twin runs: the reference's operations in the
 *  reference's types and order, with a division that is the IEEE fp32
 *  quotient on both sides (ADR-1442). The kernels store every term and add
 *  each row left to right in fp32; the host adds the rows top to bottom in
 *  fp32 (`fadm_fold_rows()`) and pools with `adm_pool_bands_s()`. The CSF
 *  weights, the reduced region and the angle threshold come from
 *  `adm_tools.c` (`feature/adm_float_reference.h`), not from copies here.
 *  `adm_p_norm` other than 3 raises each term with powf(), the device's on
 *  one side and the C library's on the other: that option is close to the
 *  CPU, not equal.
 *
 *  When `HAVE_HIPCC` is defined the kernels are built and run; without it
 *  the lifecycle helpers return -ENOSYS (scaffold posture).
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "dict.h"
#include "feature/adm_csf_fixed_point.h"
#include "feature/adm_float_reference.h"
#include "feature/adm_options.h"
#include "feature/adm_score.h"
#include "feature/float_adm_gpu_common.h"
#include "feature/nonfinite_score.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "libvmaf/picture.h"

#include "../../hip/common.h"
#include "../../hip/kernel_template.h"
#include "../../hip/picture_hip.h"
#include "../../hip/shared_frame.h"
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

#ifndef DEFAULT_ADM_MIN_VAL
#define DEFAULT_ADM_MIN_VAL 0.0
#endif

typedef struct FloatAdmStateHip {
    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    /* ADR-0574: AIM / ADM3 options, same defaults as float_adm.c. */
    int adm_bypass_cm;
    int adm_adm3_apply_hm;
    double adm_p_norm;
    double adm_dlm_weight;
    double adm_min_val;
    int adm_skip_aim_scale; /* -1 = no skip */

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned buf_stride;

    /* The reference's constants, from its own routines (ADR-1458). */
    float rfactor[FADM_SCALES][FADM_BANDS];
    AdmBorderS region[FADM_SCALES];
    float cos_1deg_sq;

    VmafHipKernelLifecycle lc;
    VmafHipContext *ctx;

#ifdef HAVE_HIPCC
    hipModule_t module;
    hipFunction_t func_dwt_vert;
    hipFunction_t func_dwt_hori;
    hipFunction_t func_decouple_csf;
    hipFunction_t func_terms;
    hipFunction_t func_row_sums;

    /* This frame's raw luma planes on the device: the context's shared frame,
     * or `planes`' own buffers when there is none (ADR-1408). */
    void *src_ref;
    void *src_dis;
    VmafHipPlaneSource planes;
    void *dwt_tmp_ref;
    void *dwt_tmp_dis;
    void *ref_band[FADM_SCALES];
    void *dis_band[FADM_SCALES];
    /* CSF of decouple_a and of decouple_r, each with its |.| / 30 companion. */
    void *csf_a;
    void *csf_fa;
    void *csf_r;
    void *csf_fr;
    /* Per-sample terms of the scale in flight, then the per-row sums of all
     * four scales (row_offset[] floats into `rows`). */
    void *terms;
    void *rows;
    float *rows_host;
#endif /* HAVE_HIPCC */

    size_t row_offset[FADM_SCALES];
    size_t row_floats;
    unsigned scale_w[FADM_SCALES];
    unsigned scale_h[FADM_SCALES];
    unsigned scale_half_w[FADM_SCALES];
    unsigned scale_half_h[FADM_SCALES];

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
     .default_val.i = 0, .min = 0, .max = 9,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM | VMAF_OPT_FLAG_DEFAULT_ONLY},
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
    {.name = "adm_bypass_cm", .alias = "bcm",
     .help = "bypass CM computation (0 = normal, 1 = bypass)",
     .offset = offsetof(FloatAdmStateHip, adm_bypass_cm), .type = VMAF_OPT_TYPE_INT,
     .default_val.i = 0, .min = 0, .max = 1, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
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
    {.name = "adm_skip_aim_scale", .alias = "sasc",
     .help = "skip AIM accumulation at this scale index (-1 = no skip)",
     .offset = offsetof(FloatAdmStateHip, adm_skip_aim_scale), .type = VMAF_OPT_TYPE_INT,
     .default_val.i = -1, .min = -1, .max = 3, .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {0}};
// clang-format on

/* Per-scale geometry, the reduced region of each scale and where each scale's
 * row sums start in the readback. */
static void compute_per_scale_dims(FloatAdmStateHip *s)
{
    unsigned cw = s->width;
    unsigned ch = s->height;
    s->row_floats = 0u;
    for (int scale = 0; scale < FADM_SCALES; scale++) {
        const unsigned hw = (cw + 1u) / 2u;
        const unsigned hh = (ch + 1u) / 2u;
        s->scale_w[scale] = cw;
        s->scale_h[scale] = ch;
        s->scale_half_w[scale] = hw;
        s->scale_half_h[scale] = hh;
        s->region[scale] = adm_border_s((int)hw, (int)hh, ADM_BORDER_FACTOR);
        s->row_offset[scale] = s->row_floats;
        s->row_floats +=
            (size_t)FADM_TERM_SLOTS * (size_t)(s->region[scale].bottom - s->region[scale].top);
        cw = hw;
        ch = hh;
    }
    /* One stride for every scale, sized for scale 0. */
    s->buf_stride = (s->scale_half_w[0] + 3u) & ~3u;
}

/* The constants the reference derives per frame, taken from its own routines
 * so they cannot drift from it (ADR-1458, as ADR-1420 did for CUDA).
 *
 * The CSF weights come from adm_csf_rfactor_s() with the options float_adm.c
 * passes: no per-scale override (this twin does not declare adm_f1sN /
 * adm_f2sN) and the reference's luminance level. In the Watson-97 mode this
 * twin supports the weights ignore adm_csf_scale / adm_csf_diag_scale, as on
 * the CPU (ADR-1214). */
static void fadm_hip_init_reference(FloatAdmStateHip *s)
{
    for (int scale = 0; scale < FADM_SCALES; scale++) {
        adm_csf_rfactor_s(scale, s->adm_norm_view_dist, s->adm_ref_display_height, s->adm_csf_mode,
                          DEFAULT_ADM_CSF_LUMINANCE_LEVEL, s->adm_csf_scale, s->adm_csf_diag_scale,
                          -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, s->rfactor[scale]);
    }
    s->cos_1deg_sq = adm_decouple_cos_1deg_sq_s();
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

/* Load the kernel blob and resolve the five kernels by name. On failure the
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
        {&s->func_terms, "float_adm_terms"},
        {&s->func_row_sums, "float_adm_row_sums"},
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

/* What the five stages of one scale share. The fields the DWT kernels take
 * are passed by address, so the struct is not const. */
typedef struct FadmScaleGeom {
    int scale;
    int cur_w;
    int cur_h;
    int half_w;
    int half_h;
    int buf_stride;
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

/* The band block every kernel past the DWT takes. */
static FloatAdmGpuBands fadm_hip_bands(const FloatAdmStateHip *s, const FadmScaleGeom *g)
{
    FloatAdmGpuBands b = {
        .ref_band = (uint64_t)(uintptr_t)g->ref_band,
        .dis_band = (uint64_t)(uintptr_t)g->dis_band,
        .csf_a = (uint64_t)(uintptr_t)s->csf_a,
        .csf_fa = (uint64_t)(uintptr_t)s->csf_fa,
        .csf_r = (uint64_t)(uintptr_t)s->csf_r,
        .csf_fr = (uint64_t)(uintptr_t)s->csf_fr,
        .half_w = g->half_w,
        .half_h = g->half_h,
        .buf_stride = g->buf_stride,
    };
    memcpy(b.rfactor, s->rfactor[g->scale], sizeof(b.rfactor));
    return b;
}

/* Stage 2 — decouple, then the CSF of both parts. */
static int fadm_launch_decouple(FloatAdmStateHip *s, const FadmScaleGeom *g, hipStream_t pstr)
{
    FloatAdmGpuDecoupleArgs args = {
        .bands = fadm_hip_bands(s, g),
        .adm_enhn_gain_limit = s->adm_enhn_gain_limit,
        .cos_1deg_sq = s->cos_1deg_sq,
        .pad_ = 0u,
    };
    void *params[] = {(void *)&args};
    const unsigned gx = ((unsigned)g->half_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)g->half_h + FADM_BY - 1u) / FADM_BY;
    return fadm_hip_rc(hipModuleLaunchKernel(s->func_decouple_csf, gx, gy, 1u, FADM_BX, FADM_BY, 1u,
                                             0u, pstr, params, NULL));
}

/* Stages 3 and 4 — the per-sample terms of the three reductions over the
 * reduced region, then one sum per row and slot into this scale's span of
 * `rows`. */
static int fadm_launch_reductions(FloatAdmStateHip *s, const FadmScaleGeom *g, hipStream_t pstr)
{
    const AdmBorderS *r = &s->region[g->scale];
    const unsigned region_w = (unsigned)(r->right - r->left);
    const unsigned region_h = (unsigned)(r->bottom - r->top);
    FloatAdmGpuTermArgs term_args = {
        .bands = fadm_hip_bands(s, g),
        .terms = (uint64_t)(uintptr_t)s->terms,
        .left = r->left,
        .top = r->top,
        .region_w = region_w,
        .region_h = region_h,
        .p_norm = (float)s->adm_p_norm,
        .is_cube = (s->adm_p_norm == 3.0) ? 1u : 0u,
        .bypass_cm = (s->adm_bypass_cm != 0) ? 1u : 0u,
    };
    void *term_params[] = {(void *)&term_args};
    const unsigned gx = (region_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = (region_h + FADM_BY - 1u) / FADM_BY;
    const int err = fadm_hip_rc(hipModuleLaunchKernel(s->func_terms, gx, gy, 1u, FADM_BX, FADM_BY,
                                                      1u, 0u, pstr, term_params, NULL));
    if (err != 0)
        return err;

    FloatAdmGpuRowArgs row_args = {
        .terms = (uint64_t)(uintptr_t)s->terms,
        .rows = (uint64_t)(uintptr_t)((float *)s->rows + s->row_offset[g->scale]),
        .region_w = region_w,
        .region_h = region_h,
    };
    void *row_params[] = {(void *)&row_args};
    const unsigned sums = FADM_TERM_SLOTS * region_h;
    const unsigned blocks = (sums + FADM_ROW_THREADS - 1u) / FADM_ROW_THREADS;
    return fadm_hip_rc(hipModuleLaunchKernel(s->func_row_sums, blocks, 1u, 1u, FADM_ROW_THREADS, 1u,
                                             1u, 0u, pstr, row_params, NULL));
}

/* The five stages of one scale, in the CUDA twin's order. */
static int fadm_hip_launch_scale(FloatAdmStateHip *s, int scale, hipStream_t pstr)
{
    FadmScaleGeom g;
    fadm_hip_scale_geom(s, scale, &g);

    int err = fadm_launch_dwt_vert(s, &g, pstr);
    if (err == 0)
        err = fadm_launch_dwt_hori(s, &g, pstr);
    if (err == 0)
        err = fadm_launch_decouple(s, &g, pstr);
    if (err == 0)
        err = fadm_launch_reductions(s, &g, pstr);
    return err;
}

static int fadm_hip_launch(FloatAdmStateHip *s, uintptr_t pic_stream_handle)
{
    hipStream_t pstr = vmaf_hip_stream_of(pic_stream_handle);
    hipStream_t str = vmaf_hip_stream_of(s->lc.str);
    hipEvent_t submit_ev = vmaf_hip_event_of(s->lc.submit);

    int err = 0;
    for (int scale = 0; scale < FADM_SCALES && err == 0; scale++)
        err = fadm_hip_launch_scale(s, scale, pstr);
    if (err != 0)
        return err;

    /* Event fence → secondary stream → D2H copy of the row sums. Every row
     * sum of every scale is written by its kernel, so nothing is cleared. */
    hipError_t rc = hipEventRecord(submit_ev, pstr);
    if (rc == hipSuccess)
        rc = hipStreamWaitEvent(str, submit_ev, 0);
    if (rc == hipSuccess) {
        rc = hipMemcpyAsync(s->rows_host, s->rows, s->row_floats * sizeof(float),
                            hipMemcpyDeviceToHost, str);
    }
    if (rc != hipSuccess)
        return fadm_hip_rc(rc);

    return vmaf_hip_kernel_submit_post_record(&s->lc, s->ctx);
}

/* Allocate every device buffer and the pinned readback of the row sums. On
 * failure the buffers already allocated stay set; fadm_hip_release() frees
 * them through fadm_hip_bufs_free(). The CSF buffers and the term buffer are
 * reused per scale and sized for scale 0, whose bands and reduced region are
 * the largest. */
static int fadm_hip_bufs_alloc(FloatAdmStateHip *s)
{
    const size_t dwt_bytes = (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(float);
    const size_t csf_bytes =
        (size_t)FADM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(float);
    const AdmBorderS *r0 = &s->region[0];
    const size_t term_bytes = (size_t)FADM_TERM_SLOTS * (size_t)(r0->right - r0->left) *
                              (size_t)(r0->bottom - r0->top) * sizeof(float);
    const size_t row_bytes = s->row_floats * sizeof(float);

    void **flat[] = {&s->dwt_tmp_ref, &s->dwt_tmp_dis, &s->csf_a, &s->csf_fa,
                     &s->csf_r,       &s->csf_fr,      &s->terms, &s->rows};
    const size_t flat_bytes[] = {dwt_bytes, dwt_bytes, csf_bytes,  csf_bytes,
                                 csf_bytes, csf_bytes, term_bytes, row_bytes};
    hipError_t rc = hipSuccess;
    for (unsigned i = 0; i < 8u && rc == hipSuccess; i++)
        rc = hipMalloc(flat[i], flat_bytes[i]);
    if (rc == hipSuccess)
        rc = hipHostMalloc((void **)&s->rows_host, row_bytes, hipHostMallocDefault);

    for (int scale = 0; scale < FADM_SCALES && rc == hipSuccess; scale++) {
        const size_t band_bytes =
            (size_t)4u * s->buf_stride * s->scale_half_h[scale] * sizeof(float);
        rc = hipMalloc(&s->ref_band[scale], band_bytes);
        if (rc == hipSuccess)
            rc = hipMalloc(&s->dis_band[scale], band_bytes);
    }
    return (rc == hipSuccess) ? 0 : -ENOMEM;
}

/* Free every buffer fadm_hip_bufs_alloc() may have allocated and unload the
 * module. Safe on a partially set up state. Returns -EIO when the module
 * fails to unload; freeing the buffers is best-effort. */
static int fadm_hip_bufs_free(FloatAdmStateHip *s)
{
    for (int scale = 0; scale < FADM_SCALES; scale++) {
        void **per_scale[] = {&s->dis_band[scale], &s->ref_band[scale]};
        for (unsigned i = 0; i < 2u; i++) {
            if (*per_scale[i] != NULL)
                (void)hipFree(*per_scale[i]);
            *per_scale[i] = NULL;
        }
    }
    if (s->rows_host != NULL)
        (void)hipHostFree(s->rows_host);
    s->rows_host = NULL;
    void **flat[] = {&s->rows,   &s->terms, &s->csf_fr,      &s->csf_r,
                     &s->csf_fa, &s->csf_a, &s->dwt_tmp_dis, &s->dwt_tmp_ref};
    for (unsigned i = 0; i < 8u; i++) {
        if (*flat[i] != NULL)
            (void)hipFree(*flat[i]);
        *flat[i] = NULL;
    }
    vmaf_hip_plane_source_close(&s->planes);
    s->src_dis = NULL;
    s->src_ref = NULL;
    int rc = 0;
    if (s->module != NULL) {
        if (hipModuleUnload(s->module) != hipSuccess)
            rc = -EIO;
        s->module = NULL;
    }
    return rc;
}
#endif /* HAVE_HIPCC */

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

    /* Same frame-size bound as the CPU float_adm, checked before any device
     * resource is claimed: below 17 pixels the scale-3 bands have one
     * sample. */
    const int size_err = adm_frame_size_check("float_adm_hip", w, h);
    if (size_err)
        return size_err;

    if (s->adm_csf_mode != 0)
        return -EINVAL;

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);
    fadm_hip_init_reference(s);

    int err = vmaf_hip_context_new(&s->ctx, fex->hip_device_index);
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

#ifdef HAVE_HIPCC
    FloatAdmStateHip *s = fex->priv;
    const uintptr_t pic_stream_handle = 0;

    /* The packed luma planes on the device. Returns once both pictures are
     * read: the caller recycles them when submit() returns
     * (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18). A plane no other twin
     * uploaded yet is uploaded on the private stream, never on the null
     * stream the kernels use: a null-stream copy would queue behind every
     * other extractor's kernels of this frame, and the wait would block the
     * host on all of them. The copies are complete before the kernels are
     * enqueued. */
    const int err = vmaf_hip_plane_source_acquire_luma(
        &s->planes, fex->hip_frame, ref_pic, dist_pic, s->lc.str, &s->src_ref, &s->src_dis);
    if (err != 0)
        return err;

    return fadm_hip_launch(s, pic_stream_handle);
#else
    (void)fex;
    (void)dist_pic;
    (void)ref_pic;
    return -ENOSYS;
#endif /* HAVE_HIPCC */
}

#ifdef HAVE_HIPCC
/* What the per-scale pooling loop produces: compute_adm()'s `scores`, `num`,
 * `den`, `aim_num` and `aim_den`. `scores` is {num, den} per scale. */
typedef struct FadmPooled {
    double scores[2 * FADM_SCALES];
    double score_num;
    double score_den;
    double aim_num;
    double aim_den;
} FadmPooled;

/* compute_adm()'s scale loop past the kernels.
 *
 * The frame accumulators are the reference's: one fp32 value per band that
 * the row sums are added to top to bottom. Each scale is then concluded by
 * the reference's own adm_pool_bands_s(), with the noise weight for the
 * denominator and the adm2 numerator and with none for the AIM numerator. */
static void fadm_hip_pool_scales(const FloatAdmStateHip *s, FadmPooled *o)
{
    for (int scale = 0; scale < FADM_SCALES; scale++) {
        const AdmBorderS *r = &s->region[scale];
        const int region_w = r->right - r->left;
        const int region_h = r->bottom - r->top;
        const float *rows = s->rows_host + s->row_offset[scale];
        float accum[FADM_TERM_SLOTS];
        for (unsigned slot = 0u; slot < FADM_TERM_SLOTS; slot++) {
            accum[slot] = fadm_fold_rows(rows + fadm_row_index(slot, 0u, (uint32_t)region_h),
                                         (uint32_t)region_h);
        }

        const float den_scale = adm_pool_bands_s(accum + FADM_SLOT_DEN, region_w, region_h,
                                                 s->adm_noise_weight, s->adm_p_norm);
        const float num_scale = adm_pool_bands_s(accum + FADM_SLOT_CM, region_w, region_h,
                                                 s->adm_noise_weight, s->adm_p_norm);
        const float aim_num_scale =
            adm_pool_bands_s(accum + FADM_SLOT_AIM, region_w, region_h, 0.0, s->adm_p_norm);

        o->score_num += num_scale;
        o->score_den += den_scale;
        if (s->adm_skip_aim_scale != scale) {
            o->aim_den += den_scale;
            o->aim_num += aim_num_scale;
        }
        o->scores[2 * scale + 0] = num_scale;
        o->scores[2 * scale + 1] = den_scale;
    }
}

/* adm2, the per-scale scores, AIM and ADM3 (ADR-0574) from the pooled sums,
 * as compute_adm() and float_adm.c's extract() conclude. The numerator and
 * denominator are floored at the reference's 1e-10; the debug features
 * report the floored values. */
static int fadm_hip_emit(FloatAdmStateHip *s, VmafFeatureCollector *fc, FadmPooled *p,
                         unsigned index)
{
    const int w = (int)s->width;
    const int h = (int)s->height;
    const double numden_limit = 1e-10 * (w * h) / (1920.0 * 1080.0);
    double score = 0.0;
    double score_aim = 0.0;
    int err = vmaf_adm_floor_pair_named("float_adm_hip", index, p->score_num, p->score_den,
                                        numden_limit, &p->score_num, &p->score_den);
    if (err)
        return err;
    err = vmaf_adm_finalize_scores_named("float_adm_hip", index, p->score_num, p->score_den,
                                         p->aim_num, p->aim_den, &score, &score_aim);
    if (err)
        return err;
    double score_adm3 = 0.0;
    err = vmaf_adm3_score_named("float_adm_hip", index, score, score_aim, s->adm_adm3_apply_hm,
                                s->adm_dlm_weight, s->adm_min_val, &score_adm3);
    if (err)
        return err;
    double scale_scores[FADM_SCALES];
    err = vmaf_adm_scale_ratios_named("float_adm_hip", index, p->scores, FADM_SCALES, scale_scores);
    if (err)
        return err;

    static const char *const scale_names[FADM_SCALES] = {
        "VMAF_feature_adm_scale0_score", "VMAF_feature_adm_scale1_score",
        "VMAF_feature_adm_scale2_score", "VMAF_feature_adm_scale3_score"};
    VmafNamedScore values[18] = {
        {"VMAF_feature_adm2_score", score},      {scale_names[0], scale_scores[0]},
        {scale_names[1], scale_scores[1]},       {scale_names[2], scale_scores[2]},
        {scale_names[3], scale_scores[3]},       {"VMAF_feature_aim_score", score_aim},
        {"VMAF_feature_adm3_score", score_adm3},
    };
    size_t value_count = 7u;
    if (s->debug) {
        static const char *const debug_names[8] = {
            "adm_num_scale0", "adm_den_scale0", "adm_num_scale1", "adm_den_scale1",
            "adm_num_scale2", "adm_den_scale2", "adm_num_scale3", "adm_den_scale3",
        };
        values[value_count++] = (VmafNamedScore){"adm", score};
        values[value_count++] = (VmafNamedScore){"adm_num", p->score_num};
        values[value_count++] = (VmafNamedScore){"adm_den", p->score_den};
        for (size_t i = 0u; i < 8u; ++i)
            values[value_count++] = (VmafNamedScore){debug_names[i], p->scores[i]};
    }
    return vmaf_feature_emit_finite_scores(fc, s->feature_name_dict, "float_adm_hip", values,
                                           value_count, index);
}
#endif /* HAVE_HIPCC */

static int collect_fex_hip(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    FloatAdmStateHip *s = fex->priv;

    int sync_err = vmaf_hip_kernel_collect_wait(&s->lc, s->ctx);
    if (sync_err != 0)
        return sync_err;

#ifdef HAVE_HIPCC
    FadmPooled pooled = {0};
    fadm_hip_pool_scales(s, &pooled);
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
                                          "adm_scale0",
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
 * every CUDA / SYCL / HIP feature extractor. */
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
    .unsuffixed_debug_key = "adm",
    .flags = VMAF_FEATURE_EXTRACTOR_HIP,
    .chars =
        {
            .n_dispatches_per_frame = 20, /* 5 stages × 4 scales (ADR-1458) */
            .is_reduction_only = false,
            .min_useful_frame_area = 1920U * 1080U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
