/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "adm_angle_flag.h"
#include "adm_cm_accumulator.h"
#include "adm_csf_fixed_point.h"
#include "adm_score.h"
#include "barten_csf_tools.h"
#include "compat_builtin.h"
#include "cpu.h"
#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "integer_adm.h"
#include "integer_adm_kernels.h"
#include "log.h"
#include "nonfinite_score.h"

#if ARCH_X86
#include "x86/adm_avx2.h"
#if HAVE_AVX512
#include "x86/adm_avx512.h"
#endif
#elif ARCH_AARCH64
#include "arm64/adm_neon.h"
#include <arm_neon.h>
#endif

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is an
 * upstream-mirror file whose Netflix source spells the null pointer constant
 * `NULL` (every upstream sync would re-conflict against a keyword rewrite) and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

typedef struct AdmState {
    size_t integer_stride;
    AdmBuffer buf;
    bool debug;
    bool adm_skip_aim;
    bool adm_skip_scale0;
    double adm_csf_diag_scale;
    double adm_csf_scale;
    double adm_dlm_weight;
    double adm_enhn_gain_limit;
    double adm_norm_view_dist;
    double adm_noise_weight;
    double adm_min_val;
    double adm_p_norm;
    int adm_ref_display_height;
    int adm_csf_mode;
    /* 0, or -EINVAL when the configured CSF weights are invalid. Evaluated
     * once in init(), enforced in extract() beside the viewing-geometry
     * guard. Finite over-range weights use a shared per-scale exponent. */
    int csf_config_err;
    bool csf_requires_normalization;
    void (*dwt2_8)(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w, int h,
                   int src_stride, int dst_stride);
    void (*dwt2_16)(const uint16_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w, int h,
                    int src_stride, int dst_stride, int inp_size_bits);
    void (*adm_decouple)(AdmBuffer *buf, int w, int h, int stride, double adm_enhn_gain_limit,
                         int32_t *adm_div_lookup);
    void (*adm_decouple_s123)(AdmBuffer *buf, int w, int h, int stride, double adm_enhn_gain_limit,
                              int32_t *adm_div_lookup);
    float (*adm_csf_den_scale)(const adm_dwt_band_t *src, int w, int h, int src_stride,
                               double adm_norm_view_dist, int adm_ref_display_height,
                               int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                               double adm_noise_weight);
    void (*adm_csf)(AdmBuffer *buf, int w, int h, int stride, double adm_norm_view_dist,
                    int adm_ref_display_height, int adm_csf_mode, double adm_csf_scale,
                    double adm_csf_diag_scale, bool measure_aim);
    float (*adm_cm)(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride,
                    double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                    double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                    double adm_p_norm, bool measure_aim);
    void (*adm_dwt2_s123_combined)(const int32_t *i4_ref_scale, const int32_t *i4_curr_dis,
                                   AdmBuffer *buf, int w, int h, int ref_stride, int dis_stride,
                                   int dst_stride, int scale);
    float (*adm_csf_den_s123)(const i4_adm_dwt_band_t *src, int scale, int w, int h, int src_stride,
                              double adm_norm_view_dist, int adm_ref_display_height,
                              int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                              double adm_noise_weight);
    void (*i4_adm_csf)(AdmBuffer *buf, int scale, int w, int h, int stride,
                       double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                       double adm_csf_scale, double adm_csf_diag_scale, bool measure_aim);
    float (*i4_adm_cm)(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride, int scale,
                       double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                       double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                       double adm_p_norm, bool measure_aim);
    VmafDictionary *feature_name_dict;
} AdmState;

static const VmafOption options[] = {
    {
        .name = "debug",
        .help = "debug mode: enable additional output",
        .offset = offsetof(AdmState, debug),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "adm_csf_scale",
        .alias = "scf",
        .help = "scale coefficient for the horizontal & vertical direction terms of CSF",
        .offset = offsetof(AdmState, adm_csf_scale),
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
        .offset = offsetof(AdmState, adm_csf_diag_scale),
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
        .offset = offsetof(AdmState, adm_dlm_weight),
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
        .offset = offsetof(AdmState, adm_enhn_gain_limit),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_ENHN_GAIN_LIMIT,
        .min = 1.0,
        .max = DEFAULT_ADM_ENHN_GAIN_LIMIT,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_norm_view_dist",
        .alias = "nvd",
        .help = "normalized viewing distance = viewing distance / ref display's physical height",
        .offset = offsetof(AdmState, adm_norm_view_dist),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_NORM_VIEW_DIST,
        .min = 0.75,
        .max = 24.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_ref_display_height",
        .alias = "rdh",
        .help = "reference display height in pixels",
        .offset = offsetof(AdmState, adm_ref_display_height),
        .type = VMAF_OPT_TYPE_INT,
        .default_val.i = DEFAULT_ADM_REF_DISPLAY_HEIGHT,
        .min = 1,
        .max = 4320,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_csf_mode",
        .alias = "csf",
        .help = "contrast sensitivity function",
        .offset = offsetof(AdmState, adm_csf_mode),
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
        .offset = offsetof(AdmState, adm_noise_weight),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = DEFAULT_ADM_NOISE_WEIGHT,
        .min = 0.0,
        .max = 1500.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_skip_aim",
        .help = "skip the calculation of AIM",
        .offset = offsetof(AdmState, adm_skip_aim),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
    },
    {
        .name = "adm_skip_scale0",
        .alias = "ssz",
        .help = "skip the calculation of scale 0",
        .offset = offsetof(AdmState, adm_skip_scale0),
        .type = VMAF_OPT_TYPE_BOOL,
        .default_val.b = false,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {
        .name = "adm_min_val",
        .alias = "min",
        .help = "minimum value allowed; lower values will be clipped to this value",
        .offset = offsetof(AdmState, adm_min_val),
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
        .offset = offsetof(AdmState, adm_p_norm),
        .type = VMAF_OPT_TYPE_DOUBLE,
        .default_val.d = 3.0,
        .min = 1.0,
        .max = 20.0,
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,
    },
    {0}};

/* ------------------------------------------------------------------------- */
/* Contrast sensitivity weights                                              */
/* ------------------------------------------------------------------------- */

/**
 * Validate and size the fixed-point representation for a CSF configuration.
 *
 * `adm_csf_mode` 1 (Barten) at the default `adm_csf_scale` produces weights
 * around 1.2 at scale 0 and 27 at scale 3, which exceed the original
 * fixed-point budgets by two orders of magnitude and therefore require a
 * shared per-scale exponent. The blended-CSF
 * tables return `-EINVAL` as a float for viewing geometries they do not
 * tabulate, which is worse still (a negative-to-unsigned conversion is UB).
 * Both used to produce silently wrong scores. Finite non-negative weights are
 * normalised; invalid table output returns -EINVAL and the helper logs the
 * offending band and scale.
 *
 * See ADR-1191, ADR-1325, and docs/metrics/features.md.
 */
static int adm_csf_config_check(AdmState *s)
{
    s->csf_requires_normalization = false;
    for (int scale = 0; scale < 4; ++scale) {
        const AdmCsfFactors f =
            adm_csf_factors(scale, s->adm_norm_view_dist, s->adm_ref_display_height,
                            s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale);
        const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
        double fixed[3];
        uint32_t normalization_shift;
        const int err =
            adm_csf_fixed_scale(scale, rfactor1, s->adm_norm_view_dist, s->adm_ref_display_height,
                                s->adm_csf_mode, fixed, &normalization_shift);
        if (err) {
            return err;
        }
        s->csf_requires_normalization |= normalization_shift > 0u;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* DWT source-index tables                                                   */
/* ------------------------------------------------------------------------- */

/* Symmetric-extension source indices of the four Daubechies-2 taps for every
 * output sample of one dimension: `ind[k][i]` is the input index read by tap
 * `k` when producing output `i`. */
static void dwt2_src_indices_1d(int *const *ind, int n, unsigned n_half)
{
    { /* i : 0 */
        ind[0][0] = 1;
        ind[1][0] = 0;
        ind[2][0] = 1;
        ind[3][0] = 2;
    }
    /* `i + 2 < n_half`, not `i < n_half - 2`: the unsigned subtraction wraps
     * for n_half < 2. */
    for (unsigned i = 1; i + 2 < n_half; ++i) { /* i : 1 to  n_half - 3*/
        const int ind1 = 2 * i;
        ind[0][i] = ind1 - 1;
        ind[1][i] = ind1;
        ind[2][i] = ind1 + 1;
        ind[3][i] = ind1 + 2;
    }
    /* The mirrored tail must never start below 1. With n_half == 2 (a scale-3
     * input of 3 or 4 samples, i.e. any frame dimension from 17 to 32) the
     * upstream bound `n_half - 2` restarts it at 0 and overwrites the i == 0
     * entries above with {-1, 0, 1, 2}: the DWT then reads row / column -1,
     * before the band and, horizontally, before the tmp_ref allocation. */
    for (unsigned i = (n_half > 2u) ? n_half - 2u : 1u; i < n_half; ++i) {
        int ind1 = 2 * i;
        int ind0 = ind1 - 1;
        int ind2 = ind1 + 1;
        int ind3 = ind1 + 2;
        if (ind0 >= n) {
            ind0 = (2 * n - ind0 - 1);
        }
        if (ind1 >= n) {
            ind1 = (2 * n - ind1 - 1);
        }
        if (ind2 >= n) {
            ind2 = (2 * n - ind2 - 1);
        }
        if (ind3 >= n) {
            ind3 = (2 * n - ind3 - 1);
        }
        ind[0][i] = ind0;
        ind[1][i] = ind1;
        ind[2][i] = ind2;
        ind[3][i] = ind3;
    }
}

static void dwt2_src_indices_filt(int *const *src_ind_y, int *const *src_ind_x, int w, int h)
{
    const unsigned h_half = (h + 1) / 2;
    const unsigned w_half = (w + 1) / 2;
    /* Vertical pass */
    dwt2_src_indices_1d(src_ind_y, h, h_half);
    /* Horizontal pass */
    dwt2_src_indices_1d(src_ind_x, w, w_half);
}

/* ------------------------------------------------------------------------- */
/* Decouple                                                                  */
/* ------------------------------------------------------------------------- */

/* Upstream-parity note: `lut` is bound through `AdmState::adm_decouple`
 * next to `adm_decouple_avx2` / `adm_decouple_avx512` (x86/adm_avx2.h,
 * x86/adm_avx512.h), whose prototypes take a mutable `int32_t *`.
 * Constifying only the scalar twin would leave the dispatch assignment
 * ill-typed. ADR-0141 §2 load-bearing invariant (shared SIMD dispatch
 * signature); see ADR-1141. */
// cppcheck-suppress constParameterCallback
// NOLINTNEXTLINE(readability-non-const-parameter) — ADR-0141 / ADR-1141
static void adm_decouple(AdmBuffer *buf, int w, int h, int stride, double gain, int32_t *lut)
{
    const float cos_1deg_sq = adm_cos_1deg_sq();

    /* The computation of the score is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);

    for (int i = b.top; i < b.bottom; ++i) {
        adm_decouple_cols(buf, i, stride, b.left, b.right, gain, lut, cos_1deg_sq);
    }
}

/* See adm_decouple above: `lut` keeps the mutable `int32_t *` of the shared
 * dispatch signature (`adm_decouple_s123_avx2` / `_avx512`). ADR-0141 §2
 * load-bearing invariant; see ADR-1141. */
// cppcheck-suppress constParameterCallback
// NOLINTNEXTLINE(readability-non-const-parameter) — ADR-0141 / ADR-1141
static void adm_decouple_s123(AdmBuffer *buf, int w, int h, int stride, double gain, int32_t *lut)
{
    const float cos_1deg_sq = adm_cos_1deg_sq();

    /* The computation of the score is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);

    for (int i = b.top; i < b.bottom; ++i) {
        adm_decouple_s123_cols(buf, i, stride, b.left, b.right, gain, lut, cos_1deg_sq);
    }
}

/* ------------------------------------------------------------------------- */
/* Contrast sensitivity filtering                                            */
/* ------------------------------------------------------------------------- */

static void adm_csf(AdmBuffer *buf, int w, int h, int stride, double adm_norm_view_dist,
                    int adm_ref_display_height, int adm_csf_mode, double adm_csf_scale,
                    double adm_csf_diag_scale, bool measure_aim)
{
    const AdmCsfBands bands = adm_csf_bands(buf, measure_aim);
    uint16_t i_rfactor[3];
    (void)adm_csf_i_rfactor(adm_norm_view_dist, adm_ref_display_height, adm_csf_mode, adm_csf_scale,
                            adm_csf_diag_scale, i_rfactor);

    /* The computation of the csf values is not required for the regions which
     * lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);

    for (int theta = 0; theta < 3; ++theta) {
        for (int i = b.top; i < b.bottom; ++i) {
            adm_csf_cols(&bands, i_rfactor, theta, (ptrdiff_t)i * stride, b.left, b.right);
        }
    }
}

static void i4_adm_csf(AdmBuffer *buf, int scale, int w, int h, int stride,
                       double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                       double adm_csf_scale, double adm_csf_diag_scale, bool measure_aim)
{
    I4AdmCsfCtx c;
    i4_adm_csf_ctx_init(&c, buf, scale, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                        adm_csf_scale, adm_csf_diag_scale, measure_aim);

    /* The computation of the csf values is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);

    for (int theta = 0; theta < 3; ++theta) {
        for (int i = b.top; i < b.bottom; ++i) {
            i4_adm_csf_cols(&c, theta, (ptrdiff_t)i * stride, b.left, b.right);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Denominator (reference-energy) reductions                                 */
/* ------------------------------------------------------------------------- */

static float adm_csf_den_scale(const adm_dwt_band_t *src, int w, int h, int src_stride,
                               double adm_norm_view_dist, int adm_ref_display_height,
                               int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                               double adm_noise_weight)
{
    AdmDenCtx c;
    adm_csf_den_ctx_init(&c, w, h, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                         adm_csf_scale, adm_csf_diag_scale);

    uint64_t accum[3] = {0, 0, 0};
    uint64_t inner[3] = {0, 0, 0};

    const int16_t *src_h = src->band_h + (ptrdiff_t)c.b.top * src_stride;
    const int16_t *src_v = src->band_v + (ptrdiff_t)c.b.top * src_stride;
    const int16_t *src_d = src->band_d + (ptrdiff_t)c.b.top * src_stride;
    for (int i = c.b.top; i < c.b.bottom; ++i) {
        adm_csf_den_cols(src_h, src_v, src_d, c.b.left, c.b.right, inner);
        adm_csf_den_fold(inner, accum, (uint32_t)c.add_shift_accum, (uint32_t)c.shift_accum);
        src_h += src_stride;
        src_v += src_stride;
        src_d += src_stride;
    }
    return adm_csf_den_result(&c, accum, adm_noise_weight);
}

static float adm_csf_den_s123(const i4_adm_dwt_band_t *src, int scale, int w, int h, int src_stride,
                              double adm_norm_view_dist, int adm_ref_display_height,
                              int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                              double adm_noise_weight)
{
    I4AdmDenCtx c;
    i4_adm_csf_den_ctx_init(&c, scale, w, h, adm_norm_view_dist, adm_ref_display_height,
                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);

    uint64_t accum[3] = {0, 0, 0};
    uint64_t inner[3] = {0, 0, 0};

    const int32_t *src_h = src->band_h + (ptrdiff_t)c.b.top * src_stride;
    const int32_t *src_v = src->band_v + (ptrdiff_t)c.b.top * src_stride;
    const int32_t *src_d = src->band_d + (ptrdiff_t)c.b.top * src_stride;
    for (int i = c.b.top; i < c.b.bottom; ++i) {
        i4_adm_csf_den_cols(&c, src_h, src_v, src_d, c.b.left, c.b.right, inner);
        adm_csf_den_fold(inner, accum, c.add_shift_accum, c.shift_accum);
        src_h += src_stride;
        src_v += src_stride;
        src_d += src_stride;
    }
    return i4_adm_csf_den_result(&c, accum, adm_noise_weight);
}

/* ------------------------------------------------------------------------- */
/* Contrast masking (numerator) reductions                                   */
/* ------------------------------------------------------------------------- */

static float adm_cm(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride,
                    double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                    double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                    double adm_p_norm, bool measure_aim)
{
    AdmCmCtx c;
    adm_cm_ctx_init(&c, buf, w, h, src_stride, csf_a_stride, adm_norm_view_dist,
                    adm_ref_display_height, adm_csf_mode, adm_csf_scale, adm_csf_diag_scale,
                    measure_aim);
    const AdmCmBounds bd = adm_cm_bounds(w, h);

    uint64_t accum[3] = {0, 0, 0};
    adm_cm_rows(&c, &bd, adm_cm_row, accum);
    return adm_cm_result(&c, &bd, accum, adm_noise_weight, adm_p_norm);
}

static float i4_adm_cm(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride, int scale,
                       double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                       double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                       double adm_p_norm, bool measure_aim)
{
    I4AdmCmCtx c;
    i4_adm_cm_ctx_init(&c, buf, w, h, src_stride, csf_a_stride, scale, adm_norm_view_dist,
                       adm_ref_display_height, adm_csf_mode, adm_csf_scale, adm_csf_diag_scale,
                       measure_aim);
    const AdmCmBounds bd = adm_cm_bounds(w, h);

    int64_t accum[3] = {0, 0, 0};
    i4_adm_cm_rows(&c, &bd, i4_adm_cm_row, accum);
    return i4_adm_cm_result(&c, &bd, accum, adm_noise_weight, adm_p_norm);
}

/* ------------------------------------------------------------------------- */
/* Daubechies-2 DWT                                                          */
/* ------------------------------------------------------------------------- */

static void i16_to_i32(const adm_dwt_band_t *src, const i4_adm_dwt_band_t *dst, int w, int h,
                       int stride)
{
    for (int i = 0; i < (h + 1) / 2; ++i) {
        const int16_t *src_band_a_addr = &src->band_a[(ptrdiff_t)i * stride];
        int32_t *dst_band_a_addr = &dst->band_a[(ptrdiff_t)i * stride];
        for (int j = 0; j < (w + 1) / 2; ++j) {
            *(dst_band_a_addr++) = (int32_t)(*(src_band_a_addr++));
        }
    }
}

static void adm_dwt2_8(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w, int h,
                       int src_stride, int dst_stride)
{
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        adm_dwt2_vpass_8(src, ind_y, i, src_stride, 0, w, tmplo, tmphi);
        adm_dwt2_hpass(tmplo, tmphi, dst, ind_x, i, 0, (w + 1) / 2, dst_stride);
    }
}

static void adm_dwt2_8_lo(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                          int h, int src_stride, int dst_stride)
{
    //8 bit only low-pass filtering using DWT2
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        adm_dwt2_vpass_8(src, ind_y, i, src_stride, 0, w, tmplo, NULL);
        adm_dwt2_hpass(tmplo, NULL, dst, ind_x, i, 0, (w + 1) / 2, dst_stride);
    }
}

static void adm_dwt2_16_lo(const uint16_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                           int h, int src_stride, int dst_stride, int inp_size_bits)
{
    //16 bit only low-pass filtering using DWT2
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        adm_dwt2_vpass_16(src, ind_y, i, src_stride, 0, w, inp_size_bits, tmplo, NULL);
        adm_dwt2_hpass(tmplo, NULL, dst, ind_x, i, 0, (w + 1) / 2, dst_stride);
    }
}

static void adm_dwt2_16(const uint16_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                        int h, int src_stride, int dst_stride, int inp_size_bits)
{
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        adm_dwt2_vpass_16(src, ind_y, i, src_stride, 0, w, inp_size_bits, tmplo, tmphi);
        adm_dwt2_hpass(tmplo, tmphi, dst, ind_x, i, 0, (w + 1) / 2, dst_stride);
    }
}

/**
 * Combined ref+distorted 2D Daubechies-2 DWT for ADM scales 1..3 (32-bit pipe).
 *
 * Why one function handles both pictures: the inner loops interleave ref
 * and distorted reads against the same `dwt2_db2_coeffs_lo` / `_hi`
 * coefficients so they stay live in registers across both pictures, and
 * both share the index tables `ind_y` / `ind_x` (which encode the symmetric
 * edge-mirror that `dwt2_src_indices_filt()` populates per scale).
 *
 * Per-scale rounding comes from `i4_dwt2_round()`. Scale 0 is NOT handled
 * here — the caller (`integer_compute_adm`) routes scale 0 through
 * `s->dwt2_8` / `s->dwt2_16` instead, because scale 0 reads the source
 * picture (8/16-bit) while scales 1..3 read prior 32-bit DWT output.
 *
 * Dispatched via `AdmState::adm_dwt2_s123_combined` (scalar / AVX2 /
 * AVX-512); see `init()` below for the runtime selection.
 */
static void adm_dwt2_s123_combined(const int32_t *i4_ref_scale, const int32_t *i4_curr_dis,
                                   AdmBuffer *buf, int w, int h, int ref_stride, int dis_stride,
                                   int dst_stride, int scale)
{
    const I4Dwt2Round r = i4_dwt2_round(scale);

    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int32_t *tmp = buf->tmp_ref;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        i4_dwt2_vpass(i4_ref_scale, i4_curr_dis, ind_y, i, ref_stride, dis_stride, w, 0, w, tmp,
                      r.add_vp, r.shift_vp);
        i4_dwt2_hpass(tmp, &buf->i4_ref_dwt2, &buf->i4_dis_dwt2, ind_x, i, w, 0, (w + 1) / 2,
                      dst_stride, r.add_hp, r.shift_hp);
    }
}

/* ------------------------------------------------------------------------- */
/* Per-frame driver                                                          */
/* ------------------------------------------------------------------------- */

/* Numerator / denominator / AIM-numerator of one DWT scale. */
typedef struct AdmScaleScores {
    float num;
    float den;
    float aim_num;
} AdmScaleScores;

typedef struct AdmResult {
    double score;
    double score_num;
    double score_den;
    double score_aim;
    double scores[8]; /* per scale: [2 * s] numerator, [2 * s + 1] denominator */
} AdmResult;

/* Scale 0: 16-bit DWT of the source pictures, then decouple / CSF / CM. With
 * `adm_skip_scale0` only the low-pass half of the DWT runs (the next scale
 * needs band_a) and the denominator is seeded with 1e-10 to keep the
 * eventual division well-defined. */
static void integer_adm_scale0(const AdmState *s, const VmafPicture *ref_pic,
                               const VmafPicture *dis_pic, int w, int h, size_t ref_stride,
                               size_t dis_stride, size_t buf_stride, AdmScaleScores *sc)
{
    AdmBuffer *buf = (AdmBuffer *)&s->buf;

    if (s->adm_skip_scale0) {
        // skip scale 0 by downsampling by 2 using low-pass filters in DWT2
        if (ref_pic->bpc == 8) {
            adm_dwt2_8_lo(ref_pic->data[0], &buf->ref_dwt2, buf, w, h, (int)ref_stride,
                          (int)buf_stride);
            adm_dwt2_8_lo(dis_pic->data[0], &buf->dis_dwt2, buf, w, h, (int)dis_stride,
                          (int)buf_stride);
        } else {
            adm_dwt2_16_lo(ref_pic->data[0], &buf->ref_dwt2, buf, w, h, (int)ref_stride,
                           (int)buf_stride, ref_pic->bpc);
            adm_dwt2_16_lo(dis_pic->data[0], &buf->dis_dwt2, buf, w, h, (int)dis_stride,
                           (int)buf_stride, dis_pic->bpc);
        }
        i16_to_i32(&buf->ref_dwt2, &buf->i4_ref_dwt2, w, h, (int)buf_stride);
        i16_to_i32(&buf->dis_dwt2, &buf->i4_dis_dwt2, w, h, (int)buf_stride);
        sc->den = 1e-10; // avoid divide by zero
        return;
    }

    if (ref_pic->bpc == 8) {
        s->dwt2_8(ref_pic->data[0], &buf->ref_dwt2, buf, w, h, (int)ref_stride, (int)buf_stride);
        s->dwt2_8(dis_pic->data[0], &buf->dis_dwt2, buf, w, h, (int)dis_stride, (int)buf_stride);
    } else {
        s->dwt2_16(ref_pic->data[0], &buf->ref_dwt2, buf, w, h, (int)ref_stride, (int)buf_stride,
                   ref_pic->bpc);
        s->dwt2_16(dis_pic->data[0], &buf->dis_dwt2, buf, w, h, (int)dis_stride, (int)buf_stride,
                   dis_pic->bpc);
    }
    i16_to_i32(&buf->ref_dwt2, &buf->i4_ref_dwt2, w, h, (int)buf_stride);
    i16_to_i32(&buf->dis_dwt2, &buf->i4_dis_dwt2, w, h, (int)buf_stride);

    const int w2 = (w + 1) / 2;
    const int h2 = (h + 1) / 2;
    const int stride = (int)buf_stride;

    s->adm_decouple(buf, w2, h2, stride, s->adm_enhn_gain_limit, div_lookup);
    sc->den = s->adm_csf_den_scale(&buf->ref_dwt2, w2, h2, stride, s->adm_norm_view_dist,
                                   s->adm_ref_display_height, s->adm_csf_mode, s->adm_csf_scale,
                                   s->adm_csf_diag_scale, s->adm_noise_weight);
    s->adm_csf(buf, w2, h2, stride, s->adm_norm_view_dist, s->adm_ref_display_height,
               s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale, false);
    sc->num = s->adm_cm(buf, w2, h2, stride, stride, s->adm_norm_view_dist,
                        s->adm_ref_display_height, s->adm_csf_mode, s->adm_csf_scale,
                        s->adm_csf_diag_scale, s->adm_noise_weight, s->adm_p_norm, false);
    if (!s->adm_skip_aim) {
        s->adm_csf(buf, w2, h2, stride, s->adm_norm_view_dist, s->adm_ref_display_height,
                   s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale, true);
        sc->aim_num = s->adm_cm(buf, w2, h2, stride, stride, s->adm_norm_view_dist,
                                s->adm_ref_display_height, s->adm_csf_mode, s->adm_csf_scale,
                                s->adm_csf_diag_scale, 0.0, s->adm_p_norm, true);
    }
}

/* Scales 1..3: 32-bit DWT of the previous scale's band_a, then the i4
 * decouple / CSF / CM twins. */
static void integer_adm_scale_s123(const AdmState *s, const int32_t *i4_ref, const int32_t *i4_dis,
                                   int w, int h, size_t ref_stride, size_t dis_stride,
                                   size_t buf_stride, int scale, AdmScaleScores *sc)
{
    AdmBuffer *buf = (AdmBuffer *)&s->buf;
    const int stride = (int)buf_stride;

    s->adm_dwt2_s123_combined(i4_ref, i4_dis, buf, w, h, (int)ref_stride, (int)dis_stride, stride,
                              scale);

    const int w2 = (w + 1) / 2;
    const int h2 = (h + 1) / 2;

    s->adm_decouple_s123(buf, w2, h2, stride, s->adm_enhn_gain_limit, div_lookup);
    sc->den = s->adm_csf_den_s123(&buf->i4_ref_dwt2, scale, w2, h2, stride, s->adm_norm_view_dist,
                                  s->adm_ref_display_height, s->adm_csf_mode, s->adm_csf_scale,
                                  s->adm_csf_diag_scale, s->adm_noise_weight);
    s->i4_adm_csf(buf, scale, w2, h2, stride, s->adm_norm_view_dist, s->adm_ref_display_height,
                  s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale, false);
    sc->num = s->i4_adm_cm(buf, w2, h2, stride, stride, scale, s->adm_norm_view_dist,
                           s->adm_ref_display_height, s->adm_csf_mode, s->adm_csf_scale,
                           s->adm_csf_diag_scale, s->adm_noise_weight, s->adm_p_norm, false);
    if (!s->adm_skip_aim) {
        s->i4_adm_csf(buf, scale, w2, h2, stride, s->adm_norm_view_dist, s->adm_ref_display_height,
                      s->adm_csf_mode, s->adm_csf_scale, s->adm_csf_diag_scale, true);
        sc->aim_num = s->i4_adm_cm(buf, w2, h2, stride, stride, scale, s->adm_norm_view_dist,
                                   s->adm_ref_display_height, s->adm_csf_mode, s->adm_csf_scale,
                                   s->adm_csf_diag_scale, 0.0, s->adm_p_norm, true);
    }
}

/**
 * Top-level ADM (Detail Loss Metric) computation over the 4 DWT scales.
 *
 * Non-obvious behaviour worth documenting for future maintainers:
 *
 * - `numden_limit` scales the precision floor with picture area
 *   (`1e-10 * w*h / (1920*1080)`). Below this threshold both `num` and
 *   `den` are clamped to zero so a tiny denominator near full picture
 *   black does not blow up `score = num/den` into +Inf. This is why a
 *   uniform-black 64x64 frame returns `score = 1.0` (the `den == 0.0`
 *   branch below).
 * - `adm_skip_scale0` short-circuits scale 0 (see integer_adm_scale0).
 *   This is the ADM-only fast-path; consumers that ignore the scale-0
 *   score still get a valid `score_aim`.
 * - Output ordering: `scores[2*scale + 0]` carries `num_scale`,
 *   `scores[2*scale + 1]` carries `den_scale` for each of the 4 scales,
 *   matching the per-scale `integer_adm_scaleN` features emitted by
 *   `extract()` below.
 */
/* Element stride of a source plane: bytes for 8-bit input, uint16 units
 * otherwise. `bpc` is the reference picture's depth for both planes. */
static size_t adm_src_stride(const VmafPicture *pic, unsigned bpc)
{
    return (bpc == 8) ? pic->stride[0] : pic->stride[0] >> 1;
}

/* Clamp the summed numerator / denominator to the area-scaled precision
 * floor and form the DLM and AIM scores. */
static int adm_result_finalise(AdmResult *res, double num, double den, double aim_num,
                               double numden_limit, unsigned index)
{
    int err = vmaf_adm_floor_pair_named("integer_adm", index, num, den, numden_limit, &num, &den);
    if (err)
        return err;

    const double pairs[4] = {num, den, aim_num, den};
    double ratios[2];
    err = vmaf_adm_scale_ratios(pairs, 2u, ratios);
    if (err) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "integer_adm: undefined or non-finite aggregate at frame %u "
                 "(num=%g den=%g aim_num=%g)\n",
                 index, num, den, aim_num);
        return err;
    }
    res->score = ratios[0];
    res->score_aim = ratios[1];
    res->score_num = num;
    res->score_den = den;
    return 0;
}

static int integer_compute_adm(const AdmState *s, const VmafPicture *ref_pic,
                               const VmafPicture *dis_pic, AdmResult *res, unsigned index)
{
    const AdmBuffer *buf = &s->buf;
    int w = ref_pic->w[0];
    int h = ref_pic->h[0];

    const double numden_limit = 1e-10 * (w * h) / (1920.0 * 1080.0);

    size_t curr_ref_stride = adm_src_stride(ref_pic, ref_pic->bpc);
    size_t curr_dis_stride = adm_src_stride(dis_pic, ref_pic->bpc);
    const size_t buf_stride = buf->ind_size_x >> 2;

    const int32_t *i4_curr_ref_scale = NULL;
    const int32_t *i4_curr_dis_scale = NULL;

    double num = 0;
    double den = 0;
    double aim_num = 0;
    for (unsigned scale = 0; scale < 4; ++scale) {
        AdmScaleScores sc = {0.0, 0.0, 0.0};

        dwt2_src_indices_filt(buf->ind_y, buf->ind_x, w, h);
        if (scale == 0) {
            integer_adm_scale0(s, ref_pic, dis_pic, w, h, curr_ref_stride, curr_dis_stride,
                               buf_stride, &sc);
        } else {
            integer_adm_scale_s123(s, i4_curr_ref_scale, i4_curr_dis_scale, w, h, curr_ref_stride,
                                   curr_dis_stride, buf_stride, (int)scale, &sc);
        }
        w = (w + 1) / 2;
        h = (h + 1) / 2;

        num += sc.num;
        den += sc.den;
        aim_num += sc.aim_num;

        i4_curr_ref_scale = buf->i4_ref_dwt2.band_a;
        i4_curr_dis_scale = buf->i4_dis_dwt2.band_a;

        curr_ref_stride = buf_stride;
        curr_dis_stride = buf_stride;

        res->scores[2 * scale + 0] = sc.num;
        res->scores[2 * scale + 1] = sc.den;
    }

    return adm_result_finalise(res, num, den, aim_num, numden_limit, index);
}

/* ------------------------------------------------------------------------- */
/* Extractor lifecycle                                                       */
/* ------------------------------------------------------------------------- */

static inline void *init_dwt_band(adm_dwt_band_t *band, char *data_top, size_t stride)
{
    band->band_a = (int16_t *)data_top;
    data_top += stride;
    band->band_h = (int16_t *)data_top;
    data_top += stride;
    band->band_v = (int16_t *)data_top;
    data_top += stride;
    band->band_d = (int16_t *)data_top;
    data_top += stride;
    return data_top;
}

static inline void *init_index(int32_t **index, char *data_top, size_t stride)
{
    index[0] = (int32_t *)data_top;
    data_top += stride;
    index[1] = (int32_t *)data_top;
    data_top += stride;
    index[2] = (int32_t *)data_top;
    data_top += stride;
    index[3] = (int32_t *)data_top;
    data_top += stride;
    return data_top;
}

static inline void *i4_init_dwt_band(i4_adm_dwt_band_t *band, char *data_top, size_t stride)
{
    band->band_a = (int32_t *)data_top;
    data_top += stride;
    band->band_h = (int32_t *)data_top;
    data_top += stride;
    band->band_v = (int32_t *)data_top;
    data_top += stride;
    band->band_d = (int32_t *)data_top;
    data_top += stride;
    return data_top;
}

static inline void *init_dwt_band_hvd(adm_dwt_band_t *band, char *data_top, size_t stride)
{
    band->band_a = NULL;
    band->band_h = (int16_t *)data_top;
    data_top += stride;
    band->band_v = (int16_t *)data_top;
    data_top += stride;
    band->band_d = (int16_t *)data_top;
    data_top += stride;
    return data_top;
}

static inline void *i4_init_dwt_band_hvd(i4_adm_dwt_band_t *band, char *data_top, size_t stride)
{
    band->band_a = NULL;
    band->band_h = (int32_t *)data_top;
    data_top += stride;
    band->band_v = (int32_t *)data_top;
    data_top += stride;
    band->band_d = (int32_t *)data_top;
    data_top += stride;
    return data_top;
}

/* Bind the ten stage function pointers to the scalar implementations. */
static void init_dispatch_scalar(AdmState *s)
{
    s->dwt2_8 = adm_dwt2_8;
    s->dwt2_16 = adm_dwt2_16;
    s->adm_decouple = adm_decouple;
    s->adm_decouple_s123 = adm_decouple_s123;
    s->adm_csf = adm_csf;
    s->i4_adm_csf = i4_adm_csf;
    s->adm_csf_den_scale = adm_csf_den_scale;
    s->adm_csf_den_s123 = adm_csf_den_s123;
    s->adm_cm = adm_cm;
    s->i4_adm_cm = i4_adm_cm;
    s->adm_dwt2_s123_combined = adm_dwt2_s123_combined;
}

/* Conditionally upgrade each stage to its AVX2 / AVX-512 / NEON twin from
 * `vmaf_get_cpu_flags()`. The `w % 8` guard on the 8-bit DWT: the AVX2 and
 * NEON 8-bit kernels require a width divisible by 8, otherwise that one
 * slot stays scalar. */
static void init_dispatch_simd(AdmState *s, unsigned w)
{
#if ARCH_X86
    const unsigned flags = vmaf_get_cpu_flags();
    if (flags & VMAF_X86_CPU_FLAG_AVX2) {
        if (!(w % 8)) {
            s->dwt2_8 = adm_dwt2_8_avx2;
        }
        s->dwt2_16 = adm_dwt2_16_avx2;
        s->adm_decouple = adm_decouple_avx2;
        s->adm_decouple_s123 = adm_decouple_s123_avx2;
        if (!s->csf_requires_normalization) {
            s->adm_csf = adm_csf_avx2;
            s->i4_adm_csf = i4_adm_csf_avx2;
            s->adm_cm = adm_cm_avx2;
            s->i4_adm_cm = i4_adm_cm_avx2;
        }
        s->adm_csf_den_scale = adm_csf_den_scale_avx2;
        s->adm_csf_den_s123 = adm_csf_den_s123_avx2;
        s->adm_dwt2_s123_combined = adm_dwt2_s123_combined_avx2;
    }
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        s->dwt2_8 = adm_dwt2_8_avx512;
        s->dwt2_16 = adm_dwt2_16_avx512;
        s->adm_decouple = adm_decouple_avx512;
        s->adm_decouple_s123 = adm_decouple_s123_avx512;
        if (!s->csf_requires_normalization) {
            s->adm_csf = adm_csf_avx512;
            s->i4_adm_csf = i4_adm_csf_avx512;
            s->adm_cm = adm_cm_avx512;
            s->i4_adm_cm = i4_adm_cm_avx512;
        }
        s->adm_csf_den_scale = adm_csf_den_scale_avx512;
        s->adm_csf_den_s123 = adm_csf_den_s123_avx512;
        s->adm_dwt2_s123_combined = adm_dwt2_s123_combined_avx512;
    }
#endif
#elif ARCH_AARCH64
    const unsigned flags = vmaf_get_cpu_flags();
    if (flags & VMAF_ARM_CPU_FLAG_NEON) {
        if (!(w % 8)) {
            s->dwt2_8 = adm_dwt2_8_neon;
        }
        s->adm_decouple = adm_decouple_neon;
    }
#else
    (void)s;
    (void)w;
#endif
}

static void free_buffers(AdmState *s)
{
    if (s->buf.data_buf) {
        aligned_free(s->buf.data_buf);
        s->buf.data_buf = NULL;
    }
    if (s->buf.tmp_ref) {
        aligned_free(s->buf.tmp_ref);
        s->buf.tmp_ref = NULL;
    }
    if (s->buf.buf_x_orig) {
        aligned_free(s->buf.buf_x_orig);
        s->buf.buf_x_orig = NULL;
    }
    if (s->buf.buf_y_orig) {
        aligned_free(s->buf.buf_y_orig);
        s->buf.buf_y_orig = NULL;
    }
}

/* Compute aligned strides and allocate the working buffers. All ADM scratch
 * (6 i16 DWT bands + 6 i32 DWT bands) lives in a single `data_buf`
 * allocation slabbed via the `init_dwt_band` helpers — saves per-frame
 * `aligned_malloc` traffic. Returns 0 or -ENOMEM (caller frees partial
 * state via free_buffers). */
static int init_buffers(AdmState *s, unsigned w, unsigned h)
{
    s->integer_stride = ALIGN_CEIL(w * sizeof(int32_t));
    s->buf.ind_size_x = ALIGN_CEIL(((w + 1) / 2) * sizeof(int32_t));
    s->buf.ind_size_y = ALIGN_CEIL(((h + 1) / 2) * sizeof(int32_t));
    const size_t buf_sz_one = s->buf.ind_size_x * ((h + 1) / 2);

    s->buf.data_buf = aligned_malloc(buf_sz_one * NUM_BUFS_ADM, MAX_ALIGN);
    if (!s->buf.data_buf) {
        return -ENOMEM;
    }
    /* Start every slab from a defined state (Netflix/vmaf 1786bd961 zeroes it
     * in adm_buffer_alloc()). No stage reads a sample it has not written once
     * the DWT index tables are correct, so this is defence in depth: a future
     * out-of-region read yields a reproducible value, not heap residue. */
    (void)memset(s->buf.data_buf, 0, buf_sz_one * NUM_BUFS_ADM);
    s->buf.tmp_ref = aligned_malloc(s->integer_stride * 4, MAX_ALIGN);
    if (!s->buf.tmp_ref) {
        return -ENOMEM;
    }
    s->buf.buf_x_orig = aligned_malloc(s->buf.ind_size_x * 4, MAX_ALIGN);
    if (!s->buf.buf_x_orig) {
        return -ENOMEM;
    }
    s->buf.buf_y_orig = aligned_malloc(s->buf.ind_size_y * 4, MAX_ALIGN);
    if (!s->buf.buf_y_orig) {
        return -ENOMEM;
    }

    void *data_top = s->buf.data_buf;
    data_top = init_dwt_band(&s->buf.ref_dwt2, data_top, buf_sz_one / 2);
    data_top = init_dwt_band(&s->buf.dis_dwt2, data_top, buf_sz_one / 2);
    data_top = init_dwt_band_hvd(&s->buf.decouple_r, data_top, buf_sz_one / 2);
    data_top = init_dwt_band_hvd(&s->buf.decouple_a, data_top, buf_sz_one / 2);
    data_top = init_dwt_band_hvd(&s->buf.csf_a, data_top, buf_sz_one / 2);
    data_top = init_dwt_band_hvd(&s->buf.csf_f, data_top, buf_sz_one / 2);

    data_top = i4_init_dwt_band(&s->buf.i4_ref_dwt2, data_top, buf_sz_one);
    data_top = i4_init_dwt_band(&s->buf.i4_dis_dwt2, data_top, buf_sz_one);
    data_top = i4_init_dwt_band_hvd(&s->buf.i4_decouple_r, data_top, buf_sz_one);
    data_top = i4_init_dwt_band_hvd(&s->buf.i4_decouple_a, data_top, buf_sz_one);
    data_top = i4_init_dwt_band_hvd(&s->buf.i4_csf_a, data_top, buf_sz_one);
    (void)i4_init_dwt_band_hvd(&s->buf.i4_csf_f, data_top, buf_sz_one);

    (void)init_index(s->buf.ind_y, s->buf.buf_y_orig, s->buf.ind_size_y);
    (void)init_index(s->buf.ind_x, s->buf.buf_x_orig, s->buf.ind_size_x);
    return 0;
}

/**
 * `VmafFeatureExtractor::init` for the integer-ADM feature: bind the stage
 * dispatch (scalar, then SIMD upgrades), allocate the working buffers,
 * populate `div_lookup` (used by the decouple stages for fast integer
 * division) and build the feature-name dictionary that drives the JSON
 * output schema. Returns `0` on success, `-EINVAL` for pictures below the
 * 17x17 minimum, `-ENOMEM` on any allocation or dictionary failure with all
 * partial state freed.
 */
static int init(VmafFeatureExtractor *fex, enum VmafPixelFormat pix_fmt, unsigned bpc, unsigned w,
                unsigned h)
{
    AdmState *s = fex->priv;
    (void)pix_fmt;
    (void)bpc;

    const int size_err = adm_frame_size_check("integer_adm", w, h);
    if (size_err) {
        return size_err;
    }

    /* ADR-1191 / ADR-1325: validate the configured CSF weights and determine
     * whether any scale needs the shared normalisation exponent. extract()
     * turns invalid table output into -EINVAL. The result is cached because
     * adm_csf_factors() runs pow()/log10() per scale and cannot change after
     * option parsing. */
    s->csf_config_err = adm_csf_config_check(s);

    init_dispatch_scalar(s);
    init_dispatch_simd(s, w);

    int err = init_buffers(s, w, h);
    if (!err) {
        div_lookup_generator();
        s->feature_name_dict =
            vmaf_feature_name_dict_from_provided_features(fex->provided_features, fex->options, s);
        if (!s->feature_name_dict) {
            err = -ENOMEM;
        }
    }
    if (err) {
        free_buffers(s);
        (void)vmaf_dictionary_free(&s->feature_name_dict);
    }
    return err;
}

static int emit_adm_scores(const AdmState *s, VmafFeatureCollector *feature_collector,
                           const AdmResult *result, double score_adm3, const double scale_scores[4],
                           unsigned index)
{
    VmafNamedScore values[18] = {
        {"VMAF_integer_feature_adm2_score", result->score},
        {"VMAF_integer_feature_aim_score", result->score_aim},
        {"VMAF_integer_feature_adm3_score", score_adm3},
        {"integer_adm_scale0", scale_scores[0]},
        {"integer_adm_scale1", scale_scores[1]},
        {"integer_adm_scale2", scale_scores[2]},
        {"integer_adm_scale3", scale_scores[3]},
    };
    size_t value_count = 7u;
    if (s->debug) {
        static const char *const debug_names[8] = {
            "integer_adm_num_scale0", "integer_adm_den_scale0", "integer_adm_num_scale1",
            "integer_adm_den_scale1", "integer_adm_num_scale2", "integer_adm_den_scale2",
            "integer_adm_num_scale3", "integer_adm_den_scale3",
        };
        values[value_count++] = (VmafNamedScore){"integer_adm", result->score};
        values[value_count++] = (VmafNamedScore){"integer_adm_num", result->score_num};
        values[value_count++] = (VmafNamedScore){"integer_adm_den", result->score_den};
        for (size_t i = 0u; i < 8u; ++i)
            values[value_count++] = (VmafNamedScore){debug_names[i], result->scores[i]};
    }
    return vmaf_feature_emit_finite_scores(feature_collector, s->feature_name_dict, "integer_adm",
                                           values, value_count, index);
}

/* `ref_pic` / `dist_pic` are only read here, but the prototype is
 * `VmafFeatureExtractor::extract` (feature_extractor.h), shared with every
 * extractor including the GPU twins that upload from mutable pictures.
 * ADR-0141 §2 frozen-prototype invariant; see ADR-1141. */
// cppcheck-suppress-begin constParameterCallback
static int extract(VmafFeatureExtractor *fex, VmafPicture *ref_pic, VmafPicture *ref_pic_90,
                   VmafPicture *dist_pic, VmafPicture *dist_pic_90, unsigned index,
                   VmafFeatureCollector *feature_collector)
// cppcheck-suppress-end constParameterCallback
{
    AdmState *s = fex->priv;
    int err = 0;

    (void)ref_pic_90;
    (void)dist_pic_90;

    /* The 16-bit pipeline cannot handle an angular frequency below 1080p at
     * 3H. Keep the reference check shared with every integer-ADM GPU twin. */
    const int geometry_err =
        adm_viewing_geometry_check("adm", s->adm_norm_view_dist, s->adm_ref_display_height);
    if (geometry_err)
        return geometry_err;

    /* The viewing-geometry test above does not catch invalid negative table
     * sentinels from the blended CSFs. init() logged the offending band; this
     * only propagates the verdict. Finite over-range weights were assigned a
     * shared per-scale normalisation exponent instead. */
    if (s->csf_config_err) {
        return s->csf_config_err;
    }

    AdmResult r;
    err = integer_compute_adm(s, ref_pic, dist_pic, &r, index);
    if (err)
        return err;

    /* NaN/Inf guard: every relational operator is false for NaN, so the MAX()
     * below would take its `adm_min_val` arm and publish the floor -- a finite,
     * plausible number -- in place of a non-measurement. `adm_dlm_weight` and
     * `adm_min_val` are both bounded [0, 1] by the option table, so the blend is
     * non-finite exactly when one of these two atoms is. Guard at runtime in
     * both builds and fail the frame (mirrors the y_funque_plus.c finite-atom
     * guard; the float_adm.c twin carries the same check). */
    if (!isfinite(r.score) || !isfinite(r.score_aim)) {
        vmaf_log(VMAF_LOG_LEVEL_WARNING,
                 "integer_adm: non-finite score at frame %u (score=%g score_aim=%g)\n", index,
                 r.score, r.score_aim);
        return -EINVAL;
    }
    double score_adm3 = 0.0;
    err = vmaf_adm3_score_named("integer_adm", index, r.score, r.score_aim, 0, s->adm_dlm_weight,
                                s->adm_min_val, &score_adm3);
    if (err)
        return err;
    double scale_scores[4];
    err = vmaf_adm_scale_ratios_named("integer_adm", index, r.scores, 4u, scale_scores);
    if (err)
        return err;
    return emit_adm_scores(s, feature_collector, &r, score_adm3, scale_scores, index);
}

static int close_fex(VmafFeatureExtractor *fex)
{
    AdmState *s = fex->priv;

    free_buffers(s);
    (void)vmaf_dictionary_free(&s->feature_name_dict);

    return 0;
}

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

// Registration struct consumed by core/src/feature/feature_extractor.cpp
// (via the fex-registry table); must retain external linkage.
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_adm = {
    .name = "adm",
    .init = init,
    .extract = extract,
    .options = options,
    .close = close_fex,
    .priv_size = sizeof(AdmState),
    .provided_features = provided_features,
    /* 16 dispatches per frame (4 scales × 4 stages: DWT + decouple + CSF
     * + reductions). Highest dispatch density of the shipped GPU
     * features — but empirical bench at 576×324 shows DIRECT still beats
     * graph-replay by ~8% even with 16 dispatches; graph setup cost
     * dominates below the 720p area threshold. AUTO + 720p area matches
     * the pre-T7-26 SYCL behaviour byte-for-byte (see ADR-0181). */
    .chars =
        {
            .n_dispatches_per_frame = 16,
            .is_reduction_only = false,
            .min_useful_frame_area = 1280U * 720U,
            .dispatch_hint = VMAF_FEATURE_DISPATCH_AUTO,
        },
};

/* NOLINTEND(modernize-use-nullptr) */
