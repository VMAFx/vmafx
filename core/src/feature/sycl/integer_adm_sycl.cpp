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
 *   3. Decouple + CSF pass -> csf_f (|csf(t - r)| / 30) and, for AIM,
 *      csf_f_aim (|csf(r)| / 30)
 *   4. One row-parallel reduction: CSF denominator, DLM contrast measure
 *      and AIM contrast measure (ADR-1362) -> accum
 *
 * Pattern: init -> submit (non-blocking) -> collect (wait + scores). The
 * whole frame is one combined-graph replay; the accumulators come back in
 * one device-to-host copy.
 */

#include <utility>

#include <sycl/sycl.hpp>

#include "sycl_compat.h"
#include "sycl_tile_index.h"

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "config.h"
#include "feature/adm_angle_flag.h"
#include "feature/adm_cm_accumulator.h"
#include "feature/adm_csf_fixed_point.h"
#include "feature/adm_score.h"
#include "feature/barten_csf_tools.h"
#include "feature/integer_adm.h"
#include "feature/nonfinite_score.h"
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

namespace
{

constexpr int ADM_NUM_SCALES = 4;
constexpr int ADM_NUM_BANDS = 3; // h, v, d (skip band_a for scoring)
constexpr double ADM_BORDER_FACTOR = 0.1;

/* Accumulator terms, in slot order: [term * ADM_TERM_SLOTS + scale * 3 + band]. */
constexpr int ADM_TERM_CM = 0;  // DLM contrast measure (CPU adm_cm(measure_aim=false))
constexpr int ADM_TERM_DEN = 1; // CSF denominator (CPU adm_csf_den_scale / _s123)
constexpr int ADM_TERM_AIM = 2; // AIM contrast measure (CPU adm_cm(measure_aim=true))
constexpr int ADM_NUM_TERMS = 3;
constexpr int ADM_TERM_SLOTS = ADM_NUM_SCALES * ADM_NUM_BANDS;
constexpr int ADM_ACCUM_SLOTS = ADM_NUM_TERMS * ADM_TERM_SLOTS;

/* Slot of band 0 of `scale` in accumulator term `term`. */
constexpr size_t adm_accum_slot(int term, int scale)
{
    return ((size_t)term * ADM_TERM_SLOTS) + ((size_t)scale * ADM_NUM_BANDS);
}

// DWT filter coefficients (DB2, 4-tap)
constexpr int32_t dwt_lo[4] = {15826, 27411, 7345, -4240};
constexpr int32_t dwt_hi[4] = {-4240, -7345, 27411, -15826};
constexpr int32_t dwt_lo_sum = 46342;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

constexpr int32_t ONE_BY_15 = 8738;
constexpr int32_t I4_ONE_BY_15 = 286331153;

/*
 * Rounding term of the scales 1-3 filter shifts (>> 32): csf_f and the 1/15
 * centre tap of the masking threshold. The CPU stores 1u << 31 in an int32_t
 * (i4_adm_round_terms() in integer_adm.c), which wraps to INT32_MIN, so it
 * subtracts 2^31 where it means to add it. The Netflix golden values encode
 * that (Netflix#955, ADR-0155), and the CUDA and HIP twins reproduce it.
 * Adding +2^31 here left every term one higher than the CPU's, and scales
 * 1-3 up to 1e-6 off the scalar CPU on noise. If Netflix#955 is ever fixed
 * upstream, this constant follows the CPU.
 */
constexpr int64_t I4_FLT_ROUND = -(int64_t{1} << 31);

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
    bool adm_skip_aim;    /* skip the AIM contrast measure (aim = 0), as on the CPU */
    double adm_dlm_weight;
    double adm_p_norm;

    VmafDictionary *feature_name_dict;

    // rfactors: 3 bands x 4 scales = 12
    float rfactor[12];
    uint32_t i_rfactor[12];
    uint32_t csf_normalization_shift[4];

    // DWT intermediate buffers
    int32_t *d_dwt_tmp_ref; // vertical DWT output for ref
    int32_t *d_dwt_tmp_dis; // vertical DWT output for dis

    // DWT band outputs: 4 bands x 2 (ref+dis)
    int32_t *d_ref_band[4]; // [0]=a(LL), [1]=h(HL), [2]=v(LH), [3]=d(HH)
    int32_t *d_dis_band[4];

    // CSF outputs kept for the 3x3 neighbourhood of the masking threshold.
    // r, t - r and both CSF-weighted values are recomputed in the CM kernel.
    int32_t *d_csf_f[3];     // DLM: |csf(t - r)| / 30 (CPU csf_f)
    int32_t *d_csf_f_aim[3]; // AIM: |csf(r)| / 30 (CPU csf_a after adm_csf(measure_aim))

    // Integer division LUT
    int32_t *d_div_lookup; // 65537 entries

    // Accumulators, device + host: [term][scale][band] with the terms
    // DLM contrast measure, CSF denominator, AIM contrast measure
    // (ADM_ACCUM_SLOTS int64, one memset and one readback per frame).
    int64_t *d_accum;
    int64_t *h_accum;

    // Deferred state
    unsigned pending_index;
    bool has_pending;
};

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

const VmafOption options[] = {
    {
        .name = "adm_csf_scale",
        .help = "scale coefficient for the horizontal & vertical direction terms of CSF",
        .alias = "scf",
        .offset = offsetof(AdmStateSycl, adm_csf_scale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = DEFAULT_ADM_CSF_SCALE},
        .min = 0.0,
        .max = 50.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_csf_diag_scale",
        .help = "scale coefficient for the diagonal direction term of CSF",
        .alias = "scfd",
        .offset = offsetof(AdmStateSycl, adm_csf_diag_scale),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = DEFAULT_ADM_CSF_DIAG_SCALE},
        .min = 0.0,
        .max = 50.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        /* Weights DLM against AIM in VMAF_integer_feature_adm3_score; like on
         * the CPU it has no arithmetic effect on adm2. */
        .name = "adm_dlm_weight",
        .help = "linear weighting between DLM and AIM; 1 corresponds to DLM-only",
        .alias = "dlmw",
        .offset = offsetof(AdmStateSycl, adm_dlm_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 0.5},
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
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
    },
    {
        .name = "adm_norm_view_dist",
        .help = "normalized viewing distance = viewing distance / ref display's physical height",
        .alias = "nvd",
        .offset = offsetof(AdmStateSycl, adm_norm_view_dist),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = DEFAULT_ADM_NORM_VIEW_DIST},
        .min = 0.75,
        .max = 24.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_ref_display_height",
        .help = "reference display height in pixels",
        .alias = "rdh",
        .offset = offsetof(AdmStateSycl, adm_ref_display_height),
        .type = VMAF_OPT_TYPE_INT,
        .default_val = {.i = DEFAULT_ADM_REF_DISPLAY_HEIGHT},
        .min = 1,
        .max = 4320,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_csf_mode",
        .help = "contrast sensitivity function",
        .alias = "csf",
        .offset = offsetof(AdmStateSycl, adm_csf_mode),
        .type = VMAF_OPT_TYPE_INT,
        .default_val = {.i = DEFAULT_ADM_CSF_MODE},
        .min = 0,
        .max = 3,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_noise_weight",
        .help = "noise weight",
        .alias = "nw",
        .offset = offsetof(AdmStateSycl, adm_noise_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = DEFAULT_ADM_NOISE_WEIGHT},
        .min = 0.0,
        .max = 1500.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_skip_aim",
        .help = "skip the calculation of AIM",
        .offset = offsetof(AdmStateSycl, adm_skip_aim),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {
        .name = "adm_skip_scale0",
        .help = "skip the calculation of scale 0",
        .alias = "ssz",
        .offset = offsetof(AdmStateSycl, adm_skip_scale0),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_min_val",
        .help = "minimum value allowed; lower values will be clipped to this value",
        .alias = "min",
        .offset = offsetof(AdmStateSycl, adm_min_val),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = DEFAULT_ADM_MIN_VAL},
        .min = 0.0,
        .max = 1.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_p_norm",
        .help = "p-norm exponent for fixed-point ADM contrast-measure finalisation",
        .alias = "apn",
        .offset = offsetof(AdmStateSycl, adm_p_norm),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val = {.d = 3.0},
        .min = 1.0,
        .max = 20.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(AdmStateSycl, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val = {.b = false},
    },
    {.name = nullptr}};

/* ------------------------------------------------------------------ */
/* CSF and visibility threshold helpers                                */
/* ------------------------------------------------------------------ */

inline float dwt_quant_step(const struct dwt_model_params *params, int lambda, int theta,
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

AdmCsfFactors adm_csf_factors(int scale, double adm_norm_view_dist, int adm_ref_display_height,
                              int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale)
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

/**
 * Validate a CSF configuration before claiming device resources. Mirrors
 * `adm_csf_config_check()` in core/src/feature/integer_adm.c so the CPU and
 * this twin reject the same invalid table output. Finite over-range weights
 * are assigned the shared per-scale normalisation exponent later.
 */
int adm_csf_config_check(const AdmStateSycl *s)
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

inline int dev_mirror_adm(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * sup - idx - 1;
    return idx;
}

/*
 * Scale 0 of the CPU pipeline stores its intermediate bands as int16_t
 * (adm_dwt_band_t in core/src/feature/integer_adm.h), and so does the CUDA
 * twin. A value that outgrows 16 bits wraps there, while this twin computes
 * in int32 / int64 and used to keep it. Full-range noise reaches the wrap:
 * integer_adm_scale0 was 2.1e-4 off the scalar CPU at 576x324
 * (T-SYCL-ADM-INT16-SEMANTICS-2026-09-18). adm_i16() is the int16_t store,
 * reduced modulo 2^16 explicitly so it does not depend on the compiler's
 * conversion rule.
 *
 * Where the CPU narrows, and where this twin now does too:
 *   - csf_a, adm_csf() (integer_adm.c:743-746): adm_s0_csf_a();
 *   - csf_f, adm_csf() (integer_adm.c:747-748): launch_decouple_csf();
 *   - the 1/15 centre tap, adm_cm_thresh() (integer_adm.c:1028):
 *     launch_csf_den_cm_3band(). This is the one 8-bit content reaches
 *     with the default weights: |csf_a| >= 15360 on the h and v bands.
 * csf_a and csf_f wrap only with h / v weights above ~44000 (the default is
 * 36453). The contrast measure itself is int32_t on the CPU, and
 * launch_csf_den_cm_3band() evaluates it modulo 2^32 too. Stores that
 * cannot overflow need no narrowing: the DWT row buffers and bands stay
 * within +-27.4k, |r| <= |o|, and a = t - r lies between 0 and t.
 */
inline int32_t adm_i16(int32_t v)
{
    return static_cast<int16_t>(static_cast<uint16_t>(v));
}

/*
 * Scale-0 csf_a of one band, as the CPU's adm_csf() computes it: i_shifts =
 * {15, 15, 17} and i_shiftsadd = {16384, 16384, 65535}. The diagonal band
 * rounds with 65535, not 1 << 16; the two differ only for diagonal weights
 * divisible by 4, which the default 49417 is not. The products fit in int32
 * because adm_csf_config_check() bounds i_rfactor below 2^16.
 */
inline int32_t adm_s0_csf_a(uint32_t i_rfactor, int32_t a_val, int band)
{
    int const shift = (band < 2) ? 15 : 17;
    int64_t const rnd = (band < 2) ? 16384 : 65535;
    return adm_i16(static_cast<int32_t>(((int64_t)i_rfactor * a_val + rnd) >> shift));
}

/* ------------------------------------------------------------------ */
/* SYCL Kernel: DWT Vertical Pass (ref+dis fused)                     */
/* ------------------------------------------------------------------ */

/* Output row n reads input rows 2n-1 .. 2n+2, inside [-1, h + 1], which one
 * reflection covers. The SLM tile also holds the rows of padding work-items,
 * up to 2 * n_start + 16: on a plane of 8 rows or fewer one reflection leaves
 * those below zero (row 16 of an 8-row plane mirrors to -1, of a 3-row plane
 * to -11), so every frame 64 rows high or less read before the band's
 * allocation at scale 3 and lost the device. read_px clamps the reflected row
 * with vmaf_sycl_tile_index() (sycl_tile_index.h). */
// NOLINTNEXTLINE(readability-function-size): SYCL kernel-launch / lifecycle entry — body is dominated by accessor declarations + a single `parallel_for` lambda. Splitting either inlines via macro (no readability win) or introduces a free function the compiler cannot inline back into the device kernel. Keeping it large is the pattern shared across every SYCL TU in this fork (ADR-0141 §2 load-bearing invariant; T7-5 sweep closeout — ADR-0278).
sycl::event launch_dwt_vert_pair(sycl::queue &q, const void *input_ref, int32_t *dwt_tmp_ref,
                                 const void *input_dis, int32_t *dwt_tmp_dis, int scale,
                                 unsigned width, unsigned height, unsigned in_stride, unsigned bpc,
                                 unsigned v_shift, unsigned v_add)
{
    // Output: dwt_tmp has interleaved lo/hi rows:
    //   row i contains lo(i) and hi(i) for half_h rows
    unsigned half_h = (height + 1) / 2;
    auto e_scale = scale;
    auto e_bpc = bpc;
    auto e_w = width;
    auto e_h = height;
    auto e_in_stride = in_stride;
    auto e_v_shift = v_shift;
    auto e_v_add = v_add;
    auto p_ref_in = input_ref;
    auto p_dis_in = input_dis;

    constexpr int WG_X = 32;
    constexpr int WG_Y = 8;
    // Each output row n needs input rows 2n-1..2n+2 (4 taps).
    // For WG_Y outputs: 2*WG_Y + 2 input rows in the tile.
    constexpr int TILE_H = 2 * WG_Y + 2; // 18
    // Z=2: ref(0) + dis(1)
    sycl::range<3> global(2, ((size_t)(half_h + WG_Y - 1) / WG_Y) * WG_Y,
                          ((size_t)(width + WG_X - 1) / WG_X) * WG_X);
    sycl::range<3> local(1, WG_Y, WG_X);

    return q.submit([&](sycl::handler &cgh) {
        // SLM tile: TILE_H rows × WG_X columns
        sycl::local_accessor<int32_t, 2> const tile(sycl::range<2>(TILE_H, WG_X), cgh);

        cgh.parallel_for(sycl::nd_range<3>(global, local), [=](sycl::nd_item<3> item) {
            // dim 0 = ref(0) or dis(1), dim 1 = row, dim 2 = col
            const int is_dis = item.get_global_id(0);
            const int gx = item.get_global_id(2);
            const int gy = item.get_global_id(1);
            const int lx = item.get_local_id(2);
            const int ly = item.get_local_id(1);
            const int lid = ly * WG_X + lx;
            constexpr int WG_SIZE = WG_X * WG_Y;

            const void *p_in = (is_dis == 0) ? p_ref_in : p_dis_in;
            int32_t *dwt_out = (is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;

            // Tile origin in input space
            int const tile_col = (int)(item.get_group(2) * WG_X);
            int const n_start = (int)(item.get_group(1) * WG_Y);
            int const row_start = 2 * n_start - 1; // first input row

            // Read pixel from source at (x, y) with mirroring (clamped, see above)
            auto read_px = [&](int x, int y) -> int32_t {
                y = vmaf_sycl_tile_index(dev_mirror_adm(y, (int)e_h), (int)e_h);
                if (std::cmp_greater_equal(x, e_w))
                    return 0;
                if (e_scale == 0) {
                    if (e_bpc <= 8) {
                        return static_cast<const uint8_t *>(p_in)[y * e_in_stride + x];
                    } else {
                        return static_cast<const uint16_t *>(p_in)[y * (e_in_stride / 2) + x];
                    }
                } else {
                    return static_cast<const int32_t *>(p_in)[y * e_in_stride + x];
                }
            };

            // Cooperative tile load: TILE_H * WG_X elements
            constexpr int TILE_ELEMS = TILE_H * WG_X;
            bool const interior = (row_start >= 0) && (row_start + TILE_H <= (int)e_h) &&
                                  (tile_col + WG_X <= (int)e_w);

            if (interior) {
                // Fast path: no boundary checks
                for (int i = lid; i < TILE_ELEMS; i += WG_SIZE) {
                    int const tr = i / WG_X;
                    int const tc = i % WG_X;
                    int const x = tile_col + tc;
                    int const y = row_start + tr;
                    if (e_scale == 0) {
                        if (e_bpc <= 8) {
                            tile[tr][tc] = static_cast<const uint8_t *>(p_in)[y * e_in_stride + x];
                        } else {
                            tile[tr][tc] =
                                static_cast<const uint16_t *>(p_in)[y * (e_in_stride / 2) + x];
                        }
                    } else {
                        tile[tr][tc] = static_cast<const int32_t *>(p_in)[y * e_in_stride + x];
                    }
                }
            } else {
                // Boundary path: mirror + bounds check
                for (int i = lid; i < TILE_ELEMS; i += WG_SIZE) {
                    int const tr = i / WG_X;
                    int const tc = i % WG_X;
                    tile[tr][tc] = read_px(tile_col + tc, row_start + tr);
                }
            }

            item.barrier(sycl::access::fence_space::local_space);

            if (std::cmp_greater_equal(gx, e_w) || std::cmp_greater_equal(gy, half_h))
                return;

            // Read 4 filter taps from shared memory
            int const base = 2 * ly; // tile row offset
            int32_t const s0 = tile[base][lx];
            int32_t const s1 = tile[base + 1][lx];
            int32_t const s2 = tile[base + 2][lx];
            int32_t const s3 = tile[base + 3][lx];

            // Lo-pass: coeffs = {15826, 27411, 7345, -4240}
            int64_t lo_val = (int64_t)dwt_lo[0] * s0 + (int64_t)dwt_lo[1] * s1 +
                             (int64_t)dwt_lo[2] * s2 + (int64_t)dwt_lo[3] * s3;

            // Hi-pass: coeffs = {-4240, -7345, 27411, -15826}
            int64_t const hi_val = (int64_t)dwt_hi[0] * s0 + (int64_t)dwt_hi[1] * s1 +
                                   (int64_t)dwt_hi[2] * s2 + (int64_t)dwt_hi[3] * s3;

            // Scale 0: subtract DC offset from lo
            if (e_scale == 0) {
                lo_val -= (int64_t)dwt_lo_sum * e_v_add;
            }

            // Quantize
            int32_t lo_out;
            int32_t hi_out;
            if (e_v_shift > 0) {
                lo_out = (int32_t)((lo_val + ((int64_t)1 << (e_v_shift - 1))) >> e_v_shift);
                hi_out = (int32_t)((hi_val + ((int64_t)1 << (e_v_shift - 1))) >> e_v_shift);
            } else {
                lo_out = (int32_t)lo_val;
                hi_out = (int32_t)hi_val;
            }

            // Interleaved output: lo followed by hi
            unsigned const out_stride = e_w * 2;
            dwt_out[gy * out_stride + gx] = lo_out;
            dwt_out[gy * out_stride + e_w + gx] = hi_out;
        });
    });
}

/* ------------------------------------------------------------------ */
/* SYCL Kernel: DWT Horizontal Pass (ref+dis fused)                   */
/* ------------------------------------------------------------------ */

// NOLINTNEXTLINE(readability-function-size): SYCL kernel-launch / lifecycle entry — body is dominated by accessor declarations + a single `parallel_for` lambda. Splitting either inlines via macro (no readability win) or introduces a free function the compiler cannot inline back into the device kernel. Keeping it large is the pattern shared across every SYCL TU in this fork (ADR-0141 §2 load-bearing invariant; T7-5 sweep closeout — ADR-0278).
sycl::event launch_dwt_hori_pair(sycl::queue &q, const int32_t *dwt_tmp_ref, int32_t *ref_band_a,
                                 int32_t *ref_band_h, int32_t *ref_band_v, int32_t *ref_band_d,
                                 const int32_t *dwt_tmp_dis, int32_t *dis_band_a,
                                 int32_t *dis_band_h, int32_t *dis_band_v, int32_t *dis_band_d,
                                 unsigned width, unsigned height, unsigned buf_stride,
                                 unsigned h_shift)
{
    unsigned const half_w = (width + 1) / 2;
    unsigned const half_h = (height + 1) / 2;
    auto e_w = width;
    auto e_half_w = half_w;
    auto e_half_h = half_h;
    auto e_buf_stride = buf_stride;
    auto e_h_shift = h_shift;

    constexpr int WG_X = 32;
    constexpr int WG_Y = 8;
    // Z=2: ref(0) + dis(1)
    sycl::range<3> global(2, ((size_t)(half_h + WG_Y - 1) / WG_Y) * WG_Y,
                          ((size_t)(half_w + WG_X - 1) / WG_X) * WG_X);
    sycl::range<3> local(1, WG_Y, WG_X);

    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<3>(global, local), [=](sycl::nd_item<3> item) {
            const int is_dis = item.get_global_id(0);
            const int gx = item.get_global_id(2);
            const int gy = item.get_global_id(1);
            if (std::cmp_greater_equal(gx, e_half_w) || std::cmp_greater_equal(gy, e_half_h))
                return;

            const int32_t *dwt_tmp = (is_dis == 0) ? dwt_tmp_ref : dwt_tmp_dis;
            int32_t *band_a = (is_dis == 0) ? ref_band_a : dis_band_a;
            int32_t *band_h = (is_dis == 0) ? ref_band_h : dis_band_h;
            int32_t *band_v = (is_dis == 0) ? ref_band_v : dis_band_v;
            int32_t *band_d = (is_dis == 0) ? ref_band_d : dis_band_d;

            unsigned tmp_stride = e_w * 2;

            // Read from lo row (first half of tmp)
            auto read_lo = [&](int x) -> int32_t {
                x = dev_mirror_adm(x, (int)e_w);
                return dwt_tmp[gy * tmp_stride + x];
            };

            // Read from hi row (second half of tmp)
            auto read_hi = [&](int x) -> int32_t {
                x = dev_mirror_adm(x, (int)e_w);
                return dwt_tmp[gy * tmp_stride + e_w + x];
            };

            int const base_x = 2 * gx;

            // Horizontal filter on lo row -> band_a, band_h
            int32_t const l0 = read_lo(base_x - 1);
            int32_t const l1 = read_lo(base_x);
            int32_t const l2 = read_lo(base_x + 1);
            int32_t const l3 = read_lo(base_x + 2);

            int64_t const a_val = (int64_t)dwt_lo[0] * l0 + (int64_t)dwt_lo[1] * l1 +
                                  (int64_t)dwt_lo[2] * l2 + (int64_t)dwt_lo[3] * l3;
            int64_t const h_val = (int64_t)dwt_hi[0] * l0 + (int64_t)dwt_hi[1] * l1 +
                                  (int64_t)dwt_hi[2] * l2 + (int64_t)dwt_hi[3] * l3;

            // Horizontal filter on hi row -> band_v, band_d
            int32_t const h0 = read_hi(base_x - 1);
            int32_t const h1 = read_hi(base_x);
            int32_t const h2 = read_hi(base_x + 1);
            int32_t const h3 = read_hi(base_x + 2);

            int64_t const v_val = (int64_t)dwt_lo[0] * h0 + (int64_t)dwt_lo[1] * h1 +
                                  (int64_t)dwt_lo[2] * h2 + (int64_t)dwt_lo[3] * h3;
            int64_t const d_val = (int64_t)dwt_hi[0] * h0 + (int64_t)dwt_hi[1] * h1 +
                                  (int64_t)dwt_hi[2] * h2 + (int64_t)dwt_hi[3] * h3;

            // Quantize
            int32_t a_out;
            int32_t h_out;
            int32_t v_out;
            int32_t d_out;
            if (e_h_shift > 0) {
                int64_t const rnd = (int64_t)1 << (e_h_shift - 1);
                a_out = (int32_t)((a_val + rnd) >> e_h_shift);
                h_out = (int32_t)((h_val + rnd) >> e_h_shift);
                v_out = (int32_t)((v_val + rnd) >> e_h_shift);
                d_out = (int32_t)((d_val + rnd) >> e_h_shift);
            } else {
                a_out = (int32_t)a_val;
                h_out = (int32_t)h_val;
                v_out = (int32_t)v_val;
                d_out = (int32_t)d_val;
            }

            unsigned const idx = gy * e_buf_stride + gx;
            band_a[idx] = a_out;
            band_h[idx] = h_out;
            band_v[idx] = v_out;
            band_d[idx] = d_out;
        });
    });
}

/* ------------------------------------------------------------------ */
/* Decouple, CSF weighting and contrast masking: device helpers        */
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
 *
 * No kernel may use 'double': all kernels of a SYCL translation unit share
 * one SPIR-V module, and one fp64 instruction in it makes the runtime reject
 * the whole module on devices without fp64 (Intel Arc A-series, iGPUs) --
 * even kernels that are never submitted (ADR-0220).
 */
struct GainLimitQ31 {
    int32_t gain_hi; // upper 22 bits of gain_q31
    int32_t gain_lo; // lower 16 bits of gain_q31
};

inline GainLimitQ31 gain_limit_to_q31(double gain_limit)
{
    int64_t const gain_q31 = (int64_t)llround(gain_limit * (1LL << 31));
    return {
        .gain_hi = (int32_t)(gain_q31 >> 16),
        .gain_lo = (int32_t)(gain_q31 & 0xFFFF),
    };
}

/* The CPU's get_best15_from32() (integer_adm.c): |o| >= 2^15 at scales 1-3
 * rounded to its top 15 bits, and the shift that took it there. */
struct AdmBest15 {
    uint32_t value;
    int shift;
};

/*
 * There is no __builtin_clz on the SYCL device, so the MSB index is found by
 * a binary scan. The caller guarantees tmp >= 2^15, so the index is at least
 * 15 and the scan is seeded with that floor; that makes the shift provably
 * positive to the static analyser (clang-analyzer-core.BitwiseShift is
 * error-level under WarningsAsErrors). The analyser sums all five conditional
 * increments without the branch conditions, so it believes n can reach 46;
 * the clamp states the real bound [15, 31] and is a no-op for every uint32_t
 * input (verified exhaustively over the low boundary and 400k random guarded
 * inputs). Bit for bit the CPU and the CUDA / HIP twins
 * (T-SYCL-ADM-NEGATIVE-SHIFT-REACHABILITY-2026-09-04).
 */
inline AdmBest15 adm_dev_best15(uint32_t tmp)
{
    int n = 15;
    uint32_t v = (tmp >> 15) & 0x1FFFFu; // no-op on uint32_t input; states v < 2^17
    if (v >= (1u << 16)) {
        n += 16;
        v >>= 16;
    }
    if (v >= (1u << 8)) {
        n += 8;
        v >>= 8;
    }
    if (v >= (1u << 4)) {
        n += 4;
        v >>= 4;
    }
    if (v >= (1u << 2)) {
        n += 2;
        v >>= 2;
    }
    if (v >= (1u << 1)) {
        n += 1;
    }
    n = n > 31 ? 31 : n;
    int const ks = n - 14; // == 17 - (31 - n), in [1, 17]
    return {.value = (tmp + (1u << (ks - 1))) >> ks, .shift = ks};
}

/*
 * Q15 ratio k = clamp(t / o, 0, 1) of one band sample with o != 0, the
 * division carried out with the reciprocal table: CPU adm_decouple_band() at
 * scale 0 (int16 bands, so |o| <= 2^15 indexes the table directly) and
 * adm_decouple_band_s123() at scales 1-3. The quotient is clamped in 64 bits
 * like the CPU's tmp_k: at scales 1-3 it passes INT32_MAX once |t / o| > 2^16,
 * and narrowing it before the clamp, as this twin used to, wrapped it.
 */
inline int32_t adm_dev_decouple_k(int scale, int32_t oh, int32_t th, const int32_t *div_lookup)
{
    int32_t const abs_oh = oh < 0 ? -oh : oh;
    int32_t const sign_oh = oh < 0 ? -1 : 1;
    bool const direct = scale == 0 || abs_oh < 32768;
    AdmBest15 const best = direct ? AdmBest15{.value = (uint32_t)abs_oh, .shift = 0} :
                                    adm_dev_best15((uint32_t)abs_oh);
    int const shift = 15 + best.shift;
    int32_t const div_val = div_lookup[32768u + best.value] * sign_oh;
    int64_t const k = ((int64_t)div_val * th + ((int64_t)1 << (shift - 1))) >> shift;
    return (int32_t)(k < 0 ? 0 : (k > 32768 ? 32768 : k));
}

/* Enhancement gain limit of one decoupled sample whose angle_flag is set.
 * rst_f is the CPU's (k / 32768) * (o / 64); only its sign is used. The
 * limit clamps against the distorted sample t, not the reference. */
inline int32_t adm_dev_gain_limit(int32_t r_val, int32_t k, int32_t oh, int32_t th, GainLimitQ31 g)
{
    // No-contract block (T7-16): the pragma has to head a compound statement.
    float rst_f;
    {
#pragma clang fp contract(off)
        float const a = (float)k / 32768.0f;
        float const b = (float)oh / 64.0f;
        rst_f = a * b;
    }
    // gained = (r_val * gain_q31) >> 31 = (r_val*hi << 16 + r_val*lo) >> 31,
    // kept in int64: at scales 1-3 r_val * 100 exceeds int32.
    int64_t const prod_hi = (int64_t)r_val * g.gain_hi;
    int64_t const prod_lo = (int64_t)r_val * g.gain_lo;
    int64_t const main_part = prod_hi >> 15;
    int64_t const rem = ((prod_hi - (main_part << 15)) << 16) + prod_lo;
    int64_t const gained = main_part + (rem >> 31);
    if (rst_f > 0.0f) {
        return (int32_t)((gained < (int64_t)th) ? gained : (int64_t)th);
    }
    if (rst_f < 0.0f) {
        return (int32_t)((gained > (int64_t)th) ? gained : (int64_t)th);
    }
    return r_val;
}

/* Restored part r of one band sample, the CPU's decouple_r. o == 0 gives
 * k = 32768 and r = 0, which the gain limit leaves alone (rst_f == 0). */
inline int32_t adm_dev_decouple(int scale, int32_t oh, int32_t th, bool angle_flag, GainLimitQ31 g,
                                const int32_t *div_lookup)
{
    if (oh == 0) {
        return 0;
    }
    int32_t const k = adm_dev_decouple_k(scale, oh, th, div_lookup);
    int32_t const r_val = (int32_t)(((int64_t)k * oh + 16384) >> 15);
    return angle_flag ? adm_dev_gain_limit(r_val, k, oh, th, g) : r_val;
}

/* CSF weighting of one decoupled sample, the CPU adm_csf() / i4_adm_csf()
 * dst: v is t - r in the DLM pass and r in the AIM pass. */
inline int32_t adm_dev_csf(int scale, uint32_t i_rfactor, int32_t v, int band)
{
    if (scale == 0) {
        return adm_s0_csf_a(i_rfactor, v, band); // int16 storage, see adm_i16()
    }
    return (int32_t)(((int64_t)i_rfactor * v + (1LL << 27)) >> 28);
}

/* |csf| / 30, one neighbourhood term of the masking threshold: the CPU
 * adm_csf() / i4_adm_csf() flt (int16 at scale 0). */
inline int32_t adm_dev_csf_f(int scale, int32_t csf)
{
    int32_t const abs_csf = csf < 0 ? -csf : csf;
    if (scale == 0) {
        return adm_i16((4369 * abs_csf + 2048) >> 12);
    }
    return (int32_t)(((int64_t)143165577 * abs_csf + I4_FLT_ROUND) >> 32);
}

/* |csf| / 15, the centre term of the masking threshold: CPU adm_cm_thresh()
 * / i4_adm_cm_thresh(). int16 at scale 0, where it wraps negative once
 * |csf| >= 15360 -- the one narrowing 8-bit content reaches with the default
 * weights. */
inline int32_t adm_dev_csf_centre(int scale, int32_t csf)
{
    int32_t const abs_csf = csf < 0 ? -csf : csf;
    if (scale == 0) {
        return adm_i16((ONE_BY_15 * abs_csf + 2048) >> 12);
    }
    return (int32_t)(((int64_t)I4_ONE_BY_15 * abs_csf + I4_FLT_ROUND) >> 32);
}

/* What the decouple of a sample needs; shared by both per-scale kernels. */
struct AdmBandInputs {
    const int32_t *ref[3]; // reference h, v, d bands
    const int32_t *dis[3]; // distorted h, v, d bands
    const int32_t *div_lookup;
    uint32_t i_rfactor[3];
    GainLimitQ31 gain;
    int scale;
    int w; // band width
    int h; // band height
    unsigned stride;
    bool aim; // !adm_skip_aim
};

/* The three bands of one sample after decoupling (CPU adm_decouple /
 * adm_decouple_s123). */
struct AdmSample {
    int32_t o[3]; // reference
    int32_t r[3]; // restored part, CPU decouple_r
    int32_t d[3]; // additive impairment t - r, CPU decouple_a
};

inline AdmSample adm_dev_sample(const AdmBandInputs &in, unsigned idx)
{
    AdmSample s{};
    int32_t t[3];
    for (int b = 0; b < 3; ++b) {
        s.o[b] = in.ref[b][idx];
        t[b] = in.dis[b][idx];
    }
    // Angle between (oh, ov) and (th, tv). ADR-1194: the CPU narrows each
    // operand to float and compares in double; adm_angle_flag_i64() is the
    // bit-identical form without binary64, which this TU must not contain.
    int64_t const ot_dp = (int64_t)s.o[0] * t[0] + (int64_t)s.o[1] * t[1];
    int64_t const o_mag_sq = (int64_t)s.o[0] * s.o[0] + (int64_t)s.o[1] * s.o[1];
    int64_t const t_mag_sq = (int64_t)t[0] * t[0] + (int64_t)t[1] * t[1];
    bool const angle_flag = adm_angle_flag_i64(ot_dp, o_mag_sq, t_mag_sq) != 0;
    for (int b = 0; b < 3; ++b) {
        s.r[b] = adm_dev_decouple(in.scale, s.o[b], t[b], angle_flag, in.gain, in.div_lookup);
        s.d[b] = t[b] - s.r[b];
    }
    return s;
}

/* ------------------------------------------------------------------ */
/* SYCL Kernel: Decouple + CSF                                         */
/* ------------------------------------------------------------------ */

struct AdmDecoupleArgs {
    AdmBandInputs in;
    int32_t *csf_f[3];     // DLM neighbourhood term, |csf(t - r)| / 30
    int32_t *csf_f_aim[3]; // AIM neighbourhood term, |csf(r)| / 30
};

/* One sample. The CPU computes these over the reduction region widened by
 * one tap (adm_border_filt); this pass covers the whole band, a superset, so
 * every neighbour the CM pass reads holds the CPU's value. */
inline void adm_dev_decouple_csf_px(const AdmDecoupleArgs &a, unsigned idx)
{
    AdmBandInputs const &in = a.in;
    AdmSample const s = adm_dev_sample(in, idx);
    for (int b = 0; b < 3; ++b) {
        int32_t const csf_d = adm_dev_csf(in.scale, in.i_rfactor[b], s.d[b], b);
        a.csf_f[b][idx] = adm_dev_csf_f(in.scale, csf_d);
        if (in.aim) {
            int32_t const csf_r = adm_dev_csf(in.scale, in.i_rfactor[b], s.r[b], b);
            a.csf_f_aim[b][idx] = adm_dev_csf_f(in.scale, csf_r);
        }
    }
}

sycl::event launch_decouple_csf(sycl::queue &q, const AdmDecoupleArgs &args)
{
    constexpr size_t WG_X = 16;
    constexpr size_t WG_Y = 16;
    AdmDecoupleArgs const a = args;
    sycl::range<2> const global(((size_t)a.in.h + WG_Y - 1) / WG_Y * WG_Y,
                                ((size_t)a.in.w + WG_X - 1) / WG_X * WG_X);
    sycl::range<2> const local(WG_Y, WG_X);

    return q.submit([&](sycl::handler &cgh) {
        cgh.parallel_for(sycl::nd_range<2>(global, local), [=](sycl::nd_item<2> item) {
            auto const gx = (int)item.get_global_id(1);
            auto const gy = (int)item.get_global_id(0);
            if (gx >= a.in.w || gy >= a.in.h) {
                return;
            }
            adm_dev_decouple_csf_px(a, ((unsigned)gy * a.in.stride) + (unsigned)gx);
        });
    });
}

/* ------------------------------------------------------------------ */
/* SYCL Kernel: CSF denominator + DLM and AIM contrast measures        */
/*                                                                     */
/* One work-group per row of the reduction region computes all three  */
/* bands of all three reductions. r and t - r are recomputed per       */
/* sample instead of read from buffers; only the |csf| / 30 bands are  */
/* stored, because the threshold reads their 3x3 neighbourhood.        */
/* ------------------------------------------------------------------ */

/* Shift budget of one scale's reductions (CPU adm_csf_den_scale /
 * adm_csf_den_s123 and adm_cm_ctx_init / i4_adm_cm_ctx_init). */
struct AdmCmShifts {
    uint32_t den_sq;    // scales 1-3: square, rounded with 1 << den_sq
    uint32_t den_cub;   // scales 1-3: cube
    uint32_t den_accum; // row total of the denominator
    uint32_t cm_inner;  // row total of the contrast measures (shift_inner_accum)
    uint32_t cm_sub[3]; // scale 0: threshold into the band's Q format
    uint32_t cm_xsq[3];
    uint32_t cm_xcub[3];
};

struct AdmCmArgs {
    AdmBandInputs in;
    const int32_t *csf_f[3];
    const int32_t *csf_f_aim[3];
    int64_t *accum; // band 0 of this scale in the DLM term
    AdmCmShifts sh;
    int left;
    int top;
    int right;
};

/* Sub-group size 16: each work-item keeps nine int64 sums live across the
 * column loop, and at 32 lanes the UHD 770 (Xe-LP) spills -- adm_sycl alone at
 * 3840x2160 took 59.5 ms per frame at 32 and 45.6 at 16. The Arc B580 (Xe2,
 * native SIMD16) shows no difference. The sums are integer, so the sub-group
 * size cannot change a score. */
constexpr int ADM_CM_WG = 256;    // work-items per row
constexpr int ADM_CM_SG = 16;     // sub-group size
constexpr int ADM_CM_MAX_SG = 32; // sub-groups per work-group, upper bound
constexpr int ADM_CM_SUMS = ADM_NUM_TERMS * ADM_NUM_BANDS;
static_assert(ADM_CM_WG / ADM_CM_SG <= ADM_CM_MAX_SG, "reduction slots");

/* The eight 3x3 neighbours of (row, col) in one |csf| / 30 band.
 * ADR-1210: the CPU rule is asymmetric -- the near edge MIRRORS to index 1,
 * the far edge CLAMPS to the last index (adm_cm_thresh() in integer_adm.c).
 * The two readings only differ once a scale's border crop collapses to 0
 * (band dimensions <= 14), the one case with row 0 / column 0 inside the
 * region. */
inline int64_t adm_dev_neighbours(const int32_t *flt, int row, int col, const AdmBandInputs &in)
{
    int const rows[3] = {(row == 0) ? 1 : row - 1, row, (row == in.h - 1) ? in.h - 1 : row + 1};
    auto const col_m1 = (unsigned)((col == 0) ? 1 : col - 1);
    auto const col_p1 = (unsigned)((col == in.w - 1) ? in.w - 1 : col + 1);
    int64_t sum = 0;
    for (int i = 0; i < 3; ++i) {
        unsigned const line = (unsigned)rows[i] * in.stride;
        sum += flt[line + col_m1];
        sum += flt[line + col_p1];
        if (i != 1) {
            sum += flt[line + (unsigned)col];
        }
    }
    return sum;
}

/* Masking threshold of one sample, the CPU's 27-term sum over h, v and d:
 * the neighbourhood of `flt` plus the 1/15 centre of csf(centre_src). DLM
 * passes csf_f and t - r, AIM csf_f_aim and r. */
inline int64_t adm_dev_threshold(const AdmBandInputs &in, const int32_t *const flt[3],
                                 const int32_t centre_src[3], int row, int col)
{
    int64_t thr = 0;
    for (int b = 0; b < 3; ++b) {
        thr += adm_dev_neighbours(flt[b], row, col, in);
        thr +=
            adm_dev_csf_centre(in.scale, adm_dev_csf(in.scale, in.i_rfactor[b], centre_src[b], b));
    }
    return thr;
}

/* |x| - thr at scale 0, x = v * rfactor. The CPU subtracts the shifted
 * threshold in int32_t, where thr << 12 can wrap on the diagonal band; so
 * does this, modulo 2^32. */
inline int64_t adm_dev_cm_excess_s0(uint32_t i_rfactor, int32_t v, int64_t thr, uint32_t shift_sub)
{
    int64_t const x = (int64_t)i_rfactor * v;
    auto const abs_x = static_cast<uint32_t>(x < 0 ? -x : x);
    return static_cast<int32_t>(abs_x - (static_cast<uint32_t>(thr) << shift_sub));
}

/* |x| - thr at scales 1-3, x the CSF-weighted sample (i4_adm_cm_scale). */
inline int64_t adm_dev_cm_excess_s123(uint32_t i_rfactor, int32_t v, int64_t thr)
{
    int64_t const scaled = ((int64_t)i_rfactor * v + (1LL << 27)) >> 28;
    return (scaled < 0 ? -scaled : scaled) - thr;
}

/* Rounded max(|x| - thr, 0)^3 of one band sample: CPU adm_cm_accum_round()
 * at scale 0, i4_adm_cm_accum_round() at scales 1-3. v is the sample the
 * pass measures: r for DLM, t - r for AIM. */
inline int64_t adm_dev_cm_cube(const AdmCmArgs &a, int band, int32_t v, int64_t thr)
{
    uint32_t const i_rfactor = a.in.i_rfactor[band];
    int64_t const excess = (a.in.scale == 0) ?
                               adm_dev_cm_excess_s0(i_rfactor, v, thr, a.sh.cm_sub[band]) :
                               adm_dev_cm_excess_s123(i_rfactor, v, thr);
    int64_t const cm = excess < 0 ? 0 : excess;
    uint32_t const xsq = a.sh.cm_xsq[band];
    uint32_t const xcub = a.sh.cm_xcub[band];
    int64_t const rnd_sq = xsq > 0 ? ((int64_t)1 << (xsq - 1)) : 0;
    auto const x_sq = (int32_t)(((cm * cm) + rnd_sq) >> xsq);
    int64_t const rnd_cub = xcub > 0 ? ((int64_t)1 << (xcub - 1)) : 0;
    return (((int64_t)x_sq * cm) + rnd_cub) >> xcub;
}

/* |o|^3 of one reference sample, the CSF-denominator term: exact at scale 0
 * (adm_csf_den_scale), rounded twice at scales 1-3 (i4_cube_term, whose
 * square rounds with 1 << den_sq rather than half of it). */
inline int64_t adm_dev_den_cube(const AdmCmArgs &a, int32_t o)
{
    int64_t const abs_o = o < 0 ? -(int64_t)o : (int64_t)o;
    if (a.in.scale == 0) {
        return abs_o * abs_o * abs_o;
    }
    int64_t const o_sq = ((abs_o * abs_o) + ((int64_t)1 << a.sh.den_sq)) >> a.sh.den_sq;
    int64_t const rnd_cub = a.sh.den_cub > 0 ? ((int64_t)1 << (a.sh.den_cub - 1)) : 0;
    return ((o_sq * abs_o) + rnd_cub) >> a.sh.den_cub;
}

/* All reductions of one sample. DLM: threshold from csf(t - r), measure r.
 * AIM (the CPU's measure_aim): the roles swap -- threshold from csf(r),
 * measure t - r -- and the pass adds no noise floor (host side). */
inline void adm_dev_cm_px(const AdmCmArgs &a, int row, int col, int64_t sums[ADM_CM_SUMS])
{
    AdmBandInputs const &in = a.in;
    AdmSample const s = adm_dev_sample(in, ((unsigned)row * in.stride) + (unsigned)col);
    int64_t const thr = adm_dev_threshold(in, a.csf_f, s.d, row, col);
    for (int b = 0; b < 3; ++b) {
        sums[(ADM_TERM_CM * ADM_NUM_BANDS) + b] += adm_dev_cm_cube(a, b, s.r[b], thr);
        sums[(ADM_TERM_DEN * ADM_NUM_BANDS) + b] += adm_dev_den_cube(a, s.o[b]);
    }
    if (!in.aim) {
        return;
    }
    int64_t const thr_aim = adm_dev_threshold(in, a.csf_f_aim, s.r, row, col);
    for (int b = 0; b < 3; ++b) {
        sums[(ADM_TERM_AIM * ADM_NUM_BANDS) + b] += adm_dev_cm_cube(a, b, s.d[b], thr_aim);
    }
}

/* Fold one complete row total into the frame accumulator of sum k. ADR-1167:
 * the rounding shift is applied once per row, after every column of the row
 * has been summed -- never to a work-item or sub-group partial. */
inline void adm_dev_fold_row(const AdmCmArgs &a, int k, int64_t row_total)
{
    int const term = k / ADM_NUM_BANDS;
    uint32_t const shift = (term == ADM_TERM_DEN) ? a.sh.den_accum : a.sh.cm_inner;
    int64_t const rounding = shift > 0 ? ((int64_t)1 << (shift - 1)) : 0;
    int64_t const shifted = adm_cm_round_row_total(row_total, rounding, shift);
    sycl::atomic_ref<int64_t, sycl::memory_order::relaxed, sycl::memory_scope::device,
                     sycl::access::address_space::global_space> const
        slot(a.accum[(term * ADM_TERM_SLOTS) + (k % ADM_NUM_BANDS)]);
    slot.fetch_add(shifted);
}

/* Work-group sum of every per-item total, then one fold per sum. */
inline void adm_dev_reduce_row(sycl::nd_item<1> item, const sycl::local_accessor<int64_t, 1> &lmem,
                               const AdmCmArgs &a, const int64_t sums[ADM_CM_SUMS])
{
    sycl::sub_group const sg = item.get_sub_group();
    uint32_t const sg_id = sg.get_group_linear_id();
    bool const sg_leader = sg.get_local_linear_id() == 0;
    for (int k = 0; k < ADM_CM_SUMS; ++k) {
        int64_t const part = sycl::reduce_over_group(sg, sums[k], sycl::plus<int64_t>{});
        if (sg_leader) {
            lmem[((size_t)k * ADM_CM_MAX_SG) + sg_id] = part;
        }
    }
    item.barrier(sycl::access::fence_space::local_space);
    if (item.get_local_id(0) != 0) {
        return;
    }
    uint32_t const n_sg = sg.get_group_linear_range();
    for (int k = 0; k < ADM_CM_SUMS; ++k) {
        int64_t row_total = 0;
        for (uint32_t i = 0; i < n_sg; ++i) {
            row_total += lmem[((size_t)k * ADM_CM_MAX_SG) + i];
        }
        adm_dev_fold_row(a, k, row_total);
    }
}

sycl::event launch_csf_den_cm(sycl::queue &q, const AdmCmArgs &args, int num_rows)
{
    AdmCmArgs const a = args;
    return q.submit([&](sycl::handler &cgh) {
        sycl::local_accessor<int64_t, 1> const lmem(
            sycl::range<1>((size_t)ADM_CM_SUMS * ADM_CM_MAX_SG), cgh);
        cgh.parallel_for(sycl::nd_range<1>((size_t)num_rows * ADM_CM_WG, ADM_CM_WG),
                         [=](sycl::nd_item<1> item) VMAF_SYCL_REQD_SG_SIZE(ADM_CM_SG) {
                             int const row = a.top + (int)item.get_group(0);
                             int64_t sums[ADM_CM_SUMS] = {};
                             for (int col = a.left + (int)item.get_local_id(0); col < a.right;
                                  col += ADM_CM_WG) {
                                 adm_dev_cm_px(a, row, col, sums);
                             }
                             adm_dev_reduce_row(item, lmem, a, sums);
                         });
    });
}

/* ------------------------------------------------------------------ */
/* Host-side launch parameters                                         */
/* ------------------------------------------------------------------ */

/* The CPU's reduction region, adm_border() in integer_adm.c. */
struct AdmRegion {
    int left;
    int top;
    int right;
    int bottom;
};

AdmRegion adm_region(int w, int h)
{
    AdmRegion r{};
    r.left = (int)(w * ADM_BORDER_FACTOR - 0.5);
    r.top = (int)(h * ADM_BORDER_FACTOR - 0.5);
    r.right = w - r.left;
    r.bottom = h - r.top;
    return r;
}

AdmCmShifts adm_cm_shifts(int scale, int w, int h, const AdmRegion &r)
{
    assert(scale >= 0 && scale < ADM_NUM_SCALES);
    // The caller skips empty regions; every log2 below needs a positive span.
    assert(r.right > r.left && r.bottom > r.top);
    AdmCmShifts sh{};
    int const active_w = r.right - r.left;
    int const active_h = r.bottom - r.top;
    if (scale == 0) {
        int const s = (int)std::ceil(std::log2(active_w * active_h) - 20);
        sh.den_accum = s > 0 ? (uint32_t)s : 0;
    } else {
        sh.den_sq = (scale == 2) ? 30 : 31;
        sh.den_cub = (uint32_t)std::ceil(std::log2(active_w));
        sh.den_accum = (uint32_t)std::ceil(std::log2(active_h));
    }
    sh.cm_inner = (uint32_t)std::ceil(std::log2((double)h));
    auto const log2_w = (uint32_t)std::ceil(std::log2((double)w));
    // adm_frame_size_check() keeps scale-0 bands >= 9 wide, so the scale-0
    // cube shift log2_w - 4 cannot wrap.
    assert(scale != 0 || log2_w >= 4u);
    for (int b = 0; b < 3; b++) {
        bool const hv = b < 2;
        sh.cm_sub[b] = (scale == 0) ? (hv ? 10 : 12) : 15;
        sh.cm_xsq[b] = (scale == 0 && hv) ? 29 : 30;
        sh.cm_xcub[b] = (scale == 0) ? (log2_w - (hv ? 4 : 3)) : log2_w;
    }
    return sh;
}

/* ------------------------------------------------------------------ */
/* CPU scoring functions                                               */
/* ------------------------------------------------------------------ */

/*
 * The CPU's own finalisation of one scale (ADR-1362). integer_adm.c ends every
 * scale in float -- adm_cm() / i4_adm_cm() and adm_csf_den_scale() /
 * adm_csf_den_s123() each return the float sum of three float band terms --
 * and integer_compute_adm() sums those floats in double. The device
 * accumulators are bit-exact with the CPU's, so repeating that arithmetic
 * expression for expression reproduces every ADM output bit for bit. This twin
 * used to finalise in double instead, which left adm2 and integer_adm_scale*
 * up to 2.9e-7 from the CPU; do not bring that back.
 */
float adm_cm_scale_cpu(const int64_t accum[3], int h, int w, int scale,
                       uint32_t normalization_shift, double noise_weight, double p_norm)
{
    AdmRegion const b = adm_region(w, h);
    auto const shift_inner_accum = (uint32_t)std::ceil(std::log2(h));
    int const restored_bits = 3 * (int)normalization_shift;
    float f_accum[3];
    if (scale == 0) {
        auto const shift_xhcub = (uint32_t)std::ceil(std::log2(w) - 4);
        auto const shift_xdcub = (uint32_t)std::ceil(std::log2(w) - 3);
        int const base_exp[3] = {52, 52, 57};
        uint32_t const shift_cub[3] = {shift_xhcub, shift_xhcub, shift_xdcub};
        for (int i = 0; i < 3; ++i) {
            int const divisor_exp =
                base_exp[i] - restored_bits - (int)shift_cub[i] - (int)shift_inner_accum;
            f_accum[i] = (float)(accum[i] / std::pow(2, divisor_exp));
        }
    } else {
        auto const shift_cub = (uint32_t)std::ceil(std::log2(w));
        int const pending = restored_bits + (int)shift_cub + (int)shift_inner_accum;
        float const final_shift[3] = {(float)std::pow(2, (45 - pending)),
                                      (float)std::pow(2, (39 - pending)),
                                      (float)std::pow(2, (36 - pending))};
        for (int i = 0; i < 3; ++i) {
            // int64 / float: the accumulator is rounded to float first, as on the CPU
            f_accum[i] = (float)(accum[i] / final_shift[scale - 1]);
        }
    }
    float const p_norm_exp = 1.0f / (float)p_norm;
    int const area = (b.bottom - b.top) * (b.right - b.left);
    float num[3];
    for (int i = 0; i < 3; ++i) {
        num[i] = powf(f_accum[i], p_norm_exp) + powf((float)(area * noise_weight), p_norm_exp);
    }
    return num[0] + num[1] + num[2];
}

/* The CPU's adm_csf_den_scale() / adm_csf_den_s123() finalisation. */
float adm_den_scale_cpu(const int64_t accum[3], int h, int w, int scale, const float rfactor[3],
                        double noise_weight)
{
    AdmRegion const b = adm_region(w, h);
    int const area = (b.bottom - b.top) * (b.right - b.left);
    double shift_csf = 0.0;
    if (scale == 0) {
        auto const shift_accum = (int32_t)std::ceil(std::log2(area) - 20);
        shift_csf = std::pow(2, (18 - (shift_accum > 0 ? shift_accum : 0)));
    } else {
        uint32_t const accum_convert_float[3] = {32, 27, 23};
        auto const shift_cub = (uint32_t)std::ceil(std::log2(b.right - b.left));
        auto const shift_accum = (uint32_t)std::ceil(std::log2(b.bottom - b.top));
        shift_csf = std::pow(2, (accum_convert_float[scale - 1] - shift_accum - shift_cub));
    }
    float const powf_add = powf((float)(area * noise_weight), 1.0f / 3.0f);
    float den[3];
    for (int i = 0; i < 3; ++i) {
        double const csf = (double)((uint64_t)accum[i] / shift_csf) * std::pow(rfactor[i], 3);
        den[i] = powf((float)csf, 1.0f / 3.0f) + powf_add;
    }
    return den[0] + den[1] + den[2];
}

/* ------------------------------------------------------------------ */
/* Feature extractor callbacks                                         */
/* ------------------------------------------------------------------ */

// Forward declarations for combined graph callbacks (defined after enqueue_adm_work_impl)
void enqueue_adm_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis);
void adm_pre_graph(void *queue_ptr, void *priv);
void adm_post_graph(void *queue_ptr, void *priv);
int close_fex_sycl(VmafFeatureExtractor *fex); /* init error paths own their cleanup — SY-2a */

constexpr size_t ADM_ACCUM_BYTES = (size_t)ADM_ACCUM_SLOTS * sizeof(int64_t);
constexpr size_t ADM_DIV_LOOKUP_ENTRIES = 65537;

/* CSF factors and fixed-point weights of every scale (ADR-1325). */
void adm_init_rfactors(AdmStateSycl *s)
{
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        AdmCsfFactors const f =
            adm_csf_factors(scale, s->adm_norm_view_dist, s->adm_ref_display_height,
                            s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale);
        size_t const band0 = (size_t)scale * ADM_NUM_BANDS;
        s->rfactor[band0 + 0] = f.factor1;
        s->rfactor[band0 + 1] = f.factor1;
        s->rfactor[band0 + 2] = f.factor2;

        double fixed[3];
        s->csf_normalization_shift[scale] = 0u;
        int const fixed_err = adm_csf_fixed_scale(scale, &s->rfactor[band0], s->adm_norm_view_dist,
                                                  s->adm_ref_display_height, s->adm_csf_mode, fixed,
                                                  &s->csf_normalization_shift[scale]);
        // init_fex_sycl() ran adm_csf_config_check(), which calls the same
        // conversion through adm_csf_check_scale(), before this point.
        assert(fixed_err == 0);
        if (fixed_err == 0) {
            s->i_rfactor[band0] = (uint32_t)fixed[0];
            s->i_rfactor[band0 + 1] = (uint32_t)fixed[1];
            s->i_rfactor[band0 + 2] = (uint32_t)fixed[2];
        }
    }
}

int32_t *adm_alloc_band(VmafSyclState *state, size_t bytes)
{
    return static_cast<int32_t *>(vmaf_sycl_malloc_device(state, bytes));
}

/* Every USM buffer. A NULL buffer handed to a kernel page-faults the device
 * instead of failing cleanly, so all of them are checked; on failure the
 * caller's close_fex_sycl() frees the partial set (it NULL-guards each free). */
int adm_alloc_buffers(AdmStateSycl *s, VmafSyclState *state, unsigned w, unsigned half_h)
{
    assert(s != nullptr && state != nullptr);
    // Bands are half the frame width, padded to buf_stride.
    assert(s->buf_stride >= (w + 1) / 2);
    size_t const dwt_tmp_size = (size_t)w * 2 * half_h * sizeof(int32_t);
    s->d_dwt_tmp_ref = adm_alloc_band(state, dwt_tmp_size);
    s->d_dwt_tmp_dis = adm_alloc_band(state, dwt_tmp_size);
    bool ok = s->d_dwt_tmp_ref && s->d_dwt_tmp_dis;

    size_t const band_size = (size_t)s->buf_stride * half_h * sizeof(int32_t);
    for (int i = 0; i < 4; i++) {
        s->d_ref_band[i] = adm_alloc_band(state, band_size);
        s->d_dis_band[i] = adm_alloc_band(state, band_size);
        ok = ok && s->d_ref_band[i] && s->d_dis_band[i];
    }
    for (int b = 0; b < ADM_NUM_BANDS; b++) {
        s->d_csf_f[b] = adm_alloc_band(state, band_size);
        s->d_csf_f_aim[b] = adm_alloc_band(state, band_size);
        ok = ok && s->d_csf_f[b] && s->d_csf_f_aim[b];
    }

    s->d_div_lookup = adm_alloc_band(state, ADM_DIV_LOOKUP_ENTRIES * sizeof(int32_t));
    s->d_accum = static_cast<int64_t *>(vmaf_sycl_malloc_device(state, ADM_ACCUM_BYTES));
    s->h_accum = static_cast<int64_t *>(vmaf_sycl_malloc_host(state, ADM_ACCUM_BYTES));
    ok = ok && s->d_div_lookup && s->d_accum && s->h_accum;
    if (!ok) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "adm_sycl: device memory allocation failed\n");
        return -ENOMEM;
    }
    return 0;
}

/* The decouple's reciprocal table (CPU div_lookup): 2^30 / i, i in [-2^15, 2^15]. */
int adm_upload_div_lookup(AdmStateSycl *s, VmafSyclState *state)
{
    auto *lut = static_cast<int32_t *>(std::calloc(ADM_DIV_LOOKUP_ENTRIES, sizeof(int32_t)));
    if (!lut) {
        return -ENOMEM;
    }
    static const int32_t Q_factor = 1073741824; // 2^30
    for (int i = 1; i <= 32768; i++) {
        int32_t const recip = (int32_t)(Q_factor / i);
        lut[32768 + i] = recip;
        lut[32768 - i] = -recip;
    }
    int const err =
        vmaf_sycl_memcpy_h2d(state, s->d_div_lookup, lut, ADM_DIV_LOOKUP_ENTRIES * sizeof(int32_t));
    std::free(lut);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "adm_sycl: div_lookup upload failed\n");
    }
    return err;
}

int init_fex_sycl(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                  unsigned h)
{
    (void)pix_fmt;
    auto *s = static_cast<AdmStateSycl *>(fex->priv);

    /* Same frame-size bound as the CPU reference, checked before any device
     * resource is claimed. */
    const int size_err = adm_frame_size_check("adm_sycl", w, h);
    if (size_err != 0) {
        return size_err;
    }

    s->width = w;
    s->height = h;
    s->bpc = bpc;
    s->has_pending = false;

    if (!fex->sycl_state) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "adm_sycl: no SYCL state\n");
        return -EINVAL;
    }

    /* Reject invalid CSF table output before any device resource is claimed.
     * Finite over-range weights are normalised with the CPU's shared
     * per-scale exponent in adm_init_rfactors(). */
    const int csf_err = adm_csf_config_check(s);
    if (csf_err) {
        return csf_err;
    }

    VmafSyclState *state = fex->sycl_state;

    // Initialize shared frame buffers (idempotent, first extractor wins)
    int err = vmaf_sycl_shared_frame_init(state, w, h, bpc);
    if (err) {
        return err;
    }

    s->buf_stride = (((w + 1) / 2) + 3) & ~3u; // half width, aligned to 4
    adm_init_rfactors(s);

    err = adm_alloc_buffers(s, state, w, (h + 1) / 2);
    if (!err) {
        err = adm_upload_div_lookup(s, state);
    }
    if (!err) {
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        err = s->feature_name_dict ? 0 : -ENOMEM;
    }
    if (!err) {
        // Register with combined command graph
        err = vmaf_sycl_graph_register(state, enqueue_adm_work, adm_pre_graph, adm_post_graph,
                                       nullptr, s, "ADM");
    }
    if (err) {
        (void)close_fex_sycl(fex);
    }
    return err;
}

/* ------------------------------------------------------------------ */
/* Enqueue all ADM compute work (used for both recording and direct)   */
/* ------------------------------------------------------------------ */

/* Decouple + CSF, then the three reductions, of one scale whose bands are in
 * d_ref_band / d_dis_band. */
void enqueue_adm_reductions(sycl::queue &q, const AdmStateSycl *s, int scale, int half_w,
                            int half_h)
{
    AdmBandInputs in{};
    for (int b = 0; b < ADM_NUM_BANDS; ++b) {
        in.ref[b] = s->d_ref_band[b + 1];
        in.dis[b] = s->d_dis_band[b + 1];
        in.i_rfactor[b] = s->i_rfactor[(scale * ADM_NUM_BANDS) + b];
    }
    in.div_lookup = s->d_div_lookup;
    in.gain = gain_limit_to_q31(s->adm_enhn_gain_limit);
    in.scale = scale;
    in.w = half_w;
    in.h = half_h;
    in.stride = s->buf_stride;
    in.aim = !s->adm_skip_aim;

    AdmDecoupleArgs dec{};
    AdmCmArgs cm{};
    dec.in = in;
    cm.in = in;
    for (int b = 0; b < ADM_NUM_BANDS; ++b) {
        dec.csf_f[b] = s->d_csf_f[b];
        dec.csf_f_aim[b] = s->d_csf_f_aim[b];
        cm.csf_f[b] = s->d_csf_f[b];
        cm.csf_f_aim[b] = s->d_csf_f_aim[b];
    }
    (void)launch_decouple_csf(q, dec);

    AdmRegion const r = adm_region(half_w, half_h);
    if (r.right <= r.left || r.bottom <= r.top) {
        return;
    }
    cm.accum = s->d_accum + adm_accum_slot(ADM_TERM_CM, scale);
    cm.sh = adm_cm_shifts(scale, half_w, half_h, r);
    cm.left = r.left;
    cm.top = r.top;
    cm.right = r.right;
    (void)launch_csf_den_cm(q, cm, r.bottom - r.top);
}

void enqueue_adm_work_impl(sycl::queue &q, AdmStateSycl *s, void *shared_ref, void *shared_dis)
{
    assert(s != nullptr);
    assert(shared_ref != nullptr);
    assert(shared_dis != nullptr);
    /* Scale 0 shifts by bpc; a zero bpc would make the `1u << (s->bpc - 1)`
     * rounding term below shift by 2^32 - 1. libvmaf accepts 8 to 16. */
    assert(s->bpc >= 8u && s->bpc <= 16u);

    // DWT shift parameters per scale
    struct DwtShifts {
        unsigned v_shift, v_add, h_shift;
    };
    DwtShifts dwt_shifts[4];
    dwt_shifts[0] = {.v_shift = s->bpc, .v_add = 1u << (s->bpc - 1), .h_shift = 16u};
    dwt_shifts[1] = {.v_shift = 0u, .v_add = 0u, .h_shift = 15u};
    dwt_shifts[2] = {.v_shift = 16u, .v_add = 32768u, .h_shift = 16u};
    dwt_shifts[3] = {.v_shift = 16u, .v_add = 32768u, .h_shift = 15u};

    unsigned cur_w = s->width;
    unsigned cur_h = s->height;
    unsigned const cur_stride = s->buf_stride;

    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        unsigned const half_w = (cur_w + 1) / 2;
        unsigned const half_h = (cur_h + 1) / 2;

        // Input source: scale 0 reads from shared frame, others from LL band
        const void *ref_src = (scale == 0) ? shared_ref : (const void *)s->d_ref_band[0];
        const void *dis_src = (scale == 0) ? shared_dis : (const void *)s->d_dis_band[0];
        unsigned const in_stride = (scale != 0) ? cur_stride : ((s->bpc <= 8) ? cur_w : cur_w * 2);

        // DWT vertical pass: ref + dis fused
        launch_dwt_vert_pair(q, ref_src, s->d_dwt_tmp_ref, dis_src, s->d_dwt_tmp_dis, scale, cur_w,
                             cur_h, in_stride, s->bpc, dwt_shifts[scale].v_shift,
                             dwt_shifts[scale].v_add);

        // DWT horizontal pass: ref + dis fused
        launch_dwt_hori_pair(q, s->d_dwt_tmp_ref, s->d_ref_band[0], s->d_ref_band[1],
                             s->d_ref_band[2], s->d_ref_band[3], s->d_dwt_tmp_dis, s->d_dis_band[0],
                             s->d_dis_band[1], s->d_dis_band[2], s->d_dis_band[3], cur_w, cur_h,
                             cur_stride, dwt_shifts[scale].h_shift);

        enqueue_adm_reductions(q, s, scale, (int)half_w, (int)half_h);

        // Next scale dimensions
        cur_w = half_w;
        cur_h = half_h;
    }
}

/* ------------------------------------------------------------------ */
/* C-compatible callbacks for combined command graph                   */
/* ------------------------------------------------------------------ */

// Pre-graph: zero accumulators (direct enqueue, outside graph)
void adm_pre_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<AdmStateSycl *>(priv);
    q.memset(s->d_accum, 0, ADM_ACCUM_BYTES);
}

// Graph-recorded: compute kernels only
void enqueue_adm_work(void *queue_ptr, void *priv, void *shared_ref, void *shared_dis)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<AdmStateSycl *>(priv);
    enqueue_adm_work_impl(q, s, shared_ref, shared_dis);
}

// Post-graph: the frame's one D2H copy, all three accumulator terms (direct enqueue, outside graph)
void adm_post_graph(void *queue_ptr, void *priv)
{
    sycl::queue &q = *static_cast<sycl::queue *>(queue_ptr);
    auto *s = static_cast<AdmStateSycl *>(priv);
    q.memcpy(s->h_accum, s->d_accum, ADM_ACCUM_BYTES);
}

/* ------------------------------------------------------------------ */
/* Submit / Collect / Extract                                          */
/* ------------------------------------------------------------------ */

int submit_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                    VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index)
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

/* Numerator, denominator and AIM numerator of one scale, in float like the
 * CPU's AdmScaleScores. */
struct AdmScaleCpu {
    float num;
    float den;
    float aim_num;
};

AdmScaleCpu adm_scale_cpu(const AdmStateSycl *s, int scale, int w, int h)
{
    assert(s != nullptr && s->h_accum != nullptr);
    AdmScaleCpu sc{};
    if (scale == 0 && s->adm_skip_scale0) {
        sc.den = (float)1e-10; // integer_adm_scale0(): avoid divide by zero
        return sc;
    }
    const int64_t *acc = s->h_accum;
    uint32_t const ns = s->csf_normalization_shift[scale];
    sc.num = adm_cm_scale_cpu(&acc[adm_accum_slot(ADM_TERM_CM, scale)], h, w, scale, ns,
                              s->adm_noise_weight, s->adm_p_norm);
    sc.den = adm_den_scale_cpu(&acc[adm_accum_slot(ADM_TERM_DEN, scale)], h, w, scale,
                               &s->rfactor[(size_t)scale * ADM_NUM_BANDS], s->adm_noise_weight);
    if (!s->adm_skip_aim) {
        // measure_aim: noise_weight 0.0, as integer_adm_scale0() / _s123() pass it
        sc.aim_num = adm_cm_scale_cpu(&acc[adm_accum_slot(ADM_TERM_AIM, scale)], h, w, scale, ns,
                                      0.0, s->adm_p_norm);
    }
    return sc;
}

/* The frame's scale terms and sums, as integer_compute_adm() forms them. */
struct AdmTerms {
    double scores[8]; /* [2 * scale] numerator, [2 * scale + 1] denominator */
    double num;
    double den;
    double aim_num;
};

void adm_terms(const AdmStateSycl *s, AdmTerms *t)
{
    assert(s != nullptr && t != nullptr);
    int w = (int)s->width;
    int h = (int)s->height;
    t->num = 0.0;
    t->den = 0.0;
    t->aim_num = 0.0;
    for (int scale = 0; scale < ADM_NUM_SCALES; scale++) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        AdmScaleCpu const sc = adm_scale_cpu(s, scale, w, h);
        size_t const pair = 2 * (size_t)scale;
        t->scores[pair] = sc.num;
        t->scores[pair + 1] = sc.den;
        t->num += sc.num;
        t->den += sc.den;
        t->aim_num += sc.aim_num;
    }
}

/* integer_adm.c::adm_result_finalise(): floor num / den at the full-frame
 * precision limit (in place, as the debug outputs report them), then
 * ratios = {num / den, aim_num / den}. */
int adm_finalise(unsigned index, AdmTerms *t, double numden_limit, double ratios[2])
{
    int err = vmaf_adm_floor_pair_named("integer_adm_sycl", index, t->num, t->den, numden_limit,
                                        &t->num, &t->den);
    if (err) {
        return err;
    }
    const double pairs[4] = {t->num, t->den, t->aim_num, t->den};
    err = vmaf_adm_scale_ratios(pairs, 2u, ratios);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "integer_adm_sycl: undefined or non-finite aggregate at frame %u "
                 "(num=%g den=%g aim_num=%g)\n",
                 index, t->num, t->den, t->aim_num);
    }
    return err;
}

int emit_adm_scores(const AdmStateSycl *s, VmafFeatureCollector *feature_collector, unsigned index,
                    const double headline[3], const AdmTerms &t, const double scale_scores[4])
{
    VmafNamedScore values[18] = {
        {.name = "VMAF_integer_feature_adm2_score", .value = headline[0]},
        {.name = "VMAF_integer_feature_aim_score", .value = headline[1]},
        {.name = "VMAF_integer_feature_adm3_score", .value = headline[2]},
        {.name = "integer_adm_scale0", .value = scale_scores[0]},
        {.name = "integer_adm_scale1", .value = scale_scores[1]},
        {.name = "integer_adm_scale2", .value = scale_scores[2]},
        {.name = "integer_adm_scale3", .value = scale_scores[3]},
    };
    size_t value_count = 7u;
    if (s->debug) {
        static const char *const debug_names[8] = {
            "integer_adm_num_scale0", "integer_adm_den_scale0", "integer_adm_num_scale1",
            "integer_adm_den_scale1", "integer_adm_num_scale2", "integer_adm_den_scale2",
            "integer_adm_num_scale3", "integer_adm_den_scale3",
        };
        values[value_count++] = VmafNamedScore{.name = "integer_adm", .value = headline[0]};
        values[value_count++] = VmafNamedScore{.name = "integer_adm_num", .value = t.num};
        values[value_count++] = VmafNamedScore{.name = "integer_adm_den", .value = t.den};
        for (size_t i = 0u; i < 8u; ++i) {
            values[value_count++] = VmafNamedScore{.name = debug_names[i], .value = t.scores[i]};
        }
    }
    return vmaf_feature_emit_finite_scores(feature_collector, s->feature_name_dict,
                                           "integer_adm_sycl", values, value_count, index);
}

int collect_fex_sycl(VmafFeatureExtractor *fex, unsigned index,
                     VmafFeatureCollector *feature_collector)
{
    auto *s = static_cast<AdmStateSycl *>(fex->priv);

    // Combined graph wait (once per frame); a failed wait leaves stale accumulators.
    s->has_pending = false;
    if (int const wait_err = vmaf_sycl_graph_wait(fex->sycl_state))
        return wait_err;

    AdmTerms t{};
    adm_terms(s, &t);

    /* numden_limit — CPU parity (integer_adm.c::integer_compute_adm): the
     * precision floor scales with the FULL-FRAME area, not the scale-3 area. */
    double const numden_limit = 1e-10 * ((double)s->width * s->height) / (1920.0 * 1080.0);
    double ratios[2];
    int err = adm_finalise(index, &t, numden_limit, ratios);
    if (err) {
        return err;
    }

    /* adm2 is emitted unclamped: ADR-0487's adm_min_val clamps adm3 only, as
     * in integer_adm.c::extract(). */
    double headline[3] = {ratios[0], ratios[1], 0.0}; // adm2, aim, adm3
    err = vmaf_adm3_score_named("integer_adm_sycl", index, ratios[0], ratios[1], 0,
                                s->adm_dlm_weight, s->adm_min_val, &headline[2]);
    if (err) {
        return err;
    }

    double scale_scores[ADM_NUM_SCALES];
    err = vmaf_adm_scale_ratios_named("integer_adm_sycl", index, t.scores, ADM_NUM_SCALES,
                                      scale_scores);
    if (err) {
        return err;
    }
    return emit_adm_scores(s, feature_collector, index, headline, t, scale_scores);
}

int extract_fex_sycl(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                     VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                     VmafFeatureCollector *feature_collector)
{
    int const err = submit_fex_sycl(fex, ref_pic, ref_pic_90, dist_pic, dist_pic_90, index);
    if (err)
        return err;
    return collect_fex_sycl(fex, index, feature_collector);
}

int flush_fex_sycl(VmafFeatureExtractor *fex, VmafFeatureCollector *feature_collector)
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

void adm_free(VmafSyclState *state, void *ptr)
{
    if (ptr) {
        vmaf_sycl_free(state, ptr);
    }
}

int close_fex_sycl(VmafFeatureExtractor *fex)
{
    assert(fex != nullptr);
    auto *s = static_cast<AdmStateSycl *>(fex->priv);
    VmafSyclState *state = fex->sycl_state;
    /* The framework never closes an extractor it did not initialise, so priv
     * is set whenever a state exists to free. */
    assert(state == nullptr || s != nullptr);

    if (state) {
        (void)vmaf_sycl_queue_wait(state);

        /* Unregister from the combined command graph before freeing priv.
         * Mirrors the fix in integer_motion_sycl.cpp (ADR-0989):
         * vmaf_sycl_graph_unregister() drains combined_queue and removes
         * this extractor's entry so a subsequent VmafContext sharing the
         * same sycl_state does not inherit a dangling priv pointer. */
        (void)vmaf_sycl_graph_unregister(state, s);

        adm_free(state, s->d_dwt_tmp_ref);
        adm_free(state, s->d_dwt_tmp_dis);
        for (int i = 0; i < 4; i++) {
            adm_free(state, s->d_ref_band[i]);
            adm_free(state, s->d_dis_band[i]);
        }
        for (int b = 0; b < ADM_NUM_BANDS; b++) {
            adm_free(state, s->d_csf_f[b]);
            adm_free(state, s->d_csf_f_aim[b]);
        }
        adm_free(state, s->d_div_lookup);
        adm_free(state, s->d_accum);
        adm_free(state, s->h_accum);
    }

    if (s->feature_name_dict)
        vmaf_dictionary_free(&s->feature_name_dict);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Feature extractor definition                                        */
/* ------------------------------------------------------------------ */

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
