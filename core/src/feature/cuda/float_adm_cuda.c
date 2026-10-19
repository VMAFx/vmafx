/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_adm feature kernel on the CUDA backend (T7-23 / batch 3
 *  part 6b — ADR-0192 / ADR-0202), with the AIM and ADM3 sub-features of
 *  ADR-0574.
 *
 *  The twin returns the CPU extractor's values bit for bit (ADR-1420). Per
 *  scale the device runs the DWT, then the decouple and the CSF in the
 *  reference's arithmetic (float_adm/float_adm_device.h), then the per-sample
 *  terms of the three reductions, then one fp32 sum per row and slot. The
 *  host adds the rows top to bottom in fp32 and concludes with the
 *  reference's own routines (feature/adm_float_reference.h): its reduced
 *  region, its CSF weights and its pooling of the band accumulators.
 *
 *  Per-frame flow: 20 launches (5 stages x 4 scales) and one pinned-host D2H
 *  copy of the row sums.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "common.h"
#include "feature/adm_csf_fixed_point.h"
#include "feature/adm_float_reference.h"
#include "feature/adm_options.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "feature/adm_score.h"
#include "feature/nonfinite_score.h"
#include "log.h"

#include "cuda/float_adm/float_adm_device.h"
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

    /* The reference's constants, from its own routines (ADR-1420). */
    float rfactor[FADM_SCALES][FADM_BANDS];
    AdmBorderS region[FADM_SCALES];
    float cos_1deg_sq;

    /* Stream + event pair owned by `cuda/kernel_template.h` lifecycle
     * (ADR-0246). Multi-stage DWT + CSF pipeline state stays outside
     * the template's single-pair readback bundle. */
    VmafCudaKernelLifecycle lc;
    CUfunction func_dwt_vert;
    CUfunction func_dwt_hori;
    CUfunction func_decouple_csf;
    CUfunction func_terms;
    CUfunction func_row_sums;

    VmafCudaBuffer *src_ref;
    VmafCudaBuffer *src_dis;
    VmafCudaBuffer *dwt_tmp_ref;
    VmafCudaBuffer *dwt_tmp_dis;
    VmafCudaBuffer *ref_band[FADM_SCALES];
    VmafCudaBuffer *dis_band[FADM_SCALES];
    /* CSF of decouple_a and of decouple_r, each with its |.| / 30 companion. */
    VmafCudaBuffer *csf_a;
    VmafCudaBuffer *csf_fa;
    VmafCudaBuffer *csf_r;
    VmafCudaBuffer *csf_fr;
    /* Per-sample terms of the scale in flight, then the per-row sums of all
     * four scales (row_offset[] floats into `rows`). */
    VmafCudaBuffer *terms;
    VmafCudaBuffer *rows;
    float *rows_host;
    size_t row_offset[FADM_SCALES];
    size_t row_floats;

    unsigned scale_w[FADM_SCALES];
    unsigned scale_h[FADM_SCALES];
    unsigned scale_half_w[FADM_SCALES];
    unsigned scale_half_h[FADM_SCALES];

    /* PTX module backing the kernels — owned here so `close_fex_cuda` can
     * unload it. Skipping the unload leaks ~200-500 KB of GPU-resident PTX
     * backing store per vmaf_close(). */
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
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM | VMAF_OPT_FLAG_DEFAULT_ONLY},
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

static void compute_per_scale_dims(FloatAdmStateCuda *s)
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
    /* buf_stride sized to scale-0 half_w0 so a single stride works at
     * every scale (parent's stride read at scale s+1 still aligns
     * because the host-side buffer was allocated with this stride). */
    s->buf_stride = (s->scale_half_w[0] + 3u) & ~3u;
}

/* ------------------------------------------------------------------ */
static void float_adm_preserve_error(int *rc, int err)
{
    if (!*rc)
        *rc = err;
}

static int float_adm_release_buffers(VmafFeatureExtractor *fex, FloatAdmStateCuda *s, int rc)
{
    float_adm_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->src_ref));
    float_adm_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->src_dis));
    float_adm_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->dwt_tmp_ref));
    float_adm_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->dwt_tmp_dis));
    for (int scale = 0; scale < FADM_SCALES; scale++) {
        float_adm_preserve_error(&rc,
                                 vmaf_cuda_buffer_free_owned(fex->cu_state, &s->ref_band[scale]));
        float_adm_preserve_error(&rc,
                                 vmaf_cuda_buffer_free_owned(fex->cu_state, &s->dis_band[scale]));
    }
    float_adm_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->csf_a));
    float_adm_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->csf_fa));
    float_adm_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->csf_r));
    float_adm_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->csf_fr));
    float_adm_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->terms));
    float_adm_preserve_error(&rc, vmaf_cuda_buffer_free_owned(fex->cu_state, &s->rows));
    float_adm_preserve_error(
        &rc, vmaf_cuda_buffer_host_free_owned(fex->cu_state, (void **)&s->rows_host));
    return rc;
}

/* float_adm_init_unwind - the single teardown path for init_fex_cuda.
 *
 * Drain the lifecycle before releasing anything queued work may reference.
 * The original init failure remains the first returned error.
 */
static int float_adm_init_unwind(VmafFeatureExtractor *fex, FloatAdmStateCuda *s, int err)
{
    const int lifecycle_rc = vmaf_cuda_kernel_lifecycle_close(&s->lc, fex->cu_state);
    if (lifecycle_rc)
        return err ? err : lifecycle_rc;

    int rc = float_adm_release_buffers(fex, s, err);
    float_adm_preserve_error(&rc, vmaf_dictionary_free(&s->feature_name_dict));
    float_adm_preserve_error(&rc, vmaf_cuda_module_unload(fex->cu_state, &s->module));
    return rc;
}

/* float_adm_init_reference - the constants the reference derives per frame,
 * taken from its own routines so they cannot drift from it (ADR-1420).
 *
 * The CSF weights come from adm_csf_rfactor_s() with the options float_adm.c
 * passes: no per-scale override (this twin does not declare adm_f1sN /
 * adm_f2sN) and the reference's luminance level. In the Watson-97 mode this
 * twin supports the weights ignore adm_csf_scale / adm_csf_diag_scale, as on
 * the CPU (ADR-1214).
 *
 * The decouple's division needs nothing from the host: the reference divides
 * and the device divides (`__fdiv_rn()`), both the IEEE fp32 quotient
 * (ADR-1442).
 */
static void float_adm_init_reference(FloatAdmStateCuda *s)
{
    for (int scale = 0; scale < FADM_SCALES; scale++) {
        adm_csf_rfactor_s(scale, s->adm_norm_view_dist, s->adm_ref_display_height, s->adm_csf_mode,
                          DEFAULT_ADM_CSF_LUMINANCE_LEVEL, s->adm_csf_scale, s->adm_csf_diag_scale,
                          -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, s->rfactor[scale]);
    }
    s->cos_1deg_sq = adm_decouple_cos_1deg_sq_s();
}

/* float_adm_resolve_module - load the fatbin and look up every kernel. The
 * caller holds the context. */
static int float_adm_resolve_module(FloatAdmStateCuda *s, CudaFunctions *cu_f)
{
    const struct {
        CUfunction *function;
        const char *name;
    } kernels[] = {
        {&s->func_dwt_vert, "float_adm_dwt_vert"},
        {&s->func_dwt_hori, "float_adm_dwt_hori"},
        {&s->func_decouple_csf, "float_adm_decouple_csf"},
        {&s->func_terms, "float_adm_terms"},
        {&s->func_row_sums, "float_adm_row_sums"},
    };
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&s->module, float_adm_score_ptx));
    for (size_t i = 0u; i < sizeof(kernels) / sizeof(kernels[0]); i++) {
        CHECK_CUDA_RETURN(cu_f,
                          cuModuleGetFunction(kernels[i].function, s->module, kernels[i].name));
    }
    return 0;
}

/* float_adm_load_kernels - module load plus every kernel handle lookup.
 *
 * The context is popped again on every path; the caller unwinds.
 */
static int float_adm_load_kernels(VmafFeatureExtractor *fex, FloatAdmStateCuda *s)
{
    CudaFunctions *cu_f = fex->cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(fex->cu_state->ctx));
    const int err = float_adm_resolve_module(s, cu_f);
    const CUresult popped = cu_f->cuCtxPopCurrent(NULL);
    return err ? err : vmaf_cuda_result_to_errno((int)popped);
}

/* float_adm_alloc_reduction_buffers - what the stages past the DWT need.
 *
 * The CSF buffers and the term buffer are reused per scale and sized for
 * scale 0, whose bands and reduced region are the largest.
 */
static int float_adm_alloc_reduction_buffers(VmafFeatureExtractor *fex, FloatAdmStateCuda *s)
{
    const size_t csf_bytes =
        (size_t)FADM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(float);
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_a, csf_bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_fa, csf_bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_r, csf_bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->csf_fr, csf_bytes);

    const AdmBorderS *r0 = &s->region[0];
    const size_t term_bytes = (size_t)FADM_TERM_SLOTS * (size_t)(r0->right - r0->left) *
                              (size_t)(r0->bottom - r0->top) * sizeof(float);
    const size_t row_bytes = s->row_floats * sizeof(float);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->terms, term_bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->rows, row_bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_host_alloc(fex->cu_state, (void **)&s->rows_host, row_bytes);
    return ret;
}

/* float_adm_alloc_device_buffers - every device and pinned-host allocation.
 *
 * Allocations stop at the first error. The caller releases any partial
 * ownership through float_adm_init_unwind().
 */
static int float_adm_alloc_device_buffers(VmafFeatureExtractor *fex, FloatAdmStateCuda *s,
                                          unsigned w, unsigned h, unsigned bpc)
{
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t raw_bytes = (size_t)w * h * bpp;
    int ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->src_ref, raw_bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->src_dis, raw_bytes);

    /* DWT scratch sized at scale 0 (worst case). */
    const size_t dwt_bytes = (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(float);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->dwt_tmp_ref, dwt_bytes);
    if (!ret)
        ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->dwt_tmp_dis, dwt_bytes);

    /* Per-scale band buffers — 4 bands x buf_stride x half_h. The
     * scale-(s+1) DWT vert kernel reads scale-s's LL band, so each
     * scale needs its own ref_band/dis_band buffer. */
    for (int scale = 0; scale < FADM_SCALES; scale++) {
        const size_t band_bytes =
            (size_t)4u * s->buf_stride * s->scale_half_h[scale] * sizeof(float);
        if (!ret)
            ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->ref_band[scale], band_bytes);
        if (!ret)
            ret = vmaf_cuda_buffer_alloc(fex->cu_state, &s->dis_band[scale], band_bytes);
    }

    if (!ret)
        ret = float_adm_alloc_reduction_buffers(fex, s);
    return ret;
}

static int init_fex_cuda(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatAdmStateCuda *s = fex->priv;

    /* Same frame-size bound as the CPU float_adm, checked before any device
     * resource is claimed: below 17 pixels the scale-3 bands have one
     * sample. */
    const int size_err = adm_frame_size_check("float_adm_cuda", w, h);
    if (size_err)
        return size_err;

    if (s->adm_csf_mode != 0)
        return -EINVAL;

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);
    float_adm_init_reference(s);

    int err = vmaf_cuda_kernel_lifecycle_init(&s->lc, fex->cu_state);
    if (!err)
        err = float_adm_load_kernels(fex, s);
    if (!err)
        err = float_adm_alloc_device_buffers(fex, s, w, h, bpc);
    if (err)
        return float_adm_init_unwind(fex, s, err);

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return float_adm_init_unwind(fex, s, -ENOMEM);
    return 0;
}

/* FloatAdmScalePass - the per-frame and per-scale constants that the kernel
 * launches of submit_fex_cuda share. */
typedef struct FloatAdmScalePass {
    CUstream stream;
    ptrdiff_t raw_stride;
    CUdeviceptr ref_raw;
    CUdeviceptr dis_raw;
    CUdeviceptr dwt_ref;
    CUdeviceptr dwt_dis;
    CUdeviceptr ref_band;
    CUdeviceptr dis_band;
    CUdeviceptr parent_ref_band;
    CUdeviceptr parent_dis_band;
    float scaler;
    float pixel_offset;
    unsigned bpc;
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
} FloatAdmScalePass;

/* fadm_init_pass - the per-frame half of FloatAdmScalePass. */
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
    p->buf_stride = (int)s->buf_stride;
}

/* fadm_set_pass_scale - the per-scale half of FloatAdmScalePass. */
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
     * scale_w[scale-1] (full parent image dims). */
    p->parent_w = (scale > 0) ? (int)s->scale_w[scale] : 0;
    p->parent_h = (scale > 0) ? (int)s->scale_h[scale] : 0;
    p->parent_half_h = (scale > 0) ? (int)s->scale_half_h[scale - 1] : 0;
    p->parent_buf_stride = (int)s->buf_stride;

    p->ref_band = (CUdeviceptr)s->ref_band[scale]->data;
    p->dis_band = (CUdeviceptr)s->dis_band[scale]->data;
    p->parent_ref_band = (scale > 0) ? (CUdeviceptr)s->ref_band[scale - 1]->data : (CUdeviceptr)0;
    p->parent_dis_band = (scale > 0) ? (CUdeviceptr)s->dis_band[scale - 1]->data : (CUdeviceptr)0;
}

/* fadm_copy_luma - one luma plane into its tightly packed raw buffer. */
static int fadm_copy_luma(const FloatAdmStateCuda *s, CudaFunctions *cu_f, const VmafPicture *pic,
                          const VmafCudaBuffer *raw, const FloatAdmScalePass *p)
{
    const CUDA_MEMCPY2D copy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = (CUdeviceptr)pic->data[0],
        .srcPitch = pic->stride[0],
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = (CUdeviceptr)raw->data,
        .dstPitch = (size_t)p->raw_stride,
        .WidthInBytes = (size_t)p->raw_stride,
        .Height = s->height,
    };
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, p->stream));
    return 0;
}

/* fadm_submit_upload - stage the two luma planes on the picture stream. */
static int fadm_submit_upload(const FloatAdmStateCuda *s, CudaFunctions *cu_f,
                              const VmafPicture *ref_pic, const VmafPicture *dist_pic,
                              const FloatAdmScalePass *p)
{
    const int err = fadm_copy_luma(s, cu_f, ref_pic, s->src_ref, p);
    if (err)
        return err;
    return fadm_copy_luma(s, cu_f, dist_pic, s->src_dis, p);
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

/* The band block every kernel past the DWT takes. */
static FloatAdmCudaBands fadm_bands(const FloatAdmStateCuda *s, const FloatAdmScalePass *p)
{
    FloatAdmCudaBands b = {
        .ref_band = (uint64_t)p->ref_band,
        .dis_band = (uint64_t)p->dis_band,
        .csf_a = (uint64_t)s->csf_a->data,
        .csf_fa = (uint64_t)s->csf_fa->data,
        .csf_r = (uint64_t)s->csf_r->data,
        .csf_fr = (uint64_t)s->csf_fr->data,
        .half_w = p->half_w,
        .half_h = p->half_h,
        .buf_stride = p->buf_stride,
    };
    memcpy(b.rfactor, s->rfactor[p->scale], sizeof(b.rfactor));
    return b;
}

/* fadm_launch_decouple - stage 2: decouple, then the CSF of both parts. */
static int fadm_launch_decouple(CudaFunctions *cu_f, const FloatAdmStateCuda *s,
                                const FloatAdmScalePass *p)
{
    FloatAdmCudaDecoupleArgs args = {
        .bands = fadm_bands(s, p),
        .adm_enhn_gain_limit = s->adm_enhn_gain_limit,
        .cos_1deg_sq = s->cos_1deg_sq,
        .pad_ = 0u,
    };
    void *params[] = {&args};
    const unsigned gx = ((unsigned)p->half_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = ((unsigned)p->half_h + FADM_BY - 1u) / FADM_BY;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_decouple_csf, gx, gy, 1, FADM_BX, FADM_BY, 1, 0,
                                           p->stream, params, NULL));
    return 0;
}

/* fadm_launch_reductions - stages 3 and 4: the per-sample terms of the three
 * reductions over the reduced region, then one sum per row and slot into this
 * scale's span of `rows`. */
static int fadm_launch_reductions(CudaFunctions *cu_f, const FloatAdmStateCuda *s,
                                  const FloatAdmScalePass *p)
{
    const AdmBorderS *r = &s->region[p->scale];
    const unsigned region_w = (unsigned)(r->right - r->left);
    const unsigned region_h = (unsigned)(r->bottom - r->top);
    FloatAdmCudaTermArgs term_args = {
        .bands = fadm_bands(s, p),
        .terms = (uint64_t)s->terms->data,
        .left = r->left,
        .top = r->top,
        .region_w = region_w,
        .region_h = region_h,
        .p_norm = (float)s->adm_p_norm,
        .is_cube = (s->adm_p_norm == 3.0) ? 1u : 0u,
        .bypass_cm = (s->adm_bypass_cm != 0) ? 1u : 0u,
    };
    void *term_params[] = {&term_args};
    const unsigned gx = (region_w + FADM_BX - 1u) / FADM_BX;
    const unsigned gy = (region_h + FADM_BY - 1u) / FADM_BY;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_terms, gx, gy, 1, FADM_BX, FADM_BY, 1, 0,
                                           p->stream, term_params, NULL));

    FloatAdmCudaRowArgs row_args = {
        .terms = (uint64_t)s->terms->data,
        .rows = (uint64_t)s->rows->data + s->row_offset[p->scale] * sizeof(float),
        .region_w = region_w,
        .region_h = region_h,
    };
    void *row_params[] = {&row_args};
    const unsigned sums = FADM_TERM_SLOTS * region_h;
    const unsigned blocks = (sums + FADM_ROW_THREADS - 1u) / FADM_ROW_THREADS;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(s->func_row_sums, blocks, 1, 1, FADM_ROW_THREADS, 1, 1,
                                           0, p->stream, row_params, NULL));
    return 0;
}

/* fadm_submit_scale - the five kernel launches one scale needs. */
static int fadm_submit_scale(CudaFunctions *cu_f, const FloatAdmStateCuda *s,
                             const FloatAdmScalePass *p)
{
    int err = fadm_launch_dwt_vert(cu_f, s, p);
    if (err)
        return err;
    err = fadm_launch_dwt_hori(cu_f, s, p);
    if (err)
        return err;
    err = fadm_launch_decouple(cu_f, s, p);
    if (err)
        return err;
    return fadm_launch_reductions(cu_f, s, p);
}

/* fadm_submit_download - sync to the secondary stream and copy the row sums. */
static int fadm_submit_download(VmafFeatureExtractor *fex, FloatAdmStateCuda *s,
                                CudaFunctions *cu_f, CUstream pic_stream)
{
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(s->lc.submit, pic_stream));
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(s->lc.str, s->lc.submit, CU_EVENT_WAIT_DEFAULT));
    CHECK_CUDA_RETURN(cu_f, cuMemcpyDtoHAsync(s->rows_host, (CUdeviceptr)s->rows->data,
                                              s->row_floats * sizeof(float), s->lc.str));
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

    for (int scale = 0; scale < FADM_SCALES; scale++) {
        fadm_set_pass_scale(s, &pass, scale);
        err = fadm_submit_scale(cu_f, s, &pass);
        if (err)
            return err;
    }

    /* Sync over to the secondary stream + D2H copy of the row sums. */
    return fadm_submit_download(fex, s, cu_f, pic_stream);
}

/* FloatAdmPooled - what the per-scale pooling loop produces: compute_adm()'s
 * `scores`, `num`, `den`, `aim_num` and `aim_den`. */
typedef struct FloatAdmPooled {
    double scores[8];
    double score_num;
    double score_den;
    double aim_num;
    double aim_den;
} FloatAdmPooled;

/* FloatAdmFinal - the headline scores, plus the numerator and denominator
 * after the numden_limit floor (the debug features report the floored
 * values, so they have to survive the split). */
typedef struct FloatAdmFinal {
    double score;
    double aim;
    double adm3;
    double score_num;
    double score_den;
} FloatAdmFinal;

/* fadm_pool_scales - compute_adm()'s scale loop past the kernels.
 *
 * The frame accumulators are the reference's: one fp32 value per band that
 * the row sums are added to top to bottom. Each scale is then concluded by
 * the reference's own adm_pool_bands_s(), with the noise weight for the
 * denominator and the adm2 numerator and with none for the AIM numerator.
 */
static void fadm_pool_scales(const FloatAdmStateCuda *s, FloatAdmPooled *o)
{
    o->score_num = 0.0;
    o->score_den = 0.0;
    o->aim_num = 0.0;
    o->aim_den = 0.0;
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

/* fadm_final_scores - numden floor, adm2, AIM and ADM3, as compute_adm() and
 * float_adm.c's extract() conclude. score_num / score_den are carried out
 * floored, which is the value the debug features report. */
static int fadm_final_scores(const FloatAdmStateCuda *s, const FloatAdmPooled *o, FloatAdmFinal *f,
                             unsigned index)
{
    f->score_num = o->score_num;
    f->score_den = o->score_den;
    const int w = (int)s->width;
    const int h = (int)s->height;
    const double numden_limit = 1e-10 * (w * h) / (1920.0 * 1080.0);
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
    double scale_scores[FADM_SCALES];
    int err =
        vmaf_adm_scale_ratios_named("float_adm_cuda", index, o->scores, FADM_SCALES, scale_scores);
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
     * The D2H copy of rows_host executes on s->lc.str.  The batch
     * drain (ADR-0242) waits on lc.finished (recorded on lc.str AFTER
     * the D2H), but the drain_stream synchronise does not block the
     * calling CPU thread until lc.str itself has retired the memcpy —
     * it only guarantees lc.finished has been signalled from the
     * driver's perspective.  An explicit cuStreamSynchronize on lc.str
     * is the conservative fix: it costs one per-frame CPU stall and
     * eliminates the window entirely. */
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(s->lc.str));

    FloatAdmPooled pooled = {0};
    fadm_pool_scales(s, &pooled);

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
    if (ret)
        return ret;

    ret = float_adm_release_buffers(fex, s, 0);
    float_adm_preserve_error(&ret, vmaf_dictionary_free(&s->feature_name_dict));
    float_adm_preserve_error(&ret, vmaf_cuda_module_unload(fex->cu_state, &s->module));
    return ret;
}

static const char *provided_features[] = {
    "VMAF_feature_adm2_score", "VMAF_feature_adm_scale0_score", "VMAF_feature_adm_scale1_score",
    "VMAF_feature_adm_scale2_score", "VMAF_feature_adm_scale3_score",
    /* ADR-0574: AIM and ADM3 sub-features. */
    "VMAF_feature_aim_score", "VMAF_feature_adm3_score", "adm_scale0", "adm_num", "adm_den",
    "adm_num_scale0", "adm_den_scale0", "adm_num_scale1", "adm_den_scale1", "adm_num_scale2",
    "adm_den_scale2", "adm_num_scale3", "adm_den_scale3", NULL};

// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required; referenced as `extern VmafFeatureExtractor vmaf_fex_float_adm_cuda` by feature_extractor.cpp's feature_extractor_list[] (ADR-0278).
VmafFeatureExtractor vmaf_fex_float_adm_cuda = {
    .name = "float_adm_cuda",
    .init = init_fex_cuda,
    .submit = submit_fex_cuda,
    .collect = collect_fex_cuda,
    .close = close_fex_cuda,
    .options = options,
    .priv_size = sizeof(FloatAdmStateCuda),
    .provided_features = provided_features,
    .unsuffixed_debug_key = "adm",
    .flags = VMAF_FEATURE_EXTRACTOR_CUDA,
};

/* NOLINTEND(modernize-use-nullptr) */
