/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_adm feature kernel on the CUDA backend (T7-23 / batch 3
 *  part 6b — ADR-0192 / ADR-0202). CUDA twin of float_adm_vulkan
 *  (PR #154 / ADR-0199). Same four pipeline stages, same `-1` mirror
 *  form, same fused stage 3 with cross-band CM threshold.
 *
 *  ADR-0574: AIM (Anchored Impairment Metric) and ADM3 sub-features
 *  added. Two new kernel stages (2b and 3b) compute the AIM CM
 *  numerator using decouple_r CSF buffers. Host-side collect()
 *  derives aim_score and adm3_score from accumulator slots 6..8.
 *
 *  Per-frame flow: 24 launches (6 stages x 4 scales) + a pinned-host
 *  D2H copy of the per-scale partial buffers. Reduction across WGs
 *  happens on the host in double precision — same trick as the Vulkan
 *  host wrapper, matches CPU adm_csf_den_scale_s / adm_cm_s
 *  row-by-row order to keep the places=4 contract.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "common.h"
#include "feature/adm_options.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/adm_score.h"
#include "feature/nonfinite_score.h"

#include "cuda/float_adm_cuda.h"
#include "cuda/kernel_template.h"
#include "cuda_helper.cuh"
#include "picture.h"
#include "picture_cuda.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FADM_NUM_SCALES 4
#define FADM_NUM_BANDS 3
#define FADM_BX 16
#define FADM_BY 16
#define FADM_BORDER_FACTOR 0.1
/* ADR-0574: 9 slots per WG: [0..2]=csf_den, [3..5]=cm_num, [6..8]=aim_cm. */
#define FADM_ACCUM_SLOTS 9

#ifndef DEFAULT_ADM_MIN_VAL
#define DEFAULT_ADM_MIN_VAL 0.0
#endif

typedef struct {
    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;

    /* ADR-0574: AIM / ADM3 options — same defaults as float_adm.c. */
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

    float rfactor[12];

    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). Multi-stage DWT + CSF pipeline state stays outside
     * the template's single-pair readback bundle. */
    VmafCudaKernelLifecycle lc;
    CUfunction func_dwt_vert;
    CUfunction func_dwt_hori;
    CUfunction func_decouple_csf;
    CUfunction func_csf_cm;
    /* ADR-0574: AIM pass kernels. */
    CUfunction func_csf_r;
    CUfunction func_aim_cm;

    VmafCudaBuffer *src_ref;
    VmafCudaBuffer *src_dis;
    VmafCudaBuffer *dwt_tmp_ref;
    VmafCudaBuffer *dwt_tmp_dis;
    VmafCudaBuffer *ref_band[FADM_NUM_SCALES];
    VmafCudaBuffer *dis_band[FADM_NUM_SCALES];
    VmafCudaBuffer *csf_a;
    VmafCudaBuffer *csf_f;
    /* ADR-0574: CSF buffers for decouple_r (AIM pass). */
    VmafCudaBuffer *csf_a_aim;
    VmafCudaBuffer *csf_f_aim;
    VmafCudaBuffer *accum[FADM_NUM_SCALES];
    float *accum_host[FADM_NUM_SCALES];

    unsigned wg_count[FADM_NUM_SCALES];
    unsigned scale_w[FADM_NUM_SCALES];
    unsigned scale_h[FADM_NUM_SCALES];
    unsigned scale_half_w[FADM_NUM_SCALES];
    unsigned scale_half_h[FADM_NUM_SCALES];

    /* PTX module backing the DWT/CSF/AIM kernels — owned here so
     * `close_fex_cuda` can unload it. Skipping the unload leaks
     * ~200-500 KB of GPU-resident PTX backing store per vmaf_close(). */
    CUmodule module;

    VmafDictionary *feature_name_dict;
} FloatAdmStateCuda;

static const VmafOption options[] = {
    {.name = "debug",
     .help = "debug mode: enable additional output",
     .offset = offsetof(FloatAdmStateCuda, debug),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val.b = false},
    {.name = "adm_enhn_gain_limit",
     .alias = "egl",
     .help = "enhancement gain imposed on adm, must be >= 1.0",
     .offset = offsetof(FloatAdmStateCuda, adm_enhn_gain_limit),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 100.0,
     .min = 1.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_norm_view_dist",
     .alias = "nvd",
     .help = "normalized viewing distance",
     .offset = offsetof(FloatAdmStateCuda, adm_norm_view_dist),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 3.0,
     .min = 0.75,
     .max = 24.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_ref_display_height",
     .alias = "rdf",
     .help = "reference display height in pixels",
     .offset = offsetof(FloatAdmStateCuda, adm_ref_display_height),
     .type = VMAF_OPT_TYPE_INT,
     .default_val.i = 1080,
     .min = 1,
     .max = 4320,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_mode",
     .alias = "csf",
     .help = "contrast sensitivity function (mode 0 only on CUDA v1)",
     .offset = offsetof(FloatAdmStateCuda, adm_csf_mode),
     .type = VMAF_OPT_TYPE_INT,
     .default_val.i = 0,
     .min = 0,
     .max = 9,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_scale",
     .alias = "scf",
     .help = "CSF band-scale multiplier for h/v bands (default 1.0 = no scaling)",
     .offset = offsetof(FloatAdmStateCuda, adm_csf_scale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = DEFAULT_ADM_CSF_SCALE,
     .min = 0.0,
     .max = 50.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_diag_scale",
     .alias = "scfd",
     .help = "CSF band-scale multiplier for diagonal bands (default 1.0 = no scaling)",
     .offset = offsetof(FloatAdmStateCuda, adm_csf_diag_scale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = DEFAULT_ADM_CSF_DIAG_SCALE,
     .min = 0.0,
     .max = 50.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_noise_weight",
     .alias = "nw",
     .help = "noise floor weight for CM numerator (default 0.03125 = 1/32)",
     .offset = offsetof(FloatAdmStateCuda, adm_noise_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = DEFAULT_ADM_NOISE_WEIGHT,
     .min = 0.0,
     .max = 100.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    /* ADR-0574: AIM / ADM3 tuning params — identical defaults to float_adm.c. */
    {.name = "adm_bypass_cm",
     .alias = "bcm",
     .help = "bypass CM computation (0 = normal, 1 = bypass)",
     .offset = offsetof(FloatAdmStateCuda, adm_bypass_cm),
     .type = VMAF_OPT_TYPE_INT,
     .default_val.i = 0,
     .min = 0,
     .max = 1,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_adm3_apply_hm",
     .alias = "aah",
     .help = "apply harmonic mean for adm3 score (false = linear blend)",
     .offset = offsetof(FloatAdmStateCuda, adm_adm3_apply_hm),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val.b = false,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_p_norm",
     .alias = "apn",
     .help = "p-norm exponent for AIM/ADM3 score (default 3.0)",
     .offset = offsetof(FloatAdmStateCuda, adm_p_norm),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 3.0,
     .min = 1.0,
     .max = 20.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_dlm_weight",
     .alias = "dlmw",
     .help = "DLM weight for linear-blend adm3 score (default 0.5)",
     .offset = offsetof(FloatAdmStateCuda, adm_dlm_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = 0.5,
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_min_val",
     .alias = "min",
     .help = "minimum clamp for adm3 score (default 0.0)",
     .offset = offsetof(FloatAdmStateCuda, adm_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val.d = DEFAULT_ADM_MIN_VAL,
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_skip_aim_scale",
     .alias = "sasc",
     .help = "skip AIM accumulation at this scale index (-1 = no skip)",
     .offset = offsetof(FloatAdmStateCuda, adm_skip_aim_scale),
     .type = VMAF_OPT_TYPE_INT,
     .default_val.i = -1,
     .min = -1,
     .max = 3,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {0}};

/* DB2/CDF-9-7 wavelet noise model — matches dwt_7_9_YCbCr_threshold[0]
 * (Y-plane row) in adm_tools.h. */
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
    /* Bit-for-bit replica of dwt_quant_step in adm_tools.h. */
    const float r = (float)(view_dist * (double)display_h * M_PI / 180.0);
    const float temp = (float)log10(pow(2.0, (double)(lambda + 1)) * (double)fadm_dwt_f0_Y *
                                    (double)fadm_dwt_g_Y[theta] / (double)r);
    const float Q = (float)(2.0 * (double)fadm_dwt_a_Y *
                            pow(10.0, (double)fadm_dwt_k_Y * (double)temp * (double)temp) /
                            (double)fadm_dwt_basis_amp[lambda][theta]);
    return Q;
}

static void compute_per_scale_dims(FloatAdmStateCuda *s)
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
    /* buf_stride sized to scale-0 half_w0 so a single stride works at
     * every scale (parent's stride read at scale s+1 still aligns
     * because the host-side buffer was allocated with this stride). */
    s->buf_stride = (s->scale_half_w[0] + 3u) & ~3u;
}

/* ------------------------------------------------------------------ */
/* float_adm_init_unwind - the single teardown path for init_fex_cuda.
 *
 * HISS-01: lifted verbatim from the former `free_buffers` label. The same
 * resources are released in the same order on every exit path, and the
 * value returned is the one the label returned.
 */
static int float_adm_init_unwind(VmafFeatureExtractor *fex, FloatAdmStateCuda *s)
{
    if (s->src_ref) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->src_ref);
        free(s->src_ref);
    }
    if (s->src_dis) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->src_dis);
        free(s->src_dis);
    }
    if (s->dwt_tmp_ref) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->dwt_tmp_ref);
        free(s->dwt_tmp_ref);
    }
    if (s->dwt_tmp_dis) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->dwt_tmp_dis);
        free(s->dwt_tmp_dis);
    }
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        if (s->ref_band[scale]) {
            (void)vmaf_cuda_buffer_free(fex->cu_state, s->ref_band[scale]);
            free(s->ref_band[scale]);
        }
        if (s->dis_band[scale]) {
            (void)vmaf_cuda_buffer_free(fex->cu_state, s->dis_band[scale]);
            free(s->dis_band[scale]);
        }
    }
    if (s->csf_a) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->csf_a);
        free(s->csf_a);
    }
    if (s->csf_f) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->csf_f);
        free(s->csf_f);
    }
    if (s->csf_a_aim) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->csf_a_aim);
        free(s->csf_a_aim);
    }
    if (s->csf_f_aim) {
        (void)vmaf_cuda_buffer_free(fex->cu_state, s->csf_f_aim);
        free(s->csf_f_aim);
    }
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        if (s->accum[scale]) {
            (void)vmaf_cuda_buffer_free(fex->cu_state, s->accum[scale]);
            free(s->accum[scale]);
        }
        if (s->accum_host[scale])
            (void)vmaf_cuda_buffer_host_free(fex->cu_state, s->accum_host[scale]);
    }
    (void)vmaf_dictionary_free(&s->feature_name_dict);
    return -ENOMEM;
}

/* float_adm_init_rfactors - per-scale CSF rfactor table.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda so that function stays
 * inside the 60-LOC limit. The arithmetic and the order of the stores are
 * unchanged, so the rfactor table is bit-identical.
 */
static void float_adm_init_rfactors(FloatAdmStateCuda *s)
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

/* float_adm_load_kernels - module load plus every kernel handle lookup.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda. CHECK_CUDA_GOTO and the
 * two labels it targets move with it, so the context is still popped
 * exactly once on every exit path and the kernel lifecycle is closed in
 * the same order and with the same returned errno as before.
 */
static int float_adm_load_kernels(VmafFeatureExtractor *fex, FloatAdmStateCuda *s,
                                  CudaFunctions *cu_f)
{
    int _cuda_err = 0;
    int ctx_pushed = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(fex->cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuModuleLoadData(&s->module, float_adm_score_ptx), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_dwt_vert, s->module, "float_adm_dwt_vert"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_dwt_hori, s->module, "float_adm_dwt_hori"),
                    fail);
    CHECK_CUDA_GOTO(cu_f,
                    cuModuleGetFunction(&s->func_decouple_csf, s->module, "float_adm_decouple_csf"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_csf_cm, s->module, "float_adm_csf_cm"),
                    fail);
    /* ADR-0574: AIM pass kernel handles. */
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_csf_r, s->module, "float_adm_csf_r"), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_aim_cm, s->module, "float_adm_aim_cm"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    (void)vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    return _cuda_err;
}

/* float_adm_alloc_device_buffers - every device and pinned-host allocation.
 *
 * HISS-04: lifted verbatim out of init_fex_cuda. The allocations run in the
 * same order and the caller still unwinds through float_adm_init_unwind(),
 * so the set and order of resources released on the error path is unchanged.
 */
static int float_adm_alloc_device_buffers(VmafFeatureExtractor *fex, FloatAdmStateCuda *s,
                                          unsigned w, unsigned h, unsigned bpc)
{
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t raw_bytes = (size_t)w * h * bpp;
    int ret = 0;
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->src_ref, raw_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->src_dis, raw_bytes);

    /* DWT scratch sized at scale 0 (worst case). */
    const size_t dwt_bytes = (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(float);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dwt_tmp_ref, dwt_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dwt_tmp_dis, dwt_bytes);

    /* Per-scale band buffers — 4 bands x buf_stride x half_h. The
     * scale-(s+1) DWT vert kernel reads scale-s's LL band, so each
     * scale needs its own ref_band/dis_band buffer. */
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const size_t band_bytes =
            (size_t)4u * s->buf_stride * s->scale_half_h[scale] * sizeof(float);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_band[scale], band_bytes);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_band[scale], band_bytes);
    }

    /* csf_a + csf_f reused per-scale (sized to scale 0 worst case). */
    const size_t csf_bytes =
        (size_t)FADM_NUM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(float);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_a, csf_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_f, csf_bytes);
    /* ADR-0574: AIM pass CSF buffers — same size as csf_a / csf_f. */
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_a_aim, csf_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_f_aim, csf_bytes);

    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const int hh = (int)s->scale_half_h[scale];
        int top = (int)((double)hh * FADM_BORDER_FACTOR - 0.5);
        if (top < 0)
            top = 0;
        const int bottom = hh - top;
        const unsigned num_rows = (bottom > top) ? (unsigned)(bottom - top) : 1u;
        const unsigned wg_count = 3u * num_rows;
        s->wg_count[scale] = wg_count;
        const size_t accum_bytes = (size_t)wg_count * FADM_ACCUM_SLOTS * sizeof(float);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->accum[scale], accum_bytes);
        ret |=
            vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->accum_host[scale], accum_bytes);
    }
    return ret;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatAdmStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    if (s->adm_csf_mode != 0)
        return -EINVAL;

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);
    float_adm_init_rfactors(s);

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;

    err = float_adm_load_kernels(fex, s, cu_f);
    if (err)
        return err;

    if (float_adm_alloc_device_buffers(fex, s, w, h, bpc))
        return float_adm_init_unwind(fex, s);

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return float_adm_init_unwind(fex, s);
    return 0;
}

/* FloatAdmScalePass - the per-frame and per-scale constants that the six
 * kernel launches of submit_fex_cuda share.
 *
 * HISS-04: introduced only so the launch blocks could move into named
 * helpers and keep submit_fex_cuda under the 60-LOC limit. Every field
 * holds exactly the value the identically named local held before; nothing
 * is recomputed and no arithmetic expression was split across the helper
 * boundary, so every launch argument is bit-identical.
 */
typedef struct FloatAdmScalePass {
    CUstream stream;
    ptrdiff_t raw_stride;
    CUdeviceptr ref_raw;
    CUdeviceptr dis_raw;
    CUdeviceptr dwt_ref;
    CUdeviceptr dwt_dis;
    CUdeviceptr csf_a;
    CUdeviceptr csf_f;
    CUdeviceptr csf_a_aim;
    CUdeviceptr csf_f_aim;
    CUdeviceptr ref_band;
    CUdeviceptr dis_band;
    CUdeviceptr parent_ref_band;
    CUdeviceptr parent_dis_band;
    CUdeviceptr accum;
    float pnorm;
    float scaler;
    float pixel_offset;
    float rfactor_h;
    float rfactor_v;
    float rfactor_d;
    float gain_limit;
    unsigned bpc;
    int bypass_cm;
    int buf_stride;
    int scale;
    int cur_w;
    int cur_h;
    int half_w;
    int half_h;
    int parent_w;
    int parent_h;
    int parent_half_h;
    int parent_buf_stride;
    int left;
    int top;
    int right;
    int bottom;
    int active_h;
} FloatAdmScalePass;

/* fadm_init_pass - the per-frame half of FloatAdmScalePass.
 *
 * HISS-04: the statements are lifted verbatim from submit_fex_cuda's
 * prologue and run in the same order.
 */
static void fadm_init_pass(const FloatAdmStateCuda *s, FloatAdmScalePass *p, CUstream stream)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    p->stream = stream;
    p->raw_stride = (ptrdiff_t)(s->width * bpp);

    p->scaler = 1.0f;
    p->pixel_offset = -128.0f;
    if (s->bpc == 10u) {
        p->scaler = 4.0f;
    } else if (s->bpc == 12u) {
        p->scaler = 16.0f;
    } else if (s->bpc == 16u) {
        p->scaler = 256.0f;
    }
    p->bpc = s->bpc;

    p->ref_raw = (CUdeviceptr)s->src_ref->data;
    p->dis_raw = (CUdeviceptr)s->src_dis->data;
    p->dwt_ref = (CUdeviceptr)s->dwt_tmp_ref->data;
    p->dwt_dis = (CUdeviceptr)s->dwt_tmp_dis->data;
    p->csf_a = (CUdeviceptr)s->csf_a->data;
    p->csf_f = (CUdeviceptr)s->csf_f->data;
    p->csf_a_aim = (CUdeviceptr)s->csf_a_aim->data;
    p->csf_f_aim = (CUdeviceptr)s->csf_f_aim->data;
    p->buf_stride = (int)s->buf_stride;

    /* adm_p_norm and adm_bypass_cm are VMAF_OPT_FLAG_FEATURE_PARAM options the
     * twin advertises. Until ADR-1220 the kernels hardcoded p = 3 and always
     * subtracted the masking threshold, so `apn` moved only the AIM exponent
     * and `bcm` did nothing at all. */
    p->pnorm = (float)s->adm_p_norm;
    p->bypass_cm = s->adm_bypass_cm;
}

/* fadm_set_pass_scale - the per-scale half of FloatAdmScalePass.
 *
 * HISS-04: lifted verbatim from the head of submit_fex_cuda's scale loop.
 */
static void fadm_set_pass_scale(const FloatAdmStateCuda *s, FloatAdmScalePass *p, int scale)
{
    p->scale = scale;
    p->cur_w = (int)s->scale_w[scale];
    p->cur_h = (int)s->scale_h[scale];
    p->half_w = (int)s->scale_half_w[scale];
    p->half_h = (int)s->scale_half_h[scale];

    /* Parent LL band dimensions = scale_w/h[scale] (per
     * `compute_per_scale_dims`: scale_w[s] is the *input* dim at
     * scale s, which equals the parent's LL output dim).
     * Mirror reads in stage 0 must clamp against these, NOT
     * scale_w[scale-1] (full parent image dims).  This matches
     * the Vulkan kernel's `read_band_a_at`, which uses
     * `pc.cur_w/cur_h` = the same thing. */
    p->parent_w = (scale > 0) ? (int)s->scale_w[scale] : 0;
    p->parent_h = (scale > 0) ? (int)s->scale_h[scale] : 0;
    p->parent_half_h = (scale > 0) ? (int)s->scale_half_h[scale - 1] : 0;
    p->parent_buf_stride = (int)s->buf_stride;

    p->ref_band = (CUdeviceptr)s->ref_band[scale]->data;
    p->dis_band = (CUdeviceptr)s->dis_band[scale]->data;
    p->parent_ref_band = (scale > 0) ? (CUdeviceptr)s->ref_band[scale - 1]->data : (CUdeviceptr)0;
    p->parent_dis_band = (scale > 0) ? (CUdeviceptr)s->dis_band[scale - 1]->data : (CUdeviceptr)0;
    p->accum = (CUdeviceptr)s->accum[scale]->data;

    int top = (int)((double)p->half_h * FADM_BORDER_FACTOR - 0.5);
    int left = (int)((double)p->half_w * FADM_BORDER_FACTOR - 0.5);
    if (top < 0)
        top = 0;
    if (left < 0)
        left = 0;
    p->top = top;
    p->left = left;
    p->bottom = p->half_h - top;
    p->right = p->half_w - left;
    p->active_h = p->bottom - top;

    p->rfactor_h = s->rfactor[scale * 3 + 0];
    p->rfactor_v = s->rfactor[scale * 3 + 1];
    p->rfactor_d = s->rfactor[scale * 3 + 2];
    p->gain_limit = (float)s->adm_enhn_gain_limit;
}

/* fadm_submit_upload - H2D staging plus the accumulator reset.
 *
 * HISS-04: lifted verbatim out of submit_fex_cuda. The two 2D copies and the
 * memsets are still issued on the picture stream in the same order.
 */
static int fadm_submit_upload(const FloatAdmStateCuda *s, CudaFunctions *cu_f,
                              const VmafPicture *ref_pic, const VmafPicture *dist_pic,
                              const FloatAdmScalePass *p)
{
    CUDA_MEMCPY2D cpy = {0};
    cpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.srcDevice = (CUdeviceptr)ref_pic->data[0];
    cpy.srcPitch = ref_pic->stride[0];
    cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cpy.dstDevice = (CUdeviceptr)s->src_ref->data;
    cpy.dstPitch = p->raw_stride;
    cpy.WidthInBytes = p->raw_stride;
    cpy.Height = s->height;
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&cpy, p->stream));

    CUDA_MEMCPY2D cpy_d = cpy;
    cpy_d.srcDevice = (CUdeviceptr)dist_pic->data[0];
    cpy_d.srcPitch = dist_pic->stride[0];
    cpy_d.dstDevice = (CUdeviceptr)s->src_dis->data;
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&cpy_d, p->stream));

    /* Reset accumulator buffers — stage 3 writes slots 0..5 per WG;
     * stage 3b writes slots 6..8. All slots start zero so that
     * skipped-scale AIM entries contribute zero to the host sum. */
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        CHECK_CUDA_RETURN(
            cu_f, cuMemsetD8Async(s->accum[scale]->data, 0,
                                  (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float),
                                  p->stream));
    }
    return 0;
}

/* fadm_launch_dwt_vert - stage 0, DWT vertical (z=2 fused ref+dis). */
static int fadm_launch_dwt_vert(CudaFunctions *cu_f, const FloatAdmStateCuda *s,
                                const FloatAdmScalePass *p)
{
    const unsigned gx = ((unsigned)p->cur_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)p->half_h + FADM_BY - 1u) / FADM_BY;
    int scale_arg = p->scale;
    int half_h_arg = p->half_h;
    int parent_half_h_arg = p->parent_half_h;
    int parent_buf_stride_arg = p->parent_buf_stride;
    int parent_w_arg = p->parent_w;
    int parent_h_arg = p->parent_h;
    int cur_w_arg = p->cur_w;
    int cur_h_arg = p->cur_h;
    unsigned bpc_arg = p->bpc;
    float scaler_arg = p->scaler;
    float pixel_offset_arg = p->pixel_offset;
    ptrdiff_t raw_stride = p->raw_stride;
    CUdeviceptr ref_raw_d = p->ref_raw;
    CUdeviceptr dis_raw_d = p->dis_raw;
    CUdeviceptr parent_ref_band_d = p->parent_ref_band;
    CUdeviceptr parent_dis_band_d = p->parent_dis_band;
    CUdeviceptr dwt_ref_d = p->dwt_ref;
    CUdeviceptr dwt_dis_d = p->dwt_dis;
    void *args[] = {&scale_arg,
                    &ref_raw_d,
                    &dis_raw_d,
                    (void *)&raw_stride,
                    &parent_ref_band_d,
                    &parent_dis_band_d,
                    &parent_buf_stride_arg,
                    &parent_half_h_arg,
                    &parent_w_arg,
                    &parent_h_arg,
                    &dwt_ref_d,
                    &dwt_dis_d,
                    &cur_w_arg,
                    &cur_h_arg,
                    &half_h_arg,
                    &bpc_arg,
                    &scaler_arg,
                    &pixel_offset_arg};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_vert, gx, gy, 2, FADM_BX, FADM_BY, 1, 0,
                                           p->stream, args, NULL));
    return 0;
}

/* fadm_launch_dwt_hori - stage 1, DWT horizontal. */
static int fadm_launch_dwt_hori(CudaFunctions *cu_f, const FloatAdmStateCuda *s,
                                const FloatAdmScalePass *p)
{
    const unsigned gx = ((unsigned)p->half_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)p->half_h + FADM_BY - 1u) / FADM_BY;
    int scale_arg = p->scale;
    int cur_w_arg = p->cur_w;
    int half_w_arg = p->half_w;
    int half_h_arg = p->half_h;
    int buf_stride_arg = p->buf_stride;
    CUdeviceptr dwt_ref_d = p->dwt_ref;
    CUdeviceptr dwt_dis_d = p->dwt_dis;
    CUdeviceptr ref_band_d = p->ref_band;
    CUdeviceptr dis_band_d = p->dis_band;
    void *args[] = {&scale_arg, &dwt_ref_d,  &dwt_dis_d,  &ref_band_d,    &dis_band_d,
                    &cur_w_arg, &half_w_arg, &half_h_arg, &buf_stride_arg};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_hori, gx, gy, 2, FADM_BX, FADM_BY, 1, 0,
                                           p->stream, args, NULL));
    return 0;
}

/* fadm_launch_csf - stages 2 and 2b, decouple + CSF.
 *
 * HISS-04: the two stages differed only in the kernel handle and in which
 * pair of CSF buffers they wrote, so both now share this body. The grid,
 * the block shape and the argument order are the ones both stages used.
 */
static int fadm_launch_csf(CudaFunctions *cu_f, CUfunction func, const FloatAdmScalePass *p,
                           CUdeviceptr csf_a, CUdeviceptr csf_f)
{
    const unsigned gx = ((unsigned)p->half_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)p->half_h + FADM_BY - 1u) / FADM_BY;
    int half_w_arg = p->half_w;
    int half_h_arg = p->half_h;
    int buf_stride_arg = p->buf_stride;
    float rfh = p->rfactor_h;
    float rfv = p->rfactor_v;
    float rfd = p->rfactor_d;
    float gl = p->gain_limit;
    CUdeviceptr ref_band_d = p->ref_band;
    CUdeviceptr dis_band_d = p->dis_band;
    CUdeviceptr csf_a_d = csf_a;
    CUdeviceptr csf_f_d = csf_f;
    void *args[] = {&ref_band_d,     &dis_band_d, &csf_a_d, &csf_f_d, &half_w_arg, &half_h_arg,
                    &buf_stride_arg, &rfh,        &rfv,     &rfd,     &gl};
    CHECK_CUDA_RETURN(
        cu_f, cuLaunchKernel(func, gx, gy, 1, FADM_BX, FADM_BY, 1, 0, p->stream, args, NULL));
    return 0;
}

/* fadm_launch_cm - stages 3 and 3b, CSF denominator + CM numerator.
 *
 * HISS-04: 1D dispatch over 3 bands x num_active_rows. Stage 3 writes accum
 * slots 0..5 and stage 3b slots 6..8; the two differed only in the kernel
 * handle and the CSF buffer pair, so both now share this body.
 */
static int fadm_launch_cm(CudaFunctions *cu_f, CUfunction func, const FloatAdmScalePass *p,
                          CUdeviceptr csf_a, CUdeviceptr csf_f)
{
    const unsigned num_rows = (unsigned)(p->active_h > 0 ? p->active_h : 1);
    const unsigned gx = 3u * num_rows;
    int half_w_arg = p->half_w;
    int half_h_arg = p->half_h;
    int buf_stride_arg = p->buf_stride;
    int active_left_arg = p->left;
    int active_top_arg = p->top;
    int active_right_arg = p->right;
    int active_bottom_arg = p->bottom;
    float rfh = p->rfactor_h;
    float rfv = p->rfactor_v;
    float rfd = p->rfactor_d;
    float gl = p->gain_limit;
    float pnorm_f = p->pnorm;
    int bypass_cm_arg = p->bypass_cm;
    CUdeviceptr ref_band_d = p->ref_band;
    CUdeviceptr dis_band_d = p->dis_band;
    CUdeviceptr csf_a_d = csf_a;
    CUdeviceptr csf_f_d = csf_f;
    CUdeviceptr accum_d = p->accum;
    void *args[] = {&ref_band_d,
                    &dis_band_d,
                    &csf_a_d,
                    &csf_f_d,
                    &accum_d,
                    &half_w_arg,
                    &half_h_arg,
                    &buf_stride_arg,
                    &active_left_arg,
                    &active_top_arg,
                    &active_right_arg,
                    &active_bottom_arg,
                    &rfh,
                    &rfv,
                    &rfd,
                    &gl,
                    &pnorm_f,
                    &bypass_cm_arg};
    CHECK_CUDA_RETURN(
        cu_f, cuLaunchKernel(func, gx, 1u, 1u, FADM_BX, FADM_BY, 1, 0, p->stream, args, NULL));
    return 0;
}

/* fadm_submit_scale - the six kernel launches one scale needs. */
static int fadm_submit_scale(CudaFunctions *cu_f, const FloatAdmStateCuda *s,
                             const FloatAdmScalePass *p)
{
    int err = fadm_launch_dwt_vert(cu_f, s, p);
    if (err)
        return err;
    err = fadm_launch_dwt_hori(cu_f, s, p);
    if (err)
        return err;
    err = fadm_launch_csf(cu_f, s->func_decouple_csf, p, p->csf_a, p->csf_f);
    if (err)
        return err;
    err = fadm_launch_cm(cu_f, s->func_csf_cm, p, p->csf_a, p->csf_f);
    if (err)
        return err;
    /* Stage 2b — CSF on decouple_r (AIM pass, ADR-0574). Writes csf_a_aim +
     * csf_f_aim for stage 3b. */
    err = fadm_launch_csf(cu_f, s->func_csf_r, p, p->csf_a_aim, p->csf_f_aim);
    if (err)
        return err;
    /* Stage 3b — AIM CM numerator (noise_weight=0, ADR-0574). Uses
     * csf_a_aim / csf_f_aim from stage 2b; skipped if
     * adm_skip_aim_scale == scale. */
    if (s->adm_skip_aim_scale == p->scale)
        return 0;
    return fadm_launch_cm(cu_f, s->func_aim_cm, p, p->csf_a_aim, p->csf_f_aim);
}

/* fadm_submit_download - sync to the secondary stream and copy the partials. */
static int fadm_submit_download(VmafFeatureExtractor *fex, FloatAdmStateCuda *s,
                                CudaFunctions *cu_f, CUstream pic_stream)
{
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        CHECK_CUDA_RETURN(
            cu_f, cuMemcpyDtoHAsync(s->accum_host[scale], (CUdeviceptr)s->accum[scale]->data,
                                    (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float),
                                    s->lc.str));
    }
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    FloatAdmStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;

    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);
    CHECK_CUDA_RETURN(cu_f,
                      cuStreamWaitEvent(pic_stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                        CU_EVENT_WAIT_DEFAULT));

    FloatAdmScalePass pass;
    fadm_init_pass(s, &pass, pic_stream);
    int err = fadm_submit_upload(s, cu_f, ref_pic, dist_pic, &pass);
    if (err)
        return err;

    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        fadm_set_pass_scale(s, &pass, scale);
        err = fadm_submit_scale(cu_f, s, &pass);
        if (err)
            return err;
    }

    /* Sync over to the secondary stream + D2H copy partials. */
    return fadm_submit_download(fex, s, cu_f, pic_stream);
}

/* FloatAdmBandTotals - the three per-scale, per-band accumulator sums that
 * collect_fex_cuda reduces out of the pinned-host partial buffers. */
typedef struct FloatAdmBandTotals {
    double cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double csf[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double aim_cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
} FloatAdmBandTotals;

/* FloatAdmPooled - what the per-scale pooling loop produces. */
typedef struct FloatAdmPooled {
    double scores[8];
    double score_num;
    double score_den;
    double aim_num;
    double aim_den;
} FloatAdmPooled;

/* FloatAdmFinal - the headline scores, plus the numerator and denominator
 * after the numden_limit clamp (the debug features report the clamped
 * values, so they have to survive the split). */
typedef struct FloatAdmFinal {
    double score;
    double aim;
    double adm3;
    double score_num;
    double score_den;
} FloatAdmFinal;

/* fadm_reduce_accum - per-scale double accumulation across WGs.
 *
 * HISS-04: lifted verbatim out of collect_fex_cuda. The traversal order is
 * scale -> workgroup -> band exactly as before, so the double sums are
 * bit-identical.
 */
static void fadm_reduce_accum(const FloatAdmStateCuda *s, FloatAdmBandTotals *t)
{
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

/* fadm_pool_scales - the per-scale pooling loop.
 *
 * HISS-04: lifted verbatim out of collect_fex_cuda. Every accumulation
 * statement is unchanged and stays whole inside this function, so no
 * expression crosses the helper boundary and FP contraction is unaffected.
 */
static void fadm_pool_scales(const FloatAdmStateCuda *s, const FloatAdmBandTotals *t,
                             FloatAdmPooled *o)
{
    o->score_num = 0.0;
    o->score_den = 0.0;
    o->aim_num = 0.0;
    o->aim_den = 0.0;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const int hw = (int)s->scale_half_w[scale];
        const int hh = (int)s->scale_half_h[scale];
        int left = (int)((double)hw * FADM_BORDER_FACTOR - 0.5);
        int top = (int)((double)hh * FADM_BORDER_FACTOR - 0.5);
        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        const int right = hw - left;
        const int bottom = hh - top;
        /* The pooling root and the noise constant are 1/adm_p_norm, not a
         * hardcoded 1/3: adm_tools.c uses powf(accum, 1.0f / adm_p_norm) and
         * get_noise_constant(..., adm_p_norm). ADR-1220. */
        const float inv_p = 1.0f / (float)s->adm_p_norm;
        const float area_cbrt =
            powf((float)((bottom - top) * (right - left)) * (float)s->adm_noise_weight, inv_p);
        float num_scale = 0.0f;
        float den_scale = 0.0f;
        for (int b = 0; b < FADM_NUM_BANDS; b++) {
            num_scale += powf((float)t->cm[scale][b], inv_p) + area_cbrt;
            den_scale += powf((float)t->csf[scale][b], inv_p) + area_cbrt;
        }
        o->scores[2 * scale + 0] = num_scale;
        o->scores[2 * scale + 1] = den_scale;
        o->score_num += num_scale;
        o->score_den += den_scale;

        /* ADR-0574: AIM accumulation — same CSF denominator as adm2
         * (den_scale). Skip this scale if adm_skip_aim_scale matches.
         * Slots 6..8 are zero for skipped scales (stage 3b was not
         * launched), so aim_num contribution is 0 naturally. */
        float aim_num_scale = 0.0f;
        for (int b = 0; b < FADM_NUM_BANDS; b++) {
            aim_num_scale += powf((float)t->aim_cm[scale][b], inv_p);
        }
        if (s->adm_skip_aim_scale != scale) {
            o->aim_den += den_scale;
            o->aim_num += aim_num_scale;
        }
    }
}

/* fadm_final_scores - numden clamp, adm2, AIM and ADM3.
 *
 * HISS-04: lifted verbatim out of collect_fex_cuda. score_num / score_den
 * are carried out clamped, which is the value the debug features have
 * always reported.
 */
static int fadm_final_scores(const FloatAdmStateCuda *s, const FloatAdmPooled *o, FloatAdmFinal *f,
                             unsigned index)
{
    f->score_num = o->score_num;
    f->score_den = o->score_den;
    /* numden_limit per ADM_OPT_SINGLE_PRECISION (matches adm.c L88). */
    const int w = (int)s->scale_w[0];
    const int h = (int)s->scale_h[0];
    const double numden_limit = 1e-2 * (double)(w * h) / (1920.0 * 1080.0);
    int err = vmaf_adm_floor_pair_named("float_adm_cuda", index, f->score_num, f->score_den,
                                        numden_limit, &f->score_num, &f->score_den);
    if (err)
        return err;
    err = vmaf_adm_finalize_scores_named("float_adm_cuda", index, f->score_num, f->score_den,
                                         o->aim_num, o->aim_den, &f->score, &f->aim);
    if (err)
        return err;

    return vmaf_adm3_score_named("float_adm_cuda", index, f->score, f->aim, s->adm_adm3_apply_hm,
                                 s->adm_dlm_weight, s->adm_min_val, &f->adm3);
}

/* Validate the complete score family before its first collector write. */
static int fadm_append_scores(VmafFeatureCollector *fc, const FloatAdmStateCuda *s,
                              const FloatAdmPooled *o, const FloatAdmFinal *f, unsigned index)
{
    double scale_scores[FADM_NUM_SCALES];
    int err = vmaf_adm_scale_ratios_named("float_adm_cuda", index, o->scores, FADM_NUM_SCALES,
                                          scale_scores);
    if (err)
        return err;

    VmafNamedScore values[18] = {
        {"VMAF_feature_adm2_score", f->score},
        {"VMAF_feature_adm_scale0_score", scale_scores[0]},
        {"VMAF_feature_adm_scale1_score", scale_scores[1]},
        {"VMAF_feature_adm_scale2_score", scale_scores[2]},
        {"VMAF_feature_adm_scale3_score", scale_scores[3]},
        {"VMAF_feature_aim_score", f->aim},
        {"VMAF_feature_adm3_score", f->adm3},
    };
    size_t value_count = 7u;
    if (s->debug) {
        static const char *const debug_names[8] = {
            "adm_num_scale0", "adm_den_scale0", "adm_num_scale1", "adm_den_scale1",
            "adm_num_scale2", "adm_den_scale2", "adm_num_scale3", "adm_den_scale3",
        };
        values[value_count++] = (VmafNamedScore){"adm", f->score};
        values[value_count++] = (VmafNamedScore){"adm_num", f->score_num};
        values[value_count++] = (VmafNamedScore){"adm_den", f->score_den};
        for (size_t i = 0u; i < 8u; ++i)
            values[value_count++] = (VmafNamedScore){debug_names[i], o->scores[i]};
    }
    return vmaf_feature_emit_finite_scores(fc, s->feature_name_dict, "float_adm_cuda", values,
                                           value_count, index);
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    FloatAdmStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
    /* Drain via the template helper so engine-scope fence batching
     * (T-GPU-OPT-1, ADR-0242) can short-circuit the per-stream
     * cuStreamSynchronize when the engine has already waited on
     * lc.finished as part of a batched drain. */
    int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err) {
        return sync_err;
    }

    /* Explicit barrier on the D2H stream (s->lc.str) after collect_wait.
     *
     * Race condition (reproduced on gfx1030 RDNA2, ~31% of frames):
     * The D2H copies of accum_host[] execute on s->lc.str.  The batch
     * drain (ADR-0242) waits on lc.finished (recorded on lc.str AFTER
     * the D2H), but the drain_stream synchronise does not block the
     * calling CPU thread until lc.str itself has retired the memcpy —
     * it only guarantees lc.finished has been signalled from the
     * driver's perspective.  On AMD GFX and some NVIDIA configs a
     * visible window exists between the event signal and the host
     * seeing the DMA data, especially when the next frame's
     * cuMemsetD8Async on pic_stream races with the D2H on lc.str for
     * the same device buffer.  An explicit cuStreamSynchronize on
     * lc.str is the conservative fix: it costs one per-frame CPU stall
     * (cheap vs. the ~24-kernel compute budget) and eliminates the
     * window entirely. */
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->lc.str));

    /* Per-scale double accumulation across WGs, mirroring the Vulkan
     * host wrapper's reduce_and_emit.
     * ADR-0574: aim_cm totals accumulate slots 6..8. */
    FloatAdmBandTotals totals = {0};
    fadm_reduce_accum(s, &totals);

    FloatAdmPooled pooled = {0};
    fadm_pool_scales(s, &totals, &pooled);

    FloatAdmFinal fin = {0};
    int err = fadm_final_scores(s, &pooled, &fin, index);
    if (err)
        return err;

    return fadm_append_scores(fc, s, &pooled, &fin, index);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    FloatAdmStateCuda *s = fex->priv;
    int ret = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (s->src_ref) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->src_ref);
        free(s->src_ref);
    }
    if (s->src_dis) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->src_dis);
        free(s->src_dis);
    }
    if (s->dwt_tmp_ref) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->dwt_tmp_ref);
        free(s->dwt_tmp_ref);
    }
    if (s->dwt_tmp_dis) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->dwt_tmp_dis);
        free(s->dwt_tmp_dis);
    }
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        if (s->ref_band[scale]) {
            ret |= vmaf_cuda_buffer_free(fex->cu_state, s->ref_band[scale]);
            free(s->ref_band[scale]);
        }
        if (s->dis_band[scale]) {
            ret |= vmaf_cuda_buffer_free(fex->cu_state, s->dis_band[scale]);
            free(s->dis_band[scale]);
        }
    }
    if (s->csf_a) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->csf_a);
        free(s->csf_a);
    }
    if (s->csf_f) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->csf_f);
        free(s->csf_f);
    }
    /* ADR-0574: free AIM pass CSF buffers. */
    if (s->csf_a_aim) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->csf_a_aim);
        free(s->csf_a_aim);
    }
    if (s->csf_f_aim) {
        ret |= vmaf_cuda_buffer_free(fex->cu_state, s->csf_f_aim);
        free(s->csf_f_aim);
    }
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        if (s->accum[scale]) {
            ret |= vmaf_cuda_buffer_free(fex->cu_state, s->accum[scale]);
            free(s->accum[scale]);
        }
        if (s->accum_host[scale])
            ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->accum_host[scale]);
    }
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    const CudaFunctions *cu_f = fex->cu_state->f;
    if (cu_f && s->module)
        (void)cu_f->cuModuleUnload(s->module);
    return ret;
}

static const char *provided_features[] = {
    "VMAF_feature_adm2_score", "VMAF_feature_adm_scale0_score", "VMAF_feature_adm_scale1_score",
    "VMAF_feature_adm_scale2_score", "VMAF_feature_adm_scale3_score",
    /* ADR-0574: AIM and ADM3 sub-features. */
    "VMAF_feature_aim_score", "VMAF_feature_adm3_score", "adm", "adm_num", "adm_den",
    "adm_num_scale0", "adm_den_scale0", "adm_num_scale1", "adm_den_scale1", "adm_num_scale2",
    "adm_den_scale2", "adm_num_scale3", "adm_den_scale3", NULL};

VmafFeatureExtractor vmaf_fex_float_adm_cuda = {
    .name = "float_adm_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(FloatAdmStateCuda),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
};

/* NOLINTEND(modernize-use-nullptr) */
