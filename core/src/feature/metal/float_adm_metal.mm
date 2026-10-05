/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  float_adm feature extractor on the Metal backend.
 *  Port of `core/src/feature/cuda/float_adm_cuda.c` and
 *  `sycl/float_adm_sycl.cpp` (ADR-1420, ADR-1434) to the exact design of
 *  those twins (ADR-1498): the CPU's float ADM arithmetic operation for
 *  operation (metal_float_adm_math.h), the CPU's own routines for everything
 *  that is not per-sample (adm_float_reference.h), and the CPU's order of
 *  addition, so the scores equal the CPU's.
 *
 *  Algorithm summary (per frame, 4 scales):
 *    Stage 0  float_adm_dwt_vert_{8,16}bpc -- DWT vertical, raw at scale 0
 *    Stage 1  float_adm_dwt_hori           -- DWT horizontal -> 4 sub-bands
 *    Stage 2  float_adm_decouple           -- decouple + CSF of both signals
 *    Stage 3  float_adm_terms              -- the per-sample terms of the
 *                                             reduced region (9 slots)
 *    Stage 4  float_adm_rows               -- one fp32 sum per (slot, row)
 *  Host collect() adds the rows of a slot top to bottom in fp32 and pools
 *  each scale with adm_pool_bands_s(), as compute_adm() does.
 *
 *  Multi-scale buffer strategy: every device buffer (raw src, dwt scratch,
 *  per-scale band buffers, csf buffers, terms, row sums) is
 *  allocated once in init() and reused for every frame — the same
 *  posture as float_ms_ssim_metal.mm. Band buffers are per-scale because
 *  the scale-(s+1) DWT-vert reads the scale-s LL band; everything else is
 *  sized to scale 0 (worst case) and reused across scales.
 *
 *  Metallib resolution: embedded __TEXT,__metallib blob, same pattern as
 *  every other Metal feature extractor.
 *
 *  Parity: equal to the CPU `float_adm` (test_metal_float_adm_parity, `==`).
 *  Only csf_mode 0 (Watson-97, the CPU default) is supported; other modes
 *  return -EINVAL at init.
 */

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

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
#include "../adm_csf_fixed_point.h"
#include "../adm_float_reference.h"
#include "../adm_options.h"
#include "../adm_score.h"
#include "../nonfinite_score.h"
}

#include "../../metal/objc_handle.h"

#include "metal_float_adm_math.h"


#define FADM_NUM_SCALES 4
#define FADM_NUM_BANDS  3
#define FADM_BX         16
#define FADM_BY         16
#define FADM_ROW_TG 64u

namespace {

/* Geometry uniform mirroring `FadmDims` in float_adm.metal. */
using FadmDimsHost = struct FadmDimsHost {
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
};

/* DWT stage uniform mirroring `FadmCsf` in float_adm.metal. */
using FadmCsfHost = struct FadmCsfHost {
    float scaler;
    float pixel_offset;
};

using FloatAdmStateMetal = struct FloatAdmStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;

    /* Pipeline states for the six kernels. */
    void *pso_dwt_vert_8;
    void *pso_dwt_vert_16;
    void *pso_dwt_hori;
    void *pso_decouple;
    void *pso_terms;
    void *pso_rows;

    /* Reused device buffers. */
    void *src_ref;
    void *src_dis;
    void *dwt_tmp_ref;
    void *dwt_tmp_dis;
    void *ref_band[FADM_NUM_SCALES];
    void *dis_band[FADM_NUM_SCALES];
    void *csf_a;
    void *csf_fa;
    void *csf_r;
    void *csf_fr;
    void *terms;
    void *rows;

    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned buf_stride;
    float scaler;

    unsigned scale_w[FADM_NUM_SCALES];
    unsigned scale_h[FADM_NUM_SCALES];
    unsigned scale_half_w[FADM_NUM_SCALES];
    unsigned scale_half_h[FADM_NUM_SCALES];
    AdmBorderS region[FADM_NUM_SCALES]; /* adm_border_s() */
    size_t row_offset[FADM_NUM_SCALES]; /* floats into `rows` */
    size_t row_floats;
    size_t term_floats;

    float rfactor[FADM_NUM_SCALES][FADM_NUM_BANDS]; /* adm_csf_rfactor_s() */
    float cos_1deg_sq;                              /* adm_decouple_cos_1deg_sq_s() */
    VmafMtlFadmGainLimit gain_limit;

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
    double adm_f1s0;
    double adm_f1s1;
    double adm_f1s2;
    double adm_f1s3;
    double adm_f2s0;
    double adm_f2s1;
    double adm_f2s2;
    double adm_f2s3;
    int adm_skip_aim_scale;
    bool adm_skip_scale0;

    unsigned index;
    VmafDictionary *feature_name_dict;
};
} // namespace

namespace {

const VmafOption options[] = {
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
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM | VMAF_OPT_FLAG_DEFAULT_ONLY},
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
    {.name = "adm_f1s0",
     .alias = "f1s0",
     .help = "factor1 scale0",
     .offset = offsetof(FloatAdmStateMetal, adm_f1s0),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f1s1",
     .alias = "f1s1",
     .help = "factor1 scale1",
     .offset = offsetof(FloatAdmStateMetal, adm_f1s1),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f1s2",
     .alias = "f1s2",
     .help = "factor1 scale2",
     .offset = offsetof(FloatAdmStateMetal, adm_f1s2),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f1s3",
     .alias = "f1s3",
     .help = "factor1 scale3",
     .offset = offsetof(FloatAdmStateMetal, adm_f1s3),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f2s0",
     .alias = "f2s0",
     .help = "factor2 scale0",
     .offset = offsetof(FloatAdmStateMetal, adm_f2s0),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f2s1",
     .alias = "f2s1",
     .help = "factor2 scale1",
     .offset = offsetof(FloatAdmStateMetal, adm_f2s1),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f2s2",
     .alias = "f2s2",
     .help = "factor2 scale2",
     .offset = offsetof(FloatAdmStateMetal, adm_f2s2),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_f2s3",
     .alias = "f2s3",
     .help = "factor2 scale3",
     .offset = offsetof(FloatAdmStateMetal, adm_f2s3),
     .type = VMAF_OPT_TYPE_DOUBLE,
     .default_val = {.d = -1.0},
     .min = -1.0,
     .max = 10.0,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_skip_aim_scale",
     .alias = "sasc",
     .help = "when set, skip AIM calculations for that scale",
     .offset = offsetof(FloatAdmStateMetal, adm_skip_aim_scale),
     .type = VMAF_OPT_TYPE_INT,
     .default_val = {.i = -1},
     .min = 0,
     .max = 3,
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name = "adm_skip_scale0",
     .alias = "ssz",
     .help = "skip the calculation of scale 0",
     .offset = offsetof(FloatAdmStateMetal, adm_skip_scale0),
     .type = VMAF_OPT_TYPE_BOOL,
     .default_val = {.b = false},
     .flags = VMAF_OPT_FLAG_FEATURE_PARAM},
    {.name=nullptr},
};

void compute_per_scale_dims(FloatAdmStateMetal *s)
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
    s->row_floats = 0u;
    s->term_floats = 0u;
    for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
        s->region[scale] = adm_border_s((int)s->scale_half_w[scale], (int)s->scale_half_h[scale],
                                        ADM_BORDER_FACTOR);
        const size_t region_w = (size_t)(s->region[scale].right - s->region[scale].left);
        const size_t region_h = (size_t)(s->region[scale].bottom - s->region[scale].top);
        s->row_offset[scale] = s->row_floats;
        s->row_floats += (size_t)VMAF_MTL_FADM_TERM_SLOTS * region_h;
        if ((size_t)VMAF_MTL_FADM_TERM_SLOTS * region_w * region_h > s->term_floats) {
            s->term_floats = (size_t)VMAF_MTL_FADM_TERM_SLOTS * region_w * region_h;
        }
    }
    s->buf_stride = (s->scale_half_w[0] + 3u) & ~3u;
}

void *make_pipeline(id<MTLDevice> device, id<MTLLibrary> lib, NSString *name)
{
    id<MTLFunction> const fn = [lib newFunctionWithName:name];
    if (fn == nil) { return nullptr; }
    NSError *err = nil;
    id<MTLComputePipelineState> const pso = [device newComputePipelineStateWithFunction:fn error:&err];
    if (pso == nil) { return nullptr; }
    return (__bridge_retained void *)pso;
}

int build_pipelines(FloatAdmStateMetal *s, id<MTLDevice> device)
{
    int load_rc = 0;
    id<MTLLibrary> const lib = vmaf_metal_library_load(device, &load_rc);
    if (lib == nil) { return load_rc; }
    NSError *err = nil;

    s->pso_dwt_vert_8 = make_pipeline(device, lib, @"float_adm_dwt_vert_8bpc");
    s->pso_dwt_vert_16 = make_pipeline(device, lib, @"float_adm_dwt_vert_16bpc");
    s->pso_dwt_hori = make_pipeline(device, lib, @"float_adm_dwt_hori");
    s->pso_decouple = make_pipeline(device, lib, @"float_adm_decouple");
    s->pso_terms = make_pipeline(device, lib, @"float_adm_terms");
    s->pso_rows = make_pipeline(device, lib, @"float_adm_rows");
    if (s->pso_dwt_vert_8 == nullptr || s->pso_dwt_vert_16 == nullptr || s->pso_dwt_hori == nullptr ||
        s->pso_decouple == nullptr || s->pso_terms == nullptr || s->pso_rows == nullptr) {
        return -ENODEV;
    }
    return 0;
}

/* Release every retained PSO. Safe on a partially-built state. */
void release_psos(FloatAdmStateMetal *s)
{
    void **const psos[] = {&s->pso_rows,        &s->pso_terms,       &s->pso_decouple,
                     &s->pso_dwt_hori,    &s->pso_dwt_vert_16, &s->pso_dwt_vert_8};
    for (auto & pso : psos) {
        if (*pso) {
            (void)(__bridge_transfer id<MTLComputePipelineState>)(*pso);
            *pso = nullptr;
        }
    }
}

/* Release every retained MTLBuffer. Safe on a partially-allocated state. */
void release_buffers(FloatAdmStateMetal *s)
{
    for (int i = 0; i < FADM_NUM_SCALES; ++i) {
        if (s->ref_band[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->ref_band[i];
            s->ref_band[i] = nullptr;
        }
        if (s->dis_band[i]) {
            (void)(__bridge_transfer id<MTLBuffer>)s->dis_band[i];
            s->dis_band[i] = nullptr;
        }
    }
    void **const single[] = {&s->src_ref, &s->src_dis, &s->dwt_tmp_ref, &s->dwt_tmp_dis, &s->csf_a,
                       &s->csf_fa,  &s->csf_r,   &s->csf_fr,      &s->terms,       &s->rows};
    for (auto & i : single) {
        if (*i) {
            (void)(__bridge_transfer id<MTLBuffer>)(*i);
            *i = nullptr;
        }
    }
}

/* One shared-storage buffer of `bytes` bytes into `*slot` (a +1 reference). */
int new_shared_buffer(id<MTLDevice> device, size_t bytes, void **slot)
{
    id<MTLBuffer> const buf = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (buf == nil) { return -ENOMEM; }
    *slot = (__bridge_retained void *)buf;
    return 0;
}

int new_shared_buffer_pair(id<MTLDevice> device, size_t bytes, void **first, void **second)
{
    const int err = new_shared_buffer(device, bytes, first);
    return (err != 0) ? err : new_shared_buffer(device, bytes, second);
}

float scaler_for_bpc(unsigned bpc)
{
    if (bpc <= 8u) { return 1.0f; }
    if (bpc == 10u) { return 4.0f; }
    return (bpc == 12u) ? 16.0f : 256.0f;
}

/* The constants the reference derives per frame, taken from its own
 * routines so they cannot drift from it (ADR-1420, ADR-1434). The CSF
 * weights come from adm_csf_rfactor_s() with the options float_adm.c
 * passes, the per-scale overrides adm_f1sN / adm_f2sN included; in the
 * Watson-97 mode this twin supports they ignore adm_csf_scale /
 * adm_csf_diag_scale, as on the CPU (ADR-1214). */
void init_reference_constants(FloatAdmStateMetal *s)
{
    for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
        adm_csf_rfactor_s(scale, s->adm_norm_view_dist, s->adm_ref_display_height,
                          s->adm_csf_mode, DEFAULT_ADM_CSF_LUMINANCE_LEVEL, s->adm_csf_scale,
                          s->adm_csf_diag_scale, s->adm_f1s0, s->adm_f1s1, s->adm_f1s2,
                          s->adm_f1s3, s->adm_f2s0, s->adm_f2s1, s->adm_f2s2, s->adm_f2s3,
                          s->rfactor[scale]);
    }
    s->cos_1deg_sq = adm_decouple_cos_1deg_sq_s();
    s->gain_limit = vmaf_mtl_fadm_make_gain_limit(s->adm_enhn_gain_limit);
}

/* Every device buffer of the frame: raw planes, the first DWT's scratch, the
 * per-scale bands, the CSF products, the term and row sums. */
int alloc_device_buffers(FloatAdmStateMetal *s, id<MTLDevice> device)
{
    const size_t bpp = (s->bpc <= 8u) ? 1u : 2u;
    int err = new_shared_buffer_pair(device, (size_t)s->width * s->height * bpp, &s->src_ref,
                                     &s->src_dis);
    if (err != 0) { return err; }

    const size_t dwt_bytes = (size_t)s->width * 2u * s->scale_half_h[0] * sizeof(float);
    err = new_shared_buffer_pair(device, dwt_bytes, &s->dwt_tmp_ref, &s->dwt_tmp_dis);
    if (err != 0) { return err; }

    for (int scale = 0; scale < FADM_NUM_SCALES && err == 0; ++scale) {
        const size_t band_bytes = (size_t)4u * s->buf_stride * s->scale_half_h[scale] * sizeof(float);
        err = new_shared_buffer_pair(device, band_bytes, &s->ref_band[scale], &s->dis_band[scale]);
    }
    if (err != 0) { return err; }

    const size_t csf_bytes =
        (size_t)FADM_NUM_BANDS * s->buf_stride * s->scale_half_h[0] * sizeof(float);
    void **const csf_slots[] = {&s->csf_a, &s->csf_fa, &s->csf_r, &s->csf_fr};
    for (auto &csf_slot : csf_slots) {
        err = new_shared_buffer(device, csf_bytes, csf_slot);
        if (err != 0) { return err; }
    }

    err = new_shared_buffer(device, (s->term_floats > 0u ? s->term_floats : 1u) * sizeof(float),
                            &s->terms);
    return (err != 0) ? err
                      : new_shared_buffer(device,
                                          (s->row_floats > 0u ? s->row_floats : 1u) * sizeof(float),
                                          &s->rows);
}

/* The device, its buffers and its pipelines; the caller unwinds on failure. */
int init_device_resources(FloatAdmStateMetal *s, int *stage)
{
    void *const dh = vmaf_metal_context_device_handle(s->ctx);
    if (dh == nullptr) { *stage = 1; return -ENODEV; }
    id<MTLDevice> const device = (__bridge id<MTLDevice>)dh;
    int err = alloc_device_buffers(s, device);
    if (err != 0) { *stage = 2; return err; }
    err = build_pipelines(s, device);
    *stage = (err != 0) ? 3 : 0;
    return err;
}

int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                          unsigned w, unsigned h)
{
    (void)pix_fmt;

    /* Below 17x17 the scale-3 bands have one sample; the CPU float_adm
     * refuses such frames (adm_frame_size_check()), and so does the twin,
     * before it reads its state or claims any device resource. */
    const int size_err = adm_frame_size_check("float_adm_metal", w, h);
    if (size_err != 0) { return size_err; }

    FloatAdmStateMetal *s = (FloatAdmStateMetal *)fex->priv;

    /* Watson-97 (mode 0) only — matches the CUDA twin (other CSF modes
     * would need the Barten / ADM sensitivity tables ported to MSL). */
    if (s->adm_csf_mode != 0) { return -EINVAL; }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    compute_per_scale_dims(s);
    s->scaler = scaler_for_bpc(bpc);
    init_reference_constants(s);

    int err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }

    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) { goto fail_ctx; }

    {
        int stage = 0;
        err = init_device_resources(s, &stage);
        if (stage == 1) { goto fail_lc; }
        if (stage == 2) { goto fail_bufs; }
        if (stage == 3) { goto fail_pso; }
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (s->feature_name_dict == nullptr) { err = -ENOMEM; goto fail_pso; }
    return 0;

fail_pso:
    release_psos(s);
fail_bufs:
    release_buffers(s);
fail_lc:
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
fail_ctx:
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = nullptr;
    return err;
}

/* Copy a Y plane (respecting source stride) into the packed src buffer. */
void fill_raw_plane(VmafPicture *pic, id<MTLBuffer> dst, unsigned w, unsigned h,
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

/* Dispatch `grid` threadgroups of `tg` threads of `pso` with the buffers and
 * bytes of one stage: buffers[i] at index i, then `bytes` at the next index. */
void dispatch_stage(id<MTLCommandBuffer> cmd, void *pso_handle,
                           NSArray<id<MTLBuffer>> *buffers, const void *bytes,
                           size_t bytes_length, MTLSize grid, MTLSize tg)
{
    id<MTLComputePipelineState> const pso = (__bridge id<MTLComputePipelineState>)pso_handle;
    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:pso];
    for (NSUInteger i = 0; i < [buffers count]; ++i) {
        [enc setBuffer:buffers[i] offset:0 atIndex:i];
    }
    [enc setBytes:bytes length:bytes_length atIndex:[buffers count]];
    [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
    [enc endEncoding];
}

MTLSize grid_2d(unsigned w, unsigned h, unsigned z)
{
    return MTLSizeMake((w + FADM_BX - 1u) / FADM_BX, (h + FADM_BY - 1u) / FADM_BY, z);
}

/* Stages 0 and 1: the DWT of one scale, reference and distorted (z = 2). */
void encode_dwt(FloatAdmStateMetal *s, id<MTLCommandBuffer> cmd, int scale)
{
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

    FadmCsfHost c;
    c.scaler = s->scaler;
    c.pixel_offset = -128.0f;

    id<MTLBuffer> const ref_band = (__bridge id<MTLBuffer>)s->ref_band[scale];
    id<MTLBuffer> const dis_band = (__bridge id<MTLBuffer>)s->dis_band[scale];
    id<MTLBuffer> const parent_ref =
        (scale > 0) ? (__bridge id<MTLBuffer>)s->ref_band[scale - 1] : ref_band;
    id<MTLBuffer> const parent_dis =
        (scale > 0) ? (__bridge id<MTLBuffer>)s->dis_band[scale - 1] : dis_band;
    id<MTLBuffer> const src_ref = (__bridge id<MTLBuffer>)s->src_ref;
    id<MTLBuffer> const src_dis = (__bridge id<MTLBuffer>)s->src_dis;
    id<MTLBuffer> const dwt_ref = (__bridge id<MTLBuffer>)s->dwt_tmp_ref;
    id<MTLBuffer> const dwt_dis = (__bridge id<MTLBuffer>)s->dwt_tmp_dis;
    const MTLSize tg = MTLSizeMake(FADM_BX, FADM_BY, 1);

    /* Stage 0 -- DWT vertical (z=2 ref/dis). */
    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)(
                                     (s->bpc <= 8u) ? s->pso_dwt_vert_8 : s->pso_dwt_vert_16)];
    [enc setBuffer:(scale == 0 ? src_ref : parent_ref) offset:0 atIndex:0];
    [enc setBuffer:(scale == 0 ? src_dis : parent_dis) offset:0 atIndex:1];
    [enc setBuffer:dwt_ref offset:0 atIndex:2];
    [enc setBuffer:dwt_dis offset:0 atIndex:3];
    [enc setBytes:&d length:sizeof(d) atIndex:4];
    [enc setBytes:&c length:sizeof(c) atIndex:5];
    [enc setBuffer:parent_ref offset:0 atIndex:6];
    [enc setBuffer:parent_dis offset:0 atIndex:7];
    [enc dispatchThreadgroups:grid_2d((unsigned)cur_w, (unsigned)half_h, 2)
        threadsPerThreadgroup:tg];
    [enc endEncoding];

    /* Stage 1 -- DWT horizontal (z=2 ref/dis). */
    dispatch_stage(cmd, s->pso_dwt_hori, @[dwt_ref, dwt_dis, ref_band, dis_band], &d, sizeof(d),
                   grid_2d((unsigned)half_w, (unsigned)half_h, 2), tg);
}

/* Stage 2: decouple + CSF of both signals. */
void encode_decouple(FloatAdmStateMetal *s, id<MTLCommandBuffer> cmd, int scale)
{
    VmafMtlFadmDecoupleArgs a;
    memset(&a, 0, sizeof(a));
    a.limit = s->gain_limit;
    a.half_w = (int32_t)s->scale_half_w[scale];
    a.half_h = (int32_t)s->scale_half_h[scale];
    a.buf_stride = (int32_t)s->buf_stride;
    a.cos_1deg_sq = s->cos_1deg_sq;
    a.rfactor_h = s->rfactor[scale][0];
    a.rfactor_v = s->rfactor[scale][1];
    a.rfactor_d = s->rfactor[scale][2];

    NSArray<id<MTLBuffer>> *buffers = @[
        (__bridge id<MTLBuffer>)s->ref_band[scale], (__bridge id<MTLBuffer>)s->dis_band[scale],
        (__bridge id<MTLBuffer>)s->csf_a, (__bridge id<MTLBuffer>)s->csf_fa,
        (__bridge id<MTLBuffer>)s->csf_r, (__bridge id<MTLBuffer>)s->csf_fr
    ];
    dispatch_stage(cmd, s->pso_decouple, buffers, &a, sizeof(a),
                   grid_2d((unsigned)a.half_w, (unsigned)a.half_h, 1),
                   MTLSizeMake(FADM_BX, FADM_BY, 1));
}

/* Stages 3 and 4: the terms of the reduced region and their row sums. A
 * scale whose region is empty has no terms and no rows. */
void encode_terms_and_rows(FloatAdmStateMetal *s, id<MTLCommandBuffer> cmd, int scale)
{
    const AdmBorderS *r = &s->region[scale];
    const int region_w = r->right - r->left;
    const int region_h = r->bottom - r->top;
    if (region_w <= 0 || region_h <= 0) { return; }

    VmafMtlFadmTermArgs t;
    memset(&t, 0, sizeof(t));
    t.half_w = (int32_t)s->scale_half_w[scale];
    t.half_h = (int32_t)s->scale_half_h[scale];
    t.buf_stride = (int32_t)s->buf_stride;
    t.left = r->left;
    t.top = r->top;
    t.region_w = (uint32_t)region_w;
    t.region_h = (uint32_t)region_h;
    /* adm_p_norm and adm_bypass_cm reach the kernel as the CPU reads them
     * (ADR-1220): `is_cube` is the `adm_p_norm == 3.0` of adm_tools.c. */
    t.p_norm = (float)s->adm_p_norm;
    t.is_cube = (s->adm_p_norm == 3.0) ? 1u : 0u;
    t.bypass_cm = (s->adm_bypass_cm != 0) ? 1u : 0u;
    t.rfactor_h = s->rfactor[scale][0];
    t.rfactor_v = s->rfactor[scale][1];
    t.rfactor_d = s->rfactor[scale][2];

    id<MTLBuffer> const terms = (__bridge id<MTLBuffer>)s->terms;
    NSArray<id<MTLBuffer>> *term_buffers = @[
        (__bridge id<MTLBuffer>)s->ref_band[scale], (__bridge id<MTLBuffer>)s->csf_a,
        (__bridge id<MTLBuffer>)s->csf_fa, (__bridge id<MTLBuffer>)s->csf_r,
        (__bridge id<MTLBuffer>)s->csf_fr, terms
    ];
    dispatch_stage(cmd, s->pso_terms, term_buffers, &t, sizeof(t),
                   grid_2d((unsigned)region_w, (unsigned)region_h, 1),
                   MTLSizeMake(FADM_BX, FADM_BY, 1));

    VmafMtlFadmRowArgs row_args;
    row_args.region_w = (uint32_t)region_w;
    row_args.region_h = (uint32_t)region_h;
    /* One work-item per (slot, row); each adds its row left to right. */
    id<MTLBuffer> const rows = (__bridge id<MTLBuffer>)s->rows;
    const NSUInteger rows_offset = (NSUInteger)(s->row_offset[scale] * sizeof(float));
    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso_rows];
    [enc setBuffer:terms offset:0 atIndex:0];
    [enc setBuffer:rows offset:rows_offset atIndex:1];
    [enc setBytes:&row_args length:sizeof(row_args) atIndex:2];
    const NSUInteger items = (NSUInteger)VMAF_MTL_FADM_TERM_SLOTS * (NSUInteger)region_h;
    [enc dispatchThreadgroups:MTLSizeMake((items + FADM_ROW_TG - 1u) / FADM_ROW_TG, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(FADM_ROW_TG, 1, 1)];
    [enc endEncoding];
}

int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                            VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                            VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    FloatAdmStateMetal *s = (FloatAdmStateMetal *)fex->priv;
    s->index = index;

    void *const dh = vmaf_metal_context_device_handle(s->ctx);
    void *const qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == nullptr || qh == nullptr) { return -ENODEV; }
    id<MTLCommandQueue> const queue = (__bridge id<MTLCommandQueue>)qh;

    fill_raw_plane(ref_pic, (__bridge id<MTLBuffer>)s->src_ref, s->width, s->height, s->bpc);
    fill_raw_plane(dist_pic, (__bridge id<MTLBuffer>)s->src_dis, s->width, s->height, s->bpc);

    id<MTLCommandBuffer> const cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }

    for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
        encode_dwt(s, cmd, scale);
        encode_decouple(s, cmd, scale);
        encode_terms_and_rows(s, cmd, scale);
    }

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}
} // namespace

namespace {

/* One scale of compute_adm() past the kernels. The frame accumulators are the
 * reference's: one fp32 value per band that the row sums are added to top to
 * bottom. The scale is then concluded by the reference's own
 * adm_pool_bands_s(), with the noise weight for the denominator and the adm2
 * numerator and with none for the AIM numerator. */
using FadmScaleSums = struct FadmScaleSums {
    float numerator;
    float denominator;
    float aim_numerator;
};
} // namespace

namespace {

FadmScaleSums pool_scale(const FloatAdmStateMetal *s, int scale)
{
    FadmScaleSums sums = {.numerator=0.0f, .denominator=0.0f, .aim_numerator=0.0f};
    if (scale == 0 && s->adm_skip_scale0) {
        /* compute_adm(): `den_scale = 1e-10; // avoid divide by zero`. */
        sums.denominator = (float)1e-10;
        return sums;
    }
    const AdmBorderS *r = &s->region[scale];
    const int region_w = r->right - r->left;
    const int region_h = r->bottom - r->top;
    const float *rows =
        (const float *)[(__bridge id<MTLBuffer>)s->rows contents] + s->row_offset[scale];
    float accum[VMAF_MTL_FADM_TERM_SLOTS] = {0.0f};
    if (region_w > 0 && region_h > 0) {
        for (unsigned slot = 0u; slot < VMAF_MTL_FADM_TERM_SLOTS; ++slot) {
            accum[slot] = vmaf_mtl_fadm_fold_rows(rows + (size_t)slot * (size_t)region_h,
                                                  (vmaf_mtl_u32)region_h);
        }
    }
    sums.numerator = adm_pool_bands_s(accum + VMAF_MTL_FADM_SLOT_CM, region_w, region_h,
                                      s->adm_noise_weight, s->adm_p_norm);
    sums.denominator = adm_pool_bands_s(accum + VMAF_MTL_FADM_SLOT_DEN, region_w, region_h,
                                        s->adm_noise_weight, s->adm_p_norm);
    sums.aim_numerator = adm_pool_bands_s(accum + VMAF_MTL_FADM_SLOT_AIM, region_w, region_h, 0.0,
                                          s->adm_p_norm);
    return sums;
}

/* The frame's sums over the four scales, as compute_adm() adds them. */
struct FadmFrameSums {
    double score_num;
    double score_den;
    double aim_num;
    double aim_den;
    double scores[8];
};

FadmFrameSums pool_frame(const FloatAdmStateMetal *s)
{
    FadmFrameSums f = {};
    for (int scale = 0; scale < FADM_NUM_SCALES; ++scale) {
        const FadmScaleSums sums = pool_scale(s, scale);
        f.scores[2 * scale + 0] = sums.numerator;
        f.scores[2 * scale + 1] = sums.denominator;
        f.score_num += sums.numerator;
        f.score_den += sums.denominator;
        if (s->adm_skip_aim_scale != scale) {
            f.aim_den += sums.denominator;
            f.aim_num += sums.aim_numerator;
        }
    }
    return f;
}

/* The scores of one frame past the floor and the finalisation. */
struct FadmFrameScores {
    double score;
    double score_aim;
    double score_adm3;
    double scale_scores[FADM_NUM_SCALES];
};

int emit_frame_scores(const FloatAdmStateMetal *s, VmafFeatureCollector *fc, unsigned index,
                      const FadmFrameSums &f, const FadmFrameScores &r)
{
    VmafNamedScore values[18] = {
        {.name="VMAF_feature_adm2_score", .value=r.score},
        {.name="VMAF_feature_aim_score", .value=r.score_aim},
        {.name="VMAF_feature_adm3_score", .value=r.score_adm3},
        {.name="VMAF_feature_adm_scale0_score", .value=r.scale_scores[0]},
        {.name="VMAF_feature_adm_scale1_score", .value=r.scale_scores[1]},
        {.name="VMAF_feature_adm_scale2_score", .value=r.scale_scores[2]},
        {.name="VMAF_feature_adm_scale3_score", .value=r.scale_scores[3]},
    };
    size_t value_count = 7u;
    if (s->debug) {
        static const char *const debug_names[8] = {
            "adm_num_scale0", "adm_den_scale0", "adm_num_scale1", "adm_den_scale1",
            "adm_num_scale2", "adm_den_scale2", "adm_num_scale3", "adm_den_scale3"};
        values[value_count++] = VmafNamedScore{.name="adm", .value=r.score};
        values[value_count++] = VmafNamedScore{.name="adm_num", .value=f.score_num};
        values[value_count++] = VmafNamedScore{.name="adm_den", .value=f.score_den};
        for (size_t i = 0u; i < 8u; ++i)
            values[value_count++] = VmafNamedScore{.name=debug_names[i], .value=f.scores[i]};
    }
    return vmaf_feature_emit_finite_scores(fc, s->feature_name_dict, "float_adm_metal", values,
                                           value_count, index);
}

int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    FloatAdmStateMetal *const s = (FloatAdmStateMetal *)fex->priv;
    FadmFrameSums f = pool_frame(s);

    /* compute_adm()'s floor of the frame sums, in its expression: 1e-10 of
     * the area relative to 1080p. The 1e-2 the twin used belonged to an
     * ADM_OPT_SINGLE_PRECISION branch no build defined, and floored sums
     * the CPU keeps (adm_noise_weight = 0 on a flat frame). */
    const int w = (int)s->scale_w[0];
    const int h = (int)s->scale_h[0];
    const double numden_limit = 1e-10 * (w * h) / (1920.0 * 1080.0);
    FadmFrameScores r = {};
    int err = vmaf_adm_floor_pair_named("float_adm_metal", index, f.score_num, f.score_den,
                                        numden_limit, &f.score_num, &f.score_den);
    if (err)
        return err;
    err = vmaf_adm_finalize_scores_named("float_adm_metal", index, f.score_num, f.score_den,
                                         f.aim_num, f.aim_den, &r.score, &r.score_aim);
    if (err)
        return err;
    err = vmaf_adm3_score_named("float_adm_metal", index, r.score, r.score_aim,
                                s->adm_adm3_apply_hm, s->adm_dlm_weight, s->adm_min_val,
                                &r.score_adm3);
    if (err)
        return err;
    err = vmaf_adm_scale_ratios_named("float_adm_metal", index, f.scores, FADM_NUM_SCALES,
                                      r.scale_scores);
    if (err)
        return err;
    return emit_frame_scores(s, fc, index, f, r);
}

int close_fex_metal(VmafFeatureExtractor *fex)
{
    FloatAdmStateMetal *s = (FloatAdmStateMetal *)fex->priv;
    int rc = vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);

    release_psos(s);
    release_buffers(s);

    if (s->feature_name_dict) {
        int const err = vmaf_dictionary_free(&s->feature_name_dict);
        if (err != 0 && rc == 0) { rc = err; }
    }
    if (s->ctx) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = nullptr;
    }
    return rc;
}

/* provided_features matches the CPU float_adm.c list EXACTLY (same names,
 * same order) — the parity test and model JSONs depend on it. */
const char *provided_features[] = {"VMAF_feature_adm2_score",
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
                                          nullptr};
} // namespace

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
    .flush             = nullptr,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(FloatAdmStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 5 * FADM_NUM_SCALES,
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1920U * 1080U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
