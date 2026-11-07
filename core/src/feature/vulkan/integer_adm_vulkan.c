/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  integer_adm_vulkan.c — Integer ADM (Adaptive Detail Model) feature
 *  extractor on the opt-in Vulkan compute path (ADR-3402; partial
 *  revival of the backend removed by ADR-0726).
 *
 *  Restored from the pre-ADR-0726 runtime (snapshot 816a5f33f) and
 *  reconciled to the PoC (`poc/vk-integer-adm` @ `0bb98ee21`, phase 1)
 *  whose conclude path reached bit-exact parity with the unmodified CPU
 *  extractor on the ADR-3402 gate fixtures (48 frames x 11 fields, both
 *  fixtures, RTX 5080 + RTX 4060; evidence #3302 comment 6689093406).
 *
 *  The pipeline (4 stages x 4 scales) and the device plumbing below are
 *  the snapshot's. The score conclude is the CPU's own code where the
 *  CPU exposes it as a shared inline (`integer_adm_kernels.h` /
 *  `adm_score.h` / `nonfinite_score.h`), so it cannot drift from the CPU
 *  contract; the hand-ported remainder follows the PoC's CPU-exact
 *  expressions. Known snapshot-host bugs fixed through the PoC
 *  (deviation 6 of ADR-3402):
 *    - `numden_limit` uses the BASE frame `w*h`
 *      (integer_adm.c:909), not the last scale's half dims;
 *    - the s>0 CM conclude uses the CPU's float division
 *      `(float)(accum / final_shift)` (integer_adm_kernels.h:1228-1235),
 *      not a double division;
 *    - the noise floor uses the CPU's double product
 *      `(float)(area * (double)adm_noise_weight)` (:604);
 *    - the per-band sums keep the CPU's left-associated `((h + v) + d)`
 *      grouping (:1084, :1243).
 *
 *  Frame transport is host upload/download (#3302 Phase 1); zero-copy
 *  import is a later optimization and the old `import.c` entry points
 *  are NOT restored (superseded by the ADR-2152 import surface).
 *
 *  Scope note: aim_score and adm3_score (CPU-only debug paths) are NOT
 *  emitted here, same as the SYCL ADM port. The canonical extractor
 *  name is `integer_adm_vulkan`; the GLSL kernel is
 *  shaders/integer_adm.comp (the two-level reduction kernel
 *  `integer_adm_reduce.comp` is deliberately not restored — its
 *  subgroupAdd(int64_t) is banned by ADR-3402 and the shader folds the
 *  stage-3 reduction as a shared-memory serial sum).
 */

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "feature/adm_score.h"
#include "feature/integer_adm_kernels.h"
#include "feature/nonfinite_score.h"
/* The DEFAULT_ADM_* macros and enum ADM_CSF_MODE arrive transitively via
 * feature/integer_adm_kernels.h -> feature/integer_adm.h. No separate
 * adm_options.h include: its 3-member enum ADM_CSF_MODE conflicts with the
 * 4-member one the integer kernels require (same resolution as the CUDA
 * twin, feature/cuda/integer_adm_cuda.c). */
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "log.h"

#include "../../vulkan/kernel_template.h"
#include "../../vulkan/vulkan_common.h"
#include "../../vulkan/picture_vulkan.h"
#include "../../vulkan/vulkan_internal.h"

#include "integer_adm_spv.h" /* per-WG accumulator kernel (ADR-0468 -> ADR-3402) */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Constants — must match integer_adm.comp + integer_adm_kernels.h.   */
/* (ADM_BORDER_FACTOR / DEFAULT_ADM_* / ONE_BY_15 come from           */
/* adm_options.h / integer_adm.h via integer_adm_kernels.h.)          */
/* ------------------------------------------------------------------ */

#define ADM_NUM_SCALES 4
#define ADM_NUM_BANDS 3
#define ADM_NUM_STAGES 4
#define ADM_DIV_LOOKUP_SIZE 65537
#define ADM_ACCUM_SLOTS_PER_WG 6 /* csf_h,csf_v,csf_d, cm_h,cm_v,cm_d */

#define ADM_WG_X 16
#define ADM_WG_Y 16
#define ADM_WG_SIZE (ADM_WG_X * ADM_WG_Y)

/* The Vulkan twin serves the CPU's default CSF model and p-norm
 * (WATSON97 + adm_p_norm 3.0) — the configuration the ADR-3402 parity
 * gate pins. `adm_csf_scale` / `adm_csf_diag_scale` are carried as
 * feature options for surface compatibility, but like the CPU's
 * WATSON97 path (adm_csf_factors' else branch) they do not enter the
 * weight computation; the Barten CSF modes are a separate feature that
 * has not passed the parity gate. */
#define VULKAN_ADM_CSF_MODE (DEFAULT_ADM_CSF_MODE)
#define VULKAN_ADM_P_NORM (3.0)

/* ------------------------------------------------------------------ */
/* Per-extractor state.                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Options. */
    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;

    /* Frame geometry. */
    unsigned width;
    unsigned height;
    unsigned bpc;
    unsigned buf_stride; /* aligned int32 elements per band row */

    /* Per-scale integer CSF weights (the CPU's fixed-point derivation,
     * shared code) and the per-scale normalisation exponent the CM
     * conclude must restore (3 * shift bits, adm_cm_restore_accum /
     * i4_adm_cm_result). */
    uint32_t i_rfactor[12]; /* 4 scales × 3 bands */
    uint32_t normalization_shift[ADM_NUM_SCALES];

    /* Vulkan context handle. The opt-in path always owns its context:
     * the public state-level API (VmafVulkanState) is not restored
     * (ADR-0726's public-API removal stands). */
    VmafVulkanContext *ctx;

    /* Pipelines: one per (stage, scale) combination = 4 stages × 4 scales = 16. */
    /* `pl` carries the shared layout / shader / DSL / pool plus the
     * (stage=0, scale=0) base pipeline. The other 15 entries in
     * `pipelines[stage][scale]` are sibling pipelines created via
     * `vmaf_vulkan_kernel_pipeline_add_variant()`.
     * `pipelines[0][0]` aliases `pl.pipeline` so the dispatch path
     * keeps a clean 2-D lookup. (ADR-0221.) */
    VmafVulkanKernelPipeline pl;
    VkPipeline pipelines[ADM_NUM_STAGES][ADM_NUM_SCALES];

    /* Submit-side pool (T-GPU-OPT-VK-1 / ADR-0256). One slot suffices:
     * all 16 dispatches are batched into a single command buffer. */
    VmafVulkanKernelSubmitPool sub_pool;

    /* Pre-allocated descriptor sets, one per scale (T-GPU-OPT-VK-4 /
     * ADR-0256). All 9 bindings (including the per-scale accum buffer at
     * slot 7) are init-time-stable; no vkUpdateDescriptorSets is needed
     * in extract(). */
    VkDescriptorSet pre_sets[ADM_NUM_SCALES];

    /* GPU buffers. dwt_tmp[]: int32 plane, sized for the largest
     * (scale 0) frame (cur_w*2 × half_h elements). All scales reuse
     * the same buffer since they fit. */
    VmafVulkanBuffer *src_ref; /* scale-0 host-uploaded source plane */
    VmafVulkanBuffer *src_dis;
    VmafVulkanBuffer *dwt_tmp_ref;
    VmafVulkanBuffer *dwt_tmp_dis;

    /* Band buffers: 4 bands packed into one SSBO each, addressed as
     * band_idx * (buf_stride * half_h) + y*buf_stride + x. */
    VmafVulkanBuffer *ref_band;
    VmafVulkanBuffer *dis_band;
    VmafVulkanBuffer *csf_f; /* 3 bands packed contiguously */

    VmafVulkanBuffer *div_lookup;

    /* Per-WG int64 accumulator buffers, one per scale. The host sums
     * the per-WG slots after the dispatch (the ADR-0350 two-level
     * reduction kernel is not restored: subgroupAdd(int64_t) is banned
     * by ADR-3402 and the reduction is an order-independent serial sum
     * either way). */
    VmafVulkanBuffer *accum[ADM_NUM_SCALES];
    unsigned wg_count[ADM_NUM_SCALES];

    /* Per-scale dimensions cached. */
    unsigned scale_w[ADM_NUM_SCALES];
    unsigned scale_h[ADM_NUM_SCALES];
    unsigned scale_half_w[ADM_NUM_SCALES];
    unsigned scale_half_h[ADM_NUM_SCALES];

    VmafDictionary *feature_name_dict;
} AdmVulkanState;

/* ------------------------------------------------------------------ */
/* Options.                                                            */
/* ------------------------------------------------------------------ */

static const VmafOption options[] = {{
                                         .name = "debug",
                                         .help = "debug mode: enable additional output",
                                         .offset = offsetof(AdmVulkanState, debug),
                                         .type = VMAF_OPT_TYPE_BOOL,
                                         .default_val.b = false,
                                     },
                                     {
                                         .name = "adm_enhn_gain_limit",
                                         .help = "enhancement gain imposed on ADM, must be >= 1.0",
                                         .offset = offsetof(AdmVulkanState, adm_enhn_gain_limit),
                                         .type = VMAF_OPT_TYPE_DOUBLE,
                                         .default_val.d = DEFAULT_ADM_ENHN_GAIN_LIMIT,
                                         .min = 1.0,
                                         .max = 100.0,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {
                                         .name = "adm_norm_view_dist",
                                         .help = "normalized viewing distance",
                                         .offset = offsetof(AdmVulkanState, adm_norm_view_dist),
                                         .type = VMAF_OPT_TYPE_DOUBLE,
                                         .default_val.d = DEFAULT_ADM_NORM_VIEW_DIST,
                                         .min = 0.75,
                                         .max = 24.0,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {
                                         .name = "adm_ref_display_height",
                                         .help = "reference display height in pixels",
                                         .offset = offsetof(AdmVulkanState, adm_ref_display_height),
                                         .type = VMAF_OPT_TYPE_INT,
                                         .default_val.i = DEFAULT_ADM_REF_DISPLAY_HEIGHT,
                                         .min = 480.0,
                                         .max = 4320.0,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {
                                         .name = "adm_csf_scale",
                                         .alias = "cs",
                                         .help = "CSF band-scale multiplier for h/v bands "
                                                 "(inert: the Vulkan twin serves the WATSON97 "
                                                 "CSF model, which does not scale; see "
                                                 "adm_csf_factors)",
                                         .offset = offsetof(AdmVulkanState, adm_csf_scale),
                                         .type = VMAF_OPT_TYPE_DOUBLE,
                                         .default_val.d = DEFAULT_ADM_CSF_SCALE,
                                         .min = 0.0,
                                         .max = 100.0,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {
                                         .name = "adm_csf_diag_scale",
                                         .alias = "cds",
                                         .help = "CSF band-scale multiplier for diagonal bands "
                                                 "(inert: the Vulkan twin serves the WATSON97 "
                                                 "CSF model, which does not scale; see "
                                                 "adm_csf_factors)",
                                         .offset = offsetof(AdmVulkanState, adm_csf_diag_scale),
                                         .type = VMAF_OPT_TYPE_DOUBLE,
                                         .default_val.d = DEFAULT_ADM_CSF_DIAG_SCALE,
                                         .min = 0.0,
                                         .max = 100.0,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {
                                         .name = "adm_noise_weight",
                                         .alias = "nw",
                                         .help = "noise floor weight for CM numerator "
                                                 "(default 0.03125 = 1/32)",
                                         .offset = offsetof(AdmVulkanState, adm_noise_weight),
                                         .type = VMAF_OPT_TYPE_DOUBLE,
                                         .default_val.d = DEFAULT_ADM_NOISE_WEIGHT,
                                         .min = 0.0,
                                         .max = 100.0,
                                         .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
                                     },
                                     {0}};

/* ------------------------------------------------------------------ */
/* Push constants — must mirror `Params` in integer_adm.comp.          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t cur_w;
    uint32_t cur_h;
    uint32_t half_w;
    uint32_t half_h;
    uint32_t buf_stride;
    uint32_t in_stride;
    uint32_t v_shift;
    uint32_t v_add;
    uint32_t h_shift;
    uint32_t i_rfactor_h;
    uint32_t i_rfactor_v;
    uint32_t i_rfactor_d;
    uint32_t num_workgroups_x;
    int32_t gain_hi;
    int32_t gain_lo;
    int32_t active_left;
    int32_t active_top;
    int32_t active_right;
    int32_t active_bottom;
    uint32_t csf_shift_sq;
    uint32_t csf_shift_cub;
    uint32_t csf_shift_accum;
    uint32_t cm_shift_inner;
    uint32_t cm_shift_sub_h;
    uint32_t cm_shift_sub_d;
    uint32_t cm_shift_xsq_h;
    uint32_t cm_shift_xsq_d;
    uint32_t cm_shift_xcub_h;
    uint32_t cm_shift_xcub_d;
} AdmPushConsts;

/* ------------------------------------------------------------------ */
/* Q31 gain split helper.                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t gain_hi;
    int32_t gain_lo;
} GainQ31;

/* snapshot integer_adm_vulkan.c:315-320 (gain limit is 100.0 by default;
 * the PoC kept this helper verbatim and gated with it). */
static GainQ31 gain_to_q31(double gain_limit)
{
    int64_t q = (int64_t)llround(gain_limit * (double)(1LL << 31));
    GainQ31 g = {(int32_t)(q >> 16), (int32_t)(q & 0xFFFF)};
    return g;
}

/* ------------------------------------------------------------------ */
/* Pipeline / descriptor-set layout creation.                          */
/* ------------------------------------------------------------------ */

struct AdmSpecData {
    int32_t width;
    int32_t height;
    int32_t bpc;
    int32_t scale;
    int32_t stage;
};

static void adm_fill_spec(struct AdmSpecData *spec_data, VkSpecializationMapEntry map[5],
                          VkSpecializationInfo *spec_info, const AdmVulkanState *s, int stage,
                          int scale)
{
    spec_data->width = (int32_t)s->width;
    spec_data->height = (int32_t)s->height;
    spec_data->bpc = (int32_t)s->bpc;
    spec_data->scale = scale;
    spec_data->stage = stage;
    map[0] = (VkSpecializationMapEntry){.constantID = 0, .offset = 0, .size = sizeof(int32_t)};
    map[1] = (VkSpecializationMapEntry){.constantID = 1, .offset = 4, .size = sizeof(int32_t)};
    map[2] = (VkSpecializationMapEntry){.constantID = 2, .offset = 8, .size = sizeof(int32_t)};
    map[3] = (VkSpecializationMapEntry){.constantID = 3, .offset = 12, .size = sizeof(int32_t)};
    map[4] = (VkSpecializationMapEntry){.constantID = 4, .offset = 16, .size = sizeof(int32_t)};
    *spec_info = (VkSpecializationInfo){
        .mapEntryCount = 5,
        .pMapEntries = map,
        .dataSize = sizeof(*spec_data),
        .pData = spec_data,
    };
}

static int build_pipeline_for(AdmVulkanState *s, int stage, int scale, VkPipeline *out_pipeline)
{
    struct AdmSpecData spec_data = {0};
    VkSpecializationMapEntry map[5];
    VkSpecializationInfo spec_info = {0};
    adm_fill_spec(&spec_data, map, &spec_info, s, stage, scale);

    VkComputePipelineCreateInfo cpci = {
        .stage =
            {
                .pName = "main",
                .pSpecializationInfo = &spec_info,
            },
    };
    return vmaf_vulkan_kernel_pipeline_add_variant(s->ctx, &s->pl, &cpci, out_pipeline);
}

static int create_pipelines(AdmVulkanState *s)
{
    /* (stage=0, scale=0) is the base pipeline. The template owns
     * the shared layout / shader / DSL / pool plus this base.
     * 9 SSBOs per set; pool sized for 4 frames × 16 (stage,scale)
     * pairs = 64 sets. */
    struct AdmSpecData spec_data = {0};
    VkSpecializationMapEntry map[5];
    VkSpecializationInfo spec_info = {0};
    adm_fill_spec(&spec_data, map, &spec_info, s, /*stage=*/0, /*scale=*/0);

    const VmafVulkanKernelPipelineDesc desc = {
        .ssbo_binding_count = 9U,
        .push_constant_size = (uint32_t)sizeof(AdmPushConsts),
        .spv_bytes = integer_adm_spv,
        .spv_size = integer_adm_spv_size,
        .pipeline_create_info =
            {
                .stage =
                    {
                        .pName = "main",
                        .pSpecializationInfo = &spec_info,
                    },
            },
        .max_descriptor_sets = (uint32_t)(ADM_NUM_SCALES * ADM_NUM_STAGES * 4),
    };
    int err = vmaf_vulkan_kernel_pipeline_create(s->ctx, &desc, &s->pl);
    if (err)
        return err;
    s->pipelines[0][0] = s->pl.pipeline;

    /* The remaining 15 (stage, scale) variants — same layout,
     * shader, DSL, pool, different stage/scale spec-constants. */
    for (int stage = 0; stage < ADM_NUM_STAGES; stage++) {
        for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
            if (stage == 0 && scale == 0)
                continue;
            err = build_pipeline_for(s, stage, scale, &s->pipelines[stage][scale]);
            if (err)
                return err;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Buffer allocation.                                                  */
/* ------------------------------------------------------------------ */

static int alloc_buffers(AdmVulkanState *s)
{
    unsigned w = s->width;
    unsigned h = s->height;
    unsigned half_w0 = (w + 1) / 2;
    unsigned half_h0 = (h + 1) / 2;
    s->buf_stride = (half_w0 + 3u) & ~3u;

    /* Source planes (scale 0 only). */
    size_t bpp = (s->bpc <= 8) ? 1 : 2;
    size_t src_bytes = (size_t)w * h * bpp;
    int err = vmaf_vulkan_buffer_alloc(s->ctx, &s->src_ref, src_bytes);
    if (err)
        return err;
    err = vmaf_vulkan_buffer_alloc(s->ctx, &s->src_dis, src_bytes);
    if (err)
        return err;

    /* DWT scratch: max size occurs at scale 0: cur_w*2 × half_h elements. */
    size_t dwt_bytes = (size_t)w * 2 * half_h0 * sizeof(int32_t);
    err = vmaf_vulkan_buffer_alloc(s->ctx, &s->dwt_tmp_ref, dwt_bytes);
    if (err)
        return err;
    err = vmaf_vulkan_buffer_alloc(s->ctx, &s->dwt_tmp_dis, dwt_bytes);
    if (err)
        return err;

    /* Band buffers: 4 bands × buf_stride × half_h, max at scale 0. */
    size_t band_bytes = (size_t)4 * s->buf_stride * half_h0 * sizeof(int32_t);
    err = vmaf_vulkan_buffer_alloc(s->ctx, &s->ref_band, band_bytes);
    if (err)
        return err;
    err = vmaf_vulkan_buffer_alloc(s->ctx, &s->dis_band, band_bytes);
    if (err)
        return err;

    /* csf_f: 3 bands × buf_stride × half_h. */
    size_t csf_bytes = (size_t)3 * s->buf_stride * half_h0 * sizeof(int32_t);
    err = vmaf_vulkan_buffer_alloc(s->ctx, &s->csf_f, csf_bytes);
    if (err)
        return err;

    /* div_lookup. */
    size_t div_bytes = (size_t)ADM_DIV_LOOKUP_SIZE * sizeof(int32_t);
    err = vmaf_vulkan_buffer_alloc(s->ctx, &s->div_lookup, div_bytes);
    if (err)
        return err;
    int32_t *lut = vmaf_vulkan_buffer_host(s->div_lookup);
    memset(lut, 0, div_bytes);
    static const int32_t Q_factor = 1073741824; /* 2^30 */
    for (int i = 1; i <= 32768; i++) {
        int32_t recip = (int32_t)(Q_factor / i);
        lut[32768 + i] = recip;
        lut[32768 - i] = -recip;
    }
    err = vmaf_vulkan_buffer_flush(s->ctx, s->div_lookup);
    if (err)
        return err;

    /* Per-scale accumulators. The stage-3 launch grid is
     *    num_wg = 3 × num_active_rows
     * each WG writes 6 int64 slots. We allocate the upper bound for
     * each scale based on its dimensions. */
    unsigned cw = w, ch = h;
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        unsigned hw = (cw + 1) / 2;
        unsigned hh = (ch + 1) / 2;
        s->scale_w[scale] = cw;
        s->scale_h[scale] = ch;
        s->scale_half_w[scale] = hw;
        s->scale_half_h[scale] = hh;

        int top = (int)((double)hh * ADM_BORDER_FACTOR - 0.5);
        int bottom = (int)hh - top;
        if (top < 0)
            top = 0;
        unsigned num_rows = (unsigned)(bottom - top);
        if (num_rows == 0)
            num_rows = 1;
        unsigned wg_count = 3u * num_rows;
        s->wg_count[scale] = wg_count;
        size_t accum_bytes = (size_t)wg_count * ADM_ACCUM_SLOTS_PER_WG * sizeof(int64_t);
        if (accum_bytes == 0)
            accum_bytes = sizeof(int64_t);
        err = vmaf_vulkan_buffer_alloc_readback(s->ctx, &s->accum[scale], accum_bytes);
        if (err)
            return err;

        cw = hw;
        ch = hh;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* init().                                                             */
/* ------------------------------------------------------------------ */

/* Forward declarations — definitions are below `init()`, but `init()`
 * calls them via the per-scale loop after the descriptor sets are
 * pre-allocated. Without an explicit `static` forward decl the C
 * compiler creates an implicit non-static one at the call site, then
 * fails with "static declaration follows non-static declaration"
 * when it sees the actual definition further down. */
static void write_descriptor_set(AdmVulkanState *s, VkDescriptorSet set);
static void write_accum_binding(AdmVulkanState *s, VkDescriptorSet set, int scale);

static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    (void)pix_fmt;
    AdmVulkanState *s = fex->priv;
    s->width = w;
    s->height = h;
    s->bpc = bpc;

    /* Same frame-size floor as the CPU extractor (shared inline). */
    int err = adm_frame_size_check("integer_adm_vulkan", w, h);
    if (err)
        return err;

    /* Per-scale integer CSF weights via the CPU's own fixed-point
     * derivation (adm_csf_factors + adm_csf_fixed_scale through
     * adm_csf_rfactor_*), including the tabulated {36453, 36453, 49417}
     * scale-0 fast path at the default geometry (adm_csf_fixed_point.h).
     * The returned normalization exponent is threaded into the CM
     * conclude exactly as the CPU does (adm_cm_restore_accum /
     * i4_adm_cm_result); it is 0 for every configuration the parity
     * gate pins. */
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        const AdmCsfFactors f = adm_csf_factors(scale, s->adm_norm_view_dist,
                                                s->adm_ref_display_height, VULKAN_ADM_CSF_MODE,
                                                s->adm_csf_scale, s->adm_csf_diag_scale);
        const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
        if (scale == 0) {
            uint16_t i_rfactor0[3];
            s->normalization_shift[0] = adm_csf_rfactor_scale0(
                rfactor1, s->adm_norm_view_dist, s->adm_ref_display_height, VULKAN_ADM_CSF_MODE,
                i_rfactor0);
            for (unsigned band = 0; band < 3; band++)
                s->i_rfactor[band] = i_rfactor0[band];
        } else {
            s->normalization_shift[scale] = adm_csf_rfactor_s123(
                scale, rfactor1, s->adm_norm_view_dist, s->adm_ref_display_height,
                VULKAN_ADM_CSF_MODE, &s->i_rfactor[scale * 3]);
        }
        if (s->normalization_shift[scale] != 0u) {
            vmaf_log(VMAF_LOG_LEVEL_WARNING,
                     "integer_adm_vulkan: scale %d needed a CSF normalization "
                     "shift of %u at adm_norm_view_dist=%g, "
                     "adm_ref_display_height=%d; the conclude restores it "
                     "exactly as the CPU does, but this geometry is not "
                     "covered by the ADR-3402 parity gate.\n",
                     scale, s->normalization_shift[scale], s->adm_norm_view_dist,
                     s->adm_ref_display_height);
        }
    }

    /* Lazy-create the Vulkan context. Device selection is ordinal-based
     * here (auto: discrete > integrated > virtual > cpu); the
     * vendorID/deviceID identity gate lives in the probe
     * (feature/vulkan/probe/), and every init log carries the attached
     * identity (research-0090: ordinals lie across drivers, identities
     * do not). */
    int err2 = vmaf_vulkan_context_new(&s->ctx, /*device_index=*/-1);
    if (err2) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_adm_vulkan: cannot create Vulkan context (%d)\n",
                 err2);
        return err2;
    }

    err2 = create_pipelines(s);
    if (err2)
        return err2;
    err2 = alloc_buffers(s);
    if (err2)
        return err2;

    err2 = vmaf_vulkan_kernel_submit_pool_create(s->ctx, /*slot_count=*/1, &s->sub_pool);
    if (err2)
        return err2;

    err2 = vmaf_vulkan_kernel_descriptor_sets_alloc(s->ctx, s->pl.desc_pool, s->pl.dsl,
                                                    (uint32_t)ADM_NUM_SCALES, s->pre_sets);
    if (err2)
        return err2;

    /* Write all 9 bindings once — all are init-time-stable.
     * write_descriptor_set fills slots 0-6 and 8; write_accum_binding
     * fills slot 7 (one per-scale accum buffer). Neither changes
     * between frames, so no per-frame vkUpdateDescriptorSets is
     * needed in extract(). (T-GPU-OPT-VK-4 / ADR-0256.) */
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        write_descriptor_set(s, s->pre_sets[scale]);
        write_accum_binding(s, s->pre_sets[scale], scale);
    }

    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-frame helpers.                                                  */
/* ------------------------------------------------------------------ */

static int upload_plane(AdmVulkanState *s, VmafPicture *pic, VmafVulkanBuffer *buf)
{
    uint8_t *dst = vmaf_vulkan_buffer_host(buf);
    const uint8_t *src = (const uint8_t *)pic->data[0];
    size_t src_stride = pic->stride[0];
    size_t dst_stride = (s->bpc <= 8) ? s->width : (s->width * 2);
    for (unsigned y = 0; y < s->height; y++)
        memcpy(dst + y * dst_stride, src + y * src_stride, dst_stride);
    return vmaf_vulkan_buffer_flush(s->ctx, buf);
}

static void write_descriptor_set(AdmVulkanState *s, VkDescriptorSet set)
{
    VkDescriptorBufferInfo dbi[9] = {
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->src_ref),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->dwt_tmp_ref),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->dwt_tmp_dis),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->ref_band),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->dis_band),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->csf_f),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->div_lookup),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = VK_NULL_HANDLE, /* accum bound per-scale below */
         .offset = 0,
         .range = VK_WHOLE_SIZE},
        {.buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->src_dis),
         .offset = 0,
         .range = VK_WHOLE_SIZE},
    };
    VkWriteDescriptorSet writes[9];
    for (int i = 0; i < 9; i++) {
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = (uint32_t)i,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &dbi[i],
        };
    }
    /* Skip slot 7 (accum) for now — written separately per scale. */
    vkUpdateDescriptorSets(s->ctx->device, 9, writes, 0, NULL);
}

static void write_accum_binding(AdmVulkanState *s, VkDescriptorSet set, int scale)
{
    VkDescriptorBufferInfo dbi = {
        .buffer = (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->accum[scale]),
        .offset = 0,
        .range = VK_WHOLE_SIZE,
    };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = 7,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &dbi,
    };
    vkUpdateDescriptorSets(s->ctx->device, 1, &w, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Score reduction (the CPU's own conclude code where it exists as a   */
/* shared inline; see the file header for the deviation-6 list).       */
/* ------------------------------------------------------------------ */

/* CPU adm_csf_den_scale / adm_csf_den_s123 conclude: the ctx carries
 * the border, the CSF factors and the per-row shift; `accum` holds the
 * per-row-shifted totals the shader produced (one shift per WG, one WG
 * per band and row), summed host-side. Shared code — identical to the
 * CPU by construction. */
static float conclude_csf_den_host(int scale, int hw, int hh, const uint64_t accum[3],
                                   const AdmVulkanState *s)
{
    if (scale == 0) {
        AdmDenCtx c;
        adm_csf_den_ctx_init(&c, hw, hh, s->adm_norm_view_dist, s->adm_ref_display_height,
                             VULKAN_ADM_CSF_MODE, s->adm_csf_scale, s->adm_csf_diag_scale);
        return adm_csf_den_result(&c, accum, s->adm_noise_weight);
    }
    I4AdmDenCtx c;
    i4_adm_csf_den_ctx_init(&c, scale, hw, hh, s->adm_norm_view_dist, s->adm_ref_display_height,
                            VULKAN_ADM_CSF_MODE, s->adm_csf_scale, s->adm_csf_diag_scale);
    return i4_adm_csf_den_result(&c, accum, s->adm_noise_weight);
}

/* CPU adm_cm (scale 0) / i4_adm_cm (scales 1..3) conclude. The
 * accumulator shifts mirror adm_cm_ctx_init / i4_adm_cm_ctx_init; the
 * conversion and the per-band sum go through the CPU's own inlines
 * (adm_cm_restore_accum / adm_num_scale). */
static float conclude_cm_host(int scale, int hw, int hh, const int64_t accum_in[3],
                              const AdmVulkanState *s)
{
    const double nw = s->adm_noise_weight;
    const float p_norm_exp = 1.0f / (float)VULKAN_ADM_P_NORM;
    const uint32_t shift_inner = (uint32_t)ceil(log2((double)hh));
    const uint32_t nsh = s->normalization_shift[scale];
    uint64_t accum[3];
    for (int b = 0; b < ADM_NUM_BANDS; b++)
        accum[b] = (uint64_t)accum_in[b];

    if (scale == 0) {
        /* integer_adm_kernels.h:1006-1010: the h/v cube shift is
         * ceil(log2(band)) - 4, the diagonal one ceil(log2(band)) - 3;
         * base exponents 52 / 52 / 57 minus 3 * normalization_shift
         * (adm_cm_restore_accum). */
        const uint32_t shift_xhcub = (uint32_t)ceil(log2((double)hw) - 4.0);
        const uint32_t shift_xdcub = (uint32_t)ceil(log2((double)hw) - 3.0);
        const float f_accum_h =
            adm_cm_restore_accum(accum[0], 52, nsh, shift_xhcub, shift_inner);
        const float f_accum_v =
            adm_cm_restore_accum(accum[1], 52, nsh, shift_xhcub, shift_inner);
        const float f_accum_d =
            adm_cm_restore_accum(accum[2], 57, nsh, shift_xdcub, shift_inner);
        const AdmBorder b = adm_border(hw, hh);
        const int active_area = (b.bottom - b.top) * (b.right - b.left);
        /* CPU: (num_scale_h + num_scale_v + num_scale_d) — left-associated
         * ((h + v) + d); float addition is not associative, so the
         * grouping must match exactly (adm_cm_result, :1084). */
        const float num_scale_h = adm_num_scale(f_accum_h, active_area, nw, p_norm_exp);
        const float num_scale_v = adm_num_scale(f_accum_v, active_area, nw, p_norm_exp);
        const float num_scale_d = adm_num_scale(f_accum_d, active_area, nw, p_norm_exp);
        return (num_scale_h + num_scale_v + num_scale_d);
    }

    /* integer_adm_kernels.h:1228-1235: one final shift per scale slot
     * (45 / 39 / 36 minus 3 * normalization_shift minus the width /
     * height shifts), shared by all three bands; the conversion is
     * int64 -> float followed by a FLOAT division. */
    static const uint32_t cm_final_shift[3] = {45u, 39u, 36u};
    const uint32_t shift_cub = (uint32_t)ceil(log2((double)hw));
    const unsigned slot = i4_scale_slot(scale);
    const float fs = (float)pow(2.0, (double)(cm_final_shift[slot] - 3u * nsh - shift_cub -
                                             shift_inner));
    const float f_accum_h = (float)(accum[0] / fs);
    const float f_accum_v = (float)(accum[1] / fs);
    const float f_accum_d = (float)(accum[2] / fs);
    const AdmBorder b = adm_border(hw, hh);
    const int active_area = (b.bottom - b.top) * (b.right - b.left);
    /* CPU: left-associated ((h + v) + d) — see the scale-0 branch note
     * (i4_adm_cm_result, :1243). */
    const float num_scale_h = adm_num_scale(f_accum_h, active_area, nw, p_norm_exp);
    const float num_scale_v = adm_num_scale(f_accum_v, active_area, nw, p_norm_exp);
    const float num_scale_d = adm_num_scale(f_accum_d, active_area, nw, p_norm_exp);
    return (num_scale_h + num_scale_v + num_scale_d);
}

/* ------------------------------------------------------------------ */
/* extract().                                                          */
/* ------------------------------------------------------------------ */

static void compute_csf_cm_shifts(unsigned hw, unsigned hh, int scale, int active_w, int active_h,
                                  AdmPushConsts *pc)
{
    if (scale == 0) {
        pc->csf_shift_sq = 0;
        pc->csf_shift_cub = 0;
        int area = active_w * active_h;
        int sa = (int)ceil(log2((double)area) - 20.0);
        pc->csf_shift_accum = sa > 0 ? (uint32_t)sa : 0u;
    } else {
        pc->csf_shift_sq = (scale == 2) ? 30u : 31u;
        pc->csf_shift_cub = (uint32_t)ceil(log2((double)active_w));
        pc->csf_shift_accum = (uint32_t)ceil(log2((double)active_h));
    }
    pc->cm_shift_inner = (uint32_t)ceil(log2((double)hh));
    if (scale == 0) {
        pc->cm_shift_sub_h = 10u;
        pc->cm_shift_sub_d = 12u;
        pc->cm_shift_xsq_h = 29u;
        pc->cm_shift_xsq_d = 30u;
        pc->cm_shift_xcub_h = (uint32_t)(ceil(log2((double)hw)) - 4.0);
        pc->cm_shift_xcub_d = (uint32_t)(ceil(log2((double)hw)) - 3.0);
    } else {
        pc->cm_shift_sub_h = 15u;
        pc->cm_shift_sub_d = 15u;
        pc->cm_shift_xsq_h = 30u;
        pc->cm_shift_xsq_d = 30u;
        pc->cm_shift_xcub_h = (uint32_t)ceil(log2((double)hw));
        pc->cm_shift_xcub_d = (uint32_t)ceil(log2((double)hw));
    }
}

static void issue_pipeline_barrier(VkCommandBuffer cmd)
{
    /* Inter-stage barrier: the producing stage writes output SSBOs;
     * the consuming stage only reads them.  dstAccessMask is read-only —
     * no atomicAdd in the inter-stage consumer path (VK-5 / perf-audit
     * 2026-05-16). */
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static int reduce_and_emit(AdmVulkanState *s, unsigned index, VmafFeatureCollector *fc)
{
    /* Host-side reduction of the per-WG int64 slots. The 6 slots per WG
     * layout is csf_h,csf_v,csf_d, cm_h,cm_v,cm_d. Two's-complement
     * addition is associative, so the serial host sum is
     * order-independent — this replaces the ADR-0350 two-level GPU
     * reduction kernel, which is not restored (its
     * subgroupAdd(int64_t) is banned by ADR-3402). */
    int64_t cm_totals[ADM_NUM_SCALES][ADM_NUM_BANDS] = {{0}};
    int64_t csf_totals[ADM_NUM_SCALES][ADM_NUM_BANDS] = {{0}};

    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        int err = vmaf_vulkan_buffer_invalidate(s->ctx, s->accum[scale]);
        if (err)
            return err;
        const int64_t *acc = vmaf_vulkan_buffer_host(s->accum[scale]);
        for (int b = 0; b < ADM_NUM_BANDS; b++) {
            int64_t c = 0, m = 0;
            for (unsigned g = 0; g < s->wg_count[scale]; g++) {
                c += acc[g * ADM_ACCUM_SLOTS_PER_WG + b];
                m += acc[g * ADM_ACCUM_SLOTS_PER_WG + ADM_NUM_BANDS + b];
            }
            csf_totals[scale][b] = c;
            cm_totals[scale][b] = m;
        }
    }

    /* Score per scale: the CPU accumulates the float per-scale values
     * into double sums in scale order (integer_adm.c:931-934), and keeps
     * the float per-scale pair for the per-scale emissions. */
    double num = 0.0, den = 0.0;
    float scores_num[ADM_NUM_SCALES];
    float scores_den[ADM_NUM_SCALES];

    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        uint64_t csf_u[ADM_NUM_BANDS];
        for (int b = 0; b < ADM_NUM_BANDS; b++)
            csf_u[b] = (uint64_t)csf_totals[scale][b];
        scores_den[scale] = conclude_csf_den_host(scale, (int)s->scale_half_w[scale],
                                                  (int)s->scale_half_h[scale], csf_u, s);
        scores_num[scale] = conclude_cm_host(scale, (int)s->scale_half_w[scale],
                                             (int)s->scale_half_h[scale], cm_totals[scale], s);
        num += scores_num[scale];
        den += scores_den[scale];
    }

    /* CPU integer_compute_adm (integer_adm.c:909): the precision floor
     * scales with the BASE frame area — the snapshot host used the last
     * scale's half dims here (deviation 6). The floor + ratio flow is
     * the CPU's own shared code (nonfinite_score.h / adm_score.h). */
    const double numden_limit = 1e-10 * (s->width * s->height) / (1920.0 * 1080.0);
    double pairs[2] = {num, den};
    double ratios[2];
    int err = vmaf_adm_floor_pair_named("integer_adm_vulkan", index, num, den, numden_limit,
                                        &pairs[0], &pairs[1]);
    if (err)
        return err;
    err = vmaf_adm_scale_ratios(pairs, 2u, ratios);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "integer_adm_vulkan: undefined or non-finite aggregate at frame %u "
                 "(num=%g den=%g)\n",
                 index, pairs[0], pairs[1]);
        return err;
    }
    const double score = ratios[0];

    err = vmaf_feature_collector_append_with_dict(
        fc, s->feature_name_dict, "VMAF_integer_feature_adm2_score", score, index);

    for (int i = 0; i < ADM_NUM_SCALES && !err; i++) {
        char name[64];
        snprintf(name, sizeof(name), "integer_adm_scale%d", i);
        double scale_scores[2] = {scores_num[i], scores_den[i]};
        double scale_ratio[1];
        err = vmaf_adm_scale_ratios(scale_scores, 1u, scale_ratio);
        if (err)
            break;
        err = vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, name,
                                                      scale_ratio[0], index);
    }

    if (s->debug && !err) {
        vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "integer_adm", score,
                                                index);
        vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "integer_adm_num",
                                                pairs[0], index);
        vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, "integer_adm_den",
                                                pairs[1], index);
        for (int i = 0; i < ADM_NUM_SCALES; i++) {
            char name[64];
            snprintf(name, sizeof(name), "integer_adm_num_scale%d", i);
            vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, name, scores_num[i],
                                                    index);
            snprintf(name, sizeof(name), "integer_adm_den_scale%d", i);
            vmaf_feature_collector_append_with_dict(fc, s->feature_name_dict, name, scores_den[i],
                                                    index);
        }
    }
    return err;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    AdmVulkanState *s = fex->priv;
    int err = 0;

    err = upload_plane(s, ref_pic, s->src_ref);
    if (err)
        return err;
    err = upload_plane(s, dist_pic, s->src_dis);
    if (err)
        return err;

    /* All 9 descriptor bindings are init-time-stable (the accum buffers
     * are allocated once in init() and never reallocated). No per-frame
     * vkUpdateDescriptorSets needed. (T-GPU-OPT-VK-4 / ADR-0256.) */

    /* Acquire a pre-allocated command buffer + fence from the submit pool.
     * Eliminates per-frame vkAllocateCommandBuffers / vkCreateFence.
     * (T-GPU-OPT-VK-1 / ADR-0256.) */
    VmafVulkanKernelSubmit submit = {0};
    err = vmaf_vulkan_kernel_submit_acquire(s->ctx, &s->sub_pool, /*pool_slot=*/0, &submit);
    if (err)
        return err;
    VkCommandBuffer cmd = submit.cmd;

    /* Zero accumulator buffers on the GPU before the first compute
     * dispatch. The stage-3 kernel writes every slot it owns, so this is
     * belt-and-braces, but it keeps an unfinished dispatch from ever
     * reading stale memory as a "result" (the ADR-0176 errata's
     * bogus-baseline lesson). A TRANSFER→COMPUTE barrier follows to
     * guarantee the zeros are visible to the shader. */
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        vkCmdFillBuffer(cmd, (VkBuffer)vmaf_vulkan_buffer_vkhandle(s->accum[scale]), 0,
                        VK_WHOLE_SIZE, 0u);
    }
    {
        VkMemoryBarrier fill_barrier = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fill_barrier, 0, NULL, 0,
                             NULL);
    }

    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        unsigned cw = s->scale_w[scale];
        unsigned ch = s->scale_h[scale];
        unsigned hw = s->scale_half_w[scale];
        unsigned hh = s->scale_half_h[scale];

        /* DWT shift parameters per scale (matches SYCL dwt_shifts). */
        unsigned v_shift, v_add, h_shift;
        if (scale == 0) {
            v_shift = s->bpc;
            v_add = 1u << (s->bpc - 1u);
            h_shift = 16u;
        } else if (scale == 1) {
            v_shift = 0u;
            v_add = 0u;
            h_shift = 15u;
        } else if (scale == 2) {
            v_shift = 16u;
            v_add = 32768u;
            h_shift = 16u;
        } else {
            v_shift = 16u;
            v_add = 32768u;
            h_shift = 15u;
        }

        int left = (int)((double)hw * ADM_BORDER_FACTOR - 0.5);
        int top = (int)((double)hh * ADM_BORDER_FACTOR - 0.5);
        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        int right = (int)hw - left;
        int bottom = (int)hh - top;
        int active_w = right - left;
        int active_h = bottom - top;

        unsigned in_stride;
        if (scale == 0)
            in_stride = (s->bpc <= 8) ? cw : cw * 2u;
        else
            in_stride = s->buf_stride;

        AdmPushConsts pc = {
            .cur_w = cw,
            .cur_h = ch,
            .half_w = hw,
            .half_h = hh,
            .buf_stride = s->buf_stride,
            .in_stride = in_stride,
            .v_shift = v_shift,
            .v_add = v_add,
            .h_shift = h_shift,
            .i_rfactor_h = s->i_rfactor[scale * 3 + 0],
            .i_rfactor_v = s->i_rfactor[scale * 3 + 1],
            .i_rfactor_d = s->i_rfactor[scale * 3 + 2],
            .num_workgroups_x = 1,
            .active_left = left,
            .active_top = top,
            .active_right = right,
            .active_bottom = bottom,
        };
        GainQ31 gq = gain_to_q31(s->adm_enhn_gain_limit);
        pc.gain_hi = gq.gain_hi;
        pc.gain_lo = gq.gain_lo;
        compute_csf_cm_shifts(hw, hh, scale, active_w, active_h, &pc);

        /* Stage 0: DWT vertical (z=2 fused ref+dis). */
        {
            uint32_t gx = (cw + ADM_WG_X - 1u) / ADM_WG_X;
            uint32_t gy = (hh + ADM_WG_Y - 1u) / ADM_WG_Y;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipelines[0][scale]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pl.pipeline_layout, 0,
                                    1, &s->pre_sets[scale], 0, NULL);
            vkCmdPushConstants(cmd, s->pl.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(pc), &pc);
            vkCmdDispatch(cmd, gx, gy, 2);
            issue_pipeline_barrier(cmd);
        }

        /* Stage 1: DWT horizontal (z=2 fused ref+dis). */
        {
            uint32_t gx = (hw + ADM_WG_X - 1u) / ADM_WG_X;
            uint32_t gy = (hh + ADM_WG_Y - 1u) / ADM_WG_Y;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipelines[1][scale]);
            vkCmdDispatch(cmd, gx, gy, 2);
            issue_pipeline_barrier(cmd);
        }

        /* Stage 2: Decouple + CSF (writes csf_f). */
        {
            uint32_t gx = (hw + ADM_WG_X - 1u) / ADM_WG_X;
            uint32_t gy = (hh + ADM_WG_Y - 1u) / ADM_WG_Y;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipelines[2][scale]);
            vkCmdDispatch(cmd, gx, gy, 1);
            issue_pipeline_barrier(cmd);
        }

        /* Stage 3: CSF denominator + CM fused. 1D dispatch over
         *   3 bands × num_active_rows. */
        {
            unsigned num_rows = (unsigned)(active_h > 0 ? active_h : 1);
            uint32_t gx = 3u * num_rows;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->pipelines[3][scale]);
            vkCmdDispatch(cmd, gx, 1u, 1u);
            issue_pipeline_barrier(cmd);
        }
    }

    err = vmaf_vulkan_kernel_submit_end_and_wait(s->ctx, &submit);
    if (err)
        goto cleanup;

    err = reduce_and_emit(s, index, feature_collector);

cleanup:
    vmaf_vulkan_kernel_submit_free(s->ctx, &submit);
    return err;
}

/* ------------------------------------------------------------------ */
/* close().                                                            */
/* ------------------------------------------------------------------ */

static int close_fex(VmafFeatureExtractor *fex)
{
    AdmVulkanState *s = fex->priv;
    if (!s->ctx)
        return 0;
    VkDevice dev = s->ctx->device;
    vkDeviceWaitIdle(dev);

    /* Destroy the 15 sibling variants first (every (stage, scale)
     * except the (0, 0) base); the base + shared layout / shader /
     * DSL / pool are owned by the template. (0, 0) is skipped to
     * avoid double-freeing — pipelines[0][0] aliases s->pl.pipeline. */
    for (int stage = 0; stage < ADM_NUM_STAGES; stage++) {
        for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
            if (stage == 0 && scale == 0)
                continue;
            if (s->pipelines[stage][scale] != VK_NULL_HANDLE)
                vkDestroyPipeline(dev, s->pipelines[stage][scale], NULL);
        }
    }
    vmaf_vulkan_kernel_submit_pool_destroy(s->ctx, &s->sub_pool);
    vmaf_vulkan_kernel_pipeline_destroy(s->ctx, &s->pl);

    if (s->src_ref)
        vmaf_vulkan_buffer_free(s->ctx, s->src_ref);
    if (s->src_dis)
        vmaf_vulkan_buffer_free(s->ctx, s->src_dis);
    if (s->dwt_tmp_ref)
        vmaf_vulkan_buffer_free(s->ctx, s->dwt_tmp_ref);
    if (s->dwt_tmp_dis)
        vmaf_vulkan_buffer_free(s->ctx, s->dwt_tmp_dis);
    if (s->ref_band)
        vmaf_vulkan_buffer_free(s->ctx, s->ref_band);
    if (s->dis_band)
        vmaf_vulkan_buffer_free(s->ctx, s->dis_band);
    if (s->csf_f)
        vmaf_vulkan_buffer_free(s->ctx, s->csf_f);
    if (s->div_lookup)
        vmaf_vulkan_buffer_free(s->ctx, s->div_lookup);
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        if (s->accum[scale])
            vmaf_vulkan_buffer_free(s->ctx, s->accum[scale]);
    }

    vmaf_vulkan_context_destroy(s->ctx);
    s->ctx = NULL;
    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Provided features + registration.                                   */
/* Matches the CPU / SYCL ADM extractor's score surface (no
 * aim_score / adm3_score). Registration is a guarded block in
 * feature_extractor.cpp — no public header, no
 * VMAF_FEATURE_EXTRACTOR_VULKAN flag: the reserved enum slots stay
 * untouched pending the ABI-bump ADR (ADR-0726 / ADR-3402).          */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {"VMAF_integer_feature_adm2_score",
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

VmafFeatureExtractor vmaf_fex_integer_adm_vulkan = {
    .name = "integer_adm_vulkan",
    .init = init,
    .extract = extract,
    .close = close_fex,
    .options = options,
    .priv_size = sizeof(AdmVulkanState),
    .flags = 0,
    .provided_features = provided_features,
};
