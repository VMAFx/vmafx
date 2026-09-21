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
#include "log.h"

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

#define FADM_OPTION(name_, alias_, help_, member_, type_, default_member_, default_, min_, max_,   \
                    flags_)                                                                        \
    {                                                                                              \
        .name = name_,                                                                             \
        .alias = alias_,                                                                           \
        .help = help_,                                                                             \
        .offset = offsetof(FloatAdmStateCuda, member_),                                            \
        .type = type_,                                                                             \
        .default_val.default_member_ = default_,                                                   \
        .min = min_,                                                                               \
        .max = max_,                                                                               \
        .flags = flags_,                                                                           \
    }

static const VmafOption options[] = {
    FADM_OPTION("debug", NULL, "debug mode: enable additional output", debug, VMAF_OPT_TYPE_BOOL, b,
                false, 0.0, 0.0, 0),
    FADM_OPTION("adm_enhn_gain_limit", "egl", "enhancement gain imposed on adm, must be >= 1.0",
                adm_enhn_gain_limit, VMAF_OPT_TYPE_DOUBLE, d, 100.0, 1.0, 100.0,
                VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_norm_view_dist", "nvd", "normalized viewing distance", adm_norm_view_dist,
                VMAF_OPT_TYPE_DOUBLE, d, 3.0, 0.75, 24.0, VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_ref_display_height", "rdf", "reference display height in pixels",
                adm_ref_display_height, VMAF_OPT_TYPE_INT, i, 1080, 1, 4320,
                VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_csf_mode", "csf", "contrast sensitivity function (mode 0 only on CUDA v1)",
                adm_csf_mode, VMAF_OPT_TYPE_INT, i, 0, 0, 9, VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_csf_scale", "scf",
                "CSF band-scale multiplier for h/v bands (default 1.0 = no scaling)", adm_csf_scale,
                VMAF_OPT_TYPE_DOUBLE, d, DEFAULT_ADM_CSF_SCALE, 0.0, 50.0,
                VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_csf_diag_scale", "scfd",
                "CSF band-scale multiplier for diagonal bands (default 1.0 = no scaling)",
                adm_csf_diag_scale, VMAF_OPT_TYPE_DOUBLE, d, DEFAULT_ADM_CSF_DIAG_SCALE, 0.0, 50.0,
                VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_noise_weight", "nw",
                "noise floor weight for CM numerator (default 0.03125 = 1/32)", adm_noise_weight,
                VMAF_OPT_TYPE_DOUBLE, d, DEFAULT_ADM_NOISE_WEIGHT, 0.0, 100.0,
                VMAF_OPT_FLAG_FEATURE_PARAM),
    /* ADR-0574: AIM / ADM3 tuning params — identical defaults to float_adm.c. */
    FADM_OPTION("adm_bypass_cm", "bcm", "bypass CM computation (0 = normal, 1 = bypass)",
                adm_bypass_cm, VMAF_OPT_TYPE_INT, i, 0, 0, 1, VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_adm3_apply_hm", "aah",
                "apply harmonic mean for adm3 score (false = linear blend)", adm_adm3_apply_hm,
                VMAF_OPT_TYPE_BOOL, b, false, 0.0, 0.0, VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_p_norm", "apn", "p-norm exponent for AIM/ADM3 score (default 3.0)", adm_p_norm,
                VMAF_OPT_TYPE_DOUBLE, d, 3.0, 1.0, 20.0, VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_dlm_weight", "dlmw", "DLM weight for linear-blend adm3 score (default 0.5)",
                adm_dlm_weight, VMAF_OPT_TYPE_DOUBLE, d, 0.5, 0.0, 1.0,
                VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_min_val", "min", "minimum clamp for adm3 score (default 0.0)", adm_min_val,
                VMAF_OPT_TYPE_DOUBLE, d, DEFAULT_ADM_MIN_VAL, 0.0, 1.0,
                VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION("adm_skip_aim_scale", "sasc",
                "skip AIM accumulation at this scale index (-1 = no skip)", adm_skip_aim_scale,
                VMAF_OPT_TYPE_INT, i, -1, -1, 3, VMAF_OPT_FLAG_FEATURE_PARAM),
    {0}};

#undef FADM_OPTION

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

static void compute_rfactor(FloatAdmStateCuda *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float f1 =
            fadm_dwt_quant_step(scale, 1, s->adm_norm_view_dist, s->adm_ref_display_height);
        const float f2 =
            fadm_dwt_quant_step(scale, 2, s->adm_norm_view_dist, s->adm_ref_display_height);
        /* ADR-1214: Watson-97 mode matches adm_tools.c exactly: these factors do
         * not consult the Barten-only adm_csf_scale options. */
        s->rfactor[scale * 3 + 0] = 1.0f / f1;
        s->rfactor[scale * 3 + 1] = 1.0f / f1;
        s->rfactor[scale * 3 + 2] = 1.0f / f2;
    }
}

static int load_fex_module(VmafFeatureExtractor *fex)
{
    FloatAdmStateCuda *s = fex->priv;
    CudaFunctions *cu_f = fex->cu_state->f;
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
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_csf_r, s->module, "float_adm_csf_r"), fail);
    CHECK_CUDA_GOTO(cu_f, cuModuleGetFunction(&s->func_aim_cm, s->module, "float_adm_aim_cm"),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed)
        (void)cu_f->cuCtxPopCurrent(NULL);
fail_after_pop:
    return _cuda_err;
}

static int alloc_fex_buffers(VmafFeatureExtractor *fex, unsigned bpc, unsigned w, unsigned h)
{
    FloatAdmStateCuda *s = fex->priv;
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t raw_bytes = (size_t)w * h * bpp;
    const size_t dwt_bytes = (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(float);
    const size_t csf_bytes =
        (size_t)FADM_NUM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(float);
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->src_ref, raw_bytes);

    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->src_dis, raw_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dwt_tmp_ref, dwt_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dwt_tmp_dis, dwt_bytes);
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const size_t band_bytes =
            (size_t)4u * s->buf_stride * s->scale_half_h[scale] * sizeof(float);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_band[scale], band_bytes);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_band[scale], band_bytes);
    }
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_a, csf_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_f, csf_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_a_aim, csf_bytes);
    ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_f_aim, csf_bytes);
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const int hh = (int)s->scale_half_h[scale];
        int top = (int)((double)hh * FADM_BORDER_FACTOR - 0.5);
        if (top < 0)
            top = 0;
        const int bottom = hh - top;
        const unsigned num_rows = (bottom > top) ? (unsigned)(bottom - top) : 1u;
        s->wg_count[scale] = 3u * num_rows;
        const size_t accum_bytes = (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->accum[scale], accum_bytes);
        ret |=
            vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->accum_host[scale], accum_bytes);
    }
    return ret ? -ENOMEM : 0;
}

static int release_cuda_buffer(VmafFeatureExtractor *fex, VmafCudaBuffer **buffer)
{
    if (!*buffer)
        return 0;
    const int ret = vmaf_cuda_buffer_free(fex->cu_state, *buffer);
    free(*buffer);
    *buffer = NULL;
    return ret;
}

static int release_fex_buffers(VmafFeatureExtractor *fex)
{
    FloatAdmStateCuda *s = fex->priv;
    int ret = release_cuda_buffer(fex, &s->src_ref);
    ret |= release_cuda_buffer(fex, &s->src_dis);
    ret |= release_cuda_buffer(fex, &s->dwt_tmp_ref);
    ret |= release_cuda_buffer(fex, &s->dwt_tmp_dis);
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        ret |= release_cuda_buffer(fex, &s->ref_band[scale]);
        ret |= release_cuda_buffer(fex, &s->dis_band[scale]);
    }
    ret |= release_cuda_buffer(fex, &s->csf_a);
    ret |= release_cuda_buffer(fex, &s->csf_f);
    ret |= release_cuda_buffer(fex, &s->csf_a_aim);
    ret |= release_cuda_buffer(fex, &s->csf_f_aim);
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        ret |= release_cuda_buffer(fex, &s->accum[scale]);
        if (s->accum_host[scale]) {
            ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->accum_host[scale]);
            s->accum_host[scale] = NULL;
        }
    }
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    return ret;
}

static int release_fex_resources(VmafFeatureExtractor *fex)
{
    FloatAdmStateCuda *s = fex->priv;
    int ret = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    ret |= release_fex_buffers(fex);
    const CudaFunctions *cu_f = fex->cu_state->f;
    if (cu_f && s->module) {
        ret |= (int)cu_f->cuModuleUnload(s->module);
        s->module = NULL;
    }
    return ret;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatAdmStateCuda *s = fex->priv;
    if (s->adm_csf_mode != 0)
        return -EINVAL;

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);
    compute_rfactor(s);

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (err)
        return err;
    err = load_fex_module(fex);
    if (!err)
        err = alloc_fex_buffers(fex, bpc, w, h);
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    if (err) {
        const int cleanup_err = release_fex_resources(fex);
        if (cleanup_err)
            vmaf_log(VMAF_LOG_LEVEL_ERROR, "float_adm_cuda: initialization cleanup failed: %d\n",
                     cleanup_err);
    }
    return err;
}

typedef struct {
    FloatAdmStateCuda *state;
    VmafCudaState *cuda_state;
    CudaFunctions *cu_f;
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
    float scaler;
    float pixel_offset;
    float pnorm;
    int bypass_cm;
} FloatAdmLaunchContext;

typedef struct {
    FloatAdmLaunchContext *launch;
    int scale;
    int cur_w;
    int cur_h;
    int half_w;
    int half_h;
    int parent_w;
    int parent_h;
    int parent_half_h;
    int buf_stride;
    int left;
    int top;
    int right;
    int bottom;
    int active_h;
    CUdeviceptr ref_band;
    CUdeviceptr dis_band;
    CUdeviceptr parent_ref_band;
    CUdeviceptr parent_dis_band;
    CUdeviceptr accum;
    float rfactor_h;
    float rfactor_v;
    float rfactor_d;
    float gain_limit;
} FloatAdmScaleContext;

static float input_scaler(unsigned bpc)
{
    if (bpc == 10u)
        return 4.0f;
    if (bpc == 12u)
        return 16.0f;
    if (bpc == 16u)
        return 256.0f;
    return 1.0f;
}

static FloatAdmScaleContext make_scale_context(FloatAdmLaunchContext *launch, int scale)
{
    FloatAdmStateCuda *s = launch->state;
    const int half_w = (int)s->scale_half_w[scale];
    const int half_h = (int)s->scale_half_h[scale];
    int top = (int)((double)half_h * FADM_BORDER_FACTOR - 0.5);
    int left = (int)((double)half_w * FADM_BORDER_FACTOR - 0.5);
    if (top < 0)
        top = 0;
    if (left < 0)
        left = 0;
    return (FloatAdmScaleContext){
        .launch = launch,
        .scale = scale,
        .cur_w = (int)s->scale_w[scale],
        .cur_h = (int)s->scale_h[scale],
        .half_w = half_w,
        .half_h = half_h,
        .parent_w = scale > 0 ? (int)s->scale_w[scale] : 0,
        .parent_h = scale > 0 ? (int)s->scale_h[scale] : 0,
        .parent_half_h = scale > 0 ? (int)s->scale_half_h[scale - 1] : 0,
        .buf_stride = (int)s->buf_stride,
        .left = left,
        .top = top,
        .right = half_w - left,
        .bottom = half_h - top,
        .active_h = half_h - 2 * top,
        .ref_band = (CUdeviceptr)s->ref_band[scale]->data,
        .dis_band = (CUdeviceptr)s->dis_band[scale]->data,
        .parent_ref_band = scale > 0 ? (CUdeviceptr)s->ref_band[scale - 1]->data : (CUdeviceptr)0,
        .parent_dis_band = scale > 0 ? (CUdeviceptr)s->dis_band[scale - 1]->data : (CUdeviceptr)0,
        .accum = (CUdeviceptr)s->accum[scale]->data,
        .rfactor_h = s->rfactor[scale * 3 + 0],
        .rfactor_v = s->rfactor[scale * 3 + 1],
        .rfactor_d = s->rfactor[scale * 3 + 2],
        .gain_limit = (float)s->adm_enhn_gain_limit,
    };
}

static int launch_dwt_vertical(FloatAdmScaleContext *ctx)
{
    FloatAdmLaunchContext *launch = ctx->launch;
    FloatAdmStateCuda *s = launch->state;
    const unsigned gx = ((unsigned)ctx->cur_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)ctx->half_h + FADM_BY - 1u) / FADM_BY;
    void *args[] = {
        &ctx->scale,           &launch->ref_raw,      &launch->dis_raw, &launch->raw_stride,
        &ctx->parent_ref_band, &ctx->parent_dis_band, &ctx->buf_stride, &ctx->parent_half_h,
        &ctx->parent_w,        &ctx->parent_h,        &launch->dwt_ref, &launch->dwt_dis,
        &ctx->cur_w,           &ctx->cur_h,           &ctx->half_h,     &s->bpc,
        &launch->scaler,       &launch->pixel_offset};
    CHECK_CUDA_RETURN(launch->cu_f, cuLaunchKernel(s->func_dwt_vert, gx, gy, 2, FADM_BX, FADM_BY, 1,
                                                   0, launch->stream, args, NULL));
    return 0;
}

static int launch_dwt_horizontal(FloatAdmScaleContext *ctx)
{
    FloatAdmLaunchContext *launch = ctx->launch;
    FloatAdmStateCuda *s = launch->state;
    const unsigned gx = ((unsigned)ctx->half_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)ctx->half_h + FADM_BY - 1u) / FADM_BY;
    void *args[] = {&ctx->scale,    &launch->dwt_ref, &launch->dwt_dis,
                    &ctx->ref_band, &ctx->dis_band,   &ctx->cur_w,
                    &ctx->half_w,   &ctx->half_h,     &ctx->buf_stride};
    CHECK_CUDA_RETURN(launch->cu_f, cuLaunchKernel(s->func_dwt_hori, gx, gy, 2, FADM_BX, FADM_BY, 1,
                                                   0, launch->stream, args, NULL));
    return 0;
}

static int launch_csf(FloatAdmScaleContext *ctx, CUfunction function, CUdeviceptr *csf_a,
                      CUdeviceptr *csf_f)
{
    FloatAdmLaunchContext *launch = ctx->launch;
    const unsigned gx = ((unsigned)ctx->half_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)ctx->half_h + FADM_BY - 1u) / FADM_BY;
    void *args[] = {
        &ctx->ref_band,  &ctx->dis_band,   csf_a,           csf_f,           &ctx->half_w,
        &ctx->half_h,    &ctx->buf_stride, &ctx->rfactor_h, &ctx->rfactor_v, &ctx->rfactor_d,
        &ctx->gain_limit};
    CHECK_CUDA_RETURN(launch->cu_f, cuLaunchKernel(function, gx, gy, 1, FADM_BX, FADM_BY, 1, 0,
                                                   launch->stream, args, NULL));
    return 0;
}

static int launch_reduction(FloatAdmScaleContext *ctx, CUfunction function, CUdeviceptr *csf_a,
                            CUdeviceptr *csf_f)
{
    FloatAdmLaunchContext *launch = ctx->launch;
    const unsigned rows = (unsigned)(ctx->active_h > 0 ? ctx->active_h : 1);
    const unsigned gx = 3u * rows;
    void *args[] = {&ctx->ref_band,  &ctx->dis_band,    csf_a,           csf_f,
                    &ctx->accum,     &ctx->half_w,      &ctx->half_h,    &ctx->buf_stride,
                    &ctx->left,      &ctx->top,         &ctx->right,     &ctx->bottom,
                    &ctx->rfactor_h, &ctx->rfactor_v,   &ctx->rfactor_d, &ctx->gain_limit,
                    &launch->pnorm,  &launch->bypass_cm};
    CHECK_CUDA_RETURN(launch->cu_f, cuLaunchKernel(function, gx, 1u, 1u, FADM_BX, FADM_BY, 1, 0,
                                                   launch->stream, args, NULL));
    return 0;
}

static int launch_scale(FloatAdmLaunchContext *launch, int scale)
{
    FloatAdmStateCuda *s = launch->state;
    FloatAdmScaleContext ctx = make_scale_context(launch, scale);
    int err = launch_dwt_vertical(&ctx);
    if (!err)
        err = launch_dwt_horizontal(&ctx);
    if (!err)
        err = launch_csf(&ctx, s->func_decouple_csf, &launch->csf_a, &launch->csf_f);
    if (!err)
        err = launch_reduction(&ctx, s->func_csf_cm, &launch->csf_a, &launch->csf_f);
    if (!err)
        err = launch_csf(&ctx, s->func_csf_r, &launch->csf_a_aim, &launch->csf_f_aim);
    if (!err && s->adm_skip_aim_scale != scale)
        err = launch_reduction(&ctx, s->func_aim_cm, &launch->csf_a_aim, &launch->csf_f_aim);
    return err;
}

static int upload_input_planes(FloatAdmLaunchContext *launch, VmafPicture *ref_pic,
                               VmafPicture *dist_pic)
{
    FloatAdmStateCuda *s = launch->state;
    CHECK_CUDA_RETURN(launch->cu_f,
                      cuStreamWaitEvent(launch->stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                        CU_EVENT_WAIT_DEFAULT));
    CUDA_MEMCPY2D copy = {0};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = (CUdeviceptr)ref_pic->data[0];
    copy.srcPitch = ref_pic->stride[0];
    copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.dstDevice = launch->ref_raw;
    copy.dstPitch = launch->raw_stride;
    copy.WidthInBytes = launch->raw_stride;
    copy.Height = s->height;
    CHECK_CUDA_RETURN(launch->cu_f, cuMemcpy2DAsync(&copy, launch->stream));

    copy.srcDevice = (CUdeviceptr)dist_pic->data[0];
    copy.srcPitch = dist_pic->stride[0];
    copy.dstDevice = launch->dis_raw;
    CHECK_CUDA_RETURN(launch->cu_f, cuMemcpy2DAsync(&copy, launch->stream));
    return 0;
}

static int clear_accumulators(FloatAdmLaunchContext *launch)
{
    FloatAdmStateCuda *s = launch->state;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        CHECK_CUDA_RETURN(launch->cu_f, cuMemsetD8Async(s->accum[scale]->data, 0,
                                                        (size_t)s->wg_count[scale] *
                                                            FADM_ACCUM_SLOTS * sizeof(float),
                                                        launch->stream));
    }
    return 0;
}

static int queue_accumulator_readback(FloatAdmLaunchContext *launch)
{
    FloatAdmStateCuda *s = launch->state;
    CHECK_CUDA_RETURN(launch->cu_f, cuEventRecord(s->lc.submit, launch->stream));
    CHECK_CUDA_RETURN(launch->cu_f,
                      cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        CHECK_CUDA_RETURN(
            launch->cu_f,
            cuMemcpyDtoHAsync(s->accum_host[scale], (CUdeviceptr)s->accum[scale]->data,
                              (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float),
                              s->lc.str));
    }
    return vmaf_cuda_kernel_submit_post_record(&s->lc, launch->cuda_state);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                           VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    FloatAdmStateCuda *s = fex->priv;
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    FloatAdmLaunchContext launch = {
        .state = s,
        .cuda_state = fex->cu_state,
        .cu_f = fex->cu_state->f,
        .stream = vmaf_cuda_picture_get_stream(ref_pic),
        .raw_stride = (ptrdiff_t)(s->width * bpp),
        .ref_raw = (CUdeviceptr)s->src_ref->data,
        .dis_raw = (CUdeviceptr)s->src_dis->data,
        .dwt_ref = (CUdeviceptr)s->dwt_tmp_ref->data,
        .dwt_dis = (CUdeviceptr)s->dwt_tmp_dis->data,
        .csf_a = (CUdeviceptr)s->csf_a->data,
        .csf_f = (CUdeviceptr)s->csf_f->data,
        .csf_a_aim = (CUdeviceptr)s->csf_a_aim->data,
        .csf_f_aim = (CUdeviceptr)s->csf_f_aim->data,
        .scaler = input_scaler(s->bpc),
        .pixel_offset = -128.0f,
        .pnorm = (float)s->adm_p_norm,
        .bypass_cm = s->adm_bypass_cm,
    };
    int err = upload_input_planes(&launch, ref_pic, dist_pic);
    if (!err)
        err = clear_accumulators(&launch);
    for (int scale = 0; scale < FADM_NUM_SCALES && !err; scale++)
        err = launch_scale(&launch, scale);
    if (!err)
        err = queue_accumulator_readback(&launch);
    return err;
}

typedef struct {
    double cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double csf[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double aim_cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
} FloatAdmTotals;

typedef struct {
    double num;
    double den;
    double aim_num;
    double aim_den;
    double scale[2 * FADM_NUM_SCALES];
    double adm2;
    double aim;
    double adm3;
} FloatAdmScores;

/* collect_wait may return after the engine-scope fence drain, before host-visible
 * DMA retirement on lc.str. The explicit stream barrier closes that race. */
static int wait_for_accumulator_copy(VmafFeatureExtractor *fex)
{
    FloatAdmStateCuda *s = fex->priv;
    const int sync_err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (sync_err)
        return sync_err;
    CHECK_CUDA_RETURN(fex->cu_state->f, cuStreamSynchronize(s->lc.str));
    return 0;
}

static void reduce_accumulators(const FloatAdmStateCuda *s, FloatAdmTotals *totals)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float *slots = s->accum_host[scale];
        for (unsigned wg = 0u; wg < s->wg_count[scale]; wg++) {
            const float *values = slots + (size_t)wg * FADM_ACCUM_SLOTS;
            for (int band = 0; band < FADM_NUM_BANDS; band++) {
                totals->csf[scale][band] += (double)values[band];
                totals->cm[scale][band] += (double)values[3 + band];
                totals->aim_cm[scale][band] += (double)values[6 + band];
            }
        }
    }
}

static void pool_adm_totals(const FloatAdmStateCuda *s, const FloatAdmTotals *totals,
                            FloatAdmScores *scores)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const int half_w = (int)s->scale_half_w[scale];
        const int half_h = (int)s->scale_half_h[scale];
        int left = (int)((double)half_w * FADM_BORDER_FACTOR - 0.5);
        int top = (int)((double)half_h * FADM_BORDER_FACTOR - 0.5);
        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        const int right = half_w - left;
        const int bottom = half_h - top;
        const float inv_p = 1.0f / (float)s->adm_p_norm;
        const float area_root =
            powf((float)((bottom - top) * (right - left)) * (float)s->adm_noise_weight, inv_p);
        float num_scale = 0.0f;
        float den_scale = 0.0f;
        for (int band = 0; band < FADM_NUM_BANDS; band++) {
            num_scale += powf((float)totals->cm[scale][band], inv_p) + area_root;
            den_scale += powf((float)totals->csf[scale][band], inv_p) + area_root;
        }
        scores->scale[2 * scale + 0] = num_scale;
        scores->scale[2 * scale + 1] = den_scale;
        scores->num += num_scale;
        scores->den += den_scale;

        float aim_num_scale = 0.0f;
        for (int band = 0; band < FADM_NUM_BANDS; band++)
            aim_num_scale += powf((float)totals->aim_cm[scale][band], inv_p);
        if (s->adm_skip_aim_scale != scale) {
            scores->aim_den += den_scale;
            scores->aim_num += aim_num_scale;
        }
    }
}

static void finalize_adm_scores(const FloatAdmStateCuda *s, FloatAdmScores *scores)
{
    const int width = (int)s->scale_w[0];
    const int height = (int)s->scale_h[0];
    const double numden_limit = 1e-2 * (double)(width * height) / (1920.0 * 1080.0);
    if (scores->num < numden_limit)
        scores->num = 0.0;
    if (scores->den < numden_limit)
        scores->den = 0.0;
    scores->adm2 = scores->den == 0.0 ? 1.0 : scores->num / scores->den;
    scores->aim = scores->aim_den == 0.0 ? 1.0 : fmin(scores->aim_num / scores->aim_den, 1.0);
    if (s->adm_adm3_apply_hm) {
        const double denominator = scores->adm2 + scores->aim;
        scores->adm3 = denominator > 0.0 ? 2.0 * scores->adm2 * scores->aim / denominator : 0.0;
    } else {
        scores->adm3 =
            scores->adm2 * s->adm_dlm_weight + (1.0 - scores->aim) * (1.0 - s->adm_dlm_weight);
    }
    if (scores->adm3 < s->adm_min_val)
        scores->adm3 = s->adm_min_val;
}

static int emit_adm_scores(const FloatAdmStateCuda *s, const FloatAdmScores *scores, unsigned index,
                           VmafFeatureCollector *fc)
{
    int err = vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_feature_adm2_score", scores->adm2, index);
    static const char *const scale_names[FADM_NUM_SCALES] = {
        "VMAF_feature_adm_scale0_score", "VMAF_feature_adm_scale1_score",
        "VMAF_feature_adm_scale2_score", "VMAF_feature_adm_scale3_score"};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        err |= vmaf_feature_collector_append_with_dict(
            fc, s->feature_name_dict, scale_names[scale],
            scores->scale[2 * scale] / scores->scale[2 * scale + 1], index);
    }
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_aim_score", scores->aim, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_adm3_score", scores->adm3, index);
    return err;
}

static int emit_adm_debug_scores(const FloatAdmStateCuda *s, const FloatAdmScores *scores,
                                 unsigned index, VmafFeatureCollector *fc)
{
    int err = vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm", scores->adm2,
                                                      index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_num", scores->num,
                                                   index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_den", scores->den,
                                                   index);
    static const char *const names[2 * FADM_NUM_SCALES] = {
        "adm_num_scale0", "adm_den_scale0", "adm_num_scale1", "adm_den_scale1",
        "adm_num_scale2", "adm_den_scale2", "adm_num_scale3", "adm_den_scale3"};
    for (int i = 0; i < 2 * FADM_NUM_SCALES && !err; i++)
        err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, names[i],
                                                       scores->scale[i], index);
    return err;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    FloatAdmStateCuda *s = fex->priv;
    int err = wait_for_accumulator_copy(fex);
    if (err)
        return err;

    FloatAdmTotals totals = {0};
    FloatAdmScores scores = {0};
    reduce_accumulators(s, &totals);
    pool_adm_totals(s, &totals, &scores);
    finalize_adm_scores(s, &scores);
    err = emit_adm_scores(s, &scores, index, fc);
    if (s->debug && !err)
        err = emit_adm_debug_scores(s, &scores, index, fc);
    return err;
}
static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    return release_fex_resources(fex);
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
