/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  integer_adm feature extractor on the Metal backend (feature "adm" — the
 *  VMAF-default ADM path): the Metal API side. Each scale runs the DWT,
 *  decouple + CSF and the two masking reductions of integer_adm.metal; the
 *  host waits once per frame and concludes the reductions.
 *
 *  Everything that does not touch the Metal API is integer_adm_metal_host.c
 *  (plain C, shared with the host replay of the kernels,
 *  core/test/test_metal_integer_adm_host_replay.c, ADR-1806): the geometry
 *  and buffer sizes, the kernels each scale runs and their grids, the
 *  uniforms (the CPU's own shifts and rounding terms, from integer_adm.c's
 *  contexts) and the scores (the CPU's adm_cm_result() / adm_csf_den_result()
 *  and the checks of integer_adm.c's extract()). This file allocates the
 *  buffers, binds them and encodes the dispatches
 *  (T-METAL-INTEGER-ADM-TWIN-DEFECTS-2026-10-05).
 *
 *  provided_features[] mirrors the CPU integer_adm.c list EXACTLY (same
 *  names, same order) — the parity test and any model JSON depend on it.
 *
 *  Metallib resolution: embedded __TEXT,__metallib blob, same pattern as
 *  every other Metal feature extractor.
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

/* Declares its own C linkage; its standard headers are included above. */
#include "integer_adm_metal_host.h"

extern "C" {
#include "dict.h"
#include "feature_collector.h"
#include "feature_name.h"
#include "libvmaf/picture.h"

#include "../../metal/common.h"
#include "../../metal/kernel_template.h"
#include "../adm_csf_fixed_point.h"
#include "../adm_options.h"
#include "../nonfinite_score.h"
}

#include "../../metal/objc_handle.h"


namespace {

using IntegerAdmStateMetal = struct IntegerAdmStateMetal {
    VmafMetalKernelLifecycle lc;
    VmafMetalContext *ctx;

    /* Pipeline states, indexed by IadmMetalKernel. */
    void *pso[IADM_METAL_KERNEL_COUNT];

    /* Reused device buffers, sized by iadm_metal_buffer_bytes(). */
    void *src_ref;
    void *src_dis;
    void *dwt_tmp_ref;
    void *dwt_tmp_dis;
    void *ref_band[IADM_METAL_NUM_SCALES]; /* int16 at scale 0, int32 at scales 1-3 */
    void *dis_band[IADM_METAL_NUM_SCALES];
    void *csf_a;
    void *csf_f;
    void *accum[IADM_METAL_NUM_SCALES]; /* reduction slots, vmaf_mtl_iadm_accum_word() */

    IadmMetalGeometry geom;

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
};
} // namespace

namespace {

/* Options mirror integer_adm.c EXACTLY (names, aliases, defaults, ranges). */
const VmafOption options[] = {
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
     .help = "contrast sensitivity function",
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
    {.name=nullptr},
};

/* The options as integer_adm_metal_host.c reads them. */
IadmMetalOptions iadm_options(const IntegerAdmStateMetal *s)
{
    IadmMetalOptions o;
    memset(&o, 0, sizeof(o));
    o.adm_enhn_gain_limit = s->adm_enhn_gain_limit;
    o.adm_norm_view_dist = s->adm_norm_view_dist;
    o.adm_csf_scale = s->adm_csf_scale;
    o.adm_csf_diag_scale = s->adm_csf_diag_scale;
    o.adm_noise_weight = s->adm_noise_weight;
    o.adm_min_val = s->adm_min_val;
    o.adm_dlm_weight = s->adm_dlm_weight;
    o.adm_p_norm = s->adm_p_norm;
    o.adm_ref_display_height = s->adm_ref_display_height;
    o.adm_csf_mode = s->adm_csf_mode;
    o.adm_skip_aim = s->adm_skip_aim;
    o.adm_skip_scale0 = s->adm_skip_scale0;
    return o;
}

int build_pipelines(IntegerAdmStateMetal *s, id<MTLDevice> device)
{
    int load_rc = 0;
    id<MTLLibrary> const lib = vmaf_metal_library_load(device, &load_rc);
    if (lib == nil) { return load_rc; }
    NSError *err = nil;

    for (int k = 0; k < IADM_METAL_KERNEL_COUNT; ++k) {
        const char *name = iadm_metal_kernel_name((IadmMetalKernel)k);
        NSString *const fn_name = [NSString stringWithUTF8String:name];
        id<MTLFunction> const fn = (fn_name != nil) ? [lib newFunctionWithName:fn_name] : nil;
        if (fn == nil) { return -ENODEV; }
        id<MTLComputePipelineState> const pso = [device newComputePipelineStateWithFunction:fn error:&err];
        if (pso == nil) { return -ENODEV; }
        s->pso[k] = (__bridge_retained void *)pso;
    }
    return 0;
}

void release_psos(IntegerAdmStateMetal *s)
{
    for (auto & k : s->pso) {
        if (k) {
            (void)(__bridge_transfer id<MTLComputePipelineState>)k;
            k = nullptr;
        }
    }
}

void release_buffer(void **slot)
{
    if (*slot) {
        (void)(__bridge_transfer id<MTLBuffer>)(*slot);
        *slot = nullptr;
    }
}

void release_buffers(IntegerAdmStateMetal *s)
{
    for (int i = 0; i < IADM_METAL_NUM_SCALES; ++i) {
        release_buffer(&s->ref_band[i]);
        release_buffer(&s->dis_band[i]);
        release_buffer(&s->accum[i]);
    }
    release_buffer(&s->src_ref);
    release_buffer(&s->src_dis);
    release_buffer(&s->dwt_tmp_ref);
    release_buffer(&s->dwt_tmp_dis);
    release_buffer(&s->csf_a);
    release_buffer(&s->csf_f);
}

int new_buffer(id<MTLDevice> device, size_t bytes, void **slot)
{
    id<MTLBuffer> const b = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (b == nil) { return -ENOMEM; }
    *slot = (__bridge_retained void *)b;
    return 0;
}

/* Every device buffer at the size integer_adm_metal_host.c gives it. */
int alloc_buffers(IntegerAdmStateMetal *s, id<MTLDevice> device)
{
    const IadmMetalGeometry *g = &s->geom;
    const size_t source = iadm_metal_buffer_bytes(g, IADM_METAL_BUF_SOURCE, 0);
    const size_t dwt = iadm_metal_buffer_bytes(g, IADM_METAL_BUF_DWT_TMP, 0);
    const size_t csf = iadm_metal_buffer_bytes(g, IADM_METAL_BUF_CSF, 0);
    int err = new_buffer(device, source, &s->src_ref);
    if (err == 0) { err = new_buffer(device, source, &s->src_dis); }
    if (err == 0) { err = new_buffer(device, dwt, &s->dwt_tmp_ref); }
    if (err == 0) { err = new_buffer(device, dwt, &s->dwt_tmp_dis); }
    if (err == 0) { err = new_buffer(device, csf, &s->csf_a); }
    if (err == 0) { err = new_buffer(device, csf, &s->csf_f); }
    for (int scale = 0; scale < IADM_METAL_NUM_SCALES && err == 0; ++scale) {
        const size_t band = iadm_metal_buffer_bytes(g, IADM_METAL_BUF_BAND, scale);
        const size_t accum = iadm_metal_buffer_bytes(g, IADM_METAL_BUF_ACCUM, scale);
        err = new_buffer(device, band, &s->ref_band[scale]);
        if (err == 0) { err = new_buffer(device, band, &s->dis_band[scale]); }
        if (err == 0) { err = new_buffer(device, accum, &s->accum[scale]); }
    }
    return err;
}

int init_device_state(IntegerAdmStateMetal *s)
{
    const void *const dh = vmaf_metal_context_device_handle(s->ctx);
    if (dh == nullptr) { return -ENODEV; }
    id<MTLDevice> const device = (__bridge id<MTLDevice>)dh;
    int err = alloc_buffers(s, device);
    if (err == 0) { err = build_pipelines(s, device); }
    return err;
}

void teardown_device_state(IntegerAdmStateMetal *s)
{
    release_psos(s);
    release_buffers(s);
    (void)vmaf_metal_kernel_lifecycle_close(&s->lc, s->ctx);
    vmaf_metal_context_destroy(s->ctx);
    s->ctx = nullptr;
}

int init_fex_metal(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                          unsigned w, unsigned h)
{
    (void)pix_fmt;
    IntegerAdmStateMetal *s = (IntegerAdmStateMetal *)fex->priv;

    int err = adm_frame_size_check("integer_adm_metal", w, h);
    if (err != 0) { return err; }
    /* The CPU's CSF configuration check (adm_csf_check_scale() per scale),
     * before any device work. */
    const IadmMetalOptions o = iadm_options(s);
    err = iadm_metal_check_options(&o);
    if (err != 0) { return err; }
    iadm_metal_geometry(&s->geom, w, h, bpc);

    err = vmaf_metal_context_new(&s->ctx, 0);
    if (err != 0) { return err; }
    err = vmaf_metal_kernel_lifecycle_init(&s->lc, s->ctx);
    if (err != 0) {
        vmaf_metal_context_destroy(s->ctx);
        s->ctx = nullptr;
        return err;
    }
    err = init_device_state(s);
    if (err == 0) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (s->feature_name_dict == nullptr) { err = -ENOMEM; }
    }
    if (err != 0) { teardown_device_state(s); }
    return err;
}

void fill_raw_plane(VmafPicture *pic, id<MTLBuffer> dst, unsigned w, unsigned h, unsigned bpc)
{
    const size_t bpp = (bpc <= 8u) ? 1u : 2u;
    const size_t row_bytes = (size_t)w * bpp;
    uint8_t *out = (uint8_t *)[dst contents];
    for (unsigned y = 0; y < h; ++y) {
        memcpy(out + (size_t)y * row_bytes,
               (const uint8_t *)pic->data[0] + (size_t)y * pic->stride[0], row_bytes);
    }
}

void bind_buffer(id<MTLComputeCommandEncoder> enc, void *buffer, NSUInteger index)
{
    [enc setBuffer:(__bridge id<MTLBuffer>)buffer offset:0 atIndex:index];
}

/* The buffers of `entry` at the indices integer_adm.metal declares. */
void bind_stage_buffers(IntegerAdmStateMetal *s, id<MTLComputeCommandEncoder> enc,
                               IadmMetalKernel entry, int scale)
{
    switch (entry) {
    case IADM_METAL_DWT_VERT_8BPC:
    case IADM_METAL_DWT_VERT_16BPC:
        bind_buffer(enc, s->src_ref, 0);
        bind_buffer(enc, s->src_dis, 1);
        bind_buffer(enc, s->dwt_tmp_ref, 2);
        bind_buffer(enc, s->dwt_tmp_dis, 3);
        break;
    case IADM_METAL_DWT_VERT_S1:
    case IADM_METAL_DWT_VERT_S123:
        /* Only scales 1..3 reach these stages; clamp so the index is never negative. */
        bind_buffer(enc, s->ref_band[(scale > 0) ? scale - 1 : 0], 6);
        bind_buffer(enc, s->dis_band[(scale > 0) ? scale - 1 : 0], 7);
        bind_buffer(enc, s->dwt_tmp_ref, 2);
        bind_buffer(enc, s->dwt_tmp_dis, 3);
        break;
    case IADM_METAL_DWT_HORI_S0:
    case IADM_METAL_DWT_HORI_S123:
        bind_buffer(enc, s->dwt_tmp_ref, 0);
        bind_buffer(enc, s->dwt_tmp_dis, 1);
        bind_buffer(enc, s->ref_band[scale], 2);
        bind_buffer(enc, s->dis_band[scale], 3);
        break;
    case IADM_METAL_DECOUPLE_CSF_S0:
    case IADM_METAL_DECOUPLE_CSF_S123:
        bind_buffer(enc, s->ref_band[scale], 0);
        bind_buffer(enc, s->dis_band[scale], 1);
        bind_buffer(enc, s->csf_a, 2);
        bind_buffer(enc, s->csf_f, 3);
        break;
    case IADM_METAL_CSF_CM_S0:
    case IADM_METAL_CSF_CM_S123:
        bind_buffer(enc, s->ref_band[scale], 0);
        bind_buffer(enc, s->dis_band[scale], 1);
        bind_buffer(enc, s->csf_f, 3);
        bind_buffer(enc, s->accum[scale], 8);
        break;
    case IADM_METAL_AIM_CM_S0:
    case IADM_METAL_AIM_CM_S123:
        bind_buffer(enc, s->ref_band[scale], 0);
        bind_buffer(enc, s->dis_band[scale], 1);
        bind_buffer(enc, s->accum[scale], 8);
        break;
    default:
        break;
    }
}

void encode_stage(IntegerAdmStateMetal *s, id<MTLCommandBuffer> cmd,
                         const IadmMetalStage *stage, int scale, const IadmDims *d,
                         const IadmCsf *c)
{
    id<MTLComputeCommandEncoder> const enc = [cmd computeCommandEncoder];
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)s->pso[stage->entry]];
    bind_stage_buffers(s, enc, stage->entry, scale);
    [enc setBytes:d length:sizeof(*d) atIndex:4];
    [enc setBytes:c length:sizeof(*c) atIndex:5];
    [enc dispatchThreadgroups:MTLSizeMake(stage->groups[0], stage->groups[1], stage->groups[2])
        threadsPerThreadgroup:MTLSizeMake(stage->threads[0], stage->threads[1],
                                          stage->threads[2])];
    [enc endEncoding];
}

/* Zero the reduction slots (a skipped stage must contribute 0). */
void zero_accumulators(IntegerAdmStateMetal *s, id<MTLCommandBuffer> cmd)
{
    id<MTLBlitCommandEncoder> const blit = [cmd blitCommandEncoder];
    for (int scale = 0; scale < IADM_METAL_NUM_SCALES; ++scale) {
        const size_t bytes = iadm_metal_buffer_bytes(&s->geom, IADM_METAL_BUF_ACCUM, scale);
        [blit fillBuffer:(__bridge id<MTLBuffer>)s->accum[scale] range:NSMakeRange(0, bytes) value:0];
    }
    [blit endEncoding];
}

int submit_fex_metal(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                            VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    IntegerAdmStateMetal *s = (IntegerAdmStateMetal *)fex->priv;
    s->index = index;

    const void *const dh = vmaf_metal_context_device_handle(s->ctx);
    const void *const qh = vmaf_metal_context_queue_handle(s->ctx);
    if (dh == nullptr || qh == nullptr) { return -ENODEV; }
    id<MTLCommandQueue> const queue = (__bridge id<MTLCommandQueue>)qh;

    fill_raw_plane(ref_pic, (__bridge id<MTLBuffer>)s->src_ref, s->geom.w, s->geom.h, s->geom.bpc);
    fill_raw_plane(dist_pic, (__bridge id<MTLBuffer>)s->src_dis, s->geom.w, s->geom.h,
                   s->geom.bpc);

    id<MTLCommandBuffer> const cmd = [queue commandBuffer];
    if (cmd == nil) { return -ENOMEM; }
    zero_accumulators(s, cmd);

    const IadmMetalOptions o = iadm_options(s);
    for (int scale = 0; scale < IADM_METAL_NUM_SCALES; ++scale) {
        IadmDims d;
        IadmCsf c;
        iadm_metal_uniforms(&o, &s->geom, scale, &d, &c);
        IadmMetalStage stages[IADM_METAL_MAX_STAGES];
        const unsigned count = iadm_metal_stages(&o, &s->geom, scale, stages);
        for (unsigned i = 0; i < count; ++i) {
            encode_stage(s, cmd, &stages[i], scale, &d, &c);
        }
    }

    [cmd commit];
    [cmd waitUntilCompleted];
    return 0;
}

int emit_scores(IntegerAdmStateMetal *s, VmafFeatureCollector *fc, const IadmMetalScores *r,
                       unsigned index)
{
    VmafNamedScore values[18] = {
        {.name="VMAF_integer_feature_adm2_score", .value=r->score},
        {.name="VMAF_integer_feature_aim_score", .value=r->score_aim},
        {.name="VMAF_integer_feature_adm3_score", .value=r->score_adm3},
        {.name="integer_adm_scale0", .value=r->scale_scores[0]},
        {.name="integer_adm_scale1", .value=r->scale_scores[1]},
        {.name="integer_adm_scale2", .value=r->scale_scores[2]},
        {.name="integer_adm_scale3", .value=r->scale_scores[3]},
    };
    size_t value_count = 7u;
    if (s->debug) {
        static const char *const debug_names[8] = {
            "integer_adm_num_scale0", "integer_adm_den_scale0", "integer_adm_num_scale1",
            "integer_adm_den_scale1", "integer_adm_num_scale2", "integer_adm_den_scale2",
            "integer_adm_num_scale3", "integer_adm_den_scale3"};
        values[value_count++] = VmafNamedScore{.name="integer_adm", .value=r->score};
        values[value_count++] = VmafNamedScore{.name="integer_adm_num", .value=r->score_num};
        values[value_count++] = VmafNamedScore{.name="integer_adm_den", .value=r->score_den};
        for (size_t i = 0u; i < 8u; ++i)
            values[value_count++] = VmafNamedScore{.name=debug_names[i], .value=r->scores[i]};
    }
    return vmaf_feature_emit_finite_scores(fc, s->feature_name_dict, "integer_adm_metal", values,
                                           value_count, index);
}

int collect_fex_metal(VmafFeatureExtractor *fex, unsigned index, VmafFeatureCollector *fc)
{
    IntegerAdmStateMetal *s = (IntegerAdmStateMetal *)fex->priv;

    const uint32_t *accum[IADM_METAL_NUM_SCALES];
    for (int scale = 0; scale < IADM_METAL_NUM_SCALES; ++scale) {
        accum[scale] = (const uint32_t *)[(__bridge id<MTLBuffer>)s->accum[scale] contents];
    }
    const IadmMetalOptions o = iadm_options(s);
    IadmMetalScores r;
    const int err = iadm_metal_scores(&o, &s->geom, accum, index, &r);
    if (err) { return err; }
    return emit_scores(s, fc, &r, index);
}

int close_fex_metal(VmafFeatureExtractor *fex)
{
    IntegerAdmStateMetal *s = (IntegerAdmStateMetal *)fex->priv;
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

/* provided_features matches the CPU integer_adm.c list EXACTLY (same names,
 * same order) — the parity test and model JSONs depend on it. Note the
 * VMAF_integer_feature_* / integer_adm_* prefixes (distinct from float_adm's
 * VMAF_feature_* / adm_* names). */
const char *provided_features[] = {"VMAF_integer_feature_adm2_score",
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
                                          nullptr};
} // namespace

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
    .flush             = nullptr,
    .close             = close_fex_metal,
    .options           = options,
    .priv_size         = sizeof(IntegerAdmStateMetal),
    .provided_features = provided_features,
    .flags             = VMAF_FEATURE_EXTRACTOR_METAL,
    .chars = {
        .n_dispatches_per_frame = 6 * IADM_METAL_NUM_SCALES,
        .is_reduction_only      = false,
        .min_useful_frame_area  = 1280U * 720U,
        .dispatch_hint          = VMAF_FEATURE_DISPATCH_AUTO,
    },
};
} /* extern "C" */
