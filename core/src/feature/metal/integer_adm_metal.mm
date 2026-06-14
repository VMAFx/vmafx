/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  integer_adm feature extractor on the Metal backend (feature "adm" — the
 *  VMAF-default ADM path). Integer (fixed-point) twin of float_adm_metal.mm:
 *  it mirrors that proven kernel's six-stage DWT -> CSF -> decouple -> CM
 *  pipeline, lifecycle, buffer-reuse posture, dispatch scaffolding, and
 *  per-(band,row) 1D reduction verbatim, and swaps the float arithmetic for
 *  the bit-exact fixed-point arithmetic of the CPU reference
 *  core/src/feature/integer_adm.c and the CUDA twin
 *  core/src/feature/cuda/integer_adm/ + integer_adm_cuda.c.
 *
 *  The integer path differs from the float twin in three substantive ways
 *  (everything else is structural parity with float_adm_metal.mm):
 *    1. Scale 0 operates on int16 DWT bands; scales 1-3 operate on int32
 *       ("i4") bands. The .mm therefore allocates int16 band buffers for
 *       scale 0 and int32 band buffers for scales 1-3, and dispatches the
 *       scale-specific MSL kernel variants.
 *    2. The CSF rfactor is the FIXED-POINT i_rfactor (Q21/Q23 at scale 0,
 *       Q32 at scales 1-3) computed exactly as integer_adm_cuda.c, with the
 *       hard-coded {36453,36453,49417} fast path at default view distance.
 *    3. The host reduction is conclude_adm_cm / conclude_adm_csf_den
 *       (per-scale power-of-2 final_shift recovery of the int64 cube
 *       accumulators), byte-for-byte with integer_adm_cuda.c — NOT the
 *       float cube-root pooling.
 *
 *  provided_features[] mirrors the CPU integer_adm.c list EXACTLY (same
 *  names, same order) — the parity test and any model JSON depend on it.
 *  Feature names use the VMAF_integer_feature_* / integer_adm_* prefixes.
 *
 *  Parity: places=4 (1e-4) vs the CPU `adm` at default options (ADR-0214
 *  cross-backend gate; the same bound the CUDA integer twin holds). csf_mode
 *  0 (Watson-97, the CPU default) only; other modes return -EINVAL at init,
 *  matching the CUDA twin which only ships mode 0.
 *
 *  Metallib resolution: embedded __TEXT,__metallib blob, same pattern as
 *  every other Metal feature extractor.
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

#define IADM_NUM_SCALES 4
#define IADM_NUM_BANDS  3
#define IADM_BX         16
#define IADM_BY         16
#define IADM_BORDER_FACTOR 0.1
#define IADM_ACCUM_SLOTS   9

/* Geometry uniform mirroring `IadmDims` in integer_adm.metal. */
typedef struct IadmDimsHost {
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
} IadmDimsHost;

/* CSF / CM uniform mirroring `IadmCsf` in integer_adm.metal. */
typedef struct IadmCsfHost {
    int32_t active_left;
    int32_t active_top;
    int32_t active_right;
    int32_t active_bottom;
    uint32_t i_rfactor_h;
    uint32_t i_rfactor_v;
    uint32_t i_rfactor_d;
    float gain_limit;
    int32_t v_shift;
    int32_t v_add_shift;
    int32_t h_shift;
    int32_t h_add_shift;
    int32_t s123_vert_add;
    int32_t s123_vert_shift;
    int32_t s123_hori_add;
    int32_t s123_hori_shift;
    int32_t den_shift_sq;
    int32_t den_add_shift_sq;
    int32_t den_shift_accum;
    int32_t den_add_shift_accum;
    int32_t den_shift_cub;
    int32_t den_add_shift_cub;
    int32_t cm_shift_sq[3];
    int32_t cm_add_shift_sq[3];
    int32_t cm_shift_cub[3];
    int32_t cm_add_shift_cub[3];
    int32_t cm_shift_sub[3];
    int32_t cm_shift_inner;
    int32_t cm_add_shift_inner;
    uint32_t _pad0;
} IadmCsfHost;

/* Watson-97 dwt model + amplitude tables — bit-for-bit replica of the CPU
 * dwt_7_9_YCbCr_threshold / dwt_7_9_basis_function_amplitudes used for the
 * integer i_rfactor computation (integer_adm.h / integer_adm_cuda.c). */
typedef struct IadmDwtModel {
    float a;
    float k;
    float f0;
    float g[4];
} IadmDwtModel;

static const IadmDwtModel iadm_dwt_threshold[3] = {
    {0.495f, 0.466f, 0.401f, {1.501f, 1.0f, 0.534f, 1.0f}},
    {1.633f, 0.353f, 0.209f, {1.520f, 1.0f, 0.502f, 1.0f}},
    {0.944f, 0.521f, 0.404f, {1.868f, 1.0f, 0.516f, 1.0f}},
};
static const float iadm_dwt_basis_amp[6][4] = {
    {0.62171f, 0.67234f, 0.72709f, 0.67234f},     {0.34537f, 0.41317f, 0.49428f, 0.41317f},
    {0.18004f, 0.22727f, 0.28688f, 0.22727f},     {0.091401f, 0.11792f, 0.15214f, 0.11792f},
    {0.045943f, 0.059758f, 0.077727f, 0.059758f}, {0.023013f, 0.030018f, 0.039156f, 0.030018f},
};

typedef struct IntegerAdmStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;

    /* Pipeline states. */
    void *pso_dwt_vert_8;
    void *pso_dwt_vert_16;
    void *pso_dwt_vert_s123;
    void *pso_dwt_hori_s0;
    void *pso_dwt_hori_s123;
    void *pso_decouple_csf_s0;
    void *pso_decouple_csf_s123;
    void *pso_csf_r_s0;
    void *pso_csf_r_s123;
    void *pso_csf_cm_s0;
    void *pso_csf_cm_s123;
    void *pso_aim_cm_s0;
    void *pso_aim_cm_s123;

    /* Reused device buffers. */
    void *src_ref;
    void *src_dis;
    void *dwt_tmp_ref;
    void *dwt_tmp_dis;
    void *ref_band[IADM_NUM_SCALES]; /* int16 at scale 0, int32 at scales 1-3 */
    void *dis_band[IADM_NUM_SCALES];
    void *csf_a;
    void *csf_f;
    void *csf_a_aim;
    void *csf_f_aim;
    void *accum[IADM_NUM_SCALES]; /* uint lo/hi pairs, 9 slots * 2 per (band,row) */

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned buf_stride;

    unsigned scale_w[IADM_NUM_SCALES];
    unsigned scale_h[IADM_NUM_SCALES];
    unsigned scale_half_w[IADM_NUM_SCALES];
    unsigned scale_half_h[IADM_NUM_SCALES];
    unsigned wg_count[IADM_NUM_SCALES];

    uint32_t i_rfactor[IADM_NUM_SCALES * 3];

    /* Options — same defaults as integer_adm.c. */
    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    bool adm_skip_aim;
    bool adm_skip_scale0;
    double adm_min_val;
    double adm_dlm_weight;
    double adm_p_norm;

    unsigned index;
    VmafDictionary *feature_name_dict;
} IntegerAdmStateMetal;

/* Options mirror integer_adm.c EXACTLY (names, aliases, defaults, ranges). */
static const VmafOption options[] = {
    {.name = "debug",
     .help = "debug mode: enable additional output",
     .offset = offsetof(IntegerAdmStateMetal, debug),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false}},
    {.name = "adm_csf_scale",
     .alias = "scf",
     .help = "scale coefficient for the horizontal & vertical direction terms of CSF",
     .offset = offsetof(IntegerAdmStateMetal, adm_csf_scale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_CSF_SCALE},
     .min = 0.0,
     .max = 50.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_diag_scale",
     .alias = "scfd",
     .help = "scale coefficient for the diagonal direction term of CSF",
     .offset = offsetof(IntegerAdmStateMetal, adm_csf_diag_scale),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_CSF_DIAG_SCALE},
     .min = 0.0,
     .max = 50.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_dlm_weight",
     .alias = "dlmw",
     .help = "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
     .offset = offsetof(IntegerAdmStateMetal, adm_dlm_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 0.5},
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_enhn_gain_limit",
     .alias = "egl",
     .help = "enhancement gain imposed on adm, must be >= 1.0, "
             "where 1.0 means the gain is completely disabled",
     .offset = offsetof(IntegerAdmStateMetal, adm_enhn_gain_limit),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_ENHN_GAIN_LIMIT},
     .min = 1.0,
     .max = DEFAULT_ADM_ENHN_GAIN_LIMIT,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_norm_view_dist",
     .alias = "nvd",
     .help = "normalized viewing distance = viewing distance / ref display's physical height",
     .offset = offsetof(IntegerAdmStateMetal, adm_norm_view_dist),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_NORM_VIEW_DIST},
     .min = 0.75,
     .max = 24.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_ref_display_height",
     .alias = "rdh",
     .help = "reference display height in pixels",
     .offset = offsetof(IntegerAdmStateMetal, adm_ref_display_height),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = DEFAULT_ADM_REF_DISPLAY_HEIGHT},
     .min = 1,
     .max = 4320,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_csf_mode",
     .alias = "csf",
     .help = "contrast sensitivity function (mode 0 / Watson-97 only on Metal)",
     .offset = offsetof(IntegerAdmStateMetal, adm_csf_mode),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = DEFAULT_ADM_CSF_MODE},
     .min = 0,
     .max = 3,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_noise_weight",
     .alias = "nw",
     .help = "noise weight",
     .offset = offsetof(IntegerAdmStateMetal, adm_noise_weight),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_NOISE_WEIGHT},
     .min = 0.0,
     .max = 1500.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_skip_aim",
     .help = "skip the calculation of AIM",
     .offset = offsetof(IntegerAdmStateMetal, adm_skip_aim),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false}},
    {.name = "adm_skip_scale0",
     .alias = "ssz",
     .help = "skip the calculation of scale 0",
     .offset = offsetof(IntegerAdmStateMetal, adm_skip_scale0),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false},
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_min_val",
     .alias = "min",
     .help = "minimum value allowed; lower values will be clipped to this value",
     .offset = offsetof(IntegerAdmStateMetal, adm_min_val),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = DEFAULT_ADM_MIN_VAL},
     .min = 0.0,
     .max = 1.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_p_norm",
     .alias = "apn",
     .help = "p-norm exponent for fixed-point ADM contrast-measure finalisation",
     .offset = offsetof(IntegerAdmStateMetal, adm_p_norm),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = 3.0},
     .min = 1.0,
     .max = 20.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {0},
};

/* dwt_quant_step — bit-for-bit replica of integer_adm.c / integer_adm_cuda.c
 * (uses the per-scale iadm_dwt_threshold model, not the global Watson). */
static float iadm_dwt_quant_step(const IadmDwtModel *params, int lambda, int theta,
                                 double view_dist, int display_h)
{
    const float r = (float)(view_dist * (double)display_h * M_PI / 180.0);
    const float temp = (float)log10(pow(2.0, (double)(lambda + 1)) * (double)params->f0 *
                                    (double)params->g[theta] / (double)r);
    const float Q = (float)(2.0 * (double)params->a *
                            pow(10.0, (double)params->k * (double)temp * temp) /
                            (double)iadm_dwt_basis_amp[lambda][theta]);
    return Q;
}

static void compute_per_scale_dims(IntegerAdmStateMetal *s)
{
    unsigned cw = s->width;
    unsigned ch = s->height;
    for (int scale = 0; scale < IADM_NUM_SCALES; ++scale) {
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

/* Compute the per-scale fixed-point i_rfactor[12] EXACTLY as
 * integer_adm_cuda.c (default-view-distance fast path + Q21/Q23/Q32). */
static void compute_i_rfactor(IntegerAdmStateMetal *s)
{
    const double pow2_32 = (double)(1ULL << 32);
    const double pow2_21 = (double)(1ULL << 21);
    const double pow2_23 = (double)(1ULL << 23);
    for (int scale = 0; scale < IADM_NUM_SCALES; ++scale) {
        const float f1 = iadm_dwt_quant_step(&iadm_dwt_threshold[0], scale, 1, s->adm_norm_view_dist,
                                             s->adm_ref_display_height);
        const float f2 = iadm_dwt_quant_step(&iadm_dwt_threshold[0], scale, 2, s->adm_norm_view_dist,
                                             s->adm_ref_display_height);
        const float rf1 = (float)s->adm_csf_scale / f1;
        const float rf2 = (float)s->adm_csf_diag_scale / f2;
        if (scale == 0) {
            if (fabs(s->adm_norm_view_dist * (double)s->adm_ref_display_height -
                     (double)DEFAULT_ADM_NORM_VIEW_DIST * (double)DEFAULT_ADM_REF_DISPLAY_HEIGHT) <
                1.0e-8) {
                s->i_rfactor[scale * 3 + 0] = 36453u;
                s->i_rfactor[scale * 3 + 1] = 36453u;
                s->i_rfactor[scale * 3 + 2] = 49417u;
            } else {
                s->i_rfactor[scale * 3 + 0] = (uint32_t)(rf1 * pow2_21);
                s->i_rfactor[scale * 3 + 1] = (uint32_t)(rf1 * pow2_21);
                s->i_rfactor[scale * 3 + 2] = (uint32_t)(rf2 * pow2_23);
            }
        } else {
            s->i_rfactor[scale * 3 + 0] = (uint32_t)(rf1 * pow2_32);
            s->i_rfactor[scale * 3 + 1] = (uint32_t)(rf1 * pow2_32);
            s->i_rfactor[scale * 3 + 2] = (uint32_t)(rf2 * pow2_32);
        }
    }
}

static int build_pipelines(IntegerAdmStateMetal *s, id<MTLDevice> device)
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

    struct {
        void **slot;
        const char *name;
    } fns[] = {
        {&s->pso_dwt_vert_8, "integer_adm_dwt_vert_8bpc"},
        {&s->pso_dwt_vert_16, "integer_adm_dwt_vert_16bpc"},
        {&s->pso_dwt_vert_s123, "integer_adm_dwt_vert_s123"},
        {&s->pso_dwt_hori_s0, "integer_adm_dwt_hori_s0"},
        {&s->pso_dwt_hori_s123, "integer_adm_dwt_hori_s123"},
        {&s->pso_decouple_csf_s0, "integer_adm_decouple_csf_s0"},
        {&s->pso_decouple_csf_s123, "integer_adm_decouple_csf_s123"},
        {&s->pso_csf_r_s0, "integer_adm_csf_r_s0"},
        {&s->pso_csf_r_s123, "integer_adm_csf_r_s123"},
        {&s->pso_csf_cm_s0, "integer_adm_csf_cm_s0"},
        {&s->pso_csf_cm_s123, "integer_adm_csf_cm_s123"},
        {&s->pso_aim_cm_s0, "integer_adm_aim_cm_s0"},
        {&s->pso_aim_cm_s123, "integer_adm_aim_cm_s123"},
    };
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); ++i) {
        id<MTLFunction> fn = [lib newFunctionWithName:[NSString stringWithUTF8String:fns[i].name]];
        if (fn == nil) { return -ENODEV; }
        id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:fn error:&err];
        if (pso == nil) { return -ENODEV; }
        *(fns[i].slot) = (__bridge_retained void *)pso;
    }
    return 0;
}

static void release_psos(IntegerAdmStateMetal *s)
{
    void **psos[] = {&s->pso_dwt_vert_8,       &s->pso_dwt_vert_16,      &s->pso_dwt_vert_s123,
                     &s->pso_dwt_hori_s0,      &s->pso_dwt_hori_s123,    &s->pso_decouple_csf_s0,
                     &s->pso_decouple_csf_s123, &s->pso_csf_r_s0,        &s->pso_csf_r_s123,
                     &s->pso_csf_cm_s0,        &s->pso_csf_cm_s123,      &s->pso_aim_cm_s0,
                     &s->pso_aim_cm_s123};
    for (size_t i = 0; i < sizeof(psos) / sizeof(psos[0]); ++i) {
        if (*psos[i]) {
            (void)(__bridge_transfer id<MTLComputePipelineState>)(*psos[i]);
            *psos[i] = NULL;
        }
    }
}

static void release_buffers(IntegerAdmStateMetal *s)
{
    for (int i = 0; i < IADM_NUM_SCALES; ++i) {
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
    IntegerAdmStateMetal *s = (IntegerAdmStateMetal *)fex->priv;

    /* Watson-97 (mode 0) only — matches the CUDA integer twin. */
    if (s->adm_csf_mode != 0) { return -EINVAL; }
    /* CPU integer_adm guards w>=17, h>=17. */
    if (w < 17u || h < 17u) { return -EINVAL; }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);
    compute_i_rfactor(s);

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

        /* dwt_tmp holds int32 lo/hi sub-rows: cur_w*2 ints per half-row. */
        const size_t dwt_bytes =
            (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(int32_t);
        id<MTLBuffer> dr = [device newBufferWithLength:dwt_bytes
                                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> dd = [device newBufferWithLength:dwt_bytes
                                               options:MTLResourceStorageModeShared];
        if (dr == nil || dd == nil) { err = -ENOMEM; goto fail_bufs; }
        s->dwt_tmp_ref = (__bridge_retained void *)dr;
        s->dwt_tmp_dis = (__bridge_retained void *)dd;

        /* Band buffers: int16 (2 bytes) at scale 0, int32 (4 bytes) at scales
         * 1-3. Each holds 4 bands (a,h,v,d). */
        for (int scale = 0; scale < IADM_NUM_SCALES; ++scale) {
            const size_t elem = (scale == 0) ? sizeof(int16_t) : sizeof(int32_t);
            const size_t band_bytes =
                (size_t)4u * s->buf_stride * s->scale_half_h[scale] * elem;
            id<MTLBuffer> br = [device newBufferWithLength:band_bytes
                                                   options:MTLResourceStorageModeShared];
            id<MTLBuffer> bd = [device newBufferWithLength:band_bytes
                                                   options:MTLResourceStorageModeShared];
            if (br == nil || bd == nil) { err = -ENOMEM; goto fail_bufs; }
            s->ref_band[scale] = (__bridge_retained void *)br;
            s->dis_band[scale] = (__bridge_retained void *)bd;
        }

        /* csf scratch: sized to int32 (worst case, scales 1-3) at scale-0
         * geometry (3 bands). */
        const size_t csf_bytes =
            (size_t)IADM_NUM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(int32_t);
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

        /* accum: 9 slots * 2 uint (lo/hi) per (band,row) threadgroup. */
        for (int scale = 0; scale < IADM_NUM_SCALES; ++scale) {
            const int hh = (int)s->scale_half_h[scale];
            int top = (int)((double)hh * IADM_BORDER_FACTOR - 0.5);
            if (top < 0) { top = 0; }
            const int bottom = hh - top;
            const unsigned num_rows = (bottom > top) ? (unsigned)(bottom - top) : 1u;
            s->wg_count[scale] = 3u * num_rows;
            const size_t accum_bytes =
                (size_t)s->wg_count[scale] * IADM_ACCUM_SLOTS * 2u * sizeof(uint32_t);
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

static void fill_raw_plane(VmafPicture *pic, id<MTLBuffer> dst, unsigned w, unsigned h, unsigned bpc)
{
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t row_bytes = (size_t)w * bpp;
    uint8_t *out = (uint8_t *)[dst contents];
    for (unsigned y = 0; y < h; ++y) {
        memcpy(out + (size_t)y * row_bytes,
               (const uint8_t *)pic->data[0] + (size_t)y * pic->stride[0], row_bytes);
    }
}

/* Per-scale (add_shift, shift) for the s123 vertical / horizontal DWT —
 * matches adm_dwt2_s123_combined_device's per-scale kernel selection. */
static void s123_dwt_shifts(int scale, int *vert_add, int *vert_shift, int *hori_add,
                            int *hori_shift)
{
    /* vert: scale 1 -> (0,0); scales 2,3 -> (32768,16). */
    if (scale == 1) {
        *vert_add = 0;
        *vert_shift = 0;
    } else {
        *vert_add = 32768;
        *vert_shift = 16;
    }
    /* hori: scale 1 -> (16384,15); scale 2 -> (32768,16); scale 3 -> (16384,15). */
    if (scale == 2) {
        *hori_add = 32768;
        *hori_shift = 16;
    } else {
        *hori_add = 16384;
        *hori_shift = 15;
    }
}

static int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                            VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    IntegerAdmStateMetal *s = (IntegerAdmStateMetal *)fex->priv;
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

    id<MTLComputePipelineState> pso_dv8 = (__bridge id<MTLComputePipelineState>)s->pso_dwt_vert_8;
    id<MTLComputePipelineState> pso_dv16 = (__bridge id<MTLComputePipelineState>)s->pso_dwt_vert_16;
    id<MTLComputePipelineState> pso_dvs123 =
        (__bridge id<MTLComputePipelineState>)s->pso_dwt_vert_s123;
    id<MTLComputePipelineState> pso_dh_s0 = (__bridge id<MTLComputePipelineState>)s->pso_dwt_hori_s0;
    id<MTLComputePipelineState> pso_dh_s123 =
        (__bridge id<MTLComputePipelineState>)s->pso_dwt_hori_s123;

    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    /* Zero the accumulators (skipped-scale AIM slots must contribute 0). */
    {
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        for (int scale = 0; scale < IADM_NUM_SCALES; ++scale) {
            id<MTLBuffer> ac = (__bridge id<MTLBuffer>)s->accum[scale];
            [blit fillBuffer:ac
                       range:NSMakeRange(0, (size_t)s->wg_count[scale] * IADM_ACCUM_SLOTS * 2u *
                                                sizeof(uint32_t))
                       value:0];
        }
        [blit endEncoding];
    }

    const float gain_limit = (float)s->adm_enhn_gain_limit;
    const int inp_bits = (s->bpc <= 8u) ? 8 : (int)s->bpc;

    for (int scale = 0; scale < IADM_NUM_SCALES; ++scale) {
        const bool is_s0 = (scale == 0);
        const int cur_w = (int)s->scale_w[scale];
        const int cur_h = (int)s->scale_h[scale];
        const int half_w = (int)s->scale_half_w[scale];
        const int half_h = (int)s->scale_half_h[scale];

        IadmDimsHost d;
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

        /* CM active region: w*BORDER-0.5, mirror to interior (CUDA convention). */
        int top = (int)((double)half_h * IADM_BORDER_FACTOR - 0.5);
        int left = (int)((double)half_w * IADM_BORDER_FACTOR - 0.5);
        if (top < 0) { top = 0; }
        if (left < 0) { left = 0; }
        const int bottom = half_h - top;
        const int right = half_w - left;
        const int active_h = bottom - top;

        /* den_shift_sq for s123 csf_den: scale1->31, scale2->30, scale3->31. */
        int den_shift_sq = 30;
        if (scale == 1 || scale == 3) { den_shift_sq = 31; }

        int s123_vert_add = 0, s123_vert_shift = 0, s123_hori_add = 0, s123_hori_shift = 0;
        s123_dwt_shifts(scale, &s123_vert_add, &s123_vert_shift, &s123_hori_add, &s123_hori_shift);

        IadmCsfHost c;
        memset(&c, 0, sizeof(c));
        c.active_left = left;
        c.active_top = top;
        c.active_right = right;
        c.active_bottom = bottom;
        c.i_rfactor_h = s->i_rfactor[scale * 3 + 0];
        c.i_rfactor_v = s->i_rfactor[scale * 3 + 1];
        c.i_rfactor_d = s->i_rfactor[scale * 3 + 2];
        c.gain_limit = gain_limit;
        c.v_shift = inp_bits;
        c.v_add_shift = 1 << (inp_bits - 1);
        c.h_shift = 16;
        c.h_add_shift = 32768;
        c.s123_vert_add = s123_vert_add;
        c.s123_vert_shift = s123_vert_shift;
        c.s123_hori_add = s123_hori_add;
        c.s123_hori_shift = s123_hori_shift;
        c.den_shift_sq = den_shift_sq;
        c.den_add_shift_sq = (den_shift_sq > 0) ? (1 << (den_shift_sq - 1)) : 0;

        /* CM / CSF cube-accumulation shift tables — replicate the CUDA
         * WarpShift / adm_csf_den shift derivation exactly. Uses w=half_w,
         * h=half_h and the BORDER active region (bottom-top)*(right-left). */
        const int hw = half_w;
        const int hh = half_h;
        const int area = (bottom - top) * (right - left);
        const int log2_w = (int)ceil(log2((double)hw));
        const int log2_h = (int)ceil(log2((double)hh));

        /* CM: scale 0 uses shift_sq {29,29,30}, cube = log2_w - {4,4,3}.
         * Scales 1-3 use shift_sq 30, cube = log2_w. shift_sub: s0 {10,10,12},
         * s123 {0,0,0}. inner-accum = log2_h. */
        const int fixed_shift[3] = {4, 4, 3};
        const int shift_xsq[3] = {29, 29, 30};
        const int add_shift_xsq[3] = {268435456, 268435456, 536870912};
        const int s0_shift_sub[3] = {10, 10, 12};
        for (int b = 0; b < 3; ++b) {
            if (is_s0) {
                c.cm_shift_sq[b] = shift_xsq[b];
                c.cm_add_shift_sq[b] = add_shift_xsq[b];
                c.cm_shift_cub[b] = log2_w - fixed_shift[b];
                c.cm_shift_sub[b] = s0_shift_sub[b];
            } else {
                c.cm_shift_sq[b] = 30;
                c.cm_add_shift_sq[b] = 1 << 29;
                c.cm_shift_cub[b] = log2_w;
                c.cm_shift_sub[b] = 0;
            }
            c.cm_add_shift_cub[b] =
                (c.cm_shift_cub[b] > 0) ? (1 << (c.cm_shift_cub[b] - 1)) : 0;
        }
        c.cm_shift_inner = log2_h;
        c.cm_add_shift_inner = (log2_h > 0) ? (1 << (log2_h - 1)) : 0;

        /* CSF denominator inner-accum / cube shifts. Scale 0: shift_accum =
         * max(0, ceil(log2(area) - 20)); no cube shift. Scales 1-3: cube
         * shift = ceil(log2(right-left)), row shift = ceil(log2(bottom-top)). */
        if (is_s0) {
            int sa = (int)ceil(log2((double)area) - 20.0);
            if (sa < 0) { sa = 0; }
            c.den_shift_accum = sa;
            c.den_add_shift_accum = (sa > 0) ? (1 << (sa - 1)) : 0;
            c.den_shift_cub = 0;
            c.den_add_shift_cub = 0;
        } else {
            const int den_cub = (int)ceil(log2((double)(right - left)));
            const int den_row = (int)ceil(log2((double)(bottom - top)));
            c.den_shift_cub = den_cub;
            c.den_add_shift_cub = (den_cub > 0) ? (1 << (den_cub - 1)) : 0;
            c.den_shift_accum = den_row;
            c.den_add_shift_accum = (den_row > 0) ? (1 << (den_row - 1)) : 0;
        }

        id<MTLBuffer> ref_band = (__bridge id<MTLBuffer>)s->ref_band[scale];
        id<MTLBuffer> dis_band = (__bridge id<MTLBuffer>)s->dis_band[scale];
        id<MTLBuffer> parent_ref =
            (scale > 0) ? (__bridge id<MTLBuffer>)s->ref_band[scale - 1] : ref_band;
        id<MTLBuffer> parent_dis =
            (scale > 0) ? (__bridge id<MTLBuffer>)s->dis_band[scale - 1] : dis_band;
        id<MTLBuffer> accum = (__bridge id<MTLBuffer>)s->accum[scale];

        const MTLSize tg = MTLSizeMake(IADM_BX, IADM_BY, 1);

        /* Stage 0 — DWT vertical (z=2 ref/dis). */
        {
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            if (is_s0) {
                [enc setComputePipelineState:(s->bpc <= 8u ? pso_dv8 : pso_dv16)];
                [enc setBuffer:src_ref offset:0 atIndex:0];
                [enc setBuffer:src_dis offset:0 atIndex:1];
            } else {
                [enc setComputePipelineState:pso_dvs123];
                [enc setBuffer:parent_ref offset:0 atIndex:6];
                [enc setBuffer:parent_dis offset:0 atIndex:7];
            }
            [enc setBuffer:dwt_ref offset:0 atIndex:2];
            [enc setBuffer:dwt_dis offset:0 atIndex:3];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            [enc setBytes:&c length:sizeof(c) atIndex:5];
            MTLSize grid = MTLSizeMake(((unsigned)cur_w + IADM_BX - 1u) / IADM_BX,
                                       ((unsigned)half_h + IADM_BY - 1u) / IADM_BY, 2);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
            [enc endEncoding];
        }

        /* Stage 1 — DWT horizontal (z=2 ref/dis). */
        {
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:(is_s0 ? pso_dh_s0 : pso_dh_s123)];
            [enc setBuffer:dwt_ref offset:0 atIndex:0];
            [enc setBuffer:dwt_dis offset:0 atIndex:1];
            [enc setBuffer:ref_band offset:0 atIndex:2];
            [enc setBuffer:dis_band offset:0 atIndex:3];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            [enc setBytes:&c length:sizeof(c) atIndex:5];
            MTLSize grid = MTLSizeMake(((unsigned)half_w + IADM_BX - 1u) / IADM_BX,
                                       ((unsigned)half_h + IADM_BY - 1u) / IADM_BY, 2);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
            [enc endEncoding];
        }

        /* Stage 2 — Decouple + CSF (decouple_a -> csf_a, csf_f). */
        {
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:(is_s0
                                              ? (__bridge id<MTLComputePipelineState>)s->pso_decouple_csf_s0
                                              : (__bridge id<MTLComputePipelineState>)s->pso_decouple_csf_s123)];
            [enc setBuffer:ref_band offset:0 atIndex:0];
            [enc setBuffer:dis_band offset:0 atIndex:1];
            [enc setBuffer:csf_a offset:0 atIndex:2];
            [enc setBuffer:csf_f offset:0 atIndex:3];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            [enc setBytes:&c length:sizeof(c) atIndex:5];
            MTLSize grid = MTLSizeMake(((unsigned)half_w + IADM_BX - 1u) / IADM_BX,
                                       ((unsigned)half_h + IADM_BY - 1u) / IADM_BY, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
            [enc endEncoding];
        }

        /* Stage 3 — CSF denom + DLM CM fused. 1D dispatch of 3*num_rows TGs. */
        {
            const unsigned num_rows = (unsigned)(active_h > 0 ? active_h : 1);
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:(is_s0
                                              ? (__bridge id<MTLComputePipelineState>)s->pso_csf_cm_s0
                                              : (__bridge id<MTLComputePipelineState>)s->pso_csf_cm_s123)];
            [enc setBuffer:ref_band offset:0 atIndex:0];
            [enc setBuffer:dis_band offset:0 atIndex:1];
            [enc setBuffer:csf_f offset:0 atIndex:3];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            [enc setBytes:&c length:sizeof(c) atIndex:5];
            [enc setBuffer:accum offset:0 atIndex:8];
            MTLSize grid = MTLSizeMake(3u * num_rows, 1, 1);
            MTLSize tg1d = MTLSizeMake(IADM_BX * IADM_BY, 1, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg1d];
            [enc endEncoding];
        }

        if (!s->adm_skip_aim) {
            /* Stage 3b — AIM CM numerator. The AIM threshold csf_r is
             * recomputed inline per-neighbour inside the CM kernel (matching
             * the CUDA AIM kernels), so no separate csf_r scratch stage is
             * needed; the csf_a_aim / csf_f_aim buffers are unused on the
             * integer path and kept only for buffer-layout parity with the
             * float twin. */
            const unsigned num_rows = (unsigned)(active_h > 0 ? active_h : 1);
            id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
            [enc setComputePipelineState:(is_s0
                                              ? (__bridge id<MTLComputePipelineState>)s->pso_aim_cm_s0
                                              : (__bridge id<MTLComputePipelineState>)s->pso_aim_cm_s123)];
            [enc setBuffer:ref_band offset:0 atIndex:0];
            [enc setBuffer:dis_band offset:0 atIndex:1];
            [enc setBytes:&d length:sizeof(d) atIndex:4];
            [enc setBytes:&c length:sizeof(c) atIndex:5];
            [enc setBuffer:accum offset:0 atIndex:8];
            MTLSize grid = MTLSizeMake(3u * num_rows, 1, 1);
            MTLSize tg1d = MTLSizeMake(IADM_BX * IADM_BY, 1, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg1d];
            [enc endEncoding];
        }
    }

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

/* Host reductions — byte-for-byte with integer_adm_cuda.c conclude_adm_cm /
 * conclude_adm_csf_den. accum holds per-(band,row) int64 cube sums (lo/hi
 * uint pairs), summed here across rows into int64 / uint64 totals first. */
static void read_band_row_totals(IntegerAdmStateMetal *s, int scale, int64_t csf_tot[3],
                                 int64_t cm_tot[3], int64_t aim_tot[3])
{
    for (int b = 0; b < 3; ++b) {
        csf_tot[b] = 0;
        cm_tot[b] = 0;
        aim_tot[b] = 0;
    }
    const uint32_t *slots = (const uint32_t *)[(__bridge id<MTLBuffer>)s->accum[scale] contents];
    const unsigned wg_count = s->wg_count[scale];
    for (unsigned wg = 0u; wg < wg_count; ++wg) {
        const uint32_t *p = slots + (size_t)wg * IADM_ACCUM_SLOTS * 2u;
        for (int b = 0; b < 3; ++b) {
            const uint64_t csf = ((uint64_t)p[(b)*2 + 1] << 32) | (uint64_t)p[(b)*2 + 0];
            const uint64_t cm = ((uint64_t)p[(3 + b) * 2 + 1] << 32) | (uint64_t)p[(3 + b) * 2 + 0];
            const uint64_t aim = ((uint64_t)p[(6 + b) * 2 + 1] << 32) | (uint64_t)p[(6 + b) * 2 + 0];
            csf_tot[b] += (int64_t)csf;
            cm_tot[b] += (int64_t)cm;
            aim_tot[b] += (int64_t)aim;
        }
    }
}

static float conclude_adm_cm(const int64_t accum[3], int h, int w, int scale, float noise_weight)
{
    int left = (int)((double)w * IADM_BORDER_FACTOR - 0.5);
    int top = (int)((double)h * IADM_BORDER_FACTOR - 0.5);
    int right = w - left;
    int bottom = h - top;
    const uint32_t shift_inner_accum = (uint32_t)ceil(log2((double)h));

    const uint32_t shift_xcub[3] = {(uint32_t)ceil(log2((double)w) - 4),
                                    (uint32_t)ceil(log2((double)w) - 4),
                                    (uint32_t)ceil(log2((double)w) - 3)};
    const int constant_offset[3] = {52, 52, 57};

    uint32_t shift_cub = (uint32_t)ceil(log2((double)w));
    float final_shift[3] = {powf(2.0f, (float)(45 - (int)shift_cub - (int)shift_inner_accum)),
                            powf(2.0f, (float)(39 - (int)shift_cub - (int)shift_inner_accum)),
                            powf(2.0f, (float)(36 - (int)shift_cub - (int)shift_inner_accum))};
    float powf_add =
        powf((float)((bottom - top) * (right - left)) * noise_weight, 1.0f / 3.0f);

    float result = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float f_accum;
        if (scale == 0) {
            f_accum = (float)((double)accum[i] /
                              pow(2.0, (double)(constant_offset[i] - (int)shift_xcub[i] -
                                                (int)shift_inner_accum)));
        } else {
            f_accum = (float)((double)accum[i] / (double)final_shift[scale - 1]);
        }
        result += powf(f_accum, 1.0f / 3.0f) + powf_add;
    }
    return result;
}

static float conclude_adm_csf_den(const int64_t accum[3], int h, int w, int scale,
                                  float norm_view_dist, float ref_display_height, float csf_scale,
                                  float csf_diag_scale, float noise_weight)
{
    const int left = (int)((double)w * IADM_BORDER_FACTOR - 0.5);
    const int top = (int)((double)h * IADM_BORDER_FACTOR - 0.5);
    const int right = w - left;
    const int bottom = h - top;
    const float factor1 =
        iadm_dwt_quant_step(&iadm_dwt_threshold[0], scale, 1, norm_view_dist, (int)ref_display_height);
    const float factor2 =
        iadm_dwt_quant_step(&iadm_dwt_threshold[0], scale, 2, norm_view_dist, (int)ref_display_height);
    const float rfactor[3] = {csf_scale / factor1, csf_scale / factor1, csf_diag_scale / factor2};
    const uint32_t accum_convert_float[4] = {18, 32, 27, 23};

    int32_t shift_accum;
    double shift_csf;
    if (scale == 0) {
        shift_accum = (int32_t)ceil(log2((double)((bottom - top) * (right - left))) - 20);
        shift_accum = shift_accum > 0 ? shift_accum : 0;
        shift_csf = pow(2.0, (double)((int)accum_convert_float[scale] - shift_accum));
    } else {
        shift_accum = (int32_t)ceil(log2((double)(bottom - top)));
        const uint32_t shift_cub = (uint32_t)ceil(log2((double)(right - left)));
        shift_csf = pow(2.0, (double)((int)accum_convert_float[scale] - shift_accum - (int)shift_cub));
    }
    const float powf_add =
        powf((float)((bottom - top) * (right - left)) * noise_weight, 1.0f / 3.0f);

    float result = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const double csf = ((double)accum[i] / shift_csf) * pow((double)rfactor[i], 3.0);
        result += powf((float)csf, 1.0f / 3.0f) + powf_add;
    }
    return result;
}

static int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    IntegerAdmStateMetal *s = (IntegerAdmStateMetal *)fex->priv;

    double scores[8];
    double num = 0.0;
    double den = 0.0;
    double aim_num = 0.0;

    for (int scale = 0; scale < IADM_NUM_SCALES; ++scale) {
        const int hw = (int)s->scale_half_w[scale];
        const int hh = (int)s->scale_half_h[scale];

        int64_t csf_tot[3], cm_tot[3], aim_tot[3];
        read_band_row_totals(s, scale, csf_tot, cm_tot, aim_tot);

        float num_scale = 0.0f;
        float den_scale = 0.0f;
        float aim_num_scale = 0.0f;

        if (scale == 0 && s->adm_skip_scale0) {
            num_scale = 0.0f;
            den_scale = 1e-10f;
        } else {
            num_scale = conclude_adm_cm(cm_tot, hh, hw, scale, (float)s->adm_noise_weight);
            den_scale =
                conclude_adm_csf_den(csf_tot, hh, hw, scale, (float)s->adm_norm_view_dist,
                                     (float)s->adm_ref_display_height, (float)s->adm_csf_scale,
                                     (float)s->adm_csf_diag_scale, (float)s->adm_noise_weight);
        }

        if (!s->adm_skip_aim) {
            aim_num_scale = conclude_adm_cm(aim_tot, hh, hw, scale, 0.0f);
        }

        num += num_scale;
        den += den_scale;
        aim_num += aim_num_scale;
        scores[2 * scale + 0] = num_scale;
        scores[2 * scale + 1] = den_scale;
    }

    const int w0 = (int)s->scale_w[0];
    const int h0 = (int)s->scale_h[0];
    const double numden_limit = 1e-10 * (double)(w0 * h0) / (1920.0 * 1080.0);
    num = num < numden_limit ? 0.0 : num;
    den = den < numden_limit ? 0.0 : den;

    double score;
    double score_aim;
    if (den == 0.0) {
        score = 1.0;
        score_aim = 1.0;
    } else {
        score = num / den;
        score_aim = aim_num / den;
    }
    const double score_num = num;
    const double score_den = den;

    double score_adm3 =
        score * s->adm_dlm_weight + (1.0 - score_aim) * (1.0 - s->adm_dlm_weight);
    if (score_adm3 < s->adm_min_val) { score_adm3 = s->adm_min_val; }

    int err = 0;
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_integer_feature_adm2_score", score, index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_integer_feature_aim_score", score_aim, index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_integer_feature_adm3_score", score_adm3, index);

    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "integer_adm_scale0", scores[0] / scores[1], index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "integer_adm_scale1", scores[2] / scores[3], index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "integer_adm_scale2", scores[4] / scores[5], index);
    err |= vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "integer_adm_scale3", scores[6] / scores[7], index);

    if (!s->debug) { return err; }

    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "integer_adm", score,
                                                   index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "integer_adm_num",
                                                   score_num, index);
    err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "integer_adm_den",
                                                   score_den, index);
    const char *names[8] = {"integer_adm_num_scale0", "integer_adm_den_scale0",
                            "integer_adm_num_scale1", "integer_adm_den_scale1",
                            "integer_adm_num_scale2", "integer_adm_den_scale2",
                            "integer_adm_num_scale3", "integer_adm_den_scale3"};
    for (int i = 0; i < 8; ++i) {
        err |= vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, names[i], scores[i],
                                                       index);
    }
    return err;
}

static int close_fex_metal(VmafFeatureExtractor *fex)
{
    IntegerAdmStateMetal *s = (IntegerAdmStateMetal *)fex->priv;
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

/* provided_features matches the CPU integer_adm.c list EXACTLY (same names,
 * same order) — the parity test and model JSONs depend on it. Note the
 * VMAF_integer_feature_* / integer_adm_* prefixes (distinct from float_adm's
 * VMAF_feature_* / adm_* names). */
static const char *provided_features[] = {"VMAF_integer_feature_adm2_score",
                                          "VMAF_integer_feature_aim_score",
                                          "VMAF_integer_feature_adm3_score",
                                          "integer_adm_scale0",
                                          "integer_adm_scale1",
                                          "integer_adm_scale2",
                                          "integer_adm_scale3",
                                          "integer_adm",
                                          "integer_adm_num",
                                          "integer_adm_den",
                                          "integer_adm_num_scale0",
                                          "integer_adm_den_scale0",
                                          "integer_adm_num_scale1",
                                          "integer_adm_den_scale1",
                                          "integer_adm_num_scale2",
                                          "integer_adm_den_scale2",
                                          "integer_adm_num_scale3",
                                          "integer_adm_den_scale3",
                                          NULL};

extern "C" {
/* Registered via extern in feature_extractor.c's feature_extractor_list[];
 * making this static would unlink the extractor from the registry — same
 * pattern every CUDA / HIP / SYCL feature extractor uses (ADR-0361 Metal
 * backend; ADR-0278 cite form). */
// NOLINTNEXTLINE(misc-use-internal-linkage) — ADR-0361 / ADR-0278
VmafFeatureExtractor vmaf_fex_integer_adm_metal = {
    .name              = "integer_adm_metal",
    .init              = init_fex_metal,
    .submit            = submit_fex_metal,
    .collect           = collect_fex_metal,
    .flush             = NULL,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(IntegerAdmStateMetal),
    .provided_features = provided_features,
    .flags             = 0,
    .chars = {
        .n_dispatches_per_frame = 6 * IADM_NUM_SCALES,
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1280U * 720U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
