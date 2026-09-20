/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

/**
 * SYCL/DPC++ ADM (Adaptive Detail Model) feature extractor.
 *
 * Implements 4-scale CDF 9/7 (DB2) Discrete Wavelet Transform, decoupling,
 * CSF weighting, and contrast masking using SYCL kernels.
 *
 * Pipeline per scale:
 *   1. DWT vertical pass -> tmp (lo/hi interleaved)
 *   2. DWT horizontal pass -> 4 sub-bands (LL, LH, HL, HH)
 *   3. Decouple+CSF fused pass -> r, csf_a, csf_f
 *   4. CSF denominator reduction -> csf_den_accum
 *   5. Contrast measure reduction -> cm_accum
 *
 * Pattern: init -> submit (non-blocking) -> collect (wait + scores)
 */

#include <utility>

#include <sycl/sycl.hpp>

#include "sycl_compat.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "config.h"
#include "feature/adm_angle_flag.h"
#include "feature/adm_csf_fixed_point.h"
#include "feature/barten_csf_tools.h"
#include "feature/integer_adm.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "sycl/common.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

// integer_adm.h defines ADM_BORDER_FACTOR as a C preprocessor macro; undefine
// it here so the constexpr declaration below compiles cleanly in C++ TUs.
// The value is identical (0.1) — this is not a redefinition.
#ifdef ADM_BORDER_FACTOR
#undef ADM_BORDER_FACTOR
#endif
#ifdef ONE_BY_15
#undef ONE_BY_15
#endif
#ifdef I4_ONE_BY_15
#undef I4_ONE_BY_15
#endif

static constexpr int ADM_NUM_SCALES = 4;
static constexpr int ADM_NUM_BANDS = 3; // h, v, d (skip band_a for scoring)
static constexpr double ADM_BORDER_FACTOR = 0.1;

// DWT filter coefficients (DB2, 4-tap)
static constexpr int32_t dwt_lo[4] = {15826, 27411, 7345, -4240};
static constexpr int32_t dwt_hi[4] = {-4240, -7345, 27411, -15826};
static constexpr int32_t dwt_lo_sum = 46342;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static constexpr int32_t ONE_BY_15 = 8738;
static constexpr int32_t I4_ONE_BY_15 = 286331153;

/* ------------------------------------------------------------------ */
/* Extractor private state                                             */
/* ------------------------------------------------------------------ */

struct AdmStateSycl {
    unsigned width, height, bpc;
    unsigned buf_stride; // aligned stride for DWT bands

    bool debug;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    int adm_ref_display_height;
    int adm_csf_mode;
    double adm_csf_scale;
    double adm_csf_diag_scale;
    double adm_noise_weight;
    double adm_min_val;   /* ADR-0487: minimum score floor (mirrors CPU + CUDA option). */
    bool adm_skip_scale0; /* host-side suppression: scale-0 excluded from score when set */
    double adm_dlm_weight;
    double adm_p_norm;

    VmafDictionary *feature_name_dict;

    // rfactors: 3 bands x 4 scales = 12
    float rfactor[12];
    uint32_t i_rfactor[12];

    // DWT intermediate buffers
    int32_t *d_dwt_tmp_ref; // vertical DWT output for ref
    int32_t *d_dwt_tmp_dis; // vertical DWT output for dis

    // DWT band outputs: 4 bands x 2 (ref+dis)
    int32_t *d_ref_band[4]; // [0]=a(LL), [1]=h(HL), [2]=v(LH), [3]=d(HH)
    int32_t *d_dis_band[4];

    // CSF outputs (d_decouple_r and d_csf_a eliminated — recomputed inline in CM kernel)
    int32_t *d_csf_f[3]; // |csf_a| / 30 — kept for 3×3 neighborhood access

    // Integer division LUT
    int32_t *d_div_lookup; // 65537 entries

    // Accumulators (device + host)
    int64_t *d_cm_accum;      // 4 scales x 3 bands = 12 int64
    int64_t *d_csf_den_accum; // 4 scales x 3 bands = 12 int64
    int64_t *h_cm_accum;
    int64_t *h_csf_den_accum;

    // Deferred state
    unsigned pending_index;
    bool has_pending;
};

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

static const VmafOption adm_csf_scale_option = {
    .name = "adm_csf_scale",
    .help = "scale coefficient for the horizontal & vertical direction terms of CSF",
    .alias = "scf",
    .offset = offsetof(AdmStateSycl, adm_csf_scale),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = DEFAULT_ADM_CSF_SCALE},
    .min = 0.0,
    .max = 50.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption adm_csf_diag_scale_option = {
    .name = "adm_csf_diag_scale",
    .help = "scale coefficient for the diagonal direction term of CSF",
    .alias = "scfd",
    .offset = offsetof(AdmStateSycl, adm_csf_diag_scale),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = DEFAULT_ADM_CSF_DIAG_SCALE},
    .min = 0.0,
    .max = 50.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

/* FEATURE_PARAM is kept for feature-name-key parity. adm_dlm_weight changes
 * adm3 arithmetic, but this twin emits adm2; see collect_fex_sycl. */
static const VmafOption adm_dlm_weight_option = {
    .name = "adm_dlm_weight",
    .help = "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
    .alias = "dlmw",
    .offset = offsetof(AdmStateSycl, adm_dlm_weight),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 0.5},
    .min = 0.0,
    .max = 1.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption adm_enhn_gain_limit_option = {
    .name = "adm_enhn_gain_limit",
    .help = "enhancement gain imposed on adm, must be >= 1.0, "
            "where 1.0 means the gain is completely disabled",
    .alias = "egl",
    .offset = offsetof(AdmStateSycl, adm_enhn_gain_limit),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = DEFAULT_ADM_ENHN_GAIN_LIMIT},
    .min = 1.0,
    .max = DEFAULT_ADM_ENHN_GAIN_LIMIT,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption adm_norm_view_dist_option = {
    .name = "adm_norm_view_dist",
    .help = "normalized viewing distance = viewing distance / ref display's physical height",
    .alias = "nvd",
    .offset = offsetof(AdmStateSycl, adm_norm_view_dist),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = DEFAULT_ADM_NORM_VIEW_DIST},
    .min = 0.75,
    .max = 24.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption adm_ref_display_height_option = {
    .name = "adm_ref_display_height",
    .help = "reference display height in pixels",
    .alias = "rdh",
    .offset = offsetof(AdmStateSycl, adm_ref_display_height),
    .type = VMAF_OPT_TYPE_INT,
    .default_val = {.i = DEFAULT_ADM_REF_DISPLAY_HEIGHT},
    .min = 1,
    .max = 4320,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption adm_csf_mode_option = {
    .name = "adm_csf_mode",
    .help = "contrast sensitivity function",
    .alias = "csf",
    .offset = offsetof(AdmStateSycl, adm_csf_mode),
    .type = VMAF_OPT_TYPE_INT,
    .default_val = {.i = DEFAULT_ADM_CSF_MODE},
    .min = 0,
    .max = 3,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption adm_noise_weight_option = {
    .name = "adm_noise_weight",
    .help = "noise weight",
    .alias = "nw",
    .offset = offsetof(AdmStateSycl, adm_noise_weight),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = DEFAULT_ADM_NOISE_WEIGHT},
    .min = 0.0,
    .max = 1500.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption adm_skip_scale0_option = {
    .name = "adm_skip_scale0",
    .help = "skip the calculation of scale 0",
    .alias = "ssz",
    .offset = offsetof(AdmStateSycl, adm_skip_scale0),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption adm_min_val_option = {
    .name = "adm_min_val",
    .help = "minimum value allowed; lower values will be clipped to this value",
    .alias = "min",
    .offset = offsetof(AdmStateSycl, adm_min_val),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = DEFAULT_ADM_MIN_VAL},
    .min = 0.0,
    .max = 1.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption adm_p_norm_option = {
    .name = "adm_p_norm",
    .help = "p-norm exponent for fixed-point ADM contrast-measure finalisation",
    .alias = "apn",
    .offset = offsetof(AdmStateSycl, adm_p_norm),
    .type = VMAF_OPT_TYPE_DOUBLE,
    .default_val = {.d = 3.0},
    .min = 1.0,
    .max = 20.0,
    .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
};

static const VmafOption adm_debug_option = {
    .name = "debug",
    .help = "debug mode: enable additional output",
    .offset = offsetof(AdmStateSycl, debug),
    .type = VMAF_OPT_TYPE_BOOL,
    .default_val = {.b = false},
};

static const VmafOption options[] = {
    adm_csf_scale_option,       adm_csf_diag_scale_option, adm_dlm_weight_option,
    adm_enhn_gain_limit_option, adm_norm_view_dist_option, adm_ref_display_height_option,
    adm_csf_mode_option,        adm_noise_weight_option,   adm_skip_scale0_option,
    adm_min_val_option,         adm_p_norm_option,         adm_debug_option,
    {.name = nullptr},
};

/* ------------------------------------------------------------------ */
/* CSF and visibility threshold helpers                                */
/* ------------------------------------------------------------------ */

static inline float dwt_quant_step(const struct dwt_model_params *params, int lambda, int theta,
                                   double adm_norm_view_dist, int adm_ref_display_height)
{
    float const r = (float)(adm_norm_view_dist * adm_ref_display_height * M_PI / 180.0);
    float const temp =
        (float)std::log10(std::pow(2.0, lambda + 1) * params->f0 * params->g[theta] / r);
    float const Q = (float)(2.0 * params->a * std::pow(10.0, params->k * (double)temp * temp) /
                            dwt_7_9_basis_function_amplitudes[lambda][theta]);
    return Q;
}

struct AdmCsfFactors {
    float factor1; /* horizontal and vertical bands */
    float factor2; /* diagonal band */
};

static AdmCsfFactors adm_csf_factors(int scale, double adm_norm_view_dist,
                                     int adm_ref_display_height, int adm_csf_mode,
                                     double adm_csf_scale, double adm_csf_diag_scale)
{
    AdmCsfFactors f;
    if (adm_csf_mode == ADM_CSF_MODE_BARTEN) {
        f.factor1 = barten_csf(scale, adm_norm_view_dist, adm_ref_display_height,
                               DEFAULT_ADM_CSF_LUM, adm_csf_scale);
        f.factor2 = barten_csf(scale, adm_norm_view_dist, adm_ref_display_height,
                               DEFAULT_ADM_CSF_LUM, adm_csf_diag_scale);
    } else if (adm_csf_mode == ADM_CSF_MODE_BARTEN_WATSON_BLEND) {
        f.factor1 = barten_watson_blend_csf(scale, 0, adm_norm_view_dist, adm_ref_display_height);
        f.factor2 = barten_watson_blend_csf(scale, 1, adm_norm_view_dist, adm_ref_display_height);
    } else if (adm_csf_mode == ADM_CSF_MODE_BARTEN_WATSON_BLEND_MAE) {
        f.factor1 =
            barten_watson_blend_csf_mae(scale, 0, adm_norm_view_dist, adm_ref_display_height);
        f.factor2 =
            barten_watson_blend_csf_mae(scale, 1, adm_norm_view_dist, adm_ref_display_height);
    } else {
        f.factor1 = 1.0f / dwt_quant_step(&dwt_7_9_YCbCr_threshold[0], scale, 1, adm_norm_view_dist,
                                          adm_ref_display_height);
        f.factor2 = 1.0f / dwt_quant_step(&dwt_7_9_YCbCr_threshold[0], scale, 2, adm_norm_view_dist,
                                          adm_ref_display_height);
    }
    return f;
}

static void adm_csf_rfactor_scale0(const float rfactor1[3], double adm_norm_view_dist,
                                   int adm_ref_display_height, int adm_csf_mode,
                                   uint32_t i_rfactor[3])
{
    if (std::fabs(adm_norm_view_dist * adm_ref_display_height -
                  DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT) < 1.0e-8 &&
        adm_csf_mode == ADM_CSF_MODE_WATSON97) {
        i_rfactor[0] = 36453;
        i_rfactor[1] = 36453;
        i_rfactor[2] = 49417;
    } else {
        double const pow2_21 = std::pow(2.0, 21.0);
        double const pow2_23 = std::pow(2.0, 23.0);
        i_rfactor[0] = (uint32_t)(rfactor1[0] * pow2_21);
        i_rfactor[1] = (uint32_t)(rfactor1[1] * pow2_21);
        i_rfactor[2] = (uint32_t)(rfactor1[2] * pow2_23);
    }
}

/**
 * Refuse a CSF configuration whose fixed-point weights would wrap
 * (ADR-1191). Mirrors `adm_csf_config_check()` in
 * core/src/feature/integer_adm.c so the CPU reference and this twin accept
 * exactly the same set of configurations -- the bounds in
 * adm_csf_fixed_point.h are the CPU pipeline's, deliberately applied here
 * too, because a twin that accepted a configuration the CPU rejects would
 * break the option / feature-name parity contract (ADR-1183). Returns 0 or
 * -EINVAL.
 */
static int adm_csf_config_check(const AdmStateSycl *s)
{
    for (int scale = 0; scale < 4; ++scale) {
        const AdmCsfFactors f =
            adm_csf_factors(scale, s->adm_norm_view_dist, s->adm_ref_display_height,
                            s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale);
        const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
        const int err = adm_csf_check_scale(scale, rfactor1, s->adm_norm_view_dist,
                                            s->adm_ref_display_height, s->adm_csf_mode);
        if (err) {
            return err;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Device-side helpers                                                 */
/* ------------------------------------------------------------------ */

static inline int dev_mirror_adm(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * sup - idx - 1;
    return idx;
}

/* ------------------------------------------------------------------ */
/* SYCL Kernel: DWT Vertical Pass (ref+dis fused)                     */
/* ------------------------------------------------------------------ */

struct AdmDwtVertParams {
    const void *input[2];
    int32_t *output[2];
    int scale;
    unsigned width;
    unsigned height;
    unsigned input_stride;
    unsigned bpc;
    unsigned shift;
    unsigned dc_offset;
};

struct AdmDwtPair {
    int32_t low;
    int32_t high;
};

static inline int32_t adm_dwt_vert_read(const AdmDwtVertParams &p, const void *input, int x, int y)
{
    y = dev_mirror_adm(y, (int)p.height);
    if (x >= (int)p.width)
        return 0;
    if (p.scale == 0) {
        if (p.bpc <= 8)
            return static_cast<const uint8_t *>(input)[y * p.input_stride + x];
        return static_cast<const uint16_t *>(input)[y * (p.input_stride / 2) + x];
    }
    return static_cast<const int32_t *>(input)[y * p.input_stride + x];
}

static inline int32_t adm_dwt_vert_read_interior(const AdmDwtVertParams &p, const void *input,
                                                 int x, int y)
{
    if (p.scale == 0) {
        if (p.bpc <= 8)
            return static_cast<const uint8_t *>(input)[y * p.input_stride + x];
        return static_cast<const uint16_t *>(input)[y * (p.input_stride / 2) + x];
    }
    return static_cast<const int32_t *>(input)[y * p.input_stride + x];
}

template <typename Tile>
static inline void adm_dwt_vert_load(const AdmDwtVertParams &p, sycl::nd_item<3> item,
                                     const Tile &tile)
{
    constexpr int workgroup_x = 32;
    constexpr int workgroup_y = 8;
    constexpr int tile_height = 2 * workgroup_y + 2;
    constexpr int tile_elements = tile_height * workgroup_x;
    constexpr int workgroup_size = workgroup_x * workgroup_y;
    int const lane = (int)item.get_local_id(1) * workgroup_x + (int)item.get_local_id(2);
    int const origin_x = (int)(item.get_group(2) * workgroup_x);
    int const origin_y = 2 * (int)(item.get_group(1) * workgroup_y) - 1;
    const void *const input = p.input[item.get_global_id(0)];
    bool const interior = origin_y >= 0 && origin_y + tile_height <= (int)p.height &&
                          origin_x + workgroup_x <= (int)p.width;
    for (int i = lane; i < tile_elements; i += workgroup_size) {
        int const row = i / workgroup_x;
        int const col = i % workgroup_x;
        int const x = origin_x + col;
        int const y = origin_y + row;
        tile[row][col] = interior ? adm_dwt_vert_read_interior(p, input, x, y) :
                                    adm_dwt_vert_read(p, input, x, y);
    }
}

template <typename Tile>
static inline AdmDwtPair adm_dwt_vert_filter(const AdmDwtVertParams &p, const Tile &tile, int row,
                                             int col)
{
    int32_t const sample0 = tile[row][col];
    int32_t const sample1 = tile[row + 1][col];
    int32_t const sample2 = tile[row + 2][col];
    int32_t const sample3 = tile[row + 3][col];
    int64_t low = (int64_t)dwt_lo[0] * sample0 + (int64_t)dwt_lo[1] * sample1 +
                  (int64_t)dwt_lo[2] * sample2 + (int64_t)dwt_lo[3] * sample3;
    int64_t const high = (int64_t)dwt_hi[0] * sample0 + (int64_t)dwt_hi[1] * sample1 +
                         (int64_t)dwt_hi[2] * sample2 + (int64_t)dwt_hi[3] * sample3;
    if (p.scale == 0)
        low -= (int64_t)dwt_lo_sum * p.dc_offset;
    if (p.shift == 0)
        return {(int32_t)low, (int32_t)high};
    int64_t const rounding = (int64_t)1 << (p.shift - 1);
    return {
        (int32_t)((low + rounding) >> p.shift),
        (int32_t)((high + rounding) >> p.shift),
    };
}

static AdmDwtVertParams adm_dwt_vert_params(const void *input_ref, int32_t *output_ref,
                                            const void *input_dis, int32_t *output_dis, int scale,
                                            unsigned width, unsigned height, unsigned input_stride,
                                            unsigned bpc, unsigned shift, unsigned dc_offset)
{
    return {
        .input = {input_ref, input_dis},
        .output = {output_ref, output_dis},
        .scale = scale,
        .width = width,
        .height = height,
        .input_stride = input_stride,
        .bpc = bpc,
        .shift = shift,
        .dc_offset = dc_offset,
    };
}

static sycl::event launch_dwt_vert_pair(sycl::queue &q, const void *input_ref, int32_t *dwt_tmp_ref,
                                        const void *input_dis, int32_t *dwt_tmp_dis, int scale,
                                        unsigned width, unsigned height, unsigned in_stride,
                                        unsigned bpc, unsigned v_shift, unsigned v_add)
{
    AdmDwtVertParams const p =
        adm_dwt_vert_params(input_ref, dwt_tmp_ref, input_dis, dwt_tmp_dis, scale, width, height,
                            in_stride, bpc, v_shift, v_add);
    unsigned const half_height = (height + 1) / 2;
    sycl::range<3> const global(2, ((size_t)(half_height + 7) / 8) * 8,
                                ((size_t)(width + 31) / 32) * 32);
    sycl::range<3> const local(1, 8, 32);
    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<int32_t, 2> const tile(sycl::range<2>(18, 32), cgh);
        cgh.parallel_for(sycl::nd_range<3>(global, local), [=](sycl::nd_item<3> item) {
            adm_dwt_vert_load(p, item, tile);
            item.barrier(sycl::access::fence_space::local_space);
            unsigned const x = item.get_global_id(2);
            unsigned const y = item.get_global_id(1);
            if (x >= p.width || y >= half_height)
                return;
            AdmDwtPair const filtered = adm_dwt_vert_filter(p, tile, 2 * (int)item.get_local_id(1),
                                                            (int)item.get_local_id(2));
            unsigned const stride = p.width * 2;
            int32_t *const output = p.output[item.get_global_id(0)];
            output[y * stride + x] = filtered.low;
            output[y * stride + p.width + x] = filtered.high;
        });
    });
}

/* SYCL Kernel: DWT Horizontal Pass (ref+dis fused)                   */
/* ------------------------------------------------------------------ */

struct AdmDwtHoriParams {
    const int32_t *input[2];
    int32_t *output[2][4];
    unsigned width;
    unsigned half_width;
    unsigned half_height;
    unsigned output_stride;
    unsigned shift;
};

struct AdmDwtBands {
    int32_t approximation;
    int32_t horizontal;
    int32_t vertical;
    int32_t diagonal;
};

static inline int32_t adm_dwt_hori_read(const AdmDwtHoriParams &p, const int32_t *input,
                                        unsigned row, bool high, int x)
{
    int const sample_x = dev_mirror_adm(x, (int)p.width);
    unsigned const offset = high ? p.width : 0;
    return input[row * (p.width * 2) + offset + sample_x];
}

static inline int32_t adm_dwt_hori_quantize(int64_t value, unsigned shift)
{
    if (shift == 0)
        return (int32_t)value;
    int64_t const rounding = (int64_t)1 << (shift - 1);
    return (int32_t)((value + rounding) >> shift);
}

static inline AdmDwtBands adm_dwt_hori_filter(const AdmDwtHoriParams &p, const int32_t *input,
                                              unsigned row, int base_x)
{
    int32_t const low0 = adm_dwt_hori_read(p, input, row, false, base_x - 1);
    int32_t const low1 = adm_dwt_hori_read(p, input, row, false, base_x);
    int32_t const low2 = adm_dwt_hori_read(p, input, row, false, base_x + 1);
    int32_t const low3 = adm_dwt_hori_read(p, input, row, false, base_x + 2);
    int64_t const approximation = (int64_t)dwt_lo[0] * low0 + (int64_t)dwt_lo[1] * low1 +
                                  (int64_t)dwt_lo[2] * low2 + (int64_t)dwt_lo[3] * low3;
    int64_t const horizontal = (int64_t)dwt_hi[0] * low0 + (int64_t)dwt_hi[1] * low1 +
                               (int64_t)dwt_hi[2] * low2 + (int64_t)dwt_hi[3] * low3;
    int32_t const high0 = adm_dwt_hori_read(p, input, row, true, base_x - 1);
    int32_t const high1 = adm_dwt_hori_read(p, input, row, true, base_x);
    int32_t const high2 = adm_dwt_hori_read(p, input, row, true, base_x + 1);
    int32_t const high3 = adm_dwt_hori_read(p, input, row, true, base_x + 2);
    int64_t const vertical = (int64_t)dwt_lo[0] * high0 + (int64_t)dwt_lo[1] * high1 +
                             (int64_t)dwt_lo[2] * high2 + (int64_t)dwt_lo[3] * high3;
    int64_t const diagonal = (int64_t)dwt_hi[0] * high0 + (int64_t)dwt_hi[1] * high1 +
                             (int64_t)dwt_hi[2] * high2 + (int64_t)dwt_hi[3] * high3;
    return {
        adm_dwt_hori_quantize(approximation, p.shift),
        adm_dwt_hori_quantize(horizontal, p.shift),
        adm_dwt_hori_quantize(vertical, p.shift),
        adm_dwt_hori_quantize(diagonal, p.shift),
    };
}

static AdmDwtHoriParams adm_dwt_hori_params(const int32_t *dwt_tmp_ref, int32_t *ref_band_a,
                                            int32_t *ref_band_h, int32_t *ref_band_v,
                                            int32_t *ref_band_d, const int32_t *dwt_tmp_dis,
                                            int32_t *dis_band_a, int32_t *dis_band_h,
                                            int32_t *dis_band_v, int32_t *dis_band_d,
                                            unsigned width, unsigned height, unsigned buf_stride,
                                            unsigned shift)
{
    return {
        .input = {dwt_tmp_ref, dwt_tmp_dis},
        .output = {{ref_band_a, ref_band_h, ref_band_v, ref_band_d},
                   {dis_band_a, dis_band_h, dis_band_v, dis_band_d}},
        .width = width,
        .half_width = (width + 1) / 2,
        .half_height = (height + 1) / 2,
        .output_stride = buf_stride,
        .shift = shift,
    };
}

static sycl::event launch_dwt_hori_pair(sycl::queue &q, const int32_t *dwt_tmp_ref,
                                        int32_t *ref_band_a, int32_t *ref_band_h,
                                        int32_t *ref_band_v, int32_t *ref_band_d,
                                        const int32_t *dwt_tmp_dis, int32_t *dis_band_a,
                                        int32_t *dis_band_h, int32_t *dis_band_v,
                                        int32_t *dis_band_d, unsigned width, unsigned height,
                                        unsigned buf_stride, unsigned h_shift)
{
    AdmDwtHoriParams const p = adm_dwt_hori_params(
        dwt_tmp_ref, ref_band_a, ref_band_h, ref_band_v, ref_band_d, dwt_tmp_dis, dis_band_a,
        dis_band_h, dis_band_v, dis_band_d, width, height, buf_stride, h_shift);
    sycl::range<3> const global(2, ((size_t)(p.half_height + 7) / 8) * 8,
                                ((size_t)(p.half_width + 31) / 32) * 32);
    sycl::range<3> const local(1, 8, 32);
    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<3>(global, local), [=](sycl::nd_item<3> item) {
            unsigned const x = item.get_global_id(2);
            unsigned const y = item.get_global_id(1);
            if (x >= p.half_width || y >= p.half_height)
                return;
            unsigned const plane = item.get_global_id(0);
            AdmDwtBands const bands = adm_dwt_hori_filter(p, p.input[plane], y, 2 * (int)x);
            unsigned const index = y * p.output_stride + x;
            p.output[plane][0][index] = bands.approximation;
            p.output[plane][1][index] = bands.horizontal;
            p.output[plane][2][index] = bands.vertical;
            p.output[plane][3][index] = bands.diagonal;
        });
    });
}

/* SYCL Kernel: Decouple + CSF (Fused)                                */
/* ------------------------------------------------------------------ */

/*
 * Enhancement gain limiting: emulate double-precision multiply using int64.
 *
 * The CPU reference does: rst = (int)(r_val * gain_limit_double), then
 * clamps via min/max against th.  gain_limit is in [1.0, 100.0].
 *
 * For production models (gain = 1.0 or 100.0), the Q31 fixed-point
 * representation is exact.  For non-integer gain values (e.g. 1.2 in
 * unit tests), the result may differ by at most ±1 from double precision
 * for extreme DWT coefficient magnitudes (|r_val| near 2^31).
 *
 * The split-multiply avoids int64 overflow:
 *   gain_q31 = round(gain * 2^31)      -- up to 38 bits
 *   gain_hi  = gain_q31 >> 16           -- up to 22 bits
 *   gain_lo  = gain_q31 & 0xFFFF        -- 16 bits
 *   product  = r_val * gain_hi << 16 + r_val * gain_lo
 *   result   = product >> 31
 */
struct GainLimitQ31 {
    int32_t gain_hi; // upper 22 bits of gain_q31
    int32_t gain_lo; // lower 16 bits of gain_q31
};

static inline GainLimitQ31 gain_limit_to_q31(double gain_limit)
{
    int64_t const gain_q31 = (int64_t)llround(gain_limit * (1LL << 31));
    return {
        .gain_hi = (int32_t)(gain_q31 >> 16),
        .gain_lo = (int32_t)(gain_q31 & 0xFFFF),
    };
}

struct AdmDivision {
    int32_t value;
    int shift;
};

struct AdmDecouple {
    int32_t value;
    int32_t gain;
};

static inline int adm_best15_shift(uint32_t value)
{
    int msb = 15;
    uint32_t scan = (value >> 15) & 0x1FFFFu;
    if (scan >= (1u << 16)) {
        msb += 16;
        scan >>= 16;
    }
    if (scan >= (1u << 8)) {
        msb += 8;
        scan >>= 8;
    }
    if (scan >= (1u << 4)) {
        msb += 4;
        scan >>= 4;
    }
    if (scan >= (1u << 2)) {
        msb += 2;
        scan >>= 2;
    }
    if (scan >= (1u << 1))
        msb += 1;
    msb = msb > 31 ? 31 : msb;
    return msb - 14;
}

static inline AdmDivision adm_division(int scale, int32_t reference, const int32_t *lookup)
{
    int32_t const magnitude = reference < 0 ? -reference : reference;
    int32_t const sign = reference < 0 ? -1 : 1;
    if (scale == 0 || magnitude < 32768)
        return {lookup[32768 + magnitude] * sign, 0};
    uint32_t const value = (uint32_t)magnitude;
    int const shift = adm_best15_shift(value);
    uint32_t const rounded = (value + (1u << (shift - 1))) >> shift;
    return {lookup[32768 + rounded] * sign, shift};
}

static inline AdmDecouple adm_decouple(int scale, int32_t reference, int32_t distorted,
                                       const int32_t *lookup)
{
    if (reference == 0)
        return {0, 32768};
    AdmDivision const division = adm_division(scale, reference, lookup);
    int64_t const product = (int64_t)division.value * distorted;
    int32_t gain;
    if (scale == 0) {
        gain = (int32_t)((product + (1 << 14)) >> 15);
    } else {
        int const shift = 15 + division.shift;
        gain = (int32_t)((product + ((int64_t)1 << (shift - 1))) >> shift);
    }
    if (gain < 0)
        gain = 0;
    if (gain > 32768)
        gain = 32768;
    int32_t const value = (int32_t)(((int64_t)gain * reference + 16384) >> 15);
    return {value, gain};
}

static inline int32_t adm_gain_limited(int32_t value, int32_t gain, int32_t reference,
                                       int32_t distorted, bool angle_flag,
                                       const GainLimitQ31 &gain_limit)
{
    if (!angle_flag)
        return value;
    float projected;
    {
#pragma clang fp contract(off)
        float const normalized_gain = (float)gain / 32768.0f;
        float const normalized_reference = (float)reference / 64.0f;
        projected = normalized_gain * normalized_reference;
    }
    int64_t const product_high = (int64_t)value * gain_limit.gain_hi;
    int64_t const product_low = (int64_t)value * gain_limit.gain_lo;
    int64_t const main_part = product_high >> 15;
    int64_t const remainder = ((product_high - (main_part << 15)) << 16) + product_low;
    int64_t const gained = main_part + (remainder >> 31);
    if (projected > 0.0f)
        return (int32_t)(gained < (int64_t)distorted ? gained : (int64_t)distorted);
    if (projected < 0.0f)
        return (int32_t)(gained > (int64_t)distorted ? gained : (int64_t)distorted);
    return value;
}

static inline int32_t adm_csf_value(int scale, int band, uint32_t rfactor, int32_t residual)
{
    if (scale == 0) {
        int const shift = band < 2 ? 15 : 17;
        int64_t const rounding = (int64_t)1 << (shift - 1);
        return (int32_t)(((int64_t)rfactor * residual + rounding) >> shift);
    }
    return (int32_t)(((int64_t)rfactor * residual + (1LL << 27)) >> 28);
}

static inline int32_t adm_csf_filter_value(int scale, int32_t csf_value)
{
    int32_t const magnitude = csf_value < 0 ? -csf_value : csf_value;
    if (scale == 0)
        return (int32_t)(((int64_t)4369 * magnitude + 2048) >> 12);
    return (int32_t)(((int64_t)143165577 * magnitude + (1LL << 31)) >> 32);
}

static inline bool adm_decouple_angle(const int32_t reference[3], const int32_t distorted[3])
{
    int64_t const dot = (int64_t)reference[0] * distorted[0] + (int64_t)reference[1] * distorted[1];
    int64_t const reference_magnitude =
        (int64_t)reference[0] * reference[0] + (int64_t)reference[1] * reference[1];
    int64_t const distorted_magnitude =
        (int64_t)distorted[0] * distorted[0] + (int64_t)distorted[1] * distorted[1];
    return adm_angle_flag_i64(dot, reference_magnitude, distorted_magnitude) != 0;
}

struct AdmDecoupleParams {
    int scale;
    unsigned width;
    unsigned height;
    unsigned stride;
    GainLimitQ31 gain_limit;
    const int32_t *reference[3];
    const int32_t *distorted[3];
    int32_t *filtered[3];
    uint32_t rfactor[3];
    const int32_t *division_lookup;
};

template <bool UseFP64>
static sycl::event
launch_decouple_csf(sycl::queue &q, int scale, unsigned half_w, unsigned half_h,
                    unsigned buf_stride, double adm_enhn_gain_limit, uint32_t i_rfactor_h,
                    uint32_t i_rfactor_v, uint32_t i_rfactor_d, const int32_t *ref_h,
                    const int32_t *ref_v, const int32_t *ref_d, const int32_t *dis_h,
                    const int32_t *dis_v, const int32_t *dis_d, int32_t *csf_f_h, int32_t *csf_f_v,
                    int32_t *csf_f_d, const int32_t *div_lookup)
{
    (void)UseFP64;
    AdmDecoupleParams const p = {
        .scale = scale,
        .width = half_w,
        .height = half_h,
        .stride = buf_stride,
        .gain_limit = gain_limit_to_q31(adm_enhn_gain_limit),
        .reference = {ref_h, ref_v, ref_d},
        .distorted = {dis_h, dis_v, dis_d},
        .filtered = {csf_f_h, csf_f_v, csf_f_d},
        .rfactor = {i_rfactor_h, i_rfactor_v, i_rfactor_d},
        .division_lookup = div_lookup,
    };
    sycl::range<2> const global(((size_t)(half_h + 15) / 16) * 16,
                                ((size_t)(half_w + 15) / 16) * 16);
    sycl::range<2> const local(16, 16);
    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<2>(global, local), [=](sycl::nd_item<2> item) {
            unsigned const x = item.get_global_id(1);
            unsigned const y = item.get_global_id(0);
            if (x >= p.width || y >= p.height)
                return;
            unsigned const index = y * p.stride + x;
            int32_t const reference[3] = {p.reference[0][index], p.reference[1][index],
                                          p.reference[2][index]};
            int32_t const distorted[3] = {p.distorted[0][index], p.distorted[1][index],
                                          p.distorted[2][index]};
            bool const angle_flag = adm_decouple_angle(reference, distorted);
            for (int band = 0; band < 3; band++) {
                AdmDecouple const decoupled =
                    adm_decouple(p.scale, reference[band], distorted[band], p.division_lookup);
                int32_t const limited =
                    adm_gain_limited(decoupled.value, decoupled.gain, reference[band],
                                     distorted[band], angle_flag, p.gain_limit);
                int32_t const csf =
                    adm_csf_value(p.scale, band, p.rfactor[band], distorted[band] - limited);
                p.filtered[band][index] = adm_csf_filter_value(p.scale, csf);
            }
        });
    });
}

/* SYCL Kernel: CSF Denominator Reduction — 3 bands fused             */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* SYCL Kernel: CSF Denominator + Contrast Measure — 3 bands fused    */
/*                                                                     */
/* Combines csf_den_3band, cm_3band, AND decouple+CSF into a single   */
/* dispatch.  Decouple+CSF values (r_val, csf_a_val) are recomputed   */
/* per pixel instead of read from global memory, eliminating           */
/* d_decouple_r[3] and d_csf_a[3] buffers (~47 MB at 4K).             */
/* d_csf_f[3] is still read from global memory (3×3 neighborhood).    */
/* ------------------------------------------------------------------ */

struct AdmCsfShifts {
    uint32_t square;
    uint32_t cube;
    uint32_t accumulation;
};

struct AdmCmShifts {
    uint32_t inner;
    uint32_t subtraction[3];
    uint32_t square[3];
    uint32_t cube[3];
};

struct AdmCombinedBuffers {
    const int32_t *reference[3];
    const int32_t *distorted[3];
    const int32_t *filtered[3];
    int64_t *csf_accum[3];
    int64_t *cm_accum[3];
    uint32_t rfactor[3];
    const int32_t *division_lookup;
};

struct AdmCombinedParams {
    int scale;
    int left;
    int top;
    int right;
    int bottom;
    unsigned width;
    unsigned height;
    unsigned stride;
    AdmCsfShifts csf_shift;
    AdmCmShifts cm_shift;
    GainLimitQ31 gain_limit;
    AdmCombinedBuffers buffers;
};

struct AdmCombinedSums {
    int64_t csf;
    int64_t contrast;
};

static AdmCsfShifts adm_csf_shifts(int scale, int width, int height)
{
    AdmCsfShifts shifts = {};
    if (scale == 0) {
        int const area = width * height;
        int const accumulation = (int)std::ceil(std::log2(area) - 20);
        shifts.accumulation = accumulation > 0 ? (uint32_t)accumulation : 0;
    } else {
        shifts.square = scale == 2 ? 30 : 31;
        shifts.cube = (uint32_t)std::ceil(std::log2(width));
        shifts.accumulation = (uint32_t)std::ceil(std::log2(height));
    }
    return shifts;
}

static AdmCmShifts adm_cm_shifts(int scale, unsigned width, unsigned height)
{
    AdmCmShifts shifts = {};
    shifts.inner = (uint32_t)std::ceil(std::log2((double)height));
    for (int band = 0; band < 3; band++) {
        if (scale == 0) {
            shifts.subtraction[band] = band < 2 ? 10 : 12;
            shifts.square[band] = band < 2 ? 29 : 30;
            shifts.cube[band] =
                (uint32_t)(std::ceil(std::log2((double)width)) - (band < 2 ? 4 : 3));
        } else {
            shifts.subtraction[band] = 15;
            shifts.square[band] = 30;
            shifts.cube[band] = (uint32_t)std::ceil(std::log2((double)width));
        }
    }
    return shifts;
}

static AdmCombinedParams adm_combined_params(int scale, unsigned width, unsigned height,
                                             unsigned stride, double gain_limit,
                                             const AdmCombinedBuffers &buffers)
{
    AdmCombinedParams p = {};
    p.scale = scale;
    p.left = (int)(width * ADM_BORDER_FACTOR - 0.5);
    p.top = (int)(height * ADM_BORDER_FACTOR - 0.5);
    p.right = (int)width - p.left;
    p.bottom = (int)height - p.top;
    if (p.left < 0)
        p.left = 0;
    if (p.top < 0)
        p.top = 0;
    p.width = width;
    p.height = height;
    p.stride = stride;
    p.csf_shift = adm_csf_shifts(scale, p.right - p.left, p.bottom - p.top);
    p.cm_shift = adm_cm_shifts(scale, width, height);
    p.gain_limit = gain_limit_to_q31(gain_limit);
    p.buffers = buffers;
    return p;
}

static inline int64_t adm_csf_cube(const AdmCombinedParams &p, int32_t sample)
{
    int32_t const magnitude = sample < 0 ? -sample : sample;
    if (p.scale == 0)
        return (int64_t)magnitude * magnitude * magnitude;
    int64_t const square_rounding = (int64_t)1 << p.csf_shift.square;
    int64_t const square = ((int64_t)magnitude * magnitude + square_rounding) >> p.csf_shift.square;
    int64_t const cube_rounding = p.csf_shift.cube > 0 ? (int64_t)1 << (p.csf_shift.cube - 1) : 0;
    return (square * magnitude + cube_rounding) >> p.csf_shift.cube;
}

static inline void adm_inline_decouple(const AdmCombinedParams &p, unsigned index,
                                       int32_t decoupled[3], int32_t csf_values[3])
{
    int32_t const reference[3] = {p.buffers.reference[0][index], p.buffers.reference[1][index],
                                  p.buffers.reference[2][index]};
    int32_t const distorted[3] = {p.buffers.distorted[0][index], p.buffers.distorted[1][index],
                                  p.buffers.distorted[2][index]};
    bool const angle_flag = adm_decouple_angle(reference, distorted);
    for (int band = 0; band < 3; band++) {
        AdmDecouple const result =
            adm_decouple(p.scale, reference[band], distorted[band], p.buffers.division_lookup);
        decoupled[band] = adm_gain_limited(result.value, result.gain, reference[band],
                                           distorted[band], angle_flag, p.gain_limit);
        csf_values[band] = adm_csf_value(p.scale, band, p.buffers.rfactor[band],
                                         distorted[band] - decoupled[band]);
    }
}

static inline int adm_cm_neighbor(int coordinate, int limit)
{
    if (coordinate < 0)
        coordinate = 1;
    if (coordinate >= limit)
        coordinate = limit - 1;
    return coordinate;
}

static inline int64_t adm_contrast_threshold(const AdmCombinedParams &p, int row, int col,
                                             const int32_t csf_values[3])
{
    int64_t threshold = 0;
    for (int band = 0; band < 3; band++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                // ADR-1210: mirror the near edge to 1 and clamp the far edge.
                int const y = adm_cm_neighbor(row + dy, (int)p.height);
                int const x = adm_cm_neighbor(col + dx, (int)p.width);
                threshold += p.buffers.filtered[band][y * p.stride + x];
            }
        }
        int32_t const magnitude = csf_values[band] < 0 ? -csf_values[band] : csf_values[band];
        if (p.scale == 0)
            threshold += ((int64_t)ONE_BY_15 * magnitude + 2048) >> 12;
        else
            threshold += ((int64_t)I4_ONE_BY_15 * magnitude + (1LL << 31)) >> 32;
    }
    return threshold;
}

static inline int64_t adm_contrast_cube(const AdmCombinedParams &p, int band, int32_t decoupled,
                                        int64_t threshold)
{
    int64_t contrast;
    if (p.scale == 0) {
        contrast = (int64_t)p.buffers.rfactor[band] * decoupled;
        contrast = contrast < 0 ? -contrast : contrast;
        contrast -= threshold << p.cm_shift.subtraction[band];
    } else {
        int64_t const scaled = ((int64_t)p.buffers.rfactor[band] * decoupled + (1LL << 27)) >> 28;
        contrast = scaled < 0 ? -scaled : scaled;
        contrast -= threshold;
    }
    if (contrast < 0)
        contrast = 0;
    uint32_t const square_shift = p.cm_shift.square[band];
    int64_t const square_rounding = square_shift > 0 ? (int64_t)1 << (square_shift - 1) : 0;
    int32_t const square = (int32_t)((contrast * contrast + square_rounding) >> square_shift);
    uint32_t const cube_shift = p.cm_shift.cube[band];
    int64_t const cube_rounding = cube_shift > 0 ? (int64_t)1 << (cube_shift - 1) : 0;
    return ((int64_t)square * contrast + cube_rounding) >> cube_shift;
}

static inline AdmCombinedSums adm_combined_local(const AdmCombinedParams &p, int band, int row,
                                                 int lane)
{
    AdmCombinedSums sums = {};
    for (int col = p.left + lane; col < p.right; col += 256) {
        unsigned const index = row * p.stride + col;
        sums.csf += adm_csf_cube(p, p.buffers.reference[band][index]);
        int32_t decoupled[3];
        int32_t csf_values[3];
        adm_inline_decouple(p, index, decoupled, csf_values);
        int64_t const threshold = adm_contrast_threshold(p, row, col, csf_values);
        sums.contrast += adm_contrast_cube(p, band, decoupled[band], threshold);
    }
    return sums;
}

template <typename LocalMemory>
static inline void adm_reduce_combined(const AdmCombinedParams &p, sycl::nd_item<1> item,
                                       const LocalMemory &local_memory, int band,
                                       const AdmCombinedSums &sums)
{
    constexpr int max_subgroups = 32;
    sycl::sub_group const subgroup = item.get_sub_group();
    int64_t const subgroup_csf = sycl::reduce_over_group(subgroup, sums.csf, sycl::plus<int64_t>{});
    int64_t const subgroup_contrast =
        sycl::reduce_over_group(subgroup, sums.contrast, sycl::plus<int64_t>{});
    uint32_t const subgroup_id = subgroup.get_group_linear_id();
    if (subgroup.get_local_linear_id() == 0) {
        local_memory[subgroup_id] = subgroup_csf;
        local_memory[max_subgroups + subgroup_id] = subgroup_contrast;
    }
    item.barrier(sycl::access::fence_space::local_space);
    if (item.get_local_id(0) != 0)
        return;
    int64_t total_csf = 0;
    int64_t total_contrast = 0;
    for (uint32_t i = 0; i < subgroup.get_group_linear_range(); i++) {
        total_csf += local_memory[i];
        total_contrast += local_memory[max_subgroups + i];
    }
    int64_t const csf_rounding =
        p.csf_shift.accumulation > 0 ? (int64_t)1 << (p.csf_shift.accumulation - 1) : 0;
    int64_t const shifted_csf = (total_csf + csf_rounding) >> p.csf_shift.accumulation;
    sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                     sycl::access::address_space::global_space> const
        csf_output(*p.buffers.csf_accum[band]);
    csf_output.fetch_add(shifted_csf);
    int64_t const contrast_rounding =
        p.cm_shift.inner > 0 ? (int64_t)1 << (p.cm_shift.inner - 1) : 0;
    int64_t const shifted_contrast = (total_contrast + contrast_rounding) >> p.cm_shift.inner;
    sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                     sycl::access::address_space::global_space> const
        contrast_output(*p.buffers.cm_accum[band]);
    contrast_output.fetch_add(shifted_contrast);
}

static sycl::event launch_csf_den_cm_3band(
    sycl::queue &q, int scale, unsigned half_w, unsigned half_h, unsigned buf_stride,
    const int32_t *ref_band_h, const int32_t *ref_band_v, const int32_t *ref_band_d,
    int64_t *csf_accum_h, int64_t *csf_accum_v, int64_t *csf_accum_d, uint32_t i_rfactor_h,
    uint32_t i_rfactor_v, uint32_t i_rfactor_d, const int32_t *dis_band_h,
    const int32_t *dis_band_v, const int32_t *dis_band_d, const int32_t *csf_f_h,
    const int32_t *csf_f_v, const int32_t *csf_f_d, const int32_t *div_lookup,
    double adm_enhn_gain_limit, int64_t *cm_accum_h, int64_t *cm_accum_v, int64_t *cm_accum_d)
{
    AdmCombinedBuffers const buffers = {
        .reference = {ref_band_h, ref_band_v, ref_band_d},
        .distorted = {dis_band_h, dis_band_v, dis_band_d},
        .filtered = {csf_f_h, csf_f_v, csf_f_d},
        .csf_accum = {csf_accum_h, csf_accum_v, csf_accum_d},
        .cm_accum = {cm_accum_h, cm_accum_v, cm_accum_d},
        .rfactor = {i_rfactor_h, i_rfactor_v, i_rfactor_d},
        .division_lookup = div_lookup,
    };
    AdmCombinedParams const p =
        adm_combined_params(scale, half_w, half_h, buf_stride, adm_enhn_gain_limit, buffers);
    int const row_count = p.bottom - p.top;
    if (p.right - p.left <= 0 || row_count <= 0)
        return sycl::event{};
    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<int64_t, 1> const local_memory(sycl::range<1>(64), cgh);
        cgh.parallel_for(sycl::nd_range<1>((size_t)3 * row_count * 256, 256),
                         [=](sycl::nd_item<1> item) VMAF_SYCL_REQD_SG_SIZE(32) {
                             int const workgroup = item.get_group(0);
                             int const band = workgroup / row_count;
                             int const row = p.top + workgroup % row_count;
                             AdmCombinedSums const sums =
                                 adm_combined_local(p, band, row, item.get_local_id(0));
                             adm_reduce_combined(p, item, local_memory, band, sums);
                         });
    });
}

/* CPU scoring functions                                               */
/* ------------------------------------------------------------------ */

static void conclude_adm_cm(const int64_t *accum, int h, int w, int scale, float noise_weight,
                            double p_norm, double *result)
{
    int const left = (int)(w * ADM_BORDER_FACTOR - 0.5);
    int const top = (int)(h * ADM_BORDER_FACTOR - 0.5);
    int const right = w - left;
    int const bottom = h - top;

    const uint32_t shift_inner_accum = (uint32_t)std::ceil(std::log2(h));
    double const p_norm_exp = 1.0 / p_norm;

    /* Promote powf_add to double to avoid fp32 precision loss on Arc A380
     * (no native fp64 device, but this function is host-side — no device impact). */
    double const powf_add =
        std::pow((double)((bottom - top) * (right - left)) * (double)noise_weight, p_norm_exp);

    *result = 0;
    for (int i = 0; i < 3; i++) {
        /* Promote f_accum to double: on fp32-only devices the per-scale
         * normalization intermediate is computed host-side and fed directly into
         * SVM; fp32 rounding here amplifies to >5e-5 final-score error (iter10
         * cross-backend-parity finding). */
        double f_accum;
        if (scale == 0) {
            // CPU uses w (full band width) for shift_xcub, not active_w
            const uint32_t shift_xcub[3] = {(uint32_t)(std::ceil(std::log2((double)w)) - 4),
                                            (uint32_t)(std::ceil(std::log2((double)w)) - 4),
                                            (uint32_t)(std::ceil(std::log2((double)w)) - 3)};
            int const constant_offset[3] = {52, 52, 57};
            f_accum =
                accum[i] / std::pow(2.0, constant_offset[i] - shift_xcub[i] - shift_inner_accum);
        } else {
            // CPU uses w (full band width) for shift_cub, not active_w
            uint32_t const shift_cub = (uint32_t)std::ceil(std::log2((double)w));
            double const final_shift[3] = {std::pow(2.0, 45.0 - shift_cub - shift_inner_accum),
                                           std::pow(2.0, 39.0 - shift_cub - shift_inner_accum),
                                           std::pow(2.0, 36.0 - shift_cub - shift_inner_accum)};
            f_accum = (double)accum[i] / final_shift[scale - 1];
        }
        *result += std::pow(f_accum, p_norm_exp) + powf_add;
    }
}

static void conclude_adm_csf_den(const uint64_t *accum, int h, int w, int scale, double *result,
                                 const float rfactor[3], float noise_weight)
{
    int const left = (int)(w * ADM_BORDER_FACTOR - 0.5);
    int const top = (int)(h * ADM_BORDER_FACTOR - 0.5);
    int const right = w - left;
    int const bottom = h - top;

    const uint32_t accum_convert[4] = {18, 32, 27, 23};

    int32_t shift_accum;
    double shift_csf;
    if (scale == 0) {
        shift_accum = (int32_t)std::ceil(std::log2((bottom - top) * (right - left)) - 20);
        if (shift_accum < 0)
            shift_accum = 0;
        shift_csf = std::pow(2.0, accum_convert[scale] - shift_accum);
    } else {
        shift_accum = (int32_t)std::ceil(std::log2(bottom - top));
        uint32_t const shift_cub = (uint32_t)std::ceil(std::log2(right - left));
        shift_csf = std::pow(2.0, accum_convert[scale] - shift_accum - shift_cub);
    }

    float const powf_add =
        powf((float)((bottom - top) * (right - left)) * noise_weight, 1.0f / 3.0f);

    *result = 0;
    for (int i = 0; i < 3; i++) {
        double const csf = (double)(accum[i] / shift_csf) * std::pow(rfactor[i], 3);
        *result += powf((float)csf, 1.0f / 3.0f) + powf_add;
    }
}

/* ------------------------------------------------------------------ */
/* Feature extractor callbacks                                         */
/* ------------------------------------------------------------------ */

// Forward declarations for combined graph callbacks (defined after enqueue_adm_work_impl)
static void enqueue_adm_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis);
static void adm_pre_graph(void *queue_ptr, void *priv);
static void adm_post_graph(void *queue_ptr, void *priv);
static int close_fex_sycl(VmafFeatureExtractor *fex); /* forward decl for init error paths */

static int
close_fex_sycl(VmafFeatureExtractor *fex); /* forward decl for init-failure cleanup — SY-2a */

static int adm_init_runtime(VmafFeatureExtractor *fex, AdmStateSycl *s, unsigned bpc, unsigned w,
                            unsigned h, VmafSyclState *&state)
{
    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->has_pending = false;
    state = fex->sycl_state;
    if (!state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "adm_sycl: no SYCL state\n");
        return -EINVAL;
    }
    int const config_error = adm_csf_config_check(s);
    if (config_error)
        return config_error;
    return vmaf_sycl_shared_frame_init(state, w, h, bpc);
}

static void adm_compute_rfactors(AdmStateSycl *s)
{
    for (unsigned scale = 0; scale < ADM_NUM_SCALES; scale++) {
        AdmCsfFactors const factors =
            adm_csf_factors(scale, s->adm_norm_view_dist, s->adm_ref_display_height,
                            s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale);
        s->rfactor[scale * 3 + 0] = factors.factor1;
        s->rfactor[scale * 3 + 1] = factors.factor1;
        s->rfactor[scale * 3 + 2] = factors.factor2;
        double const pow2_32 = std::pow(2.0, 32);
        if (scale == 0) {
            adm_csf_rfactor_scale0(&s->rfactor[0], s->adm_norm_view_dist, s->adm_ref_display_height,
                                   s->adm_csf_mode, &s->i_rfactor[0]);
        } else {
            for (int band = 0; band < ADM_NUM_BANDS; band++) {
                s->i_rfactor[scale * 3 + band] = (uint32_t)(s->rfactor[scale * 3 + band] * pow2_32);
            }
        }
    }
}

static bool adm_buffers_ready(const AdmStateSycl *s)
{
    if (!s->d_dwt_tmp_ref || !s->d_dwt_tmp_dis || !s->d_div_lookup || !s->d_cm_accum ||
        !s->d_csf_den_accum || !s->h_cm_accum || !s->h_csf_den_accum)
        return false;
    for (int band = 0; band < 4; band++) {
        if (!s->d_ref_band[band] || !s->d_dis_band[band])
            return false;
    }
    for (const auto *buffer : s->d_csf_f) {
        if (!buffer)
            return false;
    }
    return true;
}

static int adm_allocate_buffers(VmafSyclState *state, AdmStateSycl *s, unsigned w, unsigned h)
{
    unsigned const half_width = (w + 1) / 2;
    unsigned const half_height = (h + 1) / 2;
    s->buf_stride = (half_width + 3) & ~3u;
    size_t const dwt_size = (size_t)w * 2 * half_height * sizeof(int32_t);
    s->d_dwt_tmp_ref = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, dwt_size));
    s->d_dwt_tmp_dis = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, dwt_size));
    size_t const band_size = (size_t)s->buf_stride * half_height * sizeof(int32_t);
    for (int band = 0; band < 4; band++) {
        s->d_ref_band[band] = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, band_size));
        s->d_dis_band[band] = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, band_size));
    }
    for (auto &buffer : s->d_csf_f)
        buffer = static_cast<int32_t *>(vmaf_sycl_malloc_device(state, band_size));
    s->d_div_lookup =
        static_cast<int32_t *>(vmaf_sycl_malloc_device(state, 65537 * sizeof(int32_t)));
    size_t const accum_size = (ptrdiff_t)ADM_NUM_SCALES * ADM_NUM_BANDS * sizeof(int64_t);
    s->d_cm_accum = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, accum_size));
    s->d_csf_den_accum = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, accum_size));
    s->h_cm_accum = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, accum_size));
    s->h_csf_den_accum = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, accum_size));
    if (adm_buffers_ready(s))
        return 0;
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "adm_sycl: device memory allocation failed\n");
    return -ENOMEM;
}

static int adm_upload_division_lookup(VmafSyclState *state, AdmStateSycl *s)
{
    size_t const size = 65537 * sizeof(int32_t);
    int32_t *lookup = static_cast<int32_t *>(std::malloc(size));
    if (!lookup)
        return -ENOMEM;
    std::memset(lookup, 0, size);
    static const int32_t factor = 1073741824;
    for (int i = 1; i <= 32768; i++) {
        int32_t const reciprocal = (int32_t)(factor / i);
        lookup[32768 + i] = reciprocal;
        lookup[32768 - i] = -reciprocal;
    }
    int const err = vmaf_sycl_memcpy_h2d(state, s->d_div_lookup, lookup, size);
    std::free(lookup);
    if (err)
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "adm_sycl: div_lookup upload failed\n");
    return err;
}

static int adm_register_graph(VmafFeatureExtractor *fex, VmafSyclState *state, AdmStateSycl *s)
{
    s->feature_name_dict =
        vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
    if (!s->feature_name_dict)
        return -ENOMEM;
    return vmaf_sycl_graph_register(state, enqueue_adm_work, adm_pre_graph, adm_post_graph, nullptr,
                                    s, "ADM");
}

static int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc,
                         unsigned w, unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<AdmStateSycl *>(fex->priv);
    VmafSyclState *state = nullptr;
    int err = adm_init_runtime(fex, s, bpc, w, h, state);
    if (err)
        return err;
    adm_compute_rfactors(s);
    err = adm_allocate_buffers(state, s, w, h);
    if (!err)
        err = adm_upload_division_lookup(state, s);
    if (!err)
        err = adm_register_graph(fex, state, s);
    if (err)
        close_fex_sycl(fex);
    return err;
}

/* ------------------------------------------------------------------ */
/* Enqueue all ADM compute work (used for both recording and direct)   */
/* ------------------------------------------------------------------ */

struct DwtShifts {
    unsigned vertical;
    unsigned vertical_offset;
    unsigned horizontal;
};

static DwtShifts adm_dwt_shifts(const AdmStateSycl *s, int scale)
{
    static const DwtShifts later_scales[] = {
        {.vertical = 0u, .vertical_offset = 0u, .horizontal = 15u},
        {.vertical = 16u, .vertical_offset = 32768u, .horizontal = 16u},
        {.vertical = 16u, .vertical_offset = 32768u, .horizontal = 15u},
    };
    if (scale == 0)
        return {.vertical = s->bpc, .vertical_offset = 1u << (s->bpc - 1), .horizontal = 16u};
    return later_scales[scale - 1];
}

static void adm_enqueue_dwt(sycl::queue &q, AdmStateSycl *s, const void *reference,
                            const void *distorted, int scale, unsigned width, unsigned height,
                            unsigned input_stride)
{
    DwtShifts const shifts = adm_dwt_shifts(s, scale);
    launch_dwt_vert_pair(q, reference, s->d_dwt_tmp_ref, distorted, s->d_dwt_tmp_dis, scale, width,
                         height, input_stride, s->bpc, shifts.vertical, shifts.vertical_offset);
    launch_dwt_hori_pair(q, s->d_dwt_tmp_ref, s->d_ref_band[0], s->d_ref_band[1], s->d_ref_band[2],
                         s->d_ref_band[3], s->d_dwt_tmp_dis, s->d_dis_band[0], s->d_dis_band[1],
                         s->d_dis_band[2], s->d_dis_band[3], width, height, s->buf_stride,
                         shifts.horizontal);
}

static void adm_enqueue_scores(sycl::queue &q, AdmStateSycl *s, int scale, unsigned width,
                               unsigned height)
{
    size_t const offset = (size_t)scale * ADM_NUM_BANDS;
    launch_decouple_csf<false>(
        q, scale, width, height, s->buf_stride, s->adm_enhn_gain_limit, s->i_rfactor[offset + 0],
        s->i_rfactor[offset + 1], s->i_rfactor[offset + 2], s->d_ref_band[1], s->d_ref_band[2],
        s->d_ref_band[3], s->d_dis_band[1], s->d_dis_band[2], s->d_dis_band[3], s->d_csf_f[0],
        s->d_csf_f[1], s->d_csf_f[2], s->d_div_lookup);
    launch_csf_den_cm_3band(
        q, scale, width, height, s->buf_stride, s->d_ref_band[1], s->d_ref_band[2],
        s->d_ref_band[3], s->d_csf_den_accum + offset + 0, s->d_csf_den_accum + offset + 1,
        s->d_csf_den_accum + offset + 2, s->i_rfactor[offset + 0], s->i_rfactor[offset + 1],
        s->i_rfactor[offset + 2], s->d_dis_band[1], s->d_dis_band[2], s->d_dis_band[3],
        s->d_csf_f[0], s->d_csf_f[1], s->d_csf_f[2], s->d_div_lookup, s->adm_enhn_gain_limit,
        s->d_cm_accum + offset + 0, s->d_cm_accum + offset + 1, s->d_cm_accum + offset + 2);
}

static void enqueue_adm_work_impl(sycl::queue &q, AdmStateSycl *s, void *shared_ref,
                                  void *shared_dis)
{
    unsigned width = s->width;
    unsigned height = s->height;
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        const void *reference = scale == 0 ? shared_ref : (const void *)s->d_ref_band[0];
        const void *distorted = scale == 0 ? shared_dis : (const void *)s->d_dis_band[0];
        unsigned const input_stride =
            scale == 0 ? (s->bpc <= 8 ? width : width * 2) : s->buf_stride;
        adm_enqueue_dwt(q, s, reference, distorted, scale, width, height, input_stride);
        width = (width + 1) / 2;
        height = (height + 1) / 2;
        adm_enqueue_scores(q, s, scale, width, height);
    }
}

/* ------------------------------------------------------------------ */
/* C-compatible callbacks for combined command graph                   */
/* ------------------------------------------------------------------ */

// Pre-graph: zero accumulators (direct enqueue, outside graph)
static void adm_pre_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<AdmStateSycl *>(priv);
    size_t const adm_accum_size = (ptrdiff_t)ADM_NUM_SCALES * ADM_NUM_BANDS * sizeof(int64_t);
    q.memset(s->d_cm_accum, 0, adm_accum_size);
    q.memset(s->d_csf_den_accum, 0, adm_accum_size);
}

// Graph-recorded: compute kernels only
static void enqueue_adm_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<AdmStateSycl *>(priv);
    enqueue_adm_work_impl(q, s, shared_ref, shared_dis);
}

// Post-graph: D2H accumulator download (direct enqueue, outside graph)
static void adm_post_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<AdmStateSycl *>(priv);
    size_t const accum_size = (ptrdiff_t)ADM_NUM_SCALES * ADM_NUM_BANDS * sizeof(int64_t);
    q.memcpy(s->h_cm_accum, s->d_cm_accum, accum_size);
    q.memcpy(s->h_csf_den_accum, s->d_csf_den_accum, accum_size);
}

/* ------------------------------------------------------------------ */
/* Submit / Collect / Extract                                          */
/* ------------------------------------------------------------------ */

static int submit_fex_sycl(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                           const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                           const VmafPicture *dist_pic_90, unsigned index)
{
    (void)ref_pic;
    (void)ref_pic_90;
    (void)dist_pic;
    (void)dist_pic_90;

    auto *s = static_cast<AdmStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    // Combined graph submit (idempotent per frame — first extractor wins)
    int const err = vmaf_sycl_graph_submit(state);
    if (err)
        return err;

    s->pending_index = index;
    s->has_pending = true;

    return 0;
}

struct AdmScores {
    double numerator[ADM_NUM_SCALES];
    double denominator[ADM_NUM_SCALES];
    double total_numerator;
    double total_denominator;
};

static AdmScores adm_compute_scores(const AdmStateSycl *s,
                                    const int64_t cm_results[ADM_NUM_SCALES][ADM_NUM_BANDS],
                                    const int64_t csf_results[ADM_NUM_SCALES][ADM_NUM_BANDS])
{
    AdmScores scores = {};
    unsigned width = s->width;
    unsigned height = s->height;
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        width = (width + 1) / 2;
        height = (height + 1) / 2;
        double numerator;
        double denominator;
        conclude_adm_cm(cm_results[scale], height, width, scale, (float)s->adm_noise_weight,
                        s->adm_p_norm, &numerator);
        conclude_adm_csf_den((const uint64_t *)csf_results[scale], height, width, scale,
                             &denominator, &s->rfactor[(size_t)scale * ADM_NUM_BANDS],
                             (float)s->adm_noise_weight);
        if (scale == 0 && s->adm_skip_scale0) {
            scores.numerator[0] = 0.0;
            scores.denominator[0] = 1e-10;
            continue;
        }
        scores.total_numerator += numerator;
        scores.total_denominator += denominator;
        scores.numerator[scale] = numerator;
        scores.denominator[scale] = denominator;
    }
    double const limit = 1e-10 * ((double)s->width * s->height) / (1920.0 * 1080.0);
    if (scores.total_numerator < limit)
        scores.total_numerator = 0.0;
    if (scores.total_denominator < limit)
        scores.total_denominator = 0.0;
    return scores;
}

static double adm_total_score(const AdmScores &scores)
{
    return scores.total_denominator == 0.0 ? 1.0 :
                                             scores.total_numerator / scores.total_denominator;
}

static void adm_append_scale_scores(VmafFeatureCollector *collector, const AdmStateSycl *s,
                                    const AdmScores &scores, unsigned index)
{
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        char name[64];
        (void)std::snprintf(name, sizeof(name), "integer_adm_scale%d", scale);
        double const score = scores.denominator[scale] == 0.0 ?
                                 1.0 :
                                 scores.numerator[scale] / scores.denominator[scale];
        vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, name, score,
                                                index);
    }
}

static void adm_append_debug_scores(VmafFeatureCollector *collector, const AdmStateSycl *s,
                                    const AdmScores &scores, unsigned index)
{
    vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, "integer_adm",
                                            adm_total_score(scores), index);
    vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, "integer_adm_num",
                                            scores.total_numerator, index);
    vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, "integer_adm_den",
                                            scores.total_denominator, index);
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        char name[64];
        (void)std::snprintf(name, sizeof(name), "integer_adm_num_scale%d", scale);
        vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, name,
                                                scores.numerator[scale], index);
        (void)std::snprintf(name, sizeof(name), "integer_adm_den_scale%d", scale);
        vmaf_feature_collector_append_with_dict(collector, s->feature_name_dict, name,
                                                scores.denominator[scale], index);
    }
}

static int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<AdmStateSycl *>(fex->priv);
    vmaf_sycl_graph_wait(fex->sycl_state);
    int64_t cm_results[ADM_NUM_SCALES][ADM_NUM_BANDS];
    int64_t csf_results[ADM_NUM_SCALES][ADM_NUM_BANDS];
    std::memcpy(cm_results, s->h_cm_accum, sizeof(cm_results));
    std::memcpy(csf_results, s->h_csf_den_accum, sizeof(csf_results));
    AdmScores const scores = adm_compute_scores(s, cm_results, csf_results);
    int const err = vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                            "VMAF_integer_feature_adm2_score",
                                                            adm_total_score(scores), index);
    if (err)
        return err;
    adm_append_scale_scores(feature_collector, s, scores, index);
    if (s->debug)
        adm_append_debug_scores(feature_collector, s, scores, index);
    s->has_pending = false;
    return 0;
}

static int extract_fex_sycl(VmafFeatureExtractor *fex, const VmafPicture *ref_pic,
                            const VmafPicture *ref_pic_90, const VmafPicture *dist_pic,
                            const VmafPicture *dist_pic_90, unsigned index,
                            VmafFeatureCollector *feature_collector)
{
    int const err = submit_fex_sycl(fex, ref_pic, ref_pic_90, dist_pic, dist_pic_90, index);
    if (err)
        return err;
    return collect_fex_sycl(fex, index, feature_collector);
}

static int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
{
    (void)feature_collector;
    if (!fex)
        return -EINVAL;
    VmafSyclState *state = fex->sycl_state;
    if (state) {
        int const wait_err = vmaf_sycl_queue_wait(state);
        if (wait_err)
            return wait_err;
    }
    return 1; // done — collect already consumed pending work
}

static int close_fex_sycl(VmafFeatureExtractor *fex)
{
    auto *s = static_cast<AdmStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;

    if (state) {
        (void)vmaf_sycl_queue_wait(state);

        /* Unregister from the combined command graph before freeing priv.
         * Mirrors the fix in integer_motion_sycl.cpp (ADR-0989):
         * vmaf_sycl_graph_unregister() drains combined_queue and removes
         * this extractor's entry so a subsequent VmafContext sharing the
         * same sycl_state does not inherit a dangling priv pointer. */
        (void)vmaf_sycl_graph_unregister(state, s);

        if (s->d_dwt_tmp_ref)
            vmaf_sycl_free(state, s->d_dwt_tmp_ref);
        if (s->d_dwt_tmp_dis)
            vmaf_sycl_free(state, s->d_dwt_tmp_dis);

        for (int i = 0; i < 4; i++) {
            if (s->d_ref_band[i])
                vmaf_sycl_free(state, s->d_ref_band[i]);
            if (s->d_dis_band[i])
                vmaf_sycl_free(state, s->d_dis_band[i]);
        }
        for (auto *csf_buf : s->d_csf_f) {
            if (csf_buf)
                vmaf_sycl_free(state, csf_buf);
        }

        if (s->d_div_lookup)
            vmaf_sycl_free(state, s->d_div_lookup);
        if (s->d_cm_accum)
            vmaf_sycl_free(state, s->d_cm_accum);
        if (s->d_csf_den_accum)
            vmaf_sycl_free(state, s->d_csf_den_accum);
        if (s->h_cm_accum)
            vmaf_sycl_free(state, s->h_cm_accum);
        if (s->h_csf_den_accum)
            vmaf_sycl_free(state, s->h_csf_den_accum);
    }

    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Feature extractor definition                                        */
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
                                          nullptr};

extern "C" VmafFeatureExtractor vmaf_fex_integer_adm_sycl = {
    .name = "adm_sycl",
    .init = init_fex_sycl,
    .extract = extract_fex_sycl,
    .flush = flush_fex_sycl,
    .close = close_fex_sycl,
    .submit = submit_fex_sycl,
    .collect = collect_fex_sycl,
    .options = options,
    .priv_size = sizeof(AdmStateSycl),
    .flags = VMAF_FEATURE_EXTRACTOR_SYCL,
    .provided_features = provided_features,
};
