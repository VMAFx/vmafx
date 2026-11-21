/* Copyright 2026 luoye-cpu
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "integer_adm.h"
#include "nonfinite_score.h"
#include "vulkan/common.h"

/* The embedded SPIR-V of the integer ADM shader, generated from
 * shaders/integer_adm.comp by vulkan/meson.build (spv_embed.py) and
 * linked into the feature library beside this TU. */
extern const uint32_t integer_adm_spv[];
extern const size_t integer_adm_spv_size;

/*
 * Vulkan integer ADM — the first feature of the in-tree Vulkan integer
 * compute path (draft, #3319). Bit-exact against the CPU integer ADM by
 * construction: the shader is the PoC-validated integer_adm.comp with the
 * fp64-free angle predicate (adm_angle_flag_i64) and the ADR-1402 int32
 * threshold tap; the host-side conclude math below is a line-by-line port
 * of the CPU float expressions (integer_adm.c + integer_adm_kernels.h),
 * exactly as the PoC gate proved on 576x324 and 640x480 against the
 * unmodified CPU extractor on RTX 5080 / RTX 4060 Laptop / Intel
 * RaptorLake-S iGPU (the last one impossible for the fp64 form: iGPUs
 * expose shaderFloat64=0).
 *
 * Draft scope (each item is a declared Known follow-up, not a secret):
 *   - synchronous extract(): one dispatch+wait per frame; no async
 *     submit/collect pipelining yet;
 *   - default-option configuration only: the option table mirrors the CPU
 *     twin entry-for-entry, and init() refuses non-default values with
 *     -ENOSYS instead of silently producing different numbers;
 *   - 8-bit 4:2:0 input only;
 *   - input staging is host-visible memory the shader reads directly (no
 *     device-local copy path yet).
 */

/* ------------------------------------------------------------------ */
/* CPU contract constants (core/src/feature/integer_adm.h).            */
/* ------------------------------------------------------------------ */

#define ADM_BORDER_FACTOR 0.1
#define ONE_BY_15 8738 /* integer_adm.h:210 */
#define ADM_NUM_SCALES 4
#define ADM_NUM_BANDS 3
#define ADM_DIV_LOOKUP_SIZE 65537

static const float dwt_basis_amp[6][4] = {
    {0.62171f, 0.67234f, 0.72709f, 0.67234f}, {0.34537f, 0.41317f, 0.49428f, 0.41317f},
    {0.18004f, 0.22727f, 0.28688f, 0.22727f}, {0.091401f, 0.11792f, 0.15214f, 0.11792f},
    {0.045943f, 0.059758f, 0.077727f, 0.059758f}, {0.023013f, 0.030018f, 0.039156f, 0.030018f},
};
static const struct DwtModelParams {
    float a, k, f0;
    float g[4];
} dwt_model_Y = {0.495f, 0.466f, 0.401f, {1.501f, 1.0f, 0.534f, 1.0f}};

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* CPU dwt_quant_step (integer_adm_kernels.h:67-86): the log10/pow chain
 * runs in double; only the named locals are float. */
static float dwt_quant_step_cpu(int lambda, int theta, double nvd, int rdh)
{
    float r = (float)(nvd * (double)rdh * M_PI / 180.0);
    float temp = (float)log10(pow(2.0, (double)(lambda + 1)) * (double)dwt_model_Y.f0 *
                              (double)dwt_model_Y.g[theta] / (double)r);
    float kt = dwt_model_Y.k * temp * temp; /* float chain (k*temp)*temp */
    double q = 2.0 * (double)dwt_model_Y.a * pow(10.0, (double)kt) /
               (double)dwt_basis_amp[lambda][theta];
    return (float)q;
}

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
} AdmPushConsts; /* 28 x 4 = 112 bytes */

typedef struct {
    int32_t gain_hi;
    int32_t gain_lo;
} GainQ31;

/* snapshot integer_adm_vulkan.c:315-320 (gain limit is 100.0 by default) */
static GainQ31 gain_to_q31(double gain_limit)
{
    int64_t q = (int64_t)llround(gain_limit * (double)(1LL << 31));
    GainQ31 g = {(int32_t)(q >> 16), (int32_t)(q & 0xFFFF)};
    return g;
}

/* snapshot integer_adm_vulkan.c:841-871; verified against CPU
 * adm_cm/i4_adm_cm/adm_csf_den_* shift derivations. */
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

/* ------------------------------------------------------------------ */
/* CPU-exact conclude functions (PoC host, verified bit-exact).        */
/* ------------------------------------------------------------------ */

static void conclude_csf_den_host(const uint64_t accum[3], int hh, int hw, int scale, float *result)
{
    const double nvd = DEFAULT_ADM_NORM_VIEW_DIST;
    const int rdh = DEFAULT_ADM_REF_DISPLAY_HEIGHT;
    const double nw = DEFAULT_ADM_NOISE_WEIGHT;

    int left = (int)((double)hw * ADM_BORDER_FACTOR - 0.5);
    int top = (int)((double)hh * ADM_BORDER_FACTOR - 0.5);
    int right = hw - left;
    int bottom = hh - top;

    float factor1 = 1.0f / dwt_quant_step_cpu(scale, 1, nvd, rdh);
    float factor2 = 1.0f / dwt_quant_step_cpu(scale, 2, nvd, rdh);
    const float rfactor[3] = {factor1, factor1, factor2};

    float powf_add =
        powf((float)((double)((bottom - top) * (right - left)) * nw), 1.0f / 3.0f);

    float den_scale = 0.0f;
    for (int b = 0; b < 3; b++) {
        double csf;
        if (scale == 0) {
            int area = (bottom - top) * (right - left);
            int32_t shift_accum = (int32_t)ceil(log2((double)area) - 20.0);
            if (shift_accum < 0)
                shift_accum = 0;
            (void)shift_accum; /* per-row shift already applied in the shader */
            double shift_csf = pow(2.0, 18.0 - (double)shift_accum);
            csf = (double)(accum[b] / shift_csf) * pow((double)rfactor[b], 3.0);
        } else {
            static const uint32_t accum_convert[3] = {32, 27, 23};
            uint32_t shift_cub = (uint32_t)ceil(log2((double)(right - left)));
            uint32_t shift_accum = (uint32_t)ceil(log2((double)(bottom - top)));
            double shift_csf =
                pow(2.0, (double)(accum_convert[scale - 1] - shift_accum - shift_cub));
            csf = (double)(accum[b] / shift_csf) * pow((double)rfactor[b], 3.0);
        }
        den_scale += powf((float)csf, 1.0f / 3.0f) + powf_add;
    }
    *result = den_scale;
}

static void conclude_cm_host(const int64_t accum[3], int hh, int hw, int scale, float *result)
{
    const double nw = DEFAULT_ADM_NOISE_WEIGHT;

    int left = (int)((double)hw * ADM_BORDER_FACTOR - 0.5);
    int top = (int)((double)hh * ADM_BORDER_FACTOR - 0.5);
    int right = hw - left;
    int bottom = hh - top;
    float powf_add =
        powf((float)((double)((bottom - top) * (right - left)) * nw), 1.0f / 3.0f);

    float num_scale = 0.0f;
    if (scale == 0) {
        uint32_t shift_inner = (uint32_t)ceil(log2((double)hh));
        uint32_t xhcub = (uint32_t)(ceil(log2((double)hw)) - 4.0);
        uint32_t xdcub = (uint32_t)(ceil(log2((double)hw)) - 3.0);
        float f_h = (float)((double)accum[0] /
                            pow(2.0, (double)(52u - xhcub - shift_inner)));
        float f_v = (float)((double)accum[1] /
                            pow(2.0, (double)(52u - xhcub - shift_inner)));
        float f_d = (float)((double)accum[2] /
                            pow(2.0, (double)(57u - xdcub - shift_inner)));
        num_scale = ((powf(f_h, 1.0f / 3.0f) + powf_add) +
                     (powf(f_v, 1.0f / 3.0f) + powf_add)) +
                    (powf(f_d, 1.0f / 3.0f) + powf_add);
    } else {
        static const uint32_t cm_final_shift[3] = {45u, 39u, 36u};
        uint32_t shift_cub = (uint32_t)ceil(log2((double)hw));
        uint32_t shift_inner = (uint32_t)ceil(log2((double)hh));
        float fs = (float)pow(2.0, (double)(cm_final_shift[scale - 1] - shift_cub - shift_inner));
        float f_h = (float)(accum[0] / fs);
        float f_v = (float)(accum[1] / fs);
        float f_d = (float)(accum[2] / fs);
        num_scale = ((powf(f_h, 1.0f / 3.0f) + powf_add) +
                     (powf(f_v, 1.0f / 3.0f) + powf_add)) +
                    (powf(f_d, 1.0f / 3.0f) + powf_add);
    }
    *result = num_scale;
}

/* ------------------------------------------------------------------ */
/* Extractor state + options                                           */
/* ------------------------------------------------------------------ */

typedef struct AdmVulkanState {
    VmafVulkanState *vk;
    bool vk_owned; /* created locally when no context state was injected */

    unsigned w, h, bpc;
    unsigned scale_w[ADM_NUM_SCALES], scale_h[ADM_NUM_SCALES];
    unsigned scale_hw[ADM_NUM_SCALES], scale_hh[ADM_NUM_SCALES];
    unsigned buf_stride, half_w0, half_h0;
    uint32_t i_rfactor[ADM_NUM_SCALES][3];
    GainQ31 gq;

    /* options (mirror of the CPU twin; defaults enforced at init) */
    bool debug;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_dlm_weight;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    double adm_norm_view_dist_extra;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_noise_weight;
    bool adm_skip_aim;
    bool adm_skip_scale0;
    double adm_min_val;
    double adm_p_norm;

    /* Vulkan objects */
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet sets[ADM_NUM_SCALES];
    VkPipelineLayout pipe_layout;
    VkPipeline pipelines[4][ADM_NUM_SCALES];
    VkCommandPool cmd_pool;
    VkCommandBuffer cmd;
    VkFence fence;

    VmafxVulkanBuffer src_ref, src_dis, dwt_tmp_ref, dwt_tmp_dis;
    VmafxVulkanBuffer ref_band, dis_band, csf_f, div_lookup;
    VmafxVulkanBuffer accum[ADM_NUM_SCALES];
    uint32_t wg_count[ADM_NUM_SCALES];

    VmafDictionary *feature_name_dict;
} AdmVulkanState;

static const VmafOption options[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(AdmVulkanState, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "adm_csf_scale",
        .alias = "scf",
        .help = "scale coefficient for the horizontal & vertical direction terms of CSF",
        .offset = offsetof(AdmVulkanState, adm_csf_scale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_CSF_SCALE,
        .min = 0.0,
        .max = 50.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_csf_diag_scale",
        .alias = "scfd",
        .help = "scale coefficient for the diagonal direction term of CSF",
        .offset = offsetof(AdmVulkanState, adm_csf_diag_scale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_CSF_DIAG_SCALE,
        .min = 0.0,
        .max = 50.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_dlm_weight",
        .alias = "dlmw",
        .help = "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
        .offset = offsetof(AdmVulkanState, adm_dlm_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.5,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_enhn_gain_limit",
        .alias = "egl",
        .help = "enhancement gain imposed on adm, must be >= 1.0, "
                "where 1.0 means the gain is completely disabled",
        .offset = offsetof(AdmVulkanState, adm_enhn_gain_limit),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_ENHN_GAIN_LIMIT,
        .min = 1.0,
        .max = 1000.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_norm_view_dist",
        .alias = "nvd",
        .help = "normalized viewing distance in inches, overrides autodetection, "
                "value <= 0 means autodetect",
        .offset = offsetof(AdmVulkanState, adm_norm_view_dist),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_NORM_VIEW_DIST,
        .min = 0.0,
        .max = 100.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_norm_view_dist_extra",
        .help = "extra normalized viewing distance in inches, which will be used to "
                "enable a second viewing distance",
        .offset = offsetof(AdmVulkanState, adm_norm_view_dist_extra),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 0.0,
        .min = 0.0,
        .max = 100.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_ref_display_height",
        .help = "reference display height in pixels",
        .offset = offsetof(AdmVulkanState, adm_ref_display_height),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = DEFAULT_ADM_REF_DISPLAY_HEIGHT,
        .min = 0,
        .max = 4320,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_csf_mode",
        .alias = "csf",
        .help = "contrast sensitivity function",
        .offset = offsetof(AdmVulkanState, adm_csf_mode),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = DEFAULT_ADM_CSF_MODE,
        .min = 0,
        .max = 3,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_noise_weight",
        .alias = "nw",
        .help = "noise weight",
        .offset = offsetof(AdmVulkanState, adm_noise_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_NOISE_WEIGHT,
        .min = 0.0,
        .max = 1500.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_skip_aim",
        .help = "skip the calculation of AIM",
        .offset = offsetof(AdmVulkanState, adm_skip_aim),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "adm_skip_scale0",
        .alias = "ssz",
        .help = "skip the calculation of scale 0",
        .offset = offsetof(AdmVulkanState, adm_skip_scale0),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_min_val",
        .alias = "min",
        .help = "minimum value allowed; lower values will be clipped to this value",
        .offset = offsetof(AdmVulkanState, adm_min_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_MIN_VAL,
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_p_norm",
        .alias = "apn",
        .help = "p-norm exponent for fixed-point ADM contrast-measure finalisation",
        .offset = offsetof(AdmVulkanState, adm_p_norm),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 3.0,
        .min = 1.0,
        .max = 20.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0}
};

/* The draft honours the configuration the bit-exactness gate validated:
 * every FEATURE_PARAM option must be at its default (the conclude math and
 * the push constants embed exactly those values). `debug` is accepted as a
 * no-op. Anything else is refused with -ENOSYS instead of silently
 * producing different numbers. */
static int check_default_configuration(const AdmVulkanState *s)
{
    if (s->adm_csf_scale != DEFAULT_ADM_CSF_SCALE ||
        s->adm_csf_diag_scale != DEFAULT_ADM_CSF_DIAG_SCALE ||
        s->adm_dlm_weight != 0.5 ||
        s->adm_enhn_gain_limit != DEFAULT_ADM_ENHN_GAIN_LIMIT ||
        s->adm_norm_view_dist != DEFAULT_ADM_NORM_VIEW_DIST ||
        s->adm_norm_view_dist_extra != 0.0 ||
        s->adm_ref_display_height != DEFAULT_ADM_REF_DISPLAY_HEIGHT ||
        s->adm_csf_mode != DEFAULT_ADM_CSF_MODE ||
        s->adm_noise_weight != DEFAULT_ADM_NOISE_WEIGHT ||
        s->adm_skip_aim || s->adm_skip_scale0 ||
        s->adm_min_val != DEFAULT_ADM_MIN_VAL ||
        s->adm_p_norm != 3.0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "integer_adm_vulkan: draft honours the default configuration "
                 "only (non-default ADM option; see #3319 Known follow-ups)\n");
        return -ENOSYS;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* init / close                                                        */
/* ------------------------------------------------------------------ */

static int create_pipelines(AdmVulkanState *s)
{
    VkShaderModuleCreateInfo smci;
    memset(&smci, 0, sizeof(smci));
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = integer_adm_spv_size;
    smci.pCode = integer_adm_spv;
    VkShaderModule module;
    VkResult r = vmafx_vk.CreateShaderModule(s->vk->device, &smci, NULL, &module);
    if (r != VK_SUCCESS)
        return -ENOMEM;

    for (int st = 0; st < 4; st++) {
        for (int sc = 0; sc < ADM_NUM_SCALES; sc++) {
            struct {
                int32_t width, height, bpc, scale, stage;
            } spec = {(int32_t)s->w, (int32_t)s->h, 8, sc, st};
            VkSpecializationMapEntry map[5];
            for (int e = 0; e < 5; e++) {
                map[e].constantID = (uint32_t)e;
                map[e].offset = (uint32_t)(e * 4);
                map[e].size = 4;
            }
            VkSpecializationInfo si;
            memset(&si, 0, sizeof(si));
            si.mapEntryCount = 5;
            si.pMapEntries = map;
            si.dataSize = sizeof(spec);
            si.pData = &spec;
            VkPipelineShaderStageCreateInfo stage;
            memset(&stage, 0, sizeof(stage));
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = module;
            stage.pName = "main";
            stage.pSpecializationInfo = &si;
            VkComputePipelineCreateInfo cpci;
            memset(&cpci, 0, sizeof(cpci));
            cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpci.stage = stage;
            cpci.layout = s->pipe_layout;
            r = vmafx_vk.CreateComputePipelines(s->vk->device, NULL, 1, &cpci, NULL,
                                                &s->pipelines[st][sc]);
            if (r != VK_SUCCESS) {
                for (int st2 = 0; st2 < 4; st2++)
                    for (int sc2 = 0; sc2 < ADM_NUM_SCALES; sc2++)
                        if (s->pipelines[st2][sc2])
                            vmafx_vk.DestroyPipeline(s->vk->device,
                                                     s->pipelines[st2][sc2], NULL);
                vmafx_vk.DestroyShaderModule(s->vk->device, module, NULL);
                return -ENOMEM;
            }
        }
    }
    vmafx_vk.DestroyShaderModule(s->vk->device, module, NULL);
    return 0;
}

static int init( VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt,
                 unsigned bpc, unsigned w, unsigned h)
{
    AdmVulkanState *s = fex->priv;
    s->w = w;
    s->h = h;
    s->bpc = bpc;

    if (bpc != 8 || pix_fmt != VMAF_PIX_FMT_YUV420P) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "integer_adm_vulkan: draft supports 8-bit 4:2:0 only "
                 "(got bpc=%u fmt=%d; see #3319 Known follow-ups)\n", bpc, (int)pix_fmt);
        return -ENOSYS;
    }

    int err = check_default_configuration(s);
    if (err)
        return err;

    /* Device: framework-injected state, else a local one (self-contained
     * path, used by the parity test without a context). */
    if (fex->vulkan_state) {
        s->vk = fex->vulkan_state;
        s->vk_owned = false;
    } else {
        err = vmafx_vulkan_state_create(&s->vk, -1);
        if (err)
            return err;
        s->vk_owned = true;
    }

    /* Per-scale geometry (PoC host). */
    {
        unsigned cw = w, ch = h;
        for (int sc = 0; sc < ADM_NUM_SCALES; sc++) {
            s->scale_w[sc] = cw;
            s->scale_h[sc] = ch;
            s->scale_hw[sc] = (cw + 1) / 2;
            s->scale_hh[sc] = (ch + 1) / 2;
            cw = s->scale_hw[sc];
            ch = s->scale_hh[sc];
        }
        s->half_w0 = s->scale_hw[0];
        s->half_h0 = s->scale_hh[0];
        s->buf_stride = (s->half_w0 + 3u) & ~3u;
    }

    /* Integer rfactors (CPU adm_csf/adm_cm/i4_adm_csf). */
    {
        const double nvd = DEFAULT_ADM_NORM_VIEW_DIST;
        const int rdh = DEFAULT_ADM_REF_DISPLAY_HEIGHT;
        const double csf_scale = DEFAULT_ADM_CSF_SCALE;
        const double diag_scale = DEFAULT_ADM_CSF_DIAG_SCALE;
        const int hardcode =
            (fabs(nvd * rdh - DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT) <
             1.0e-8);
        for (int sc = 0; sc < ADM_NUM_SCALES; sc++) {
            float f1 = csf_scale / dwt_quant_step_cpu(sc, 1, nvd, rdh);
            float f2 = diag_scale / dwt_quant_step_cpu(sc, 2, nvd, rdh);
            if (sc == 0 && hardcode) {
                /* CPU tabulates {36453,36453,49417} for default geometry +
                 * WATSON97 (adm_csf_fixed_point.h:221-223). */
                s->i_rfactor[0][0] = 36453;
                s->i_rfactor[0][1] = 36453;
                s->i_rfactor[0][2] = 49417;
            } else if (sc == 0) {
                s->i_rfactor[0][0] = (uint32_t)((double)f1 * pow(2.0, 21.0));
                s->i_rfactor[0][1] = (uint32_t)((double)f1 * pow(2.0, 21.0));
                s->i_rfactor[0][2] = (uint32_t)((double)f2 * pow(2.0, 23.0));
            } else {
                s->i_rfactor[sc][0] = (uint32_t)((double)f1 * pow(2.0, 32.0));
                s->i_rfactor[sc][1] = (uint32_t)((double)f1 * pow(2.0, 32.0));
                s->i_rfactor[sc][2] = (uint32_t)((double)f2 * pow(2.0, 32.0));
            }
        }
    }
    s->gq = gain_to_q31(DEFAULT_ADM_ENHN_GAIN_LIMIT);

    /* Buffers. */
    const VkDeviceSize src_row_bytes =
        (VkDeviceSize)(((w + 3u) >> 2) * 4u);
    err = vmafx_vulkan_create_buffer(s->vk, src_row_bytes * h, true, &s->src_ref);
    if (!err) err = vmafx_vulkan_create_buffer(s->vk, src_row_bytes * h, true, &s->src_dis);
    if (!err) err = vmafx_vulkan_create_buffer(s->vk, (VkDeviceSize)w * 2 * s->half_h0 * 4,
                                               true, &s->dwt_tmp_ref);
    if (!err) err = vmafx_vulkan_create_buffer(s->vk, (VkDeviceSize)w * 2 * s->half_h0 * 4,
                                               true, &s->dwt_tmp_dis);
    if (!err) err = vmafx_vulkan_create_buffer(s->vk, (VkDeviceSize)4 * s->buf_stride * s->half_h0 * 4,
                                               true, &s->ref_band);
    if (!err) err = vmafx_vulkan_create_buffer(s->vk, (VkDeviceSize)4 * s->buf_stride * s->half_h0 * 4,
                                               true, &s->dis_band);
    if (!err) err = vmafx_vulkan_create_buffer(s->vk, (VkDeviceSize)3 * s->buf_stride * s->half_h0 * 4,
                                               true, &s->csf_f);
    if (!err) err = vmafx_vulkan_create_buffer(s->vk, (VkDeviceSize)ADM_DIV_LOOKUP_SIZE * 4,
                                               true, &s->div_lookup);
    for (int sc = 0; sc < ADM_NUM_SCALES && !err; sc++) {
        int top = (int)((double)s->scale_hh[sc] * ADM_BORDER_FACTOR - 0.5);
        if (top < 0)
            top = 0;
        int bottom = (int)s->scale_hh[sc] - top;
        unsigned num_rows = (unsigned)(bottom - top);
        if (num_rows == 0)
            num_rows = 1;
        s->wg_count[sc] = 3u * num_rows;
        err = vmafx_vulkan_create_buffer(s->vk, (VkDeviceSize)s->wg_count[sc] * 6 * 8,
                                         true, &s->accum[sc]);
    }
    if (err)
        goto free_buffers_fail;

    /* Divisor lookup (CPU integer_adm.h:12, Q = 2^30). */
    {
        int32_t *lut = (int32_t *)s->div_lookup.mapped;
        const int32_t Q = 1073741824;
        for (int i = 1; i <= 32768; i++) {
            int32_t recip = (int32_t)(Q / i);
            lut[32768 + i] = recip;
            lut[32768 - i] = 0 - recip;
        }
    }

    /* Descriptor sets: 9 storage buffers, per scale (accum differs). */
    {
        VkDescriptorSetLayoutBinding bindings[9];
        for (int b = 0; b < 9; b++) {
            memset(&bindings[b], 0, sizeof(bindings[b]));
            bindings[b].binding = (uint32_t)b;
            bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dlci;
        memset(&dlci, 0, sizeof(dlci));
        dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dlci.bindingCount = 9;
        dlci.pBindings = bindings;
        if (vmafx_vk.CreateDescriptorSetLayout(s->vk->device, &dlci, NULL, &s->dsl) !=
            VK_SUCCESS) {
            err = -ENOMEM;
            goto free_buffers_fail;
        }
        VkDescriptorPoolSize ps;
        memset(&ps, 0, sizeof(ps));
        ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ps.descriptorCount = 9 * ADM_NUM_SCALES;
        VkDescriptorPoolCreateInfo pci;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = ADM_NUM_SCALES;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &ps;
        if (vmafx_vk.CreateDescriptorPool(s->vk->device, &pci, NULL, &s->pool) !=
            VK_SUCCESS) {
            err = -ENOMEM;
            goto free_dsl_fail;
        }
        VkDescriptorSetLayout layouts[ADM_NUM_SCALES];
        for (int sc = 0; sc < ADM_NUM_SCALES; sc++)
            layouts[sc] = s->dsl;
        VkDescriptorSetAllocateInfo sai;
        memset(&sai, 0, sizeof(sai));
        sai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        sai.descriptorPool = s->pool;
        sai.descriptorSetCount = ADM_NUM_SCALES;
        sai.pSetLayouts = layouts;
        if (vmafx_vk.AllocateDescriptorSets(s->vk->device, &sai, s->sets) != VK_SUCCESS) {
            err = -ENOMEM;
            goto free_pool_fail;
        }
        for (int sc = 0; sc < ADM_NUM_SCALES; sc++) {
            VkDescriptorBufferInfo dbi[9] = {
                {s->src_ref.buf, 0, VK_WHOLE_SIZE},     /* 0 ref_src */
                {s->dwt_tmp_ref.buf, 0, VK_WHOLE_SIZE}, /* 1 dwt_tmp_ref */
                {s->dwt_tmp_dis.buf, 0, VK_WHOLE_SIZE}, /* 2 dwt_tmp_dis */
                {s->ref_band.buf, 0, VK_WHOLE_SIZE},    /* 3 ref_band */
                {s->dis_band.buf, 0, VK_WHOLE_SIZE},    /* 4 dis_band */
                {s->csf_f.buf, 0, VK_WHOLE_SIZE},       /* 5 csf_f */
                {s->div_lookup.buf, 0, VK_WHOLE_SIZE},  /* 6 div_lookup */
                {s->accum[sc].buf, 0, VK_WHOLE_SIZE},   /* 7 accum_out */
                {s->src_dis.buf, 0, VK_WHOLE_SIZE},     /* 8 dis_src */
            };
            VkWriteDescriptorSet writes[9];
            for (int b = 0; b < 9; b++) {
                memset(&writes[b], 0, sizeof(writes[b]));
                writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[b].dstSet = s->sets[sc];
                writes[b].dstBinding = (uint32_t)b;
                writes[b].descriptorCount = 1;
                writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[b].pBufferInfo = &dbi[b];
            }
            vmafx_vk.UpdateDescriptorSets(s->vk->device, 9, writes, 0, NULL);
        }
    }

    /* Pipeline layout + pipelines + command infrastructure. */
    {
        VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   (uint32_t)sizeof(AdmPushConsts)};
        VkPipelineLayoutCreateInfo plci;
        memset(&plci, 0, sizeof(plci));
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &s->dsl;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (vmafx_vk.CreatePipelineLayout(s->vk->device, &plci, NULL,
                                          &s->pipe_layout) != VK_SUCCESS) {
            err = -ENOMEM;
            goto free_pool_fail;
        }
        err = create_pipelines(s);
        if (err)
            goto free_pl_fail;

        VkCommandPoolCreateInfo cpoi;
        memset(&cpoi, 0, sizeof(cpoi));
        cpoi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpoi.queueFamilyIndex = s->vk->compute_queue_family;
        if (vmafx_vk.CreateCommandPool(s->vk->device, &cpoi, NULL, &s->cmd_pool) !=
            VK_SUCCESS) {
            err = -ENOMEM;
            goto free_pipes_fail;
        }
        VkCommandBufferAllocateInfo cbai;
        memset(&cbai, 0, sizeof(cbai));
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = s->cmd_pool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (vmafx_vk.AllocateCommandBuffers(s->vk->device, &cbai, &s->cmd) != VK_SUCCESS) {
            err = -ENOMEM;
            goto free_cmdpool_fail;
        }
        VkFenceCreateInfo fci;
        memset(&fci, 0, sizeof(fci));
        if (vmafx_vk.CreateFence(s->vk->device, &fci, NULL, &s->fence) != VK_SUCCESS) {
            err = -ENOMEM;
            goto free_cmdpool_fail;
        }
    }

    s->feature_name_dict = vmaf_feature_name_dict_from_provided_features(
        fex->provided_features, options, s);
    if (!s->feature_name_dict) {
        err = -ENOMEM;
        goto free_fence_fail;
    }

    return 0;

free_fence_fail:
    vmafx_vk.DestroyFence(s->vk->device, s->fence, NULL);
free_cmdpool_fail:
    vmafx_vk.DestroyCommandPool(s->vk->device, s->cmd_pool, NULL);
free_pipes_fail:
    for (int st = 0; st < 4; st++)
        for (int sc = 0; sc < ADM_NUM_SCALES; sc++)
            if (s->pipelines[st][sc])
                vmafx_vk.DestroyPipeline(s->vk->device, s->pipelines[st][sc], NULL);
free_pl_fail:
    vmafx_vk.DestroyPipelineLayout(s->vk->device, s->pipe_layout, NULL);
free_pool_fail:
    vmafx_vk.DestroyDescriptorPool(s->vk->device, s->pool, NULL);
free_dsl_fail:
    vmafx_vk.DestroyDescriptorSetLayout(s->vk->device, s->dsl, NULL);
free_buffers_fail:
    for (int sc = 0; sc < ADM_NUM_SCALES; sc++)
        vmafx_vulkan_destroy_buffer(s->vk, &s->accum[sc]);
    vmafx_vulkan_destroy_buffer(s->vk, &s->div_lookup);
    vmafx_vulkan_destroy_buffer(s->vk, &s->csf_f);
    vmafx_vulkan_destroy_buffer(s->vk, &s->dis_band);
    vmafx_vulkan_destroy_buffer(s->vk, &s->ref_band);
    vmafx_vulkan_destroy_buffer(s->vk, &s->dwt_tmp_dis);
    vmafx_vulkan_destroy_buffer(s->vk, &s->dwt_tmp_ref);
    vmafx_vulkan_destroy_buffer(s->vk, &s->src_dis);
    vmafx_vulkan_destroy_buffer(s->vk, &s->src_ref);
    if (s->vk_owned) {
        vmafx_vulkan_state_destroy(s->vk);
        s->vk = NULL;
    }
    return err;
}

static int close_fex(VmafFeatureExtractor *fex)
{
    AdmVulkanState *s = fex->priv;
    if (!s)
        return 0;

    (void)vmaf_dictionary_free(&s->feature_name_dict);

    if (s->vk) {
        vmafx_vk.DeviceWaitIdle(s->vk->device);
        vmafx_vk.DestroyFence(s->vk->device, s->fence, NULL);
        vmafx_vk.DestroyCommandPool(s->vk->device, s->cmd_pool, NULL);
        for (int st = 0; st < 4; st++)
            for (int sc = 0; sc < ADM_NUM_SCALES; sc++)
                vmafx_vk.DestroyPipeline(s->vk->device, s->pipelines[st][sc], NULL);
        vmafx_vk.DestroyPipelineLayout(s->vk->device, s->pipe_layout, NULL);
        vmafx_vk.DestroyDescriptorPool(s->vk->device, s->pool, NULL);
        vmafx_vk.DestroyDescriptorSetLayout(s->vk->device, s->dsl, NULL);
        for (int sc = 0; sc < ADM_NUM_SCALES; sc++)
            vmafx_vulkan_destroy_buffer(s->vk, &s->accum[sc]);
        vmafx_vulkan_destroy_buffer(s->vk, &s->div_lookup);
        vmafx_vulkan_destroy_buffer(s->vk, &s->csf_f);
        vmafx_vulkan_destroy_buffer(s->vk, &s->dis_band);
        vmafx_vulkan_destroy_buffer(s->vk, &s->ref_band);
        vmafx_vulkan_destroy_buffer(s->vk, &s->dwt_tmp_dis);
        vmafx_vulkan_destroy_buffer(s->vk, &s->dwt_tmp_ref);
        vmafx_vulkan_destroy_buffer(s->vk, &s->src_dis);
        vmafx_vulkan_destroy_buffer(s->vk, &s->src_ref);
    }
    if (s->vk_owned) {
        vmafx_vulkan_state_destroy(s->vk);
        s->vk = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* extract: upload -> record 16 dispatches -> wait -> reduce -> emit   */
/* ------------------------------------------------------------------ */

static int record_frame(AdmVulkanState *s)
{
    VkCommandBufferBeginInfo bbi;
    memset(&bbi, 0, sizeof(bbi));
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vmafx_vk.BeginCommandBuffer(s->cmd, &bbi);

    for (int sc = 0; sc < ADM_NUM_SCALES; sc++) {
        unsigned cw = s->scale_w[sc], ch = s->scale_h[sc];
        unsigned hw = s->scale_hw[sc], hh = s->scale_hh[sc];
        uint32_t v_shift, v_add, h_shift;
        if (sc == 0) {
            v_shift = 8; v_add = 128; h_shift = 16;
        } else if (sc == 1) {
            v_shift = 0; v_add = 0; h_shift = 15;
        } else if (sc == 2) {
            v_shift = 16; v_add = 32768; h_shift = 16;
        } else {
            v_shift = 16; v_add = 32768; h_shift = 15;
        }
        int left = (int)((double)hw * ADM_BORDER_FACTOR - 0.5);
        int top = (int)((double)hh * ADM_BORDER_FACTOR - 0.5);
        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        int right = (int)hw - left;
        int bottom = (int)hh - top;
        int active_w = right - left, active_h = bottom - top;
        unsigned in_stride = (sc == 0) ? s->w : s->buf_stride;

        AdmPushConsts pc;
        memset(&pc, 0, sizeof(pc));
        pc.cur_w = cw;
        pc.cur_h = ch;
        pc.half_w = hw;
        pc.half_h = hh;
        pc.buf_stride = s->buf_stride;
        pc.in_stride = in_stride;
        pc.v_shift = v_shift;
        pc.v_add = v_add;
        pc.h_shift = h_shift;
        pc.i_rfactor_h = s->i_rfactor[sc][0];
        pc.i_rfactor_v = s->i_rfactor[sc][1];
        pc.i_rfactor_d = s->i_rfactor[sc][2];
        pc.num_workgroups_x = 1;
        pc.gain_hi = s->gq.gain_hi;
        pc.gain_lo = s->gq.gain_lo;
        pc.active_left = left;
        pc.active_top = top;
        pc.active_right = right;
        pc.active_bottom = bottom;
        compute_csf_cm_shifts(hw, hh, sc, active_w, active_h, &pc);

        VkMemoryBarrier mb;
        memset(&mb, 0, sizeof(mb));
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        vmafx_vk.CmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 s->pipelines[0][sc]);
        vmafx_vk.CmdBindDescriptorSets(s->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                       s->pipe_layout, 0, 1, &s->sets[sc], 0, NULL);
        vmafx_vk.CmdPushConstants(s->cmd, s->pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                  0, sizeof(pc), &pc);
        vmafx_vk.CmdDispatch(s->cmd, (cw + 15u) / 16u, (hh + 15u) / 16u, 2);
        vmafx_vk.CmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                                    0, NULL, 0, NULL);
        vmafx_vk.CmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 s->pipelines[1][sc]);
        vmafx_vk.CmdDispatch(s->cmd, (hw + 15u) / 16u, (hh + 15u) / 16u, 2);
        vmafx_vk.CmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                                    0, NULL, 0, NULL);
        vmafx_vk.CmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 s->pipelines[2][sc]);
        vmafx_vk.CmdDispatch(s->cmd, (hw + 15u) / 16u, (hh + 15u) / 16u, 1);
        vmafx_vk.CmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                                    0, NULL, 0, NULL);
        unsigned num_rows = (unsigned)(active_h > 0 ? active_h : 1);
        vmafx_vk.CmdBindPipeline(s->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 s->pipelines[3][sc]);
        vmafx_vk.CmdDispatch(s->cmd, 3u * num_rows, 1u, 1u);
        vmafx_vk.CmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                                    0, NULL, 0, NULL);
    }
    vmafx_vk.EndCommandBuffer(s->cmd);
    return 0;
}

static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic,
                   VmafPicture *ref_pic_90, VmafPicture *dist_pic,
                   VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
{
    (void)ref_pic_90;
    (void)dist_pic_90;
    AdmVulkanState *s = fex->priv;

    /* Upload the Y planes with the padded word stride the shader expects. */
    const VkDeviceSize row_bytes = (VkDeviceSize)(((s->w + 3u) >> 2) * 4u);
    uint8_t *refm = (uint8_t *)s->src_ref.mapped;
    uint8_t *dism = (uint8_t *)s->src_dis.mapped;
    const uint8_t *refy = (const uint8_t *)ref_pic->data[0];
    const uint8_t *disy = (const uint8_t *)dist_pic->data[0];
    memset(refm, 0, (size_t)row_bytes * s->h);
    memset(dism, 0, (size_t)row_bytes * s->h);
    for (unsigned y = 0; y < s->h; y++) {
        memcpy(refm + (size_t)y * row_bytes, refy + (size_t)y * ref_pic->stride[0], s->w);
        memcpy(dism + (size_t)y * row_bytes, disy + (size_t)y * dist_pic->stride[0], s->w);
    }

    for (int sc = 0; sc < ADM_NUM_SCALES; sc++)
        memset(s->accum[sc].mapped, 0, (size_t)s->wg_count[sc] * 6 * 8);

    int err = record_frame(s);
    if (err)
        return err;

    vmafx_vk.ResetFences(s->vk->device, 1, &s->fence);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cmd;
    VkResult r = vmafx_vk.QueueSubmit(s->vk->queue, 1, &si, s->fence);
    if (r != VK_SUCCESS) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_adm_vulkan: vkQueueSubmit: %s\n",
                 vmafx_vulkan_result_str(r));
        return -EIO;
    }
    r = vmafx_vk.WaitForFences(s->vk->device, 1, &s->fence, VK_TRUE,
                               120ull * 1000ull * 1000ull * 1000ull);
    if (r != VK_SUCCESS) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "integer_adm_vulkan: fence wait: %s\n",
                 vmafx_vulkan_result_str(r));
        return -EIO;
    }

    /* Host reduction of the per-WG slots + CPU-exact conclude. */
    int64_t csf_tot[ADM_NUM_SCALES][3], cm_tot[ADM_NUM_SCALES][3];
    for (int sc = 0; sc < ADM_NUM_SCALES; sc++) {
        const int64_t *acc = (const int64_t *)s->accum[sc].mapped;
        for (int b = 0; b < 3; b++) {
            int64_t c = 0, m = 0;
            for (uint32_t g = 0; g < s->wg_count[sc]; g++) {
                c += acc[g * 6 + b];
                m += acc[g * 6 + 3 + b];
            }
            csf_tot[sc][b] = c;
            cm_tot[sc][b] = m;
        }
    }

    float num_scale[ADM_NUM_SCALES], den_scale[ADM_NUM_SCALES];
    for (int sc = 0; sc < ADM_NUM_SCALES; sc++) {
        uint64_t cu[3];
        for (int b = 0; b < 3; b++)
            cu[b] = (uint64_t)csf_tot[sc][b];
        conclude_csf_den_host(cu, (int)s->scale_hh[sc], (int)s->scale_hw[sc], sc,
                              &den_scale[sc]);
        conclude_cm_host(cm_tot[sc], (int)s->scale_hh[sc], (int)s->scale_hw[sc], sc,
                         &num_scale[sc]);
    }

    /* CPU integer_compute_adm: float per-scale values summed into doubles
     * in scale order; numden_limit uses the BASE frame. */
    double num = 0.0, den = 0.0;
    for (int sc = 0; sc < ADM_NUM_SCALES; sc++) {
        num += num_scale[sc];
        den += den_scale[sc];
    }
    double numden_limit = 1e-10 * (double)((long long)s->w * s->h) / (1920.0 * 1080.0);
    if (num < numden_limit)
        num = 0.0;
    if (den < numden_limit)
        den = 0.0;
    double score = (den == 0.0) ? 1.0 : num / den;

    double scale_scores[ADM_NUM_SCALES];
    for (int sc = 0; sc < ADM_NUM_SCALES; sc++)
        scale_scores[sc] = (den_scale[sc] == 0.0f) ? 1.0 : (double)num_scale[sc] / den_scale[sc];

    /* Same name set the CPU twin files (minus the AIM/ADM3 atoms the draft
     * does not compute — they resolve to the CPU twin through the normal
     * twin resolution). Fixed static storage: the emit call copies from
     * these, and stack buffers would alias across the loops. */
    static const char *const value_names[] = {
        "VMAF_integer_feature_adm2_score",
        "integer_adm_scale0", "integer_adm_scale1", "integer_adm_scale2",
        "integer_adm_scale3",
        "integer_adm", "integer_adm_num", "integer_adm_den",
        "integer_adm_num_scale0", "integer_adm_den_scale0",
        "integer_adm_num_scale1", "integer_adm_den_scale1",
        "integer_adm_num_scale2", "integer_adm_den_scale2",
        "integer_adm_num_scale3", "integer_adm_den_scale3",
    };
    VmafNamedScore values[16];
    size_t value_count = 0;
    values[value_count++] = (VmafNamedScore){value_names[0], score};
    for (int sc = 0; sc < ADM_NUM_SCALES; sc++)
        values[value_count++] = (VmafNamedScore){value_names[1 + sc], scale_scores[sc]};
    values[value_count++] = (VmafNamedScore){value_names[5], score};
    values[value_count++] = (VmafNamedScore){value_names[6], num};
    values[value_count++] = (VmafNamedScore){value_names[7], den};
    for (int sc = 0; sc < ADM_NUM_SCALES; sc++) {
        values[value_count++] = (VmafNamedScore){value_names[8 + sc * 2], num_scale[sc]};
        values[value_count++] = (VmafNamedScore){value_names[9 + sc * 2], den_scale[sc]};
    }

    return vmaf_feature_emit_finite_scores(feature_collector, s->feature_name_dict,
                                           "integer_adm", values, value_count, index);
}

/* ------------------------------------------------------------------ */
/* Registry struct                                                     */
/* ------------------------------------------------------------------ */

static const char *provided_features[] = {
    "VMAF_integer_feature_adm2_score",
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
    NULL,
};

// Registration struct consumed by core/src/feature/feature_extractor.cpp
// (via the fex-registry table); must retain external linkage.
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_adm_vulkan = {
    .name = "adm_vulkan",
    .init = init,
    .extract = extract,
    .options = options,
    .close = close_fex,
    .priv_size = sizeof(AdmVulkanState),
    .provided_features = provided_features,
    .flags = VMAF_FEATURE_EXTRACTOR_VULKAN,
    .chars =
        {
            .n_dispatches_per_frame = 16,
            .is_reduction_only = false,
            .min_useful_frame_area = 0,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_DIRECT,
        },
};
