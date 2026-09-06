/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
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

#include "barten_csf_tools.h"
#include "compat_builtin.h"
#include "cpu.h"
#include "dict.h"
#include "feature_collector.h"
#include "feature_extractor.h"
#include "feature_name.h"
#include "integer_adm.h"
#include "log.h"

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

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

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

/*
 * lambda = 0 (finest scale), 1, 2, 3 (coarsest scale);
 * theta = 0 (ll), 1 (lh - vertical), 2 (hh - diagonal), 3(hl - horizontal).
 */
static inline float dwt_quant_step(const struct dwt_model_params *params, int lambda, int theta,
                                   double adm_norm_view_dist, int adm_ref_display_height)
{
    // Formula (1), page 1165 - display visual resolution (DVR), in pixels/degree
    // of visual angle. This should be 56.55
    float r = adm_norm_view_dist * adm_ref_display_height * M_PI / 180.0;

    // Formula (9), page 1171
    float temp = log10(pow(2.0, lambda + 1) * params->f0 * params->g[theta] / r);
    float Q = 2.0 * params->a * pow(10.0, params->k * (double)temp * temp) /
              dwt_7_9_basis_function_amplitudes[lambda][theta];

    return Q;
}

typedef struct AdmCsfFactors {
    float factor1; /* horizontal and vertical bands */
    float factor2; /* diagonal band */
} AdmCsfFactors;

/* CSF weights of one DWT scale for the selected contrast-sensitivity model.
 * ADM scales run 0..3 while the noise-floor paper numbers them 1..4 (finest
 * to coarsest); `scale` is 0 for the 16-bit pipeline and 1..3 for the
 * 32-bit pipeline. */
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

/**
 * Scale-0 CSF weights converted to fixed point: rfactor[0,1] are scaled by
 * 2^21 and rfactor[2] by 2^23. For adm_norm_view_dist 3.0 and
 * adm_ref_display_height 1080 under ADM_CSF_MODE_WATSON97 the constants are
 * the upstream-tabulated { 36453, 36453, 49417 }.
 */
static void adm_csf_rfactor_scale0(const float rfactor1[3], double adm_norm_view_dist,
                                   int adm_ref_display_height, int adm_csf_mode,
                                   uint16_t i_rfactor[3])
{
    if (fabs(adm_norm_view_dist * adm_ref_display_height -
             DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT) < 1.0e-8 &&
        adm_csf_mode == ADM_CSF_MODE_WATSON97) {
        i_rfactor[0] = 36453;
        i_rfactor[1] = 36453;
        i_rfactor[2] = 49417;
    } else {
        const double pow2_21 = pow(2, 21);
        const double pow2_23 = pow(2, 23);
        i_rfactor[0] = (uint16_t)(rfactor1[0] * pow2_21);
        i_rfactor[1] = (uint16_t)(rfactor1[1] * pow2_21);
        i_rfactor[2] = (uint16_t)(rfactor1[2] * pow2_23);
    }
}

/* ------------------------------------------------------------------------- */
/* Frame-border bookkeeping                                                  */
/* ------------------------------------------------------------------------- */

typedef struct AdmBorder {
    int left;
    int top;
    int right;
    int bottom;
} AdmBorder;

/* Region that takes part in the ADM reductions: ADM_BORDER_FACTOR of each
 * frame edge is excluded. */
static AdmBorder adm_border(int w, int h)
{
    AdmBorder b;
    b.left = (int)(w * ADM_BORDER_FACTOR - 0.5);
    b.top = (int)(h * ADM_BORDER_FACTOR - 0.5);
    b.right = w - b.left;
    b.bottom = h - b.top;
    return b;
}

/* The same region widened by one filter tap on each side (-1 / +2) and
 * clamped to the frame, for the decouple and CSF stages that feed the
 * 3x3 contrast-masking neighbourhood. */
static AdmBorder adm_border_filt(int w, int h)
{
    AdmBorder b;
    b.left = (int)(w * ADM_BORDER_FACTOR - 0.5 - 1); // -1 for filter tap
    b.top = (int)(h * ADM_BORDER_FACTOR - 0.5 - 1);
    b.right = w - b.left + 2; // +2 for filter tap
    b.bottom = h - b.top + 2;
    if (b.left < 0) {
        b.left = 0;
    }
    if (b.right > w) {
        b.right = w;
    }
    if (b.top < 0) {
        b.top = 0;
    }
    if (b.bottom > h) {
        b.bottom = h;
    }
    return b;
}

/* Half-LSB rounding term for a right shift by `shift`. Guarded so that a
 * zero shift (log2 of a very small scaled width) does not wrap `shift - 1`
 * to UINT32_MAX and cast +Inf to an integer. */
static inline uint32_t adm_half_shift(uint32_t shift)
{
    return (shift > 0u) ? (uint32_t)pow(2.0, (double)(shift - 1u)) : 0u;
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
    for (unsigned i = 1; i < n_half - 2; ++i) { /* i : 1 to  n_half - 3*/
        const int ind1 = 2 * i;
        ind[0][i] = ind1 - 1;
        ind[1][i] = ind1;
        ind[2][i] = ind1 + 1;
        ind[3][i] = ind1 + 2;
    }
    for (unsigned i = n_half - 2; i < n_half; ++i) { /* i : n_half - 3 to  n_half */
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

/* Determine if angle between (oh,ov) and (th,tv) is less than 1 degree.
 * Given that u is the angle (oh,ov) and v is the angle (th,tv), this can
 * be done by testing the inequvality.
 *
 * { (u.v.) >= 0 } AND { (u.v)^2 >= cos(1deg)^2 * ||u||^2 * ||v||^2 }
 *
 * Proof:
 *
 * cos(theta) = (u.v) / (||u|| * ||v||)
 *
 * IF u.v >= 0 THEN
 *   cos(theta)^2 = (u.v)^2 / (||u||^2 * ||v||^2)
 *   (u.v)^2 = cos(theta)^2 * ||u||^2 * ||v||^2
 *
 *   IF |theta| < 1deg THEN
 *     (u.v)^2 >= cos(1deg)^2 * ||u||^2 * ||v||^2
 *   END
 * ELSE
 *   |theta| > 90deg
 * END
 *
 * angle_flag is calculated in floating-point by converting fixed-point
 * variables back to floating-point.
 */
static inline int adm_angle_flag(int64_t ot_dp, int64_t o_mag_sq, int64_t t_mag_sq,
                                 float cos_1deg_sq)
{
    return (((float)ot_dp / 4096.0) >= 0.0f) &&
           (((float)ot_dp / 4096.0) * ((float)ot_dp / 4096.0) >=
            cos_1deg_sq * ((float)o_mag_sq / 4096.0) * ((float)t_mag_sq / 4096.0));
}

/**
 * One band of the 16-bit decouple: restore `o` toward `t` with the Q15
 * ratio k = t / o taken from the reciprocal table (division carried out as
 * a multiplication), then bound the enhancement gain.
 *
 * `lut` is `div_lookup`; index +32768 recentres the signed operand.
 */
static inline int16_t adm_decouple_band(const int32_t *lut, double gain, int angle_flag, int16_t o,
                                        int16_t t)
{
    const int32_t tmp_k = (o == 0) ? 32768 : (((int64_t)lut[o + 32768] * t) + 16384) >> 15;
    const int32_t k = tmp_k < 0 ? 0 : (tmp_k > 32768 ? 32768 : tmp_k);

    /**
     * k is in Q15 type and o is in Q16 type hence shifted by 15 to make
     * the result Q16
     */
    int16_t rst = ((k * o) + 16384) >> 15;
    const float rst_f = ((float)k / 32768) * ((float)o / 64);

    if (angle_flag && (rst_f > 0.)) {
        rst = MIN((rst * gain), t);
    }
    if (angle_flag && (rst_f < 0.)) {
        rst = MAX((rst * gain), t);
    }
    return rst;
}

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
    const float cos_1deg_sq = cos(1.0 * M_PI / 180.0) * cos(1.0 * M_PI / 180.0);

    const adm_dwt_band_t *ref = &buf->ref_dwt2;
    const adm_dwt_band_t *dis = &buf->dis_dwt2;
    const adm_dwt_band_t *r = &buf->decouple_r;
    const adm_dwt_band_t *a = &buf->decouple_a;

    /* The computation of the score is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);

    for (int i = b.top; i < b.bottom; ++i) {
        for (int j = b.left; j < b.right; ++j) {
            const ptrdiff_t idx = (ptrdiff_t)i * stride + j;
            const int16_t oh = ref->band_h[idx];
            const int16_t ov = ref->band_v[idx];
            const int16_t od = ref->band_d[idx];
            const int16_t th = dis->band_h[idx];
            const int16_t tv = dis->band_v[idx];
            const int16_t td = dis->band_d[idx];

            const int angle_flag = adm_angle_flag((int64_t)oh * th + (int64_t)ov * tv,
                                                  (int64_t)oh * oh + (int64_t)ov * ov,
                                                  (int64_t)th * th + (int64_t)tv * tv, cos_1deg_sq);

            const int16_t rst_h = adm_decouple_band(lut, gain, angle_flag, oh, th);
            const int16_t rst_v = adm_decouple_band(lut, gain, angle_flag, ov, tv);
            const int16_t rst_d = adm_decouple_band(lut, gain, angle_flag, od, td);

            r->band_h[idx] = rst_h;
            r->band_v[idx] = rst_v;
            r->band_d[idx] = rst_d;

            a->band_h[idx] = th - rst_h;
            a->band_v[idx] = tv - rst_v;
            a->band_d[idx] = td - rst_d;
        }
    }
}

static inline uint16_t get_best15_from32(uint32_t temp, int *x)
{
    int k = __builtin_clz(temp); //built in for intel
    k = 17 - k;
    temp = (temp + (1 << (k - 1))) >> k;
    *x = k;
    return temp;
}

/**
 * One band of the 32-bit (scales 1..3) decouple.
 *
 * Division t/o is carried using the lookup table and converted to a
 * multiplication; int64 / int32 is converted to multiplication using the
 * following method
 * num /den :
 * DenAbs = Abs(den)
 * MSBDen = MSB(DenAbs)     (gives position of first 1 bit form msb side)
 * If (DenAbs < (1 << 15))
 *      Round = (1<<14)
 *      Score = (num *  div_lookup[den] + Round ) >> 15
 * else
 *      RoundD  = (1<< (16 - MSBDen))
 *      Round   = (1<< (14 + (17 - MSBDen))
 *      Score   = (num * div_lookup[(DenAbs + RoundD )>>(17 - MSBDen)]*sign(Denominator) + Round)
 *                  >> ((15 + (17 - MSBDen))
 */
static inline int32_t adm_decouple_band_s123(const int32_t *lut, double gain, int angle_flag,
                                             int32_t o, int32_t t)
{
    int32_t k_shift = 0;
    const uint32_t abs_o = abs(o);
    const int8_t k_sign = (o < 0 ? -1 : 1);
    const uint16_t k_msb = (abs_o < (32768) ? abs_o : get_best15_from32(abs_o, &k_shift));

    /* Use 1u to avoid signed-integer left-shift UB when k_shift is large
     * (shift amount can reach 17, making 1<<31 overflow signed int).
     * The result is immediately widened into the int64_t expression. */
    const int64_t tmp_k =
        (o == 0) ?
            32768 :
            (((int64_t)lut[k_msb + 32768] * t) * (k_sign) + (int64_t)(1u << (14 + k_shift))) >>
                (15 + k_shift);
    const int64_t k = tmp_k < 0 ? 0 : (tmp_k > 32768 ? 32768 : tmp_k);

    int32_t rst = ((k * o) + 16384) >> 15;
    const float rst_f = ((float)k / 32768) * ((float)o / 64);

    if (angle_flag && (rst_f > 0.)) {
        rst = MIN((rst * gain), t);
    }
    if (angle_flag && (rst_f < 0.)) {
        rst = MAX((rst * gain), t);
    }
    return rst;
}

/* See adm_decouple above: `lut` keeps the mutable `int32_t *` of the shared
 * dispatch signature (`adm_decouple_s123_avx2` / `_avx512`). ADR-0141 §2
 * load-bearing invariant; see ADR-1141. */
// cppcheck-suppress constParameterCallback
// NOLINTNEXTLINE(readability-non-const-parameter) — ADR-0141 / ADR-1141
static void adm_decouple_s123(AdmBuffer *buf, int w, int h, int stride, double gain, int32_t *lut)
{
    const float cos_1deg_sq = cos(1.0 * M_PI / 180.0) * cos(1.0 * M_PI / 180.0);

    const i4_adm_dwt_band_t *ref = &buf->i4_ref_dwt2;
    const i4_adm_dwt_band_t *dis = &buf->i4_dis_dwt2;
    const i4_adm_dwt_band_t *r = &buf->i4_decouple_r;
    const i4_adm_dwt_band_t *a = &buf->i4_decouple_a;

    /* The computation of the score is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);

    for (int i = b.top; i < b.bottom; ++i) {
        for (int j = b.left; j < b.right; ++j) {
            const ptrdiff_t idx = (ptrdiff_t)i * stride + j;
            const int32_t oh = ref->band_h[idx];
            const int32_t ov = ref->band_v[idx];
            const int32_t od = ref->band_d[idx];
            const int32_t th = dis->band_h[idx];
            const int32_t tv = dis->band_v[idx];
            const int32_t td = dis->band_d[idx];

            const int angle_flag = adm_angle_flag((int64_t)oh * th + (int64_t)ov * tv,
                                                  (int64_t)oh * oh + (int64_t)ov * ov,
                                                  (int64_t)th * th + (int64_t)tv * tv, cos_1deg_sq);

            const int32_t rst_h = adm_decouple_band_s123(lut, gain, angle_flag, oh, th);
            const int32_t rst_v = adm_decouple_band_s123(lut, gain, angle_flag, ov, tv);
            const int32_t rst_d = adm_decouple_band_s123(lut, gain, angle_flag, od, td);

            r->band_h[idx] = rst_h;
            r->band_v[idx] = rst_v;
            r->band_d[idx] = rst_d;

            a->band_h[idx] = th - rst_h;
            a->band_v[idx] = tv - rst_v;
            a->band_d[idx] = td - rst_d;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Contrast sensitivity filtering                                            */
/* ------------------------------------------------------------------------- */

static void adm_csf(AdmBuffer *buf, int w, int h, int stride, double adm_norm_view_dist,
                    int adm_ref_display_height, int adm_csf_mode, double adm_csf_scale,
                    double adm_csf_diag_scale, bool measure_aim)
{
    const adm_dwt_band_t *src = measure_aim ? &buf->decouple_r : &buf->decouple_a;
    const adm_dwt_band_t *dst = measure_aim ? &buf->csf_f : &buf->csf_a;
    const adm_dwt_band_t *flt = measure_aim ? &buf->csf_a : &buf->csf_f;

    const int16_t *const src_angles[3] = {src->band_h, src->band_v, src->band_d};
    int16_t *const dst_angles[3] = {dst->band_h, dst->band_v, dst->band_d};
    int16_t *const flt_angles[3] = {flt->band_h, flt->band_v, flt->band_d};

    // 0 is scale zero passed to dwt_quant_step
    const AdmCsfFactors f = adm_csf_factors(0, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
    uint16_t i_rfactor[3];
    adm_csf_rfactor_scale0(rfactor1, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                           i_rfactor);

    /**
     * Shifts pending from previous stage is 6
     * hence variables multiplied by i_rfactor[0,1] has to be shifted by 21+6=27 to convert
     * into floating-point. But shifted by 15 to make it Q16
     * and variables multiplied by i_factor[2] has to be shifted by 23+6=29 to convert into
     * floating-point. But shifted by 17 to make it Q16
     * Hence remaining shifts after shifting by i_shifts is 12 to make it equivalent to
     * floating-point
     */
    const uint8_t i_shifts[3] = {15, 15, 17};
    const uint16_t i_shiftsadd[3] = {16384, 16384, 65535};
    const uint16_t FIX_ONE_BY_30 = 4369; //(1/30)*2^17

    /* The computation of the csf values is not required for the regions which
     * lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);

    for (int theta = 0; theta < 3; ++theta) {
        const int16_t *src_ptr = src_angles[theta];
        int16_t *dst_ptr = dst_angles[theta];
        int16_t *flt_ptr = flt_angles[theta];

        for (int i = b.top; i < b.bottom; ++i) {
            const ptrdiff_t offset = (ptrdiff_t)i * stride;

            for (int j = b.left; j < b.right; ++j) {
                const int32_t dst_val = i_rfactor[theta] * (int32_t)src_ptr[offset + j];
                const int16_t i16_dst_val =
                    ((int16_t)((dst_val + i_shiftsadd[theta]) >> i_shifts[theta]));
                dst_ptr[offset + j] = i16_dst_val;
                flt_ptr[offset + j] =
                    ((int16_t)(((FIX_ONE_BY_30 * abs((int32_t)i16_dst_val)) + 2048) >> 12));
            }
        }
    }
}

/* Right-shift budgets of the 32-bit CSF outputs (scales 1..3). */
static const uint32_t i4_shift_dst[3] = {28, 28, 28};
static const uint32_t i4_shift_flt[3] = {32, 32, 32};

/**
 * Rounding terms paired with i4_shift_dst / i4_shift_flt.
 *
 * Netflix#955 / ADR-0155: `1u << 31` is `0x80000000`, which wraps
 * to `-2147483648` on assignment into `int32_t add_bef_shift_flt[]`.
 * The rounding term for scales 1-3 is therefore sign-negated;
 * every downstream `(prod + add_bef_shift) >> 32` subtracts 2^31
 * instead of adding it. The buggy arithmetic is encoded in the
 * Netflix golden assertions (project hard rule #1 /
 * ADR-0024) — do NOT widen `add_bef_shift_flt[]` to `uint32_t`
 * or `int64_t` without a coordinated Netflix-side golden-number
 * update. See docs/adr/0155-adm-i4-rounding-deferred-netflix-955.md.
 */
static void i4_adm_round_terms(int32_t add_bef_shift_dst[3], int32_t add_bef_shift_flt[3])
{
    for (unsigned idx = 0; idx < 3; ++idx) {
        /* UBSan: cast unsigned shift result to int32_t explicitly; the wrap
         * for i4_shift_flt[idx]==32 is intentional per ADR-0155 (Netflix#955). */
        add_bef_shift_dst[idx] = (int32_t)(1u << (i4_shift_dst[idx] - 1));
        add_bef_shift_flt[idx] = (int32_t)(1u << (i4_shift_flt[idx] - 1));
    }
}

static void i4_adm_csf(AdmBuffer *buf, int scale, int w, int h, int stride,
                       double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                       double adm_csf_scale, double adm_csf_diag_scale, bool measure_aim)
{
    const i4_adm_dwt_band_t *src = measure_aim ? &buf->i4_decouple_r : &buf->i4_decouple_a;
    const i4_adm_dwt_band_t *dst = measure_aim ? &buf->i4_csf_f : &buf->i4_csf_a;
    const i4_adm_dwt_band_t *flt = measure_aim ? &buf->i4_csf_a : &buf->i4_csf_f;

    const int32_t *const src_angles[3] = {src->band_h, src->band_v, src->band_d};
    int32_t *const dst_angles[3] = {dst->band_h, dst->band_v, dst->band_d};
    int32_t *const flt_angles[3] = {flt->band_h, flt->band_v, flt->band_d};

    const AdmCsfFactors f = adm_csf_factors(scale, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};

    //i_rfactor in fixed-point
    const double pow2_32 = pow(2, 32);
    const uint32_t i_rfactor[3] = {(uint32_t)(rfactor1[0] * pow2_32),
                                   (uint32_t)(rfactor1[1] * pow2_32),
                                   (uint32_t)(rfactor1[2] * pow2_32)};

    const uint32_t FIX_ONE_BY_30 = 143165577;
    int32_t add_bef_shift_dst[3];
    int32_t add_bef_shift_flt[3];
    i4_adm_round_terms(add_bef_shift_dst, add_bef_shift_flt);

    /* The computation of the csf values is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border_filt(w, h);

    for (int theta = 0; theta < 3; ++theta) {
        const int32_t *src_ptr = src_angles[theta];
        int32_t *dst_ptr = dst_angles[theta];
        int32_t *flt_ptr = flt_angles[theta];

        for (int i = b.top; i < b.bottom; ++i) {
            const ptrdiff_t offset = (ptrdiff_t)i * stride;

            for (int j = b.left; j < b.right; ++j) {
                const int32_t dst_val =
                    (int32_t)(((i_rfactor[theta] * (int64_t)src_ptr[offset + j]) +
                               add_bef_shift_dst[scale - 1]) >>
                              i4_shift_dst[scale - 1]);
                dst_ptr[offset + j] = dst_val;
                flt_ptr[offset + j] = (int32_t)((((int64_t)FIX_ONE_BY_30 * abs(dst_val)) +
                                                 add_bef_shift_flt[scale - 1]) >>
                                                i4_shift_flt[scale - 1]);
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Denominator (reference-energy) reductions                                 */
/* ------------------------------------------------------------------------- */

/* Cube-root finalisation shared by both denominator reductions: the per-band
 * CSF energies are converted to floating point and offset by the noise floor
 * of the reduced area. */
static float adm_den_scale_finalise(const double csf[3], int area, double adm_noise_weight)
{
    const float powf_add = powf(area * adm_noise_weight, 1.0f / 3.0f);
    const float den_scale_h = powf(csf[0], 1.0f / 3.0f) + powf_add;
    const float den_scale_v = powf(csf[1], 1.0f / 3.0f) + powf_add;
    const float den_scale_d = powf(csf[2], 1.0f / 3.0f) + powf_add;

    return (den_scale_h + den_scale_v + den_scale_d);
}

/**
 * Scale-0 denominator: cubed reference-band energy inside the border.
 *
 * The rfactor is multiplied at the end after cubing, because
 * d+ = (a[i]^3)*(r^3) is equivalent to d+=a[i]^3 and d=d*(r^3).
 *
 * max_value of h^3, v^3, d^3 is 1.205624776 * —10^13; accum_h can hold till
 * 1.844674407 * —10^19 and its maximum is reached when it is
 * 2^20 * max(h^3). Therefore accum_h,v,d is shifted based on width and
 * height subtracted by 20.
 *
 * accum_h,v,d is converted to floating-point for score calculation: 6 bits
 * are yet to be shifted from the previous stage (after dwt), hence after
 * cubing 18 bits are to be shifted, i.e. the final shift is 18-shift_accum.
 */
static float adm_csf_den_scale(const adm_dwt_band_t *src, int w, int h, int src_stride,
                               double adm_norm_view_dist, int adm_ref_display_height,
                               int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                               double adm_noise_weight)
{
    const AdmCsfFactors f = adm_csf_factors(0, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const float rfactor[3] = {f.factor1, f.factor1, f.factor2};

    uint64_t accum_h = 0;
    uint64_t accum_v = 0;
    uint64_t accum_d = 0;

    /* The computation of the denominator scales is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border(w, h);
    const int area = (b.bottom - b.top) * (b.right - b.left);

    int32_t shift_accum = (int32_t)ceil(log2(area) - 20);
    shift_accum = shift_accum > 0 ? shift_accum : 0;
    const int32_t add_shift_accum = shift_accum > 0 ? (1 << (shift_accum - 1)) : 0;

    const int16_t *src_h = src->band_h + (ptrdiff_t)b.top * src_stride;
    const int16_t *src_v = src->band_v + (ptrdiff_t)b.top * src_stride;
    const int16_t *src_d = src->band_d + (ptrdiff_t)b.top * src_stride;
    for (int i = b.top; i < b.bottom; ++i) {
        uint64_t accum_inner_h = 0;
        uint64_t accum_inner_v = 0;
        uint64_t accum_inner_d = 0;
        for (int j = b.left; j < b.right; ++j) {
            const uint16_t h_abs = (uint16_t)abs(src_h[j]);
            const uint16_t v_abs = (uint16_t)abs(src_v[j]);
            const uint16_t d_abs = (uint16_t)abs(src_d[j]);

            accum_inner_h += ((uint64_t)h_abs * h_abs) * h_abs;
            accum_inner_v += ((uint64_t)v_abs * v_abs) * v_abs;
            accum_inner_d += ((uint64_t)d_abs * d_abs) * d_abs;
        }
        accum_h += (accum_inner_h + add_shift_accum) >> shift_accum;
        accum_v += (accum_inner_v + add_shift_accum) >> shift_accum;
        accum_d += (accum_inner_d + add_shift_accum) >> shift_accum;
        src_h += src_stride;
        src_v += src_stride;
        src_d += src_stride;
    }
    const double shift_csf = pow(2, (18 - shift_accum));
    const double csf[3] = {(double)(accum_h / shift_csf) * pow(rfactor[0], 3),
                           (double)(accum_v / shift_csf) * pow(rfactor[1], 3),
                           (double)(accum_d / shift_csf) * pow(rfactor[2], 3)};

    return adm_den_scale_finalise(csf, area, adm_noise_weight);
}

/* Rounded ((x^2 + add_sq) >> shift_sq) * x, then rounded and shifted by
 * shift_cub: the per-sample cube term of the 32-bit denominator. */
static inline uint64_t i4_cube_term(uint32_t x_abs, uint32_t add_shift_sq, uint32_t shift_sq,
                                    uint32_t add_shift_cub, uint32_t shift_cub)
{
    return ((((((uint64_t)x_abs * x_abs) + add_shift_sq) >> shift_sq) * x_abs) + add_shift_cub) >>
           shift_cub;
}

static float adm_csf_den_s123(const i4_adm_dwt_band_t *src, int scale, int w, int h, int src_stride,
                              double adm_norm_view_dist, int adm_ref_display_height,
                              int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                              double adm_noise_weight)
{
    const AdmCsfFactors f = adm_csf_factors(scale, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const float rfactor[3] = {f.factor1, f.factor1, f.factor2};

    uint64_t accum_h = 0;
    uint64_t accum_v = 0;
    uint64_t accum_d = 0;
    const uint32_t shift_sq[3] = {31, 30, 31};
    const uint32_t accum_convert_float[3] = {32, 27, 23};
    const uint32_t add_shift_sq[3] = {1u << shift_sq[0], 1u << shift_sq[1], 1u << shift_sq[2]};

    /* The computation of the denominator scales is not required for the regions
     * which lie outside the frame borders */
    const AdmBorder b = adm_border(w, h);

    const uint32_t shift_cub = (uint32_t)ceil(log2(b.right - b.left));
    const uint32_t add_shift_cub = adm_half_shift(shift_cub);
    const uint32_t shift_accum = (uint32_t)ceil(log2(b.bottom - b.top));
    const uint32_t add_shift_accum = adm_half_shift(shift_accum);

    const int32_t *src_h = src->band_h + (ptrdiff_t)b.top * src_stride;
    const int32_t *src_v = src->band_v + (ptrdiff_t)b.top * src_stride;
    const int32_t *src_d = src->band_d + (ptrdiff_t)b.top * src_stride;
    for (int i = b.top; i < b.bottom; ++i) {
        uint64_t accum_inner_h = 0;
        uint64_t accum_inner_v = 0;
        uint64_t accum_inner_d = 0;
        for (int j = b.left; j < b.right; ++j) {
            const uint32_t h_abs = (uint32_t)abs(src_h[j]);
            const uint32_t v_abs = (uint32_t)abs(src_v[j]);
            const uint32_t d_abs = (uint32_t)abs(src_d[j]);

            accum_inner_h += i4_cube_term(h_abs, add_shift_sq[scale - 1], shift_sq[scale - 1],
                                          add_shift_cub, shift_cub);
            accum_inner_v += i4_cube_term(v_abs, add_shift_sq[scale - 1], shift_sq[scale - 1],
                                          add_shift_cub, shift_cub);
            accum_inner_d += i4_cube_term(d_abs, add_shift_sq[scale - 1], shift_sq[scale - 1],
                                          add_shift_cub, shift_cub);
        }

        accum_h += (accum_inner_h + add_shift_accum) >> shift_accum;
        accum_v += (accum_inner_v + add_shift_accum) >> shift_accum;
        accum_d += (accum_inner_d + add_shift_accum) >> shift_accum;

        src_h += src_stride;
        src_v += src_stride;
        src_d += src_stride;
    }
    /**
     * All the results are converted to floating-point to calculate the scores
     * For all scales the final shift is 3*shifts from dwt - total shifts done here
     */
    const double shift_csf = pow(2, (accum_convert_float[scale - 1] - shift_accum - shift_cub));
    const double csf[3] = {(double)(accum_h / shift_csf) * pow(rfactor[0], 3),
                           (double)(accum_v / shift_csf) * pow(rfactor[1], 3),
                           (double)(accum_d / shift_csf) * pow(rfactor[2], 3)};

    return adm_den_scale_finalise(csf, (b.bottom - b.top) * (b.right - b.left), adm_noise_weight);
}

/* ------------------------------------------------------------------------- */
/* Contrast masking (numerator) reductions                                   */
/* ------------------------------------------------------------------------- */

/**
 * Masking threshold at (i, j): the 3x3 neighbourhood sum of the CSF-filtered
 * bands, with the centre tap taken from the unfiltered band scaled by 1/15.
 *
 * The row / column before the first edge mirrors to index 1 and the row /
 * column past the last edge clamps to the last index. This is the closed
 * form of the nine upstream ADM_CM_THRESH_S_{0_0, 0_J, 0_W_M_1, I_0, I_J,
 * I_W_M_1, H_M_1_0, H_M_1_J, H_M_1_W_M_1} corner / edge / interior macro
 * variants; the term order matches them one-to-one.
 */
static inline int32_t adm_cm_thresh(int16_t *const *angles, int16_t *const *flt_angles,
                                    int src_stride, int w, int h, int i, int j)
{
    const int i_m1 = (i == 0) ? 1 : i - 1;
    const int i_p1 = (i == h - 1) ? h - 1 : i + 1;
    const int j_m1 = (j == 0) ? 1 : j - 1;
    const int j_p1 = (j == w - 1) ? w - 1 : j + 1;
    int32_t accum = 0;

    for (int theta = 0; theta < 3; ++theta) {
        const int16_t *src_ptr = angles[theta] + (ptrdiff_t)i * src_stride;
        const int16_t *flt_m1 = flt_angles[theta] + (ptrdiff_t)i_m1 * src_stride;
        const int16_t *flt_0 = flt_angles[theta] + (ptrdiff_t)i * src_stride;
        const int16_t *flt_p1 = flt_angles[theta] + (ptrdiff_t)i_p1 * src_stride;
        int32_t sum = 0;
        sum += flt_m1[j_m1];
        sum += flt_m1[j];
        sum += flt_m1[j_p1];
        sum += flt_0[j_m1];
        sum += (int16_t)(((ONE_BY_15 * abs((int32_t)src_ptr[j])) + 2048) >> 12);
        sum += flt_0[j_p1];
        sum += flt_p1[j_m1];
        sum += flt_p1[j];
        sum += flt_p1[j_p1];
        accum += sum;
    }
    return accum;
}

/* 32-bit twin of adm_cm_thresh (upstream I4_ADM_CM_THRESH_S_* macros). */
static inline int32_t i4_adm_cm_thresh(int32_t *const *angles, int32_t *const *flt_angles,
                                       int src_stride, int w, int h, int i, int j,
                                       int32_t add_bef_shift, uint32_t shift)
{
    const int i_m1 = (i == 0) ? 1 : i - 1;
    const int i_p1 = (i == h - 1) ? h - 1 : i + 1;
    const int j_m1 = (j == 0) ? 1 : j - 1;
    const int j_p1 = (j == w - 1) ? w - 1 : j + 1;
    int32_t accum = 0;

    for (int theta = 0; theta < 3; ++theta) {
        const int32_t *src_ptr = angles[theta] + (ptrdiff_t)i * src_stride;
        const int32_t *flt_m1 = flt_angles[theta] + (ptrdiff_t)i_m1 * src_stride;
        const int32_t *flt_0 = flt_angles[theta] + (ptrdiff_t)i * src_stride;
        const int32_t *flt_p1 = flt_angles[theta] + (ptrdiff_t)i_p1 * src_stride;
        int32_t sum = 0;
        sum += flt_m1[j_m1];
        sum += flt_m1[j];
        sum += flt_m1[j_p1];
        sum += flt_0[j_m1];
        sum += (int32_t)((((int64_t)I4_ONE_BY_15 * abs((int32_t)src_ptr[j])) + add_bef_shift) >>
                         shift);
        sum += flt_0[j_p1];
        sum += flt_p1[j_m1];
        sum += flt_p1[j];
        sum += flt_p1[j_p1];
        accum += sum;
    }
    return accum;
}

/* Per-band fixed-point parameters of the contrast-masking cube reduction. */
typedef struct AdmCmBand {
    int32_t shift_sub;
    int32_t add_shift_sq;
    int32_t shift_sq;
    uint32_t add_shift_cub;
    uint32_t shift_cub;
} AdmCmBand;

/* Rounded (|x| - thr)^3 contribution of one band
 * (upstream ADM_CM_ACCUM_ROUND). */
static inline int64_t adm_cm_accum_round(int32_t x, int32_t thr, const AdmCmBand *p)
{
    int32_t v = abs(x) - ((int32_t)(thr) << p->shift_sub);
    v = v < 0 ? 0 : v;
    const int32_t v_sq = (int32_t)((((int64_t)v * v) + p->add_shift_sq) >> p->shift_sq);
    return (((int64_t)v_sq * v) + p->add_shift_cub) >> p->shift_cub;
}

/* 32-bit twin (upstream I4_ADM_CM_ACCUM_ROUND): the threshold is already in
 * the band's Q format, so it is right-shifted by shift_sub instead. */
static inline int64_t i4_adm_cm_accum_round(int32_t x, int32_t thr, const AdmCmBand *p)
{
    int32_t v = abs(x) - (thr >> p->shift_sub);
    v = v < 0 ? 0 : v;
    const int32_t v_sq = (int32_t)((((int64_t)v * v) + p->add_shift_sq) >> p->shift_sq);
    return (((int64_t)v_sq * v) + p->add_shift_cub) >> p->shift_cub;
}

/* Fold a row accumulator into the frame accumulator (shift is done based
 * on height) and reset it for the next row. */
static inline void adm_cm_fold(int64_t inner[3], int64_t accum[3], uint32_t add_shift_inner_accum,
                               uint32_t shift_inner_accum)
{
    for (int k = 0; k < 3; ++k) {
        accum[k] += (inner[k] + add_shift_inner_accum) >> shift_inner_accum;
        inner[k] = 0;
    }
}

/* p-norm of the accumulated contrast plus the noise floor of the area. */
static inline float adm_num_scale(float f_accum, int area, double adm_noise_weight,
                                  float p_norm_exp)
{
    return powf(f_accum, p_norm_exp) + powf(area * adm_noise_weight, p_norm_exp);
}

/* Scale-0 (16-bit) contrast-masking state. */
typedef struct AdmCmCtx {
    const adm_dwt_band_t *src;
    int16_t *angles[3];
    int16_t *flt_angles[3];
    int src_stride;
    int csf_a_stride;
    int w;
    int h;
    uint16_t i_rfactor[3];
    AdmCmBand band[3];
    uint32_t shift_inner_accum;
    uint32_t add_shift_inner_accum;
} AdmCmCtx;

static void adm_cm_ctx_init(AdmCmCtx *c, AdmBuffer *buf, int w, int h, int src_stride,
                            int csf_a_stride, double adm_norm_view_dist, int adm_ref_display_height,
                            int adm_csf_mode, double adm_csf_scale, double adm_csf_diag_scale,
                            bool measure_aim)
{
    const adm_dwt_band_t *csf_f = measure_aim ? &buf->csf_a : &buf->csf_f;
    const adm_dwt_band_t *csf_a = measure_aim ? &buf->csf_f : &buf->csf_a;
    c->src = measure_aim ? &buf->decouple_a : &buf->decouple_r;
    c->angles[0] = csf_a->band_h;
    c->angles[1] = csf_a->band_v;
    c->angles[2] = csf_a->band_d;
    c->flt_angles[0] = csf_f->band_h;
    c->flt_angles[1] = csf_f->band_v;
    c->flt_angles[2] = csf_f->band_d;
    c->src_stride = src_stride;
    c->csf_a_stride = csf_a_stride;
    c->w = w;
    c->h = h;

    // 0 is scale zero passed to dwt_quant_step
    const AdmCsfFactors f = adm_csf_factors(0, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
    adm_csf_rfactor_scale0(rfactor1, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                           c->i_rfactor);

    /**
     * max value of xh_sq and xv_sq is 1301381973 and that of xd_sq is 1195806729
     *
     * max(val before shift for h and v) is 9.995357299 * —10^17.
     * 9.995357299 * —10^17 * 2^4 is close to 2^64.
     * Hence shift is done based on width subtracting 4
     *
     * max(val before shift for d) is 1.355006643 * —10^18
     * 1.355006643 * —10^18 * 2^3 is close to 2^64
     * Hence shift is done based on width subtracting 3
     */
    const uint32_t shift_xhcub = (uint32_t)ceil(log2(w) - 4);
    const uint32_t shift_xdcub = (uint32_t)ceil(log2(w) - 3);
    c->band[0] = (AdmCmBand){.shift_sub = 10,
                             .add_shift_sq = 268435456,
                             .shift_sq = 29,
                             .add_shift_cub = adm_half_shift(shift_xhcub),
                             .shift_cub = shift_xhcub};
    c->band[1] = c->band[0]; /* vertical band shares the horizontal budget */
    c->band[2] = (AdmCmBand){.shift_sub = 12,
                             .add_shift_sq = 536870912,
                             .shift_sq = 30,
                             .add_shift_cub = adm_half_shift(shift_xdcub),
                             .shift_cub = shift_xdcub};

    c->shift_inner_accum = (uint32_t)ceil(log2(h));
    c->add_shift_inner_accum = adm_half_shift(c->shift_inner_accum);
}

/* Accumulate the three bands of one sample into the row accumulator. */
static inline void adm_cm_accum_px(const AdmCmCtx *c, int i, int j, int64_t inner[3])
{
    const ptrdiff_t idx = (ptrdiff_t)i * c->src_stride + j;
    const int32_t xh = c->src->band_h[idx] * c->i_rfactor[0];
    const int32_t xv = c->src->band_v[idx] * c->i_rfactor[1];
    const int32_t xd = c->src->band_d[idx] * c->i_rfactor[2];
    //thr is shifted to make it's Q format equivalent to xh,xv,xd
    const int32_t thr = adm_cm_thresh(c->angles, c->flt_angles, c->csf_a_stride, c->w, c->h, i, j);

    inner[0] += adm_cm_accum_round(xh, thr, &c->band[0]);
    inner[1] += adm_cm_accum_round(xv, thr, &c->band[1]);
    inner[2] += adm_cm_accum_round(xd, thr, &c->band[2]);
}

/* One row: the optional first / last column (when the border region reaches
 * the frame edge) plus the interior columns. */
static void adm_cm_row(const AdmCmCtx *c, int i, bool left_edge, bool right_edge, int start_col,
                       int end_col, int64_t inner[3])
{
    if (left_edge) {
        adm_cm_accum_px(c, i, 0, inner);
    }
    for (int j = start_col; j < end_col; ++j) {
        adm_cm_accum_px(c, i, j, inner);
    }
    if (right_edge) {
        adm_cm_accum_px(c, i, c->w - 1, inner);
    }
}

static float adm_cm(AdmBuffer *buf, int w, int h, int src_stride, int csf_a_stride,
                    double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                    double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                    double adm_p_norm, bool measure_aim)
{
    AdmCmCtx c;
    adm_cm_ctx_init(&c, buf, w, h, src_stride, csf_a_stride, adm_norm_view_dist,
                    adm_ref_display_height, adm_csf_mode, adm_csf_scale, adm_csf_diag_scale,
                    measure_aim);

    /* The computation of the scales is not required for the regions which lie
     * outside the frame borders */
    const AdmBorder b = adm_border(w, h);
    const bool left_edge = b.left <= 0;
    const bool right_edge = b.right > (w - 1);
    const int start_col = (b.left > 1) ? b.left : 1;
    const int end_col = (b.right < (w - 1)) ? b.right : (w - 1);
    const int start_row = (b.top > 1) ? b.top : 1;
    const int end_row = (b.bottom < (h - 1)) ? b.bottom : (h - 1);

    int64_t accum[3] = {0, 0, 0};
    int64_t inner[3] = {0, 0, 0};

    /* i=0 */
    if (b.top <= 0) {
        adm_cm_row(&c, 0, left_edge, right_edge, start_col, end_col, inner);
    }
    adm_cm_fold(inner, accum, c.add_shift_inner_accum, c.shift_inner_accum);
    /* 0 < i < h-1 */
    for (int i = start_row; i < end_row; ++i) {
        adm_cm_row(&c, i, left_edge, right_edge, start_col, end_col, inner);
        adm_cm_fold(inner, accum, c.add_shift_inner_accum, c.shift_inner_accum);
    }
    /* i=h-1 */
    if (b.bottom > (h - 1)) {
        adm_cm_row(&c, h - 1, left_edge, right_edge, start_col, end_col, inner);
    }
    adm_cm_fold(inner, accum, c.add_shift_inner_accum, c.shift_inner_accum);

    /**
     * For h and v total shifts pending from last stage is 6 rfactor[0,1] has 21 shifts
     * => after cubing (6+21)*3=81 after squaring shifted by 29
     * hence pending is 52-shift's done based on width and height
     *
     * For d total shifts pending from last stage is 6 rfactor[2] has 23 shifts
     * => after cubing (6+23)*3=87 after squaring shifted by 30
     * hence pending is 57-shift's done based on width and height
     */
    const float f_accum_h =
        (float)(accum[0] / pow(2, (52 - c.band[0].shift_cub - c.shift_inner_accum)));
    const float f_accum_v =
        (float)(accum[1] / pow(2, (52 - c.band[1].shift_cub - c.shift_inner_accum)));
    const float f_accum_d =
        (float)(accum[2] / pow(2, (57 - c.band[2].shift_cub - c.shift_inner_accum)));

    const float p_norm_exp = 1.0f / (float)adm_p_norm;
    const int area = (b.bottom - b.top) * (b.right - b.left);
    const float num_scale_h = adm_num_scale(f_accum_h, area, adm_noise_weight, p_norm_exp);
    const float num_scale_v = adm_num_scale(f_accum_v, area, adm_noise_weight, p_norm_exp);
    const float num_scale_d = adm_num_scale(f_accum_d, area, adm_noise_weight, p_norm_exp);

    return (num_scale_h + num_scale_v + num_scale_d);
}

/* Scales 1..3 (32-bit) contrast-masking state. */
typedef struct I4AdmCmCtx {
    const i4_adm_dwt_band_t *src;
    int32_t *angles[3];
    int32_t *flt_angles[3];
    int src_stride;
    int csf_a_stride;
    int w;
    int h;
    uint32_t rfactor[3];
    int32_t add_bef_shift_dst;
    int32_t add_bef_shift_flt;
    uint32_t shift_dst;
    uint32_t shift_flt;
    AdmCmBand band;
    uint32_t shift_inner_accum;
    uint32_t add_shift_inner_accum;
} I4AdmCmCtx;

static void i4_adm_cm_ctx_init(I4AdmCmCtx *c, AdmBuffer *buf, int w, int h, int src_stride,
                               int csf_a_stride, int scale, double adm_norm_view_dist,
                               int adm_ref_display_height, int adm_csf_mode, double adm_csf_scale,
                               double adm_csf_diag_scale, bool measure_aim)
{
    const i4_adm_dwt_band_t *csf_f = measure_aim ? &buf->i4_csf_a : &buf->i4_csf_f;
    const i4_adm_dwt_band_t *csf_a = measure_aim ? &buf->i4_csf_f : &buf->i4_csf_a;
    c->src = measure_aim ? &buf->i4_decouple_a : &buf->i4_decouple_r;
    c->angles[0] = csf_a->band_h;
    c->angles[1] = csf_a->band_v;
    c->angles[2] = csf_a->band_d;
    c->flt_angles[0] = csf_f->band_h;
    c->flt_angles[1] = csf_f->band_v;
    c->flt_angles[2] = csf_f->band_d;
    c->src_stride = src_stride;
    c->csf_a_stride = csf_a_stride;
    c->w = w;
    c->h = h;

    const AdmCsfFactors f = adm_csf_factors(scale, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
    c->rfactor[0] = (uint32_t)(rfactor1[0] * pow(2, 32));
    c->rfactor[1] = (uint32_t)(rfactor1[1] * pow(2, 32));
    c->rfactor[2] = (uint32_t)(rfactor1[2] * pow(2, 32));

    /* Netflix#955 / ADR-0155: second occurrence of the same overflow —
     * see i4_adm_round_terms. Preserved for Netflix-golden bit-exactness. */
    int32_t add_bef_shift_dst[3];
    int32_t add_bef_shift_flt[3];
    i4_adm_round_terms(add_bef_shift_dst, add_bef_shift_flt);
    c->add_bef_shift_dst = add_bef_shift_dst[scale - 1];
    c->add_bef_shift_flt = add_bef_shift_flt[scale - 1];
    c->shift_dst = i4_shift_dst[scale - 1];
    c->shift_flt = i4_shift_flt[scale - 1];

    const uint32_t shift_cub = (uint32_t)ceil(log2(w));
    c->band = (AdmCmBand){.shift_sub = 0,
                          .add_shift_sq = 536870912, //2^29
                          .shift_sq = 30,
                          .add_shift_cub = adm_half_shift(shift_cub),
                          .shift_cub = shift_cub};

    c->shift_inner_accum = (uint32_t)ceil(log2(h));
    c->add_shift_inner_accum = adm_half_shift(c->shift_inner_accum);
}

/* CSF-weight one 32-bit band sample and round it back to 32 bits. */
static inline int32_t i4_adm_cm_scale(const I4AdmCmCtx *c, int32_t v, uint32_t rfactor)
{
    return (int32_t)((((int64_t)v * rfactor) + c->add_bef_shift_dst) >> c->shift_dst);
}

static inline void i4_adm_cm_accum_px(const I4AdmCmCtx *c, int i, int j, int64_t inner[3])
{
    const ptrdiff_t idx = (ptrdiff_t)i * c->src_stride + j;
    const int32_t xh = i4_adm_cm_scale(c, c->src->band_h[idx], c->rfactor[0]);
    const int32_t xv = i4_adm_cm_scale(c, c->src->band_v[idx], c->rfactor[1]);
    const int32_t xd = i4_adm_cm_scale(c, c->src->band_d[idx], c->rfactor[2]);
    const int32_t thr = i4_adm_cm_thresh(c->angles, c->flt_angles, c->csf_a_stride, c->w, c->h, i,
                                         j, c->add_bef_shift_flt, c->shift_flt);

    inner[0] += i4_adm_cm_accum_round(xh, thr, &c->band);
    inner[1] += i4_adm_cm_accum_round(xv, thr, &c->band);
    inner[2] += i4_adm_cm_accum_round(xd, thr, &c->band);
}

static void i4_adm_cm_row(const I4AdmCmCtx *c, int i, bool left_edge, bool right_edge,
                          int start_col, int end_col, int64_t inner[3])
{
    if (left_edge) {
        i4_adm_cm_accum_px(c, i, 0, inner);
    }
    for (int j = start_col; j < end_col; ++j) {
        i4_adm_cm_accum_px(c, i, j, inner);
    }
    if (right_edge) {
        i4_adm_cm_accum_px(c, i, c->w - 1, inner);
    }
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

    /* The computation of the scales is not required for the regions which lie
     * outside the frame borders */
    const AdmBorder b = adm_border(w, h);
    const bool left_edge = b.left <= 0;
    const bool right_edge = b.right > (w - 1);
    const int start_col = (b.left > 1) ? b.left : 1;
    const int end_col = (b.right < (w - 1)) ? b.right : (w - 1);
    const int start_row = (b.top > 1) ? b.top : 1;
    const int end_row = (b.bottom < (h - 1)) ? b.bottom : (h - 1);

    int64_t accum[3] = {0, 0, 0};
    int64_t inner[3] = {0, 0, 0};

    /* i=0 */
    if (b.top <= 0) {
        i4_adm_cm_row(&c, 0, left_edge, right_edge, start_col, end_col, inner);
    }
    adm_cm_fold(inner, accum, c.add_shift_inner_accum, c.shift_inner_accum);
    /* 0 < i < h-1 */
    for (int i = start_row; i < end_row; ++i) {
        i4_adm_cm_row(&c, i, left_edge, right_edge, start_col, end_col, inner);
        adm_cm_fold(inner, accum, c.add_shift_inner_accum, c.shift_inner_accum);
    }
    /* i=h-1 */
    if (b.bottom > (h - 1)) {
        i4_adm_cm_row(&c, h - 1, left_edge, right_edge, start_col, end_col, inner);
    }
    adm_cm_fold(inner, accum, c.add_shift_inner_accum, c.shift_inner_accum);

    /**
     * Converted to floating-point for calculating the final scores
     * Final shifts is calculated from 3*(shifts_from_previous_stage(i.e src comes from dwt)+32)-total_shifts_done_in_this_function
     */
    const float final_shift[3] = {pow(2, (45 - c.band.shift_cub - c.shift_inner_accum)),
                                  pow(2, (39 - c.band.shift_cub - c.shift_inner_accum)),
                                  pow(2, (36 - c.band.shift_cub - c.shift_inner_accum))};
    const float f_accum_h = (float)(accum[0] / final_shift[scale - 1]);
    const float f_accum_v = (float)(accum[1] / final_shift[scale - 1]);
    const float f_accum_d = (float)(accum[2] / final_shift[scale - 1]);

    const float p_norm_exp = 1.0f / (float)adm_p_norm;
    const int area = (b.bottom - b.top) * (b.right - b.left);
    const float num_scale_h = adm_num_scale(f_accum_h, area, adm_noise_weight, p_norm_exp);
    const float num_scale_v = adm_num_scale(f_accum_v, area, adm_noise_weight, p_norm_exp);
    const float num_scale_d = adm_num_scale(f_accum_d, area, adm_noise_weight, p_norm_exp);

    return (num_scale_h + num_scale_v + num_scale_d);
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

/* Four-tap filter response, accumulated in int32 tap by tap as upstream. */
static inline int32_t adm_dwt2_tap4(const int16_t *filter, int32_t s0, int32_t s1, int32_t s2,
                                    int32_t s3)
{
    int32_t accum = 0;
    accum += (int32_t)filter[0] * s0;
    accum += (int32_t)filter[1] * s1;
    accum += (int32_t)filter[2] * s2;
    accum += (int32_t)filter[3] * s3;
    return accum;
}

/* Vertical pass over output row `i` of an 8-bit source: low-pass into
 * `tmplo` and, when `tmphi` is given, high-pass into `tmphi`. Normalizing
 * subtracts the coefficient sum so the (0..N) range maps to (-N/2..N/2). */
static inline void adm_dwt2_vpass_8(const uint8_t *src, int *const *ind_y, int i, int src_stride,
                                    int w, int16_t *tmplo, int16_t *tmphi)
{
    const int16_t shift_VP = 8;
    const int32_t add_shift_VP = 128;

    for (int j = 0; j < w; ++j) {
        const uint16_t u_s0 = src[ind_y[0][i] * src_stride + j];
        const uint16_t u_s1 = src[ind_y[1][i] * src_stride + j];
        const uint16_t u_s2 = src[ind_y[2][i] * src_stride + j];
        const uint16_t u_s3 = src[ind_y[3][i] * src_stride + j];

        int32_t accum = adm_dwt2_tap4(dwt2_db2_coeffs_lo, u_s0, u_s1, u_s2, u_s3);
        /* normalizing is done for range from(0 to N) to (-N/2 to N/2) */
        accum -= (int32_t)dwt2_db2_coeffs_lo_sum * add_shift_VP;
        tmplo[j] = (accum + add_shift_VP) >> shift_VP;

        if (tmphi) {
            accum = adm_dwt2_tap4(dwt2_db2_coeffs_hi, u_s0, u_s1, u_s2, u_s3);
            accum -= (int32_t)dwt2_db2_coeffs_hi_sum * add_shift_VP;
            tmphi[j] = (accum + add_shift_VP) >> shift_VP;
        }
    }
}

/* 16-bit twin of adm_dwt2_vpass_8; the normalisation shift follows the
 * input bit depth. */
static inline void adm_dwt2_vpass_16(const uint16_t *src, int *const *ind_y, int i, int src_stride,
                                     int w, int inp_size_bits, int16_t *tmplo, int16_t *tmphi)
{
    const int16_t shift_VP = inp_size_bits;
    const int32_t add_shift_VP = 1 << (inp_size_bits - 1);

    for (int j = 0; j < w; ++j) {
        const uint16_t u_s0 = src[ind_y[0][i] * src_stride + j];
        const uint16_t u_s1 = src[ind_y[1][i] * src_stride + j];
        const uint16_t u_s2 = src[ind_y[2][i] * src_stride + j];
        const uint16_t u_s3 = src[ind_y[3][i] * src_stride + j];

        int32_t accum = adm_dwt2_tap4(dwt2_db2_coeffs_lo, u_s0, u_s1, u_s2, u_s3);
        /* normalizing is done for range from(0 to N) to (-N/2 to N/2) */
        accum -= (int32_t)dwt2_db2_coeffs_lo_sum * add_shift_VP;
        tmplo[j] = (accum + add_shift_VP) >> shift_VP;

        if (tmphi) {
            accum = adm_dwt2_tap4(dwt2_db2_coeffs_hi, u_s0, u_s1, u_s2, u_s3);
            accum -= (int32_t)dwt2_db2_coeffs_hi_sum * add_shift_VP;
            tmphi[j] = (accum + add_shift_VP) >> shift_VP;
        }
    }
}

/* Horizontal pass of output row `i`: low-pass of `tmplo` into band_a and,
 * when `tmphi` is given, the remaining three bands. */
static inline void adm_dwt2_hpass(const int16_t *tmplo, const int16_t *tmphi,
                                  const adm_dwt_band_t *dst, int *const *ind_x, int i, int w,
                                  int dst_stride)
{
    const int16_t shift_HP = 16;
    const int32_t add_shift_HP = 32768;
    const ptrdiff_t row = (ptrdiff_t)i * dst_stride;

    for (int j = 0; j < (w + 1) / 2; ++j) {
        const int j0 = ind_x[0][j];
        const int j1 = ind_x[1][j];
        const int j2 = ind_x[2][j];
        const int j3 = ind_x[3][j];

        int32_t accum =
            adm_dwt2_tap4(dwt2_db2_coeffs_lo, tmplo[j0], tmplo[j1], tmplo[j2], tmplo[j3]);
        dst->band_a[row + j] = (accum + add_shift_HP) >> shift_HP;

        if (!tmphi) {
            continue;
        }
        accum = adm_dwt2_tap4(dwt2_db2_coeffs_hi, tmplo[j0], tmplo[j1], tmplo[j2], tmplo[j3]);
        dst->band_v[row + j] = (accum + add_shift_HP) >> shift_HP;

        accum = adm_dwt2_tap4(dwt2_db2_coeffs_lo, tmphi[j0], tmphi[j1], tmphi[j2], tmphi[j3]);
        dst->band_h[row + j] = (accum + add_shift_HP) >> shift_HP;

        accum = adm_dwt2_tap4(dwt2_db2_coeffs_hi, tmphi[j0], tmphi[j1], tmphi[j2], tmphi[j3]);
        dst->band_d[row + j] = (accum + add_shift_HP) >> shift_HP;
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
        adm_dwt2_vpass_8(src, ind_y, i, src_stride, w, tmplo, tmphi);
        adm_dwt2_hpass(tmplo, tmphi, dst, ind_x, i, w, dst_stride);
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
        adm_dwt2_vpass_8(src, ind_y, i, src_stride, w, tmplo, NULL);
        adm_dwt2_hpass(tmplo, NULL, dst, ind_x, i, w, dst_stride);
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
        adm_dwt2_vpass_16(src, ind_y, i, src_stride, w, inp_size_bits, tmplo, NULL);
        adm_dwt2_hpass(tmplo, NULL, dst, ind_x, i, w, dst_stride);
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
        adm_dwt2_vpass_16(src, ind_y, i, src_stride, w, inp_size_bits, tmplo, tmphi);
        adm_dwt2_hpass(tmplo, tmphi, dst, ind_x, i, w, dst_stride);
    }
}

/* Four-tap filter response of the 32-bit pipeline, accumulated in int64 tap
 * by tap and rounded back to 32 bits. */
static inline int32_t i4_dwt2_tap4(const int16_t *filter, int32_t s0, int32_t s1, int32_t s2,
                                   int32_t s3, int32_t add, int16_t shift)
{
    int64_t accum = 0;
    accum += (int64_t)filter[0] * s0;
    accum += (int64_t)filter[1] * s1;
    accum += (int64_t)filter[2] * s2;
    accum += (int64_t)filter[3] * s3;
    return (int32_t)((accum + add) >> shift);
}

/* Vertical pass of output row `i` for the reference and distorted planes
 * together; `tmp` holds tmplo_ref, tmphi_ref, tmplo_dis, tmphi_dis (w each). */
static inline void i4_dwt2_vpass(const int32_t *i4_ref_scale, const int32_t *i4_curr_dis,
                                 int *const *ind_y, int i, int ref_stride, int dis_stride, int w,
                                 int32_t *tmp, int32_t add, int16_t shift)
{
    int32_t *tmplo_ref = tmp;
    int32_t *tmphi_ref = tmplo_ref + w;
    int32_t *tmplo_dis = tmphi_ref + w;
    int32_t *tmphi_dis = tmplo_dis + w;

    for (int j = 0; j < w; ++j) {
        int32_t s10 = i4_ref_scale[ind_y[0][i] * ref_stride + j];
        int32_t s11 = i4_ref_scale[ind_y[1][i] * ref_stride + j];
        int32_t s12 = i4_ref_scale[ind_y[2][i] * ref_stride + j];
        int32_t s13 = i4_ref_scale[ind_y[3][i] * ref_stride + j];
        tmplo_ref[j] = i4_dwt2_tap4(dwt2_db2_coeffs_lo, s10, s11, s12, s13, add, shift);
        tmphi_ref[j] = i4_dwt2_tap4(dwt2_db2_coeffs_hi, s10, s11, s12, s13, add, shift);

        s10 = i4_curr_dis[ind_y[0][i] * dis_stride + j];
        s11 = i4_curr_dis[ind_y[1][i] * dis_stride + j];
        s12 = i4_curr_dis[ind_y[2][i] * dis_stride + j];
        s13 = i4_curr_dis[ind_y[3][i] * dis_stride + j];
        tmplo_dis[j] = i4_dwt2_tap4(dwt2_db2_coeffs_lo, s10, s11, s12, s13, add, shift);
        tmphi_dis[j] = i4_dwt2_tap4(dwt2_db2_coeffs_hi, s10, s11, s12, s13, add, shift);
    }
}

/* Horizontal pass of one output sample `out` from the row buffers of one
 * plane into its four bands. */
static inline void i4_dwt2_hpass_bands(const int32_t *tmplo, const int32_t *tmphi,
                                       const i4_adm_dwt_band_t *dst, const int jx[4], ptrdiff_t out,
                                       int32_t add, int16_t shift)
{
    int32_t s10 = tmplo[jx[0]];
    int32_t s11 = tmplo[jx[1]];
    int32_t s12 = tmplo[jx[2]];
    int32_t s13 = tmplo[jx[3]];
    dst->band_a[out] = i4_dwt2_tap4(dwt2_db2_coeffs_lo, s10, s11, s12, s13, add, shift);
    dst->band_v[out] = i4_dwt2_tap4(dwt2_db2_coeffs_hi, s10, s11, s12, s13, add, shift);

    s10 = tmphi[jx[0]];
    s11 = tmphi[jx[1]];
    s12 = tmphi[jx[2]];
    s13 = tmphi[jx[3]];
    dst->band_h[out] = i4_dwt2_tap4(dwt2_db2_coeffs_lo, s10, s11, s12, s13, add, shift);
    dst->band_d[out] = i4_dwt2_tap4(dwt2_db2_coeffs_hi, s10, s11, s12, s13, add, shift);
}

static inline void i4_dwt2_hpass(const int32_t *tmp, const i4_adm_dwt_band_t *i4_ref_dwt2,
                                 const i4_adm_dwt_band_t *i4_dis_dwt2, int *const *ind_x, int i,
                                 int w, int dst_stride, int32_t add, int16_t shift)
{
    const int32_t *tmplo_ref = tmp;
    const int32_t *tmphi_ref = tmplo_ref + w;
    const int32_t *tmplo_dis = tmphi_ref + w;
    const int32_t *tmphi_dis = tmplo_dis + w;

    for (int j = 0; j < (w + 1) / 2; ++j) {
        const int jx[4] = {ind_x[0][j], ind_x[1][j], ind_x[2][j], ind_x[3][j]};
        const ptrdiff_t out = (ptrdiff_t)i * dst_stride + j;
        i4_dwt2_hpass_bands(tmplo_ref, tmphi_ref, i4_ref_dwt2, jx, out, add, shift);
        i4_dwt2_hpass_bands(tmplo_dis, tmphi_dis, i4_dis_dwt2, jx, out, add, shift);
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
 * Per-scale rounding is encoded in the small `add_bef_shift_round_VP/HP`
 * and `shift_VerticalPass/HorizontalPass` LUTs. Scale 0 is NOT handled
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
    const int32_t add_bef_shift_round_VP[3] = {0, 32768, 32768};
    const int32_t add_bef_shift_round_HP[3] = {16384, 32768, 16384};
    const int16_t shift_VerticalPass[3] = {0, 16, 16};
    const int16_t shift_HorizontalPass[3] = {15, 16, 15};

    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int32_t *tmp = buf->tmp_ref;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        i4_dwt2_vpass(i4_ref_scale, i4_curr_dis, ind_y, i, ref_stride, dis_stride, w, tmp,
                      add_bef_shift_round_VP[scale - 1], shift_VerticalPass[scale - 1]);
        i4_dwt2_hpass(tmp, &buf->i4_ref_dwt2, &buf->i4_dis_dwt2, ind_x, i, w, dst_stride,
                      add_bef_shift_round_HP[scale - 1], shift_HorizontalPass[scale - 1]);
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
static void adm_result_finalise(AdmResult *res, double num, double den, double aim_num,
                                double numden_limit)
{
    num = num < numden_limit ? 0 : num;
    den = den < numden_limit ? 0 : den;

    if (den == 0.0) {
        /* Flat/black frame: no distortion energy — both scores are perfect.
         * score_aim MUST be initialised here; the caller reads it
         * unconditionally and the else-branch would be skipped.           */
        res->score = 1.0f;
        res->score_aim = 1.0f;
    } else {
        /* Normalize AIM score by the DLM denominator. */
        res->score_aim = aim_num / den;
        res->score = num / den;
    }
    res->score_num = num;
    res->score_den = den;
}

static void integer_compute_adm(const AdmState *s, const VmafPicture *ref_pic,
                                const VmafPicture *dis_pic, AdmResult *res)
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

    adm_result_finalise(res, num, den, aim_num, numden_limit);
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
        s->adm_csf = adm_csf_avx2;
        s->i4_adm_csf = i4_adm_csf_avx2;
        s->adm_csf_den_scale = adm_csf_den_scale_avx2;
        s->adm_csf_den_s123 = adm_csf_den_s123_avx2;
        s->adm_cm = adm_cm_avx2;
        s->i4_adm_cm = i4_adm_cm_avx2;
        s->adm_dwt2_s123_combined = adm_dwt2_s123_combined_avx2;
    }
#if HAVE_AVX512
    if (flags & VMAF_X86_CPU_FLAG_AVX512) {
        s->dwt2_8 = adm_dwt2_8_avx512;
        s->dwt2_16 = adm_dwt2_16_avx512;
        s->adm_decouple = adm_decouple_avx512;
        s->adm_decouple_s123 = adm_decouple_s123_avx512;
        s->adm_csf = adm_csf_avx512;
        s->i4_adm_csf = i4_adm_csf_avx512;
        s->adm_csf_den_scale = adm_csf_den_scale_avx512;
        s->adm_csf_den_s123 = adm_csf_den_s123_avx512;
        s->adm_cm = adm_cm_avx512;
        s->i4_adm_cm = i4_adm_cm_avx512;
        s->adm_dwt2_s123_combined = adm_dwt2_s123_combined_avx512;
    }
#endif
#elif ARCH_AARCH64
    const unsigned flags = vmaf_get_cpu_flags();
    if (flags & VMAF_ARM_CPU_FLAG_NEON) {
        if (!(w % 8)) {
#if defined(__APPLE__)
            s->dwt2_8 = adm_dwt2_8_neon_apple_legacy;
#else
            s->dwt2_8 = adm_dwt2_8_neon;
#endif
        }
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

    if (w < 17u || h < 17u) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "integer_adm requires width >= 17 and height >= 17 (got %ux%u)\n", w, h);
        return -EINVAL;
    }

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

static const char *const scale_feature_names[4] = {"integer_adm_scale0", "integer_adm_scale1",
                                                   "integer_adm_scale2", "integer_adm_scale3"};

static const char *const debug_scale_feature_names[8] = {
    "integer_adm_num_scale0", "integer_adm_den_scale0", "integer_adm_num_scale1",
    "integer_adm_den_scale1", "integer_adm_num_scale2", "integer_adm_den_scale2",
    "integer_adm_num_scale3", "integer_adm_den_scale3"};

/* `debug=true` extras: the raw score plus the per-scale numerators and
 * denominators. */
static int extract_debug_features(const AdmState *s, VmafFeatureCollector *feature_collector,
                                  const AdmResult *r, unsigned index)
{
    int err = 0;

    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_adm", r->score, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_adm_num", r->score_num, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "integer_adm_den", r->score_den, index);
    for (size_t k = 0; k < 8; ++k) {
        err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                       debug_scale_feature_names[k], r->scores[k],
                                                       index);
    }
    return err;
}

/* ADR-1174: the scale-0 CSF factors are packed into `uint16_t i_rfactor[]` as
 * `factor1 * 2^21` / `factor2 * 2^23`. A CSF configuration whose factors do not
 * fit that 16-bit budget wrapped silently and corrupted every ADM score, so it
 * is rejected up front. Kept out of `extract()` so that function stays inside
 * the readability-function-size threshold (ADR-0141). */
static int check_csf_scale0_budget(const AdmState *s)
{
    static const char *const csf_mode_names[] = {
        "WATSON97",
        "BARTEN",
        "BARTEN_WATSON_BLEND",
        "BARTEN_WATSON_BLEND_MAE",
    };
    const double pow2_21 = 2097152.0;
    const double pow2_23 = 8388608.0;

    const AdmCsfFactors csf_f0 =
        adm_csf_factors(0, s->adm_norm_view_dist, s->adm_ref_display_height, s->adm_csf_mode,
                        s->adm_csf_scale, s->adm_csf_diag_scale);
    const double budget1 = (double)csf_f0.factor1 * pow2_21;
    const double budget2 = (double)csf_f0.factor2 * pow2_23;

    if (budget1 < 65536.0 && budget2 < 65536.0) {
        return 0;
    }

    const char *mode_name =
        (s->adm_csf_mode >= 0 &&
         s->adm_csf_mode < (int)(sizeof(csf_mode_names) / sizeof(csf_mode_names[0]))) ?
            csf_mode_names[s->adm_csf_mode] :
            "UNKNOWN";
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "integer_adm: csf_mode %d (%s) scale-0 factor overflows 16-bit fixed-point budget "
             "(factor1*2^21=%.1f, factor2*2^23=%.1f >= 65536.0)\n",
             s->adm_csf_mode, mode_name, budget1, budget2);
    return -EINVAL;
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

    // current implementation is limited by the 16-bit data pipeline, thus
    // cannot handle an angular frequency smaller than 1080p * 3H
    if (s->adm_norm_view_dist * s->adm_ref_display_height <
        DEFAULT_ADM_NORM_VIEW_DIST * DEFAULT_ADM_REF_DISPLAY_HEIGHT) {
        return -EINVAL;
    }

    err = check_csf_scale0_budget(s);
    if (err) {
        return err;
    }

    AdmResult r;
    integer_compute_adm(s, ref_pic, dist_pic, &r);

    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_adm2_score", r.score, index);
    err |= vmaf_feature_collector_append_with_dict(feature_collector, s->feature_name_dict,
                                                   "VMAF_integer_feature_aim_score", r.score_aim,
                                                   index);
    err |= vmaf_feature_collector_append_with_dict(
        feature_collector, s->feature_name_dict, "VMAF_integer_feature_adm3_score",
        MAX(r.score * s->adm_dlm_weight + (1 - r.score_aim) * (1 - s->adm_dlm_weight),
            s->adm_min_val),
        index);
    for (size_t k = 0; k < 4; ++k) {
        err |= vmaf_feature_collector_append_with_dict(
            feature_collector, s->feature_name_dict, scale_feature_names[k],
            r.scores[2 * k] / r.scores[2 * k + 1], index);
    }

    if (!s->debug) {
        return err;
    }
    return err | extract_debug_features(s, feature_collector, &r, index);
}

static int close(VmafFeatureExtractor *fex)
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

// Registration struct consumed by libvmaf/src/feature/feature_extractor.cpp
// (via the fex-registry table); must retain external linkage.
// NOLINTNEXTLINE(misc-use-internal-linkage): cross-TU registry pattern — external linkage required (ADR-0278).
VmafFeatureExtractor vmaf_fex_integer_adm = {
    .name = "adm",
    .init = init,
    .extract = extract,
    .options = options,
    .close = close,
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
