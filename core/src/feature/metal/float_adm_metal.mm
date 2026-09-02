/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  float_adm feature extractor on the Metal backend.
 *  Port of `core/src/feature/cuda/float_adm_cuda.c` (CUDA twin,
 *  ADR-0192 / ADR-0202 / ADR-0574) — same six-stage DWT → CSF →
 *  decouple → CM pipeline, same Watson-97 (csf_mode 0) CSF model, same
 *  cross-band CM threshold, same AIM (Anchored Impairment Metric) pass,
 *  same host-side double-precision reduction and cube-root / p-norm
 *  pooling.
 *
 *  Algorithm summary (per frame, 4 scales):
 *    Stage 0  float_adm_dwt_vert_{8,16}bpc — DWT vertical, raw at scale 0
 *    Stage 1  float_adm_dwt_hori          — DWT horizontal → 4 sub-bands
 *    Stage 2  float_adm_decouple_csf      — decouple_a → csf_a / csf_f
 *    Stage 3  float_adm_csf_cm            — CSF denom + CM (slots 0..5)
 *    Stage 2b float_adm_csf_r             — decouple_r → csf_a_aim / _f_aim
 *    Stage 3b float_adm_aim_cm            — AIM CM numerator (slots 6..8)
 *  Host collect() reduces accum slots across threadgroups in double and
 *  applies the cube-root pooling (matches float_adm_cuda.c::collect).
 *
 *  Multi-scale buffer strategy: every device buffer (raw src, dwt scratch,
 *  per-scale band buffers, csf scratch, per-scale accumulators) is
 *  allocated once in init() and reused for every frame — the same
 *  posture as float_ms_ssim_metal.mm. Band buffers are per-scale because
 *  the scale-(s+1) DWT-vert reads the scale-s LL band; everything else is
 *  sized to scale 0 (worst case) and reused across scales.
 *
 *  Metallib resolution: embedded __TEXT,__metallib blob, same pattern as
 *  every other Metal feature extractor.
 *
 *  Parity: places=4 (1e-4) vs the CPU `float_adm` at default options
 *  (ADR-0214 cross-backend gate; same bound the CUDA twin holds — see
 *  core/test/test_cuda_float_adm_parity.c). Only csf_mode 0 (Watson-97,
 *  the CPU default) is supported; other modes return -EINVAL at init.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

/* feature_extractor.h uses `#if defined(__cplusplus)` to include <atomic>
 * (Xcode 16.4 / macOS 15 libc++ emits "templates must have C++ linkage"
 * when that header is pulled into an extern "C" block — ADR-fix macOS-Metal). */
#include "feature_extractor.h"

extern "C" {
#include "dict.h"
#include "feature_collector.h"
#include "feature_name.h"
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
#include "../adm_options.h"
}

extern "C" {
extern const unsigned char libvmaf_metallib_start[] __asm("section$start$__TEXT$__metallib");
extern const unsigned char libvmaf_metallib_end[]   __asm("section$end$__TEXT$__metallib");
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FADM_NUM_SCALES 4
#define FADM_NUM_BANDS  3
#define FADM_BX         16
#define FADM_BY         16
#define FADM_BORDER_FACTOR 0.1
#define FADM_ACCUM_SLOTS   9

/* Geometry uniform mirroring `FadmDims` in float_adm.metal. */
typedef struct FadmDimsHost {
    int32_t scale;
    int32_t cur_w;
    int32_t cur_h;
    int32_t half_w;
    int32_t half_h;
    int32_t buf_stride;
    int32_t parent_w;
    int32_t parent_h;
    int32_t parent_half_h;
    int32_t parent_buf_stride;
    uint32_t bpc;
    uint32_t _pad0;
} FadmDimsHost;

/* CSF / CM uniform mirroring `FadmCsf` in float_adm.metal. */
typedef struct FadmCsfHost {
    int32_t active_left;
    int32_t active_top;
    int32_t active_right;
    int32_t active_bottom;
    float rfactor_h;
    float rfactor_v;
    float rfactor_d;
    float gain_limit;
    float scaler;
    float pixel_offset;
    uint32_t _pad0;
    uint32_t _pad1;
} FadmCsfHost;

typedef struct FloatAdmStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;

    /* Pipeline states for the six kernels. */
    void *pso_dwt_vert_8;
    void *pso_dwt_vert_16;
    void *pso_dwt_hori;
    void *pso_decouple_csf;
    void *pso_csf_cm;
    void *pso_csf_r;
    void *pso_aim_cm;

    /* Reused device buffers. */
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

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned buf_stride;
    float scaler;

    unsigned scale_w[FADM_NUM_SCALES];
    unsigned scale_h[FADM_NUM_SCALES];
    unsigned scale_half_w[FADM_NUM_SCALES];
    unsigned scale_half_h[FADM_NUM_SCALES];
    unsigned wg_count[FADM_NUM_SCALES];

    float rfactor[FADM_NUM_SCALES * 3];

    /* Options — same defaults as float_adm.c. */
    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    int adm_bypass_cm;
    int adm_adm3_apply_hm;
    double adm_p_norm;
    double adm_dlm_weight;
    double adm_min_val;
    int adm_skip_aim_scale;
    bool adm_skip_scale0;

    unsigned index;
    VmafDictionary *feature_name_dict;
} FloatAdmStateMetal;

static const VmafOption options[] = {
    {.name = "debug",
     .help = "debug mode: enable additional output",
     .offset = offsetof(FloatAdmStateMetal, debug),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false}},
    {.name = "adm_enhn_gain_limit",
     .alias = "egl",
     .help = "enhancement gain imposed on adm, must be >= 1.0",
     .offset = offsetof(FloatAdmStateMetal, adm_enhn_gain_limit),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_ENHN_GAIN_LIMIT},
     .min = 1.0,
     .max = DEFAULT_ADM_ENHN_GAIN_LIMIT,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_norm_view_dist",
     .alias = "nvd",
     .help = "normalized viewing distance = viewing distance / ref display's physical height",
     .offset = offsetof(FloatAdmStateMetal, adm_norm_view_dist),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_NORM_VIEW_DIST},
     .min = 0.75,
     .max = 24.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_ref_display_height",
     .alias = "rdf",
     .help = "reference display height in pixels",
     .offset = offsetof(FloatAdmStateMetal, adm_ref_display_height),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = DEFAULT_ADM_REF_DISPLAY_HEIGHT},
     .min = 1,
     .max = 4320,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_mode",
     .alias = "csf",
     .help = "contrast sensitivity function (mode 0 / Watson-97 only on Metal)",
     .offset = offsetof(FloatAdmStateMetal, adm_csf_mode),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = DEFAULT_ADM_CSF_MODE},
     .min = 0,
     .max = 9,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_scale",
     .alias = "scf",
     .help = "scale factor for the CSF",
     .offset = offsetof(FloatAdmStateMetal, adm_csf_scale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_CSF_SCALE},
     .min = 0.0,
     .max = 50.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_diag_scale",
     .alias = "scfd",
     .help = "scale factor for the CSF diag",
     .offset = offsetof(FloatAdmStateMetal, adm_csf_diag_scale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_CSF_DIAG_SCALE},
     .min = 0.0,
     .max = 50.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_noise_weight",
     .alias = "nw",
     .help = "noise weight",
     .offset = offsetof(FloatAdmStateMetal, adm_noise_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_NOISE_WEIGHT},
     .min = 0.0,
     .max = 1500.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_bypass_cm",
     .alias = "bcm",
     .help = "bypass contrast masking (CM)",
     .offset = offsetof(FloatAdmStateMetal, adm_bypass_cm),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = 0},
     .min = 0,
     .max = 1,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_adm3_apply_hm",
     .alias = "aah",
     .help = "apply harmonic mean to combine DLM and AIM",
     .offset = offsetof(FloatAdmStateMetal, adm_adm3_apply_hm),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false},
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_p_norm",
     .alias = "apn",
     .help = "p-norm for energy vector",
     .offset = offsetof(FloatAdmStateMetal, adm_p_norm),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 3.0},
     .min = 1.0,
     .max = 20.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_dlm_weight",
     .alias = "dlmw",
     .help = "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
     .offset = offsetof(FloatAdmStateMetal, adm_dlm_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 0.5},
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_min_val",
     .alias = "min",
     .help = "minimum value allowed; lower values will be clipped to this value",
     .offset = offsetof(FloatAdmStateMetal, adm_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_MIN_VAL},
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_skip_aim_scale",
     .alias = "sasc",
     .help = "when set, skip AIM calculations for that scale",
     .offset = offsetof(FloatAdmStateMetal, adm_skip_aim_scale),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = -1},
     .min = -1,
     .max = 3,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_skip_scale0",
     .alias = "ssz",
     .help = "skip the calculation of scale 0",
     .offset = offsetof(FloatAdmStateMetal, adm_skip_scale0),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false},
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {0},
};

/* ---- DWT quant-step (CSF rfactor) — bit-for-bit replica of the CUDA
 * twin's fadm_dwt_quant_step (= dwt_quant_step in adm_tools.h). ---- */
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

static void compute_per_scale_dims(FloatAdmStateMetal *s)
{
    unsigned cw = s->width;
    unsigned ch = s->height;
    for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
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

static int build_pipelines(FloatAdmStateMetal *s, id<MTLDevice> device)
{
    const size_t blob_size = (size_t)(libvmaf_metallib_end - libvmaf_metallib_start);
    if (blob_size == 0) { return -ENODEV; }

    dispatch_data_t data = dispatch_data_create(
        libvmaf_metallib_start, blob_size,
        dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if (data == NULL) { return -ENOMEM; }

    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithData:data error:&err];
    if (lib == nil) { return -ENODEV; }

    id<MTLFunction> fn_dv8 = [lib newFunctionWithName:@"float_adm_dwt_vert_8bpc"];
    id<MTLFunction> fn_dv16 = [lib newFunctionWithName:@"float_adm_dwt_vert_16bpc"];
    id<MTLFunction> fn_dh = [lib newFunctionWithName:@"float_adm_dwt_hori"];
    id<MTLFunction> fn_dc = [lib newFunctionWithName:@"float_adm_decouple_csf"];
    id<MTLFunction> fn_cc = [lib newFunctionWithName:@"float_adm_csf_cm"];
    id<MTLFunction> fn_cr = [lib newFunctionWithName:@"float_adm_csf_r"];
    id<MTLFunction> fn_ac = [lib newFunctionWithName:@"float_adm_aim_cm"];
    if (fn_dv8 == nil || fn_dv16 == nil || fn_dh == nil || fn_dc == nil || fn_cc == nil ||
        fn_cr == nil || fn_ac == nil) {
        return -ENODEV;
    }

    id<MTLComputePipelineState> p_dv8 = [device newComputePipelineStateWithFunction:fn_dv8
                                                                             error:&err];
    id<MTLComputePipelineState> p_dv16 = [device newComputePipelineStateWithFunction:fn_dv16
                                                                              error:&err];
    id<MTLComputePipelineState> p_dh = [device newComputePipelineStateWithFunction:fn_dh
                                                                            error:&err];
    id<MTLComputePipelineState> p_dc = [device newComputePipelineStateWithFunction:fn_dc
                                                                            error:&err];
    id<MTLComputePipelineState> p_cc = [device newComputePipelineStateWithFunction:fn_cc
                                                                            error:&err];
    id<MTLComputePipelineState> p_cr = [device newComputePipelineStateWithFunction:fn_cr
                                                                            error:&err];
    id<MTLComputePipelineState> p_ac = [device newComputePipelineStateWithFunction:fn_ac
                                                                            error:&err];
    if (p_dv8 == nil || p_dv16 == nil || p_dh == nil || p_dc == nil || p_cc == nil ||
        p_cr == nil || p_ac == nil) {
        return -ENODEV;
    }

    s->pso_dwt_vert_8 = (__bridge_retained void *)p_dv8;
    s->pso_dwt_vert_16 = (__bridge_retained void *)p_dv16;
    s->pso_dwt_hori = (__bridge_retained void *)p_dh;
    s->pso_decouple_csf = (__bridge_retained void *)p_dc;
    s->pso_csf_cm = (__bridge_retained void *)p_cc;
    s->pso_csf_r = (__bridge_retained void *)p_cr;
    s->pso_aim_cm = (__bridge_retained void *)p_ac;
    return 0;
}

/* Release every retained PSO. Safe on a partially-built state. */
static void release_psos(FloatAdmStateMetal *s)
{
    void **psos[] = {&s->pso_aim_cm,       &s->pso_csf_r,   &s->pso_csf_cm,
                     &s->pso_decouple_csf, &s->pso_dwt_hori, &s->pso_dwt_vert_16,
                     &s->pso_dwt_vert_8};
    for (size_t i = 0; i < sizeof(psos) / sizeof(psos[0]); ++i) {
        if (*psos[i]) {
            (void)(__bridge_transfer id<MTLComputePipelineState>)(*psos[i]);
            *psos[i] = NULL;
        }
    }
}

/* Release every retained MTLBuffer. Safe on a partially-allocated state. */
static void release_buffers(FloatAdmStateMetal *s)
{
    for (int i = 0; i < FADM_NUM_SCALES; ++i) {
        if (s->ref_band[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->ref_band[i];
            s->ref_band[i] = NULL;
        }
        if (s->dis_band[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->dis_band[i];
            s->dis_band[i] = NULL;
        }
        if (s->accum[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->accum[i];
            s->accum[i] = NULL;
        }
    }
    void **single[] = {&s->src_ref, &s->src_dis,    &s->dwt_tmp_ref, &s->dwt_tmp_dis,
                       &s->csf_a,   &s->csf_f,       &s->csf_a_aim,   &s->csf_f_aim};
    for (size_t i = 0; i < sizeof(single) / sizeof(single[0]); ++i) {
        if (*single[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)(*single[i]);
            *single[i] = NULL;
        }
    }
}

static int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                          unsigned w, unsigned h)
{
    (void)pix_fmt;
    FloatAdmStateMetal *s = (FloatAdmStateMetal *)fex->priv;

    /* Watson-97 (mode 0) only — matches the CUDA twin (other CSF modes
     * would need the Barten / ADM sensitivity tables ported to MSL). */
    if (s->adm_csf_mode != 0) { return -EINVAL; }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);

    if (bpc <= 8u) { s->scaler = 1.0f; }
    else if (bpc == 10u) { s->scaler = 4.0f; }
    else if (bpc == 12u) { s->scaler = 16.0f; }
    else { s->scaler = 256.0f; }

    for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
        const float f1 =
            fadm_dwt_quant_step(scale, 1, s->adm_norm_view_dist, s->adm_ref_display_height);
        const float f2 =
            fadm_dwt_quant_step(scale, 2, s->adm_norm_view_dist, s->adm_ref_display_height);
        s->rfactor[scale * 3 + 0] = (float)s->adm_csf_scale / f1;
        s->rfactor[scale * 3 + 1] = (float)s->adm_csf_scale / f1;
        s->rfactor[scale * 3 + 2] = (float)s->adm_csf_diag_scale / f2;
    }

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    {
        void *dh = vmaf_metal_context_device_handle(s->ctx);
        if (dh == NULL) { err = -ENODEV; goto fail_lc; }
        id<MTLDevice> device = (__bridge id<MTLDevice>)dh;

        const size_t bpp = (bpc <= 8u) ? 1u : 2u;
        const size_t raw_bytes = (size_t)w * h * bpp;
        id<MTLBuffer> sr = [device newBufferWithLength:raw_bytes
                                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> sd = [device newBufferWithLength:raw_bytes
                                               options:MTLResourceStorageModeShared];
        if (sr == nil || sd == nil) { err = -ENOMEM; goto fail_bufs; }
        s->src_ref = (__bridge_retained void *)sr;
        s->src_dis = (__bridge_retained void *)sd;

        const size_t dwt_bytes =
            (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(float);
        id<MTLBuffer> dr = [device newBufferWithLength:dwt_bytes
                                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> dd = [device newBufferWithLength:dwt_bytes
                                               options:MTLResourceStorageModeShared];
        if (dr == nil || dd == nil) { err = -ENOMEM; goto fail_bufs; }
        s->dwt_tmp_ref = (__bridge_retained void *)dr;
        s->dwt_tmp_dis = (__bridge_retained void *)dd;

        for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
            const size_t band_bytes =
                (size_t)4u * s->buf_stride * s->scale_half_h[scale] * sizeof(float);
            id<MTLBuffer> br = [device newBufferWithLength:band_bytes
                                                   options:MTLResourceStorageModeShared];
            id<MTLBuffer> bd = [device newBufferWithLength:band_bytes
                                                   options:MTLResourceStorageModeShared];
            if (br == nil || bd == nil) { err = -ENOMEM; goto fail_bufs; }
            s->ref_band[scale] = (__bridge_retained void *)br;
            s->dis_band[scale] = (__bridge_retained void *)bd;
        }

        const size_t csf_bytes =
            (size_t)FADM_NUM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(float);
        id<MTLBuffer> ca = [device newBufferWithLength:csf_bytes
                                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> cf = [device newBufferWithLength:csf_bytes
                                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> caa = [device newBufferWithLength:csf_bytes
                                                options:MTLResourceStorageModeShared];
        id<MTLBuffer> cfa = [device newBufferWithLength:csf_bytes
                                                options:MTLResourceStorageModeShared];
        if (ca == nil || cf == nil || caa == nil || cfa == nil) { err = -ENOMEM; goto fail_bufs; }
        s->csf_a = (__bridge_retained void *)ca;
        s->csf_f = (__bridge_retained void *)cf;
        s->csf_a_aim = (__bridge_retained void *)caa;
        s->csf_f_aim = (__bridge_retained void *)cfa;

        for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
            const int hh = (int)s->scale_half_h[scale];
            int top = (int)((double)hh * FADM_BORDER_FACTOR - 0.5);
            if (top < 0) { top = 0; }
            const int bottom = hh - top;
            const unsigned num_rows = (bottom > top) ? (unsigned)(bottom - top) : 1u;
            s->wg_count[scale] = 3u * num_rows;
            const size_t accum_bytes =
                (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS * sizeof(float);
            id<MTLBuffer> ac = [device newBufferWithLength:accum_bytes
                                                   options:MTLResourceStorageModeShared];
            if (ac == nil) { err = -ENOMEM; goto fail_bufs; }
            s->accum[scale] = (__bridge_retained void *)ac;
        }

        err = build_pipelines(s, device);
    }
    if (err != 0) { goto fail_pso; }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == NULL) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    release_psos(s);
fail_bufs:
    release_buffers(s);
fail_lc:
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
fail_ctx:
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = NULL;
    return err;
}

/* Copy a Y plane (respecting source stride) into the packed src buffer. */
static void fill_raw_plane(VmafPicture *pic, id<MTLBuffer> dst, unsigned w, unsigned h,
                           unsigned bpc)
{
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t row_bytes = (size_t)w * bpp;
    uint8_t *out = (uint8_t *)[dst contents];
    for (unsigned y = 0; y < h; ++y) {
        memcpy(out + (size_t)y * row_bytes,
               (const uint8_t *)pic->data[0] + (size_t)y * pic->stride[0], row_bytes);
    }
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    FloatAdmStateMetal *s = (FloatAdmStateMetal *)fex->priv;
    s->index = index;

    void *dh = vmaf_metal_context_device_handle(s->ctx);
    void *qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == NULL || qh == NULL) { return -ENODEV; }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)qh;

    id<MTLBuffer> src_ref = (__bridge id<MTLBuffer>)s->src_ref;
    id<MTLBuffer> src_dis = (__bridge id<MTLBuffer>)s->src_dis;
    fill_raw_plane(ref_pic, src_ref, s->width, s->height, s->bpc);
    fill_raw_plane(dist_pic, src_dis, s->width, s->height, s->bpc);

    id<MTLBuffer> dwt_ref = (__bridge id<MTLBuffer>)s->dwt_tmp_ref;
    id<MTLBuffer> dwt_dis = (__bridge id<MTLBuffer>)s->dwt_tmp_dis;
    id<MTLBuffer> csf_a = (__bridge id<MTLBuffer>)s->csf_a;
    id<MTLBuffer> csf_f = (__bridge id<MTLBuffer>)s->csf_f;
    id<MTLBuffer> csf_a_aim = (__bridge id<MTLBuffer>)s->csf_a_aim;
    id<MTLBuffer> csf_f_aim = (__bridge id<MTLBuffer>)s->csf_f_aim;

    id<MTLComputePipelineState> pso_dv =
        (s->bpc <= 8u) ? (__bridge id<MTLComputePipelineState>)s->pso_dwt_vert_8
                       : (__bridge id<MTLComputePipelineState>)s->pso_dwt_vert_16;
    id<MTLComputePipelineState> pso_dh = (__bridge id<MTLComputePipelineState>)s->pso_dwt_hori;
    id<MTLComputePipelineState> pso_dc = (__bridge id<MTLComputePipelineState>)s->pso_decouple_csf;
    id<MTLComputePipelineState> pso_cc = (__bridge id<MTLComputePipelineState>)s->pso_csf_cm;
    id<MTLComputePipelineState> pso_cr = (__bridge id<MTLComputePipelineState>)s->pso_csf_r;
    id<MTLComputePipelineState> pso_ac = (__bridge id<MTLComputePipelineState>)s->pso_aim_cm;

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    /* Zero the accumulators (skipped-scale AIM slots must contribute 0). */
    {
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
            id<MTLBuffer> ac = (__bridge id<MTLBuffer>)s->accum[scale];
            [blit fillBuffer:ac
                       range:NSMakeRange(0, (size_t)s->wg_count[scale] * FADM_ACCUM_SLOTS *
                                                sizeof(float))
                       value:0];
        }
        [blit endEncoding];
    }

    const float gain_limit = (float)s->adm_enhn_gain_limit;

    for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
        const int cur_w = (int)s->scale_w[scale];
        const int cur_h = (int)s->scale_h[scale];
        const int half_w = (int)s->scale_half_w[scale];
        const int half_h = (int)s->scale_half_h[scale];

        FadmDimsHost d;
        memset(&d, 0, sizeof(d));
        d.scale = scale;
        d.cur_w = cur_w;
        d.cur_h = cur_h;
        d.half_w = half_w;
        d.half_h = half_h;
        d.buf_stride = (int)s->buf_stride;
        d.parent_w = (scale > 0) ? (int)s->scale_w[scale] : 0;
        d.parent_h = (scale > 0) ? (int)s->scale_h[scale] : 0;
        d.parent_half_h = (scale > 0) ? (int)s->scale_half_h[scale - 1] : 0;
        d.parent_buf_stride = (int)s->buf_stride;
        d.bpc = s->bpc;

        int top = (int)((double)half_h * FADM_BORDER_FACTOR - 0.5);
        int left = (int)((double)half_w * FADM_BORDER_FACTOR - 0.5);
        if (top < 0) { top = 0; }
        if (left < 0) { left = 0; }
        const int bottom = half_h - top;
        const int right = half_w - left;
        const int active_h = bottom - top;

        FadmCsfHost c;
        memset(&c, 0, sizeof(c));
        c.active_left = left;
        c.active_top = top;
        c.active_right = right;
        c.active_bottom = bottom;
        c.rfactor_h = s->rfactor[scale * 3 + 0];
        c.rfactor_v = s->rfactor[scale * 3 + 1];
        c.rfactor_d = s->rfactor[scale * 3 + 2];
        c.gain_limit = gain_limit;
        c.scaler = s->scaler;
        c.pixel_offset = -128.0f;

        id<MTLBuffer> ref_band = (__bridge id<MTLBuffer>)s->ref_band[scale];
        id<MTLBuffer> dis_band = (__bridge id<MTLBuffer>)s->dis_band[scale];
        id<MTLBuffer> parent_ref = (scale > 0) ? (__bridge id<MTLBuffer>)s->ref_band[scale - 1]
                                               : ref_band;
        id<MTLBuffer> parent_dis = (scale > 0) ? (__bridge id<MTLBuffer>)s->dis_band[scale - 1]
                                               : dis_band;
        id<MTLBuffer> accum = (__bridge id<MTLBuffer>)s->accum[scale];

        const MTLSize tg = MTLSizeMake(FADM_BX, FADM_BY, 1);

        /* Stage 0 — DWT vertical (z=2 ref/dis). */
        {
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:pso_dv];
            [enc setBuffer:(scale == 0 ? src_ref : parent_ref) offset:0 atIndex:0];
            [enc setBuffer:(scale == 0 ? src_dis : parent_dis) offset:0 atIndex:1];
            [enc setBuffer:dwt_ref offset:0 atIndex:2];
            [enc setBuffer:dwt_dis offset:0 atIndex:3];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            [enc setBytes:&c length:sizeof(c) atIndex:5];
            [enc setBuffer:parent_ref offset:0 atIndex:6];
            [enc setBuffer:parent_dis offset:0 atIndex:7];
            MTLSize grid = MTLSizeMake(((unsigned)cur_w + FADM_BX - 1u) / FADM_BX,
                                       ((unsigned)half_h + FADM_BY - 1u) / FADM_BY, 2);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
            [enc endEncoding];
        }

        /* Stage 1 — DWT horizontal (z=2 ref/dis). */
        {
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:pso_dh];
            [enc setBuffer:dwt_ref offset:0 atIndex:0];
            [enc setBuffer:dwt_dis offset:0 atIndex:1];
            [enc setBuffer:ref_band offset:0 atIndex:2];
            [enc setBuffer:dis_band offset:0 atIndex:3];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            MTLSize grid = MTLSizeMake(((unsigned)half_w + FADM_BX - 1u) / FADM_BX,
                                       ((unsigned)half_h + FADM_BY - 1u) / FADM_BY, 2);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
            [enc endEncoding];
        }

        /* Stage 2 — Decouple + CSF (decouple_a → csf_a, csf_f). */
        {
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:pso_dc];
            [enc setBuffer:ref_band offset:0 atIndex:0];
            [enc setBuffer:dis_band offset:0 atIndex:1];
            [enc setBuffer:csf_a offset:0 atIndex:2];
            [enc setBuffer:csf_f offset:0 atIndex:3];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            [enc setBytes:&c length:sizeof(c) atIndex:5];
            MTLSize grid = MTLSizeMake(((unsigned)half_w + FADM_BX - 1u) / FADM_BX,
                                       ((unsigned)half_h + FADM_BY - 1u) / FADM_BY, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
            [enc endEncoding];
        }

        /* Stage 3 — CSF denom + CM fused. 1D dispatch of 3*num_rows TGs. */
        {
            const unsigned num_rows = (unsigned)(active_h > 0 ? active_h : 1);
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:pso_cc];
            [enc setBuffer:ref_band offset:0 atIndex:0];
            [enc setBuffer:dis_band offset:0 atIndex:1];
            [enc setBuffer:csf_a offset:0 atIndex:2];
            [enc setBuffer:csf_f offset:0 atIndex:3];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            [enc setBytes:&c length:sizeof(c) atIndex:5];
            [enc setBuffer:accum offset:0 atIndex:8];
            MTLSize grid = MTLSizeMake(3u * num_rows, 1, 1);
            MTLSize tg1d = MTLSizeMake(FADM_BX * FADM_BY, 1, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg1d];
            [enc endEncoding];
        }

        /* Stage 2b — CSF on decouple_r (AIM pass). */
        {
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:pso_cr];
            [enc setBuffer:ref_band offset:0 atIndex:0];
            [enc setBuffer:dis_band offset:0 atIndex:1];
            [enc setBuffer:csf_a_aim offset:0 atIndex:2];
            [enc setBuffer:csf_f_aim offset:0 atIndex:3];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            [enc setBytes:&c length:sizeof(c) atIndex:5];
            MTLSize grid = MTLSizeMake(((unsigned)half_w + FADM_BX - 1u) / FADM_BX,
                                       ((unsigned)half_h + FADM_BY - 1u) / FADM_BY, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
            [enc endEncoding];
        }

        /* Stage 3b — AIM CM numerator. Skipped if adm_skip_aim_scale==scale. */
        if (s->adm_skip_aim_scale != scale) {
            const unsigned num_rows = (unsigned)(active_h > 0 ? active_h : 1);
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:pso_ac];
            [enc setBuffer:ref_band offset:0 atIndex:0];
            [enc setBuffer:dis_band offset:0 atIndex:1];
            [enc setBuffer:csf_a_aim offset:0 atIndex:2];
            [enc setBuffer:csf_f_aim offset:0 atIndex:3];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            [enc setBytes:&c length:sizeof(c) atIndex:5];
            [enc setBuffer:accum offset:0 atIndex:8];
            MTLSize grid = MTLSizeMake(3u * num_rows, 1, 1);
            MTLSize tg1d = MTLSizeMake(FADM_BX * FADM_BY, 1, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg1d];
            [enc endEncoding];
        }
    }

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    FloatAdmStateMetal *s = (FloatAdmStateMetal *)fex->priv;

    double cm_totals[FADM_NUM_SCALES][FADM_NUM_BANDS] = {{0.0}};
    double csf_totals[FADM_NUM_SCALES][FADM_NUM_BANDS] = {{0.0}};
    double aim_cm_totals[FADM_NUM_SCALES][FADM_NUM_BANDS] = {{0.0}};

    for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
        const float *slots = (const float *)[(__bridge id<MTLBuffer>)s->accum[scale] contents];
        const unsigned wg_count = s->wg_count[scale];
        for (unsigned wg = 0u; wg < wg_count; ++wg) {
            const float *p = slots + (size_t)wg * FADM_ACCUM_SLOTS;
            for (int b = 0; b < FADM_NUM_BANDS; ++b) {
                csf_totals[scale][b] += (double)p[b];
                cm_totals[scale][b] += (double)p[3 + b];
                aim_cm_totals[scale][b] += (double)p[6 + b];
            }
        }
    }

    double score_num = 0.0;
    double score_den = 0.0;
    double aim_num = 0.0;
    double aim_den = 0.0;
    double scores[8];
    for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
        const int hw = (int)s->scale_half_w[scale];
        const int hh = (int)s->scale_half_h[scale];
        int left = (int)((double)hw * FADM_BORDER_FACTOR - 0.5);
        int top = (int)((double)hh * FADM_BORDER_FACTOR - 0.5);
        if (left < 0) { left = 0; }
        if (top < 0) { top = 0; }
        const int right = hw - left;
        const int bottom = hh - top;
        const float area_cbrt = powf(
            (float)((bottom - top) * (right - left)) * (float)s->adm_noise_weight, 1.0f / 3.0f);
        float num_scale = 0.0f;
        float den_scale = 0.0f;
        for (int b = 0; b < FADM_NUM_BANDS; ++b) {
            num_scale += powf((float)cm_totals[scale][b], 1.0f / 3.0f) + area_cbrt;
            den_scale += powf((float)csf_totals[scale][b], 1.0f / 3.0f) + area_cbrt;
        }
        scores[2 * scale + 0] = num_scale;
        scores[2 * scale + 1] = den_scale;
        score_num += num_scale;
        score_den += den_scale;

        float aim_num_scale = 0.0f;
        for (int b = 0; b < FADM_NUM_BANDS; ++b) {
            aim_num_scale += powf((float)aim_cm_totals[scale][b], 1.0f / (float)s->adm_p_norm);
        }
        if (s->adm_skip_aim_scale != scale) {
            aim_den += den_scale;
            aim_num += aim_num_scale;
        }
    }

    const int w = (int)s->scale_w[0];
    const int h = (int)s->scale_h[0];
    const double numden_limit = 1e-2 * (double)(w * h) / (1920.0 * 1080.0);
    if (score_num < numden_limit) { score_num = 0.0; }
    if (score_den < numden_limit) { score_den = 0.0; }
    const double score = (score_den == 0.0) ? 1.0 : score_num / score_den;

    const double score_aim = (aim_den == 0.0) ? 1.0 : fmin(aim_num / aim_den, 1.0);
    double score_adm3;
    if (s->adm_adm3_apply_hm) {
        const double hm_denom = score + score_aim;
        score_adm3 = (hm_denom > 0.0) ? (2.0 * score * score_aim / hm_denom) : 0.0;
    } else {
        score_adm3 = score * s->adm_dlm_weight + (1.0 - score_aim) * (1.0 - s->adm_dlm_weight);
    }
    if (score_adm3 < s->adm_min_val) { score_adm3 = s->adm_min_val; }

    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_adm2_score", score, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_aim_score", score_aim, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict,
                                                   "VMAF_feature_adm3_score", score_adm3, index);

    if (s->adm_skip_scale0) {
        err |= vmaf_feature_collector_append_with_dict(
            fc, s->feature_name_dict, "VMAF_feature_adm_scale0_score", 0.0, index);
    } else {
        err |= vmaf_feature_collector_append_with_dict(
            fc, s->feature_name_dict, "VMAF_feature_adm_scale0_score", scores[0] / scores[1], index);
    }
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_feature_adm_scale1_score", scores[2] / scores[3], index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_feature_adm_scale2_score", scores[4] / scores[5], index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_feature_adm_scale3_score", scores[6] / scores[7], index);

    if (!s->debug) { return err; }

    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm", score, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_num", score_num,
                                                   index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "adm_den", score_den,
                                                   index);
    const char *names[8] = {"adm_num_scale0", "adm_den_scale0", "adm_num_scale1", "adm_den_scale1",
                            "adm_num_scale2", "adm_den_scale2", "adm_num_scale3", "adm_den_scale3"};
    for (int i = 0; i < 8; ++i) {
        err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, names[i],
                                                       scores[i], index);
    }
    return err;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatAdmStateMetal *s = (FloatAdmStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    release_psos(s);
    release_buffers(s);

    if (s->feature_name_dict) {
        int err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0) { rc = err; }
    }
    if (s->ctx) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = NULL;
    }
    return rc;
}

/* provided_features matches the CPU float_adm.c list EXACTLY (same names,
 * same order) — the parity test and model JSONs depend on it. */
static const char *provided_features[] = {"VMAF_feature_adm2_score",
                                          "VMAF_feature_aim_score",
                                          "VMAF_feature_adm3_score",
                                          "VMAF_feature_adm_scale0_score",
                                          "VMAF_feature_adm_scale1_score",
                                          "VMAF_feature_adm_scale2_score",
                                          "VMAF_feature_adm_scale3_score",
                                          "adm_num",
                                          "adm_den",
                                          "adm_scale0",
                                          "adm_num_scale0",
                                          "adm_den_scale0",
                                          "adm_num_scale1",
                                          "adm_den_scale1",
                                          "adm_num_scale2",
                                          "adm_den_scale2",
                                          "adm_num_scale3",
                                          "adm_den_scale3",
                                          NULL};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_float_adm_metal = {
    .name              = "float_adm_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = NULL,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(FloatAdmStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 6 * FADM_NUM_SCALES,
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
