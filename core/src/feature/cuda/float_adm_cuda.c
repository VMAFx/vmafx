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

#include "vmaf_nullptr.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "common.h"
#include "feature/adm_options.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"

#include "cuda/float_adm_cuda.h"
#include "cuda/kernel_template.h"
#include "cuda_helper.cuh"
#include "picture.h"
#include "picture_cuda.h"

/* lint rationale: C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `VMAF_NULLPTR` and
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

#define FADM_OPTION_DOUBLE(NAME, ALIAS, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM)                    \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .alias = (ALIAS),                                                                          \
        .help = (HELP),                                                                            \
        .offset = offsetof(FloatAdmStateCuda, FIELD),                                              \
        .type = VMAF_OPT_TYPE_DOUBLE,                                                              \
        .default_val.d = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }
#define FADM_OPTION_INT(NAME, ALIAS, HELP, FIELD, DEFAULT, MINIMUM, MAXIMUM)                       \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .alias = (ALIAS),                                                                          \
        .help = (HELP),                                                                            \
        .offset = offsetof(FloatAdmStateCuda, FIELD),                                              \
        .type = VMAF_OPT_TYPE_INT,                                                                 \
        .default_val.i = (DEFAULT),                                                                \
        .min = (MINIMUM),                                                                          \
        .max = (MAXIMUM),                                                                          \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }
#define FADM_OPTION_BOOL(NAME, ALIAS, HELP, FIELD, FLAGS)                                          \
    {                                                                                              \
        .name = (NAME),                                                                            \
        .alias = (ALIAS),                                                                          \
        .help = (HELP),                                                                            \
        .offset = offsetof(FloatAdmStateCuda, FIELD),                                              \
        .type = VMAF_OPT_TYPE_BOOL,                                                                \
        .default_val.b = false,                                                                    \
        .flags = (FLAGS),                                                                          \
    }

static const VmafOption options[] = {
    FADM_OPTION_BOOL("debug", VMAF_NULLPTR, "debug mode: enable additional output", debug, 0),
    FADM_OPTION_DOUBLE("adm_enhn_gain_limit", "egl",
                       "enhancement gain imposed on adm, must be >= 1.0", adm_enhn_gain_limit,
                       100.0, 1.0, 100.0),
    FADM_OPTION_DOUBLE("adm_norm_view_dist", "nvd", "normalized viewing distance",
                       adm_norm_view_dist, 3.0, 0.75, 24.0),
    FADM_OPTION_INT("adm_ref_display_height", "rdf", "reference display height in pixels",
                    adm_ref_display_height, 1080, 1, 4320),
    FADM_OPTION_INT("adm_csf_mode", "csf", "contrast sensitivity function (mode 0 only on CUDA v1)",
                    adm_csf_mode, 0, 0, 9),
    FADM_OPTION_DOUBLE("adm_csf_scale", "scf",
                       "CSF band-scale multiplier for h/v bands (default 1.0 = no scaling)",
                       adm_csf_scale, DEFAULT_ADM_CSF_SCALE, 0.0, 50.0),
    FADM_OPTION_DOUBLE("adm_csf_diag_scale", "scfd",
                       "CSF band-scale multiplier for diagonal bands (default 1.0 = no scaling)",
                       adm_csf_diag_scale, DEFAULT_ADM_CSF_DIAG_SCALE, 0.0, 50.0),
    FADM_OPTION_DOUBLE("adm_noise_weight", "nw",
                       "noise floor weight for CM numerator (default 0.03125 = 1/32)",
                       adm_noise_weight, DEFAULT_ADM_NOISE_WEIGHT, 0.0, 100.0),
    FADM_OPTION_INT("adm_bypass_cm", "bcm", "bypass CM computation (0 = normal, 1 = bypass)",
                    adm_bypass_cm, 0, 0, 1),
    FADM_OPTION_BOOL("adm_adm3_apply_hm", "aah",
                     "apply harmonic mean for adm3 score (false = linear blend)", adm_adm3_apply_hm,
                     VMAF_OPT_FLAG_FEATURE_PARAM),
    FADM_OPTION_DOUBLE("adm_p_norm", "apn", "p-norm exponent for AIM/ADM3 score (default 3.0)",
                       adm_p_norm, 3.0, 1.0, 20.0),
    FADM_OPTION_DOUBLE("adm_dlm_weight", "dlmw",
                       "DLM weight for linear-blend adm3 score (default 0.5)", adm_dlm_weight, 0.5,
                       0.0, 1.0),
    FADM_OPTION_DOUBLE("adm_min_val", "min", "minimum clamp for adm3 score (default 0.0)",
                       adm_min_val, DEFAULT_ADM_MIN_VAL, 0.0, 1.0),
    FADM_OPTION_INT("adm_skip_aim_scale", "sasc",
                    "skip AIM accumulation at this scale index (-1 = no skip)", adm_skip_aim_scale,
                    -1, -1, 3),
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

static void fadm_active_range(int half, int *lo, int *hi)
{
    int border = (int)((double)half * FADM_BORDER_FACTOR - 0.5);
    if (border < 0)
        border = 0;
    *lo = border;
    *hi = half - border;
}

static void fadm_cuda_init_rfactor(FloatAdmStateCuda *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float f1 =
            fadm_dwt_quant_step(scale, 1, s->adm_norm_view_dist, s->adm_ref_display_height);
        const float f2 =
            fadm_dwt_quant_step(scale, 2, s->adm_norm_view_dist, s->adm_ref_display_height);
        s->rfactor[scale * 3 + 0] = 1.0f / f1;
        s->rfactor[scale * 3 + 1] = 1.0f / f1;
        s->rfactor[scale * 3 + 2] = 1.0f / f2;
    }
}

static void fadm_cuda_init_wg_counts(FloatAdmStateCuda *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        int top = 0;
        int bottom = 0;
        fadm_active_range((int)s->scale_half_h[scale], &top, &bottom);
        const unsigned num_rows = bottom > top ? (unsigned)(bottom - top) : 1u;
        s->wg_count[scale] = 3u * num_rows;
    }
}

typedef struct FadmCudaKernelSlot {
    CUfunction *slot;
    const char *name;
} FadmCudaKernelSlot;

static int fadm_cuda_load_functions(VmafFeatureExtractor *fex, FloatAdmStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module, float_adm_score_ptx));
    const FadmCudaKernelSlot kernels[] = {
        {&s->func_dwt_vert, "float_adm_dwt_vert"},
        {&s->func_dwt_hori, "float_adm_dwt_hori"},
        {&s->func_decouple_csf, "float_adm_decouple_csf"},
        {&s->func_csf_cm, "float_adm_csf_cm"},
        {&s->func_csf_r, "float_adm_csf_r"},
        {&s->func_aim_cm, "float_adm_aim_cm"},
    };
    for (size_t i = 0; i < sizeof(kernels) / sizeof(kernels[0]); i++) {
        CHECK_CUDA_RETURN(cu_f, cuModuleGetFunction(kernels[i].slot, s->module, kernels[i].name));
    }
    return 0;
}

static int fadm_cuda_push_context(VmafFeatureExtractor *fex)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    return 0;
}

static int fadm_cuda_pop_context(VmafFeatureExtractor *fex)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPopCurrent(VMAF_NULLPTR));
    return 0;
}

static int fadm_cuda_module_load(VmafFeatureExtractor *fex, FloatAdmStateCuda *s)
{
    int err = fadm_cuda_push_context(fex);
    if (err)
        return err;
    err = fadm_cuda_load_functions(fex, s);
    const int pop_err = fadm_cuda_pop_context(fex);
    return err ? err : pop_err;
}

static int fadm_cuda_buffers_alloc(VmafFeatureExtractor *fex, FloatAdmStateCuda *s)
{
    const size_t bpp = s->bpc <= 8u ? 1u : 2u;
    const size_t raw_bytes = (size_t)s->width * s->height * bpp;
    const size_t dwt_bytes = (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(float);
    const size_t csf_bytes =
        (size_t)FADM_NUM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(float);
    VmafCudaBuffer **flat[] = {&s->src_ref, &s->src_dis, &s->dwt_tmp_ref, &s->dwt_tmp_dis,
                               &s->csf_a,   &s->csf_f,   &s->csf_a_aim,   &s->csf_f_aim};
    const size_t sizes[] = {raw_bytes, raw_bytes, dwt_bytes, dwt_bytes,
                            csf_bytes, csf_bytes, csf_bytes, csf_bytes};
    int ret = 0;
    for (size_t i = 0; i < sizeof(flat) / sizeof(flat[0]); i++)
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, flat[i], sizes[i]);
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const size_t band_bytes =
            (size_t)4u * s->buf_stride * s->scale_half_h[scale] * sizeof(float);
        const size_t accum_bytes = (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_band[scale], band_bytes);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_band[scale], band_bytes);
        ret |= vmaf_cuda_buffer_alloc(fex->cu_state, &s->accum[scale], accum_bytes);
        ret |=
            vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->accum_host[scale], accum_bytes);
    }
    return ret ? -ENOMEM : 0;
}

static int fadm_cuda_buffer_free(VmafFeatureExtractor *fex, VmafCudaBuffer **buffer)
{
    if (!*buffer)
        return 0;
    const int ret = vmaf_cuda_buffer_free(fex->cu_state, *buffer);
    free(*buffer);
    *buffer = VMAF_NULLPTR;
    return ret;
}

static int fadm_cuda_buffers_free(VmafFeatureExtractor *fex, FloatAdmStateCuda *s)
{
    int ret = 0;
    VmafCudaBuffer **flat[] = {&s->src_ref, &s->src_dis, &s->dwt_tmp_ref, &s->dwt_tmp_dis,
                               &s->csf_a,   &s->csf_f,   &s->csf_a_aim,   &s->csf_f_aim};
    for (size_t i = 0; i < sizeof(flat) / sizeof(flat[0]); i++)
        ret |= fadm_cuda_buffer_free(fex, flat[i]);
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        ret |= fadm_cuda_buffer_free(fex, &s->ref_band[scale]);
        ret |= fadm_cuda_buffer_free(fex, &s->dis_band[scale]);
        ret |= fadm_cuda_buffer_free(fex, &s->accum[scale]);
        if (s->accum_host[scale]) {
            ret |= vmaf_cuda_buffer_host_free(fex->cu_state, s->accum_host[scale]);
            s->accum_host[scale] = VMAF_NULLPTR;
        }
    }
    return ret;
}

static int fadm_cuda_release(VmafFeatureExtractor *fex)
{
    FloatAdmStateCuda *s = fex->priv;
    int ret = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    ret |= fadm_cuda_buffers_free(fex, s);
    ret |= vmaf_dictionary_free(&s->feature_name_dict);
    const CudaFunctions *cu_f = fex->cu_state->f;
    if (cu_f && s->module) {
        (void)cu_f->cuModuleUnload(s->module);
        s->module = VMAF_NULLPTR;
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
    fadm_cuda_init_rfactor(s);
    fadm_cuda_init_wg_counts(s);
    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (!err)
        err = fadm_cuda_module_load(fex, s);
    if (!err)
        err = fadm_cuda_buffers_alloc(fex, s);
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict)
            err = -ENOMEM;
    }
    if (err)
        (void)fadm_cuda_release(fex);
    return err;
}

typedef struct FadmScaleGeom {
    int scale;
    int cur_w;
    int cur_h;
    int half_w;
    int half_h;
    int buf_stride;
    int left;
    int top;
    int right;
    int bottom;
    float rfh;
    float rfv;
    float rfd;
    float gain_limit;
    float pnorm;
    int bypass_cm;
    CUdeviceptr ref_band;
    CUdeviceptr dis_band;
} FadmScaleGeom;

static void fadm_cuda_scale_geom(const FloatAdmStateCuda *s, int scale, FadmScaleGeom *g)
{
    g->scale = scale;
    g->cur_w = (int)s->scale_w[scale];
    g->cur_h = (int)s->scale_h[scale];
    g->half_w = (int)s->scale_half_w[scale];
    g->half_h = (int)s->scale_half_h[scale];
    g->buf_stride = (int)s->buf_stride;
    fadm_active_range(g->half_w, &g->left, &g->right);
    fadm_active_range(g->half_h, &g->top, &g->bottom);
    g->rfh = s->rfactor[scale * 3 + 0];
    g->rfv = s->rfactor[scale * 3 + 1];
    g->rfd = s->rfactor[scale * 3 + 2];
    g->gain_limit = (float)s->adm_enhn_gain_limit;
    g->pnorm = (float)s->adm_p_norm;
    g->bypass_cm = s->adm_bypass_cm;
    g->ref_band = (CUdeviceptr)s->ref_band[scale]->data;
    g->dis_band = (CUdeviceptr)s->dis_band[scale]->data;
}

static int fadm_cuda_upload(VmafFeatureExtractor *fex, FloatAdmStateCuda *s, const VmafPicture *ref_pic,
                            const VmafPicture *dist_pic, CUstream pic_stream)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const size_t bpp = s->bpc <= 8u ? 1u : 2u;
    const ptrdiff_t raw_stride = (ptrdiff_t)(s->width * bpp);
    CHECK_CUDA_RETURN(cu_f,
                      cuStreamWaitEvent(pic_stream, vmaf_cuda_picture_get_ready_event(dist_pic),
                                        CU_EVENT_WAIT_DEFAULT));
    CUDA_MEMCPY2D copy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = (CUdeviceptr)ref_pic->data[0],
        .srcPitch = ref_pic->stride[0],
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = (CUdeviceptr)s->src_ref->data,
        .dstPitch = raw_stride,
        .WidthInBytes = raw_stride,
        .Height = s->height,
    };
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, pic_stream));
    copy.srcDevice = (CUdeviceptr)dist_pic->data[0];
    copy.srcPitch = dist_pic->stride[0];
    copy.dstDevice = (CUdeviceptr)s->src_dis->data;
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, pic_stream));
    return 0;
}

static int fadm_cuda_reset_accumulators(VmafFeatureExtractor *fex, FloatAdmStateCuda *s,
                                        CUstream pic_stream)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const size_t bytes = (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float);
        CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(s->accum[scale]->data, 0, bytes, pic_stream));
    }
    return 0;
}

static float fadm_cuda_scaler(unsigned bpc)
{
    if (bpc == 10u)
        return 4.0f;
    if (bpc == 12u)
        return 16.0f;
    if (bpc == 16u)
        return 256.0f;
    return 1.0f;
}

static int fadm_cuda_launch_dwt_vert(VmafFeatureExtractor *fex, FloatAdmStateCuda *s,
                                     FadmScaleGeom *g, CUstream pic_stream)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    const size_t bpp = s->bpc <= 8u ? 1u : 2u;
    const ptrdiff_t raw_stride = (ptrdiff_t)(s->width * bpp);
    CUdeviceptr ref_raw = (CUdeviceptr)s->src_ref->data;
    CUdeviceptr dis_raw = (CUdeviceptr)s->src_dis->data;
    const bool has_parent = g->scale > 0;
    CUdeviceptr parent_ref = has_parent ? (CUdeviceptr)s->ref_band[g->scale - 1]->data : 0;
    CUdeviceptr parent_dis = has_parent ? (CUdeviceptr)s->dis_band[g->scale - 1]->data : 0;
    int parent_half_h = has_parent ? (int)s->scale_half_h[g->scale - 1] : 0;
    int parent_w = has_parent ? g->cur_w : 0;
    int parent_h = has_parent ? g->cur_h : 0;
    CUdeviceptr dwt_ref = (CUdeviceptr)s->dwt_tmp_ref->data;
    CUdeviceptr dwt_dis = (CUdeviceptr)s->dwt_tmp_dis->data;
    unsigned bpc = s->bpc;
    float scaler = fadm_cuda_scaler(s->bpc);
    float pixel_offset = -128.0f;
    const unsigned gx = ((unsigned)g->cur_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)g->half_h + FADM_BY - 1u) / FADM_BY;
    void *args[] = {&g->scale,   &ref_raw,       &dis_raw,       (void *)&raw_stride, &parent_ref,
                    &parent_dis, &g->buf_stride, &parent_half_h, &parent_w,           &parent_h,
                    &dwt_ref,    &dwt_dis,       &g->cur_w,      &g->cur_h,           &g->half_h,
                    &bpc,        &scaler,        &pixel_offset};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_vert, gx, gy, 2, FADM_BX, FADM_BY, 1, 0,
                                           pic_stream, args, VMAF_NULLPTR));
    return 0;
}

static int fadm_cuda_launch_dwt_hori(VmafFeatureExtractor *fex, FloatAdmStateCuda *s,
                                     FadmScaleGeom *g, CUstream pic_stream)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CUdeviceptr dwt_ref = (CUdeviceptr)s->dwt_tmp_ref->data;
    CUdeviceptr dwt_dis = (CUdeviceptr)s->dwt_tmp_dis->data;
    const unsigned gx = ((unsigned)g->half_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)g->half_h + FADM_BY - 1u) / FADM_BY;
    void *args[] = {&g->scale, &dwt_ref,   &dwt_dis,   &g->ref_band,  &g->dis_band,
                    &g->cur_w, &g->half_w, &g->half_h, &g->buf_stride};
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_dwt_hori, gx, gy, 2, FADM_BX, FADM_BY, 1, 0,
                                           pic_stream, args, VMAF_NULLPTR));
    return 0;
}

static int fadm_cuda_launch_csf(VmafFeatureExtractor *fex, CUfunction func, FadmScaleGeom *g,
                                VmafCudaBuffer *csf_a_buf, VmafCudaBuffer *csf_f_buf,
                                CUstream pic_stream)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CUdeviceptr csf_a = (CUdeviceptr)csf_a_buf->data;
    CUdeviceptr csf_f = (CUdeviceptr)csf_f_buf->data;
    const unsigned gx = ((unsigned)g->half_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)g->half_h + FADM_BY - 1u) / FADM_BY;
    void *args[] = {&g->ref_band,   &g->dis_band, &csf_a,  &csf_f,  &g->half_w,    &g->half_h,
                    &g->buf_stride, &g->rfh,      &g->rfv, &g->rfd, &g->gain_limit};
    CHECK_CUDA_RETURN(
        cu_f, cuLaunchKernel(func, gx, gy, 1, FADM_BX, FADM_BY, 1, 0, pic_stream, args, VMAF_NULLPTR));
    return 0;
}

static int fadm_cuda_launch_cm(VmafFeatureExtractor *fex, FloatAdmStateCuda *s, CUfunction func,
                               FadmScaleGeom *g, VmafCudaBuffer *csf_a_buf,
                               VmafCudaBuffer *csf_f_buf, CUstream pic_stream)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CUdeviceptr csf_a = (CUdeviceptr)csf_a_buf->data;
    CUdeviceptr csf_f = (CUdeviceptr)csf_f_buf->data;
    CUdeviceptr accum = (CUdeviceptr)s->accum[g->scale]->data;
    const int active_h = g->bottom - g->top;
    const unsigned gx = 3u * (unsigned)(active_h > 0 ? active_h : 1);
    void *args[] = {&g->ref_band,   &g->dis_band, &csf_a,         &csf_f,   &accum,
                    &g->half_w,     &g->half_h,   &g->buf_stride, &g->left, &g->top,
                    &g->right,      &g->bottom,   &g->rfh,        &g->rfv,  &g->rfd,
                    &g->gain_limit, &g->pnorm,    &g->bypass_cm};
    CHECK_CUDA_RETURN(
        cu_f, cuLaunchKernel(func, gx, 1u, 1u, FADM_BX, FADM_BY, 1, 0, pic_stream, args, VMAF_NULLPTR));
    return 0;
}

static int fadm_cuda_launch_scale(VmafFeatureExtractor *fex, FloatAdmStateCuda *s, int scale,
                                  CUstream pic_stream)
{
    FadmScaleGeom g;
    fadm_cuda_scale_geom(s, scale, &g);
    int err = fadm_cuda_launch_dwt_vert(fex, s, &g, pic_stream);
    if (!err)
        err = fadm_cuda_launch_dwt_hori(fex, s, &g, pic_stream);
    if (!err)
        err = fadm_cuda_launch_csf(fex, s->func_decouple_csf, &g, s->csf_a, s->csf_f, pic_stream);
    if (!err)
        err = fadm_cuda_launch_cm(fex, s, s->func_csf_cm, &g, s->csf_a, s->csf_f, pic_stream);
    if (!err)
        err = fadm_cuda_launch_csf(fex, s->func_csf_r, &g, s->csf_a_aim, s->csf_f_aim, pic_stream);
    if (!err && s->adm_skip_aim_scale != scale) {
        err =
            fadm_cuda_launch_cm(fex, s, s->func_aim_cm, &g, s->csf_a_aim, s->csf_f_aim, pic_stream);
}
    return err;
}

static int fadm_cuda_readback(VmafFeatureExtractor *fex, FloatAdmStateCuda *s, CUstream pic_stream)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const size_t bytes = (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float);
        CHECK_CUDA_RETURN(cu_f,
                          cuMemcpyDtoHAsync(s->accum_host[scale],
                                            (CUdeviceptr)s->accum[scale]->data, bytes, s->lc.str));
    }
    return vmaf_cuda_kernel_submit_post_record(&s->lc, fex->cu_state);
}

static int submit_fex_cuda(VmafFeatureExtractor *fex, const VmafPicture *ref_pic, const VmafPicture *ref_pic_90,
                           const VmafPicture *dist_pic, const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    (void)index;
    FloatAdmStateCuda *s = fex->priv;
    CUstream pic_stream = vmaf_cuda_picture_get_stream(ref_pic);
    int err = fadm_cuda_upload(fex, s, ref_pic, dist_pic, pic_stream);
    if (!err)
        err = fadm_cuda_reset_accumulators(fex, s, pic_stream);
    for (int scale = 0; scale < FADM_NUM_SCALES && !err; scale++)
        err = fadm_cuda_launch_scale(fex, s, scale, pic_stream);
    if (!err)
        err = fadm_cuda_readback(fex, s, pic_stream);
    return err;
}

typedef struct FadmTotals {
    double cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double csf[FADM_NUM_SCALES][FADM_NUM_BANDS];
    double aim_cm[FADM_NUM_SCALES][FADM_NUM_BANDS];
} FadmTotals;

typedef struct FadmPooled {
    double scores[2 * FADM_NUM_SCALES];
    double score_num;
    double score_den;
    double aim_num;
    double aim_den;
} FadmPooled;

static void fadm_cuda_reduce(const FloatAdmStateCuda *s, FadmTotals *totals)
{
    memset(totals, 0, sizeof(*totals));
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const float *slots = s->accum_host[scale];
        for (unsigned wg = 0u; wg < s->wg_count[scale]; wg++) {
            const float *p = slots + (size_t)wg * FADM_ACCUM_SLOTS;
            for (int band = 0; band < FADM_NUM_BANDS; band++) {
                totals->csf[scale][band] += (double)p[band];
                totals->cm[scale][band] += (double)p[3 + band];
                totals->aim_cm[scale][band] += (double)p[6 + band];
            }
        }
    }
}

static void fadm_cuda_pool_scale(const FloatAdmStateCuda *s, const FadmTotals *totals, int scale,
                                 FadmPooled *pooled)
{
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
    fadm_active_range((int)s->scale_half_w[scale], &left, &right);
    fadm_active_range((int)s->scale_half_h[scale], &top, &bottom);
    const float inv_p = 1.0f / (float)s->adm_p_norm;
    const float noise =
        powf((float)((bottom - top) * (right - left)) * (float)s->adm_noise_weight, inv_p);
    float num_scale = 0.0f;
    float den_scale = 0.0f;
    float aim_num_scale = 0.0f;
    for (int band = 0; band < FADM_NUM_BANDS; band++) {
        num_scale += powf((float)totals->cm[scale][band], inv_p) + noise;
        den_scale += powf((float)totals->csf[scale][band], inv_p) + noise;
        aim_num_scale += powf((float)totals->aim_cm[scale][band], inv_p);
    }
    pooled->scores[2 * scale + 0] = num_scale;
    pooled->scores[2 * scale + 1] = den_scale;
    pooled->score_num += num_scale;
    pooled->score_den += den_scale;
    if (s->adm_skip_aim_scale != scale) {
        pooled->aim_den += den_scale;
        pooled->aim_num += aim_num_scale;
    }
}

static int fadm_cuda_emit_debug(FloatAdmStateCuda *s, VmafFeatureCollector *fc,
                                const FadmPooled *pooled, double score, unsigned index)
{
    int err =
        vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm", score, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_num",
                                                   pooled->score_num, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_den",
                                                   pooled->score_den, index);
    const char *names[8] = {"adm_num_scale0", "adm_den_scale0", "adm_num_scale1", "adm_den_scale1",
                            "adm_num_scale2", "adm_den_scale2", "adm_num_scale3", "adm_den_scale3"};
    for (int i = 0; i < 8 && !err; i++) {
        err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, names[i],
                                                       pooled->scores[i], index);
    }
    return err;
}

static double fadm_cuda_adm3_score(const FloatAdmStateCuda *s, double score, double aim_score)
{
    double adm3;
    if (s->adm_adm3_apply_hm) {
        const double denominator = score + aim_score;
        adm3 = denominator > 0.0 ? 2.0 * score * aim_score / denominator : 0.0;
    } else {
        adm3 = score * s->adm_dlm_weight + (1.0 - aim_score) * (1.0 - s->adm_dlm_weight);
    }
    return adm3 < s->adm_min_val ? s->adm_min_val : adm3;
}

static int fadm_cuda_emit(FloatAdmStateCuda *s, VmafFeatureCollector *fc, FadmPooled *pooled,
                          unsigned index)
{
    const int w = (int)s->scale_w[0];
    const int h = (int)s->scale_h[0];
    const double limit = 1e-2 * (double)(w * h) / (1920.0 * 1080.0);
    if (pooled->score_num < limit)
        pooled->score_num = 0.0;
    if (pooled->score_den < limit)
        pooled->score_den = 0.0;
    const double score = pooled->score_den == 0.0 ? 1.0 : pooled->score_num / pooled->score_den;
    const double aim_score =
        pooled->aim_den == 0.0 ? 1.0 : fmin(pooled->aim_num / pooled->aim_den, 1.0);
    const double adm3_score = fadm_cuda_adm3_score(s, score, aim_score);
    int err = vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                      "VMAF_feature_adm2_score", score, index);
    const char *scale_names[FADM_NUM_SCALES] = {
        "VMAF_feature_adm_scale0_score", "VMAF_feature_adm_scale1_score",
        "VMAF_feature_adm_scale2_score", "VMAF_feature_adm_scale3_score"};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++) {
        const int offset = 2 * scale;
        err |= vmaf_feature_collector_append_with_dict(
            fc, s->feature_name_dict, scale_names[scale],
            pooled->scores[offset] / pooled->scores[offset + 1], index);
    }
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_aim_score", aim_score, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_adm3_score", adm3_score, index);
    if (s->debug && !err)
        err |= fadm_cuda_emit_debug(s, fc, pooled, score, index);
    return err;
}

static int collect_fex_cuda(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    FloatAdmStateCuda *s = fex->priv;
    int err = vmaf_cuda_kernel_collect_wait(&s->lc, fex->cu_state);
    if (err)
        return err;
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->lc.str));
    FadmTotals totals;
    fadm_cuda_reduce(s, &totals);
    FadmPooled pooled = {0};
    for (int scale = 0; scale < FADM_NUM_SCALES; scale++)
        fadm_cuda_pool_scale(s, &totals, scale, &pooled);
    return fadm_cuda_emit(s, fc, &pooled, index);
}

static int close_fex_cuda(VmafFeatureExtractor *fex)
{
    return fadm_cuda_release(fex);
}

static const char *provided_features[] = {
    "VMAF_feature_adm2_score", "VMAF_feature_adm_scale0_score", "VMAF_feature_adm_scale1_score",
    "VMAF_feature_adm_scale2_score", "VMAF_feature_adm_scale3_score",
    /* ADR-0574: AIM and ADM3 sub-features. */
    "VMAF_feature_aim_score", "VMAF_feature_adm3_score", "adm", "adm_num", "adm_den",
    "adm_num_scale0", "adm_den_scale0", "adm_num_scale1", "adm_den_scale1", "adm_num_scale2",
    "adm_den_scale2", "adm_num_scale3", "adm_den_scale3", VMAF_NULLPTR};

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
