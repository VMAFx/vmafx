/**
 *
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
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

/*
 * Scalar kernels of the integer ADM pipeline.
 *
 * integer_adm.c is the scalar reference; x86/adm_avx2.c and x86/adm_avx512.c
 * vectorise the interior of each stage and finish the columns a vector does
 * not cover in scalar code. Those scalar columns used to be a second and a
 * third copy of the reference arithmetic. They are this header now, so a tail
 * column, an edge row and the scalar reference are one implementation and
 * cannot drift apart.
 *
 * Every kernel that walks a row takes a half-open column range [j0, j1): the
 * reference passes the whole row, a SIMD caller passes what its vector loop
 * left over.
 *
 * C only. The CUDA, HIP, SYCL and Metal twins keep their own kernels and
 * include integer_adm.h, not this file.
 */

#ifndef FEATURE_INTEGER_ADM_KERNELS_H_
#define FEATURE_INTEGER_ADM_KERNELS_H_

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "adm_angle_flag.h"
#include "adm_cm_accumulator.h"
#include "adm_csf_fixed_point.h"
#include "barten_csf_tools.h"
#include "compat_builtin.h"
#include "integer_adm.h"

#define ADM_KERNEL_MIN(x, y) (((x) < (y)) ? (x) : (y))
#define ADM_KERNEL_MAX(x, y) (((x) > (y)) ? (x) : (y))

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
static inline AdmCsfFactors adm_csf_factors(int scale, double adm_norm_view_dist,
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
 *
 * All three bands share one power-of-two normalisation exponent. The caller
 * restores three times that exponent after the contrast-masking cube.
 */
static inline uint32_t adm_csf_rfactor_scale0(const float rfactor1[3], double adm_norm_view_dist,
                                              int adm_ref_display_height, int adm_csf_mode,
                                              uint16_t i_rfactor[3])
{
    double fixed[3];
    uint32_t normalization_shift = 0u;
    const int err = adm_csf_fixed_scale(0, rfactor1, adm_norm_view_dist, adm_ref_display_height,
                                        adm_csf_mode, fixed, &normalization_shift);
    if (err) {
        i_rfactor[0] = 0u;
        i_rfactor[1] = 0u;
        i_rfactor[2] = 0u;
        return 0u;
    }
    for (unsigned band = 0; band < 3; ++band) {
        i_rfactor[band] = (uint16_t)fixed[band];
    }
    return normalization_shift;
}

static inline uint32_t adm_csf_rfactor_s123(int scale, const float rfactor1[3],
                                            double adm_norm_view_dist, int adm_ref_display_height,
                                            int adm_csf_mode, uint32_t i_rfactor[3])
{
    double fixed[3];
    uint32_t normalization_shift = 0u;
    const int err = adm_csf_fixed_scale(scale, rfactor1, adm_norm_view_dist, adm_ref_display_height,
                                        adm_csf_mode, fixed, &normalization_shift);
    if (err) {
        i_rfactor[0] = 0u;
        i_rfactor[1] = 0u;
        i_rfactor[2] = 0u;
        return 0u;
    }
    for (unsigned band = 0; band < 3; ++band) {
        i_rfactor[band] = (uint32_t)fixed[band];
    }
    return normalization_shift;
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
static inline AdmBorder adm_border(int w, int h)
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
static inline AdmBorder adm_border_filt(int w, int h)
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
    /* The expression itself lives in adm_angle_flag.h so that the CUDA, HIP,
     * SYCL and Metal twins evaluate the same predicate instead of four
     * near-misses (ADR-1194). This wrapper is kept so the call sites below
     * read unchanged. */
    return adm_angle_flag_fp64(ot_dp, o_mag_sq, t_mag_sq, cos_1deg_sq);
}

/* cos(1 degree)^2, narrowed to float as every decouple stage uses it. */
static inline float adm_cos_1deg_sq(void)
{
    return cos(1.0 * M_PI / 180.0) * cos(1.0 * M_PI / 180.0);
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
        rst = ADM_KERNEL_MIN((rst * gain), t);
    }
    if (angle_flag && (rst_f < 0.)) {
        rst = ADM_KERNEL_MAX((rst * gain), t);
    }
    return rst;
}

/* Decouple columns [j0, j1) of row `i` of the 16-bit pipeline. */
static inline void adm_decouple_cols(const AdmBuffer *buf, int i, int stride, int j0, int j1,
                                     double gain, const int32_t *lut, float cos_1deg_sq)
{
    const adm_dwt_band_t *ref = &buf->ref_dwt2;
    const adm_dwt_band_t *dis = &buf->dis_dwt2;
    const adm_dwt_band_t *r = &buf->decouple_r;
    const adm_dwt_band_t *a = &buf->decouple_a;

    for (int j = j0; j < j1; ++j) {
        const ptrdiff_t idx = (ptrdiff_t)i * stride + j;
        const int16_t oh = ref->band_h[idx];
        const int16_t ov = ref->band_v[idx];
        const int16_t od = ref->band_d[idx];
        const int16_t th = dis->band_h[idx];
        const int16_t tv = dis->band_v[idx];
        const int16_t td = dis->band_d[idx];

        const int angle_flag =
            adm_angle_flag((int64_t)oh * th + (int64_t)ov * tv, (int64_t)oh * oh + (int64_t)ov * ov,
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

/* The 15 most significant bits of `temp` (>= 32768) and, in `x`, how far it
 * was shifted down to get them. */
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
        rst = ADM_KERNEL_MIN((rst * gain), t);
    }
    if (angle_flag && (rst_f < 0.)) {
        rst = ADM_KERNEL_MAX((rst * gain), t);
    }
    return rst;
}

/* Decouple columns [j0, j1) of row `i` of the 32-bit pipeline. */
static inline void adm_decouple_s123_cols(const AdmBuffer *buf, int i, int stride, int j0, int j1,
                                          double gain, const int32_t *lut, float cos_1deg_sq)
{
    const i4_adm_dwt_band_t *ref = &buf->i4_ref_dwt2;
    const i4_adm_dwt_band_t *dis = &buf->i4_dis_dwt2;
    const i4_adm_dwt_band_t *r = &buf->i4_decouple_r;
    const i4_adm_dwt_band_t *a = &buf->i4_decouple_a;

    for (int j = j0; j < j1; ++j) {
        const ptrdiff_t idx = (ptrdiff_t)i * stride + j;
        const int32_t oh = ref->band_h[idx];
        const int32_t ov = ref->band_v[idx];
        const int32_t od = ref->band_d[idx];
        const int32_t th = dis->band_h[idx];
        const int32_t tv = dis->band_v[idx];
        const int32_t td = dis->band_d[idx];

        const int angle_flag =
            adm_angle_flag((int64_t)oh * th + (int64_t)ov * tv, (int64_t)oh * oh + (int64_t)ov * ov,
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

/* ------------------------------------------------------------------------- */
/* Contrast sensitivity filtering                                            */
/* ------------------------------------------------------------------------- */

/**
 * Shifts pending from previous stage is 6
 * hence variables multiplied by i_rfactor[0,1] has to be shifted by 21+6=27 to convert
 * into floating-point. But shifted by 15 to make it Q16
 * and variables multiplied by i_factor[2] has to be shifted by 23+6=29 to convert into
 * floating-point. But shifted by 17 to make it Q16
 * Hence remaining shifts after shifting by i_shifts is 12 to make it equivalent to
 * floating-point
 */
static const uint8_t adm_csf_shifts[3] = {15, 15, 17};
static const uint16_t adm_csf_shiftsadd[3] = {16384, 16384, 65535};
#define ADM_FIX_ONE_BY_30 4369 //(1/30)*2^17

/* Source, CSF-weighted and (1/30)-filtered bands of the scale-0 CSF stage.
 * With `measure_aim` the restored signal takes the place of the additive
 * impairment, and the two output buffers swap roles. */
typedef struct AdmCsfBands {
    const int16_t *src[3];
    int16_t *dst[3];
    int16_t *flt[3];
} AdmCsfBands;

static inline AdmCsfBands adm_csf_bands(const AdmBuffer *buf, bool measure_aim)
{
    const adm_dwt_band_t *src = measure_aim ? &buf->decouple_r : &buf->decouple_a;
    const adm_dwt_band_t *dst = measure_aim ? &buf->csf_f : &buf->csf_a;
    const adm_dwt_band_t *flt = measure_aim ? &buf->csf_a : &buf->csf_f;
    const AdmCsfBands b = {{src->band_h, src->band_v, src->band_d},
                           {dst->band_h, dst->band_v, dst->band_d},
                           {flt->band_h, flt->band_v, flt->band_d}};
    return b;
}

/* Scale-0 CSF weights in the fixed point the CSF and contrast-masking stages
 * share. Returns the normalisation exponent (adm_csf_rfactor_scale0()). */
static inline uint32_t adm_csf_i_rfactor(double adm_norm_view_dist, int adm_ref_display_height,
                                         int adm_csf_mode, double adm_csf_scale,
                                         double adm_csf_diag_scale, uint16_t i_rfactor[3])
{
    // 0 is scale zero passed to dwt_quant_step
    const AdmCsfFactors f = adm_csf_factors(0, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
    return adm_csf_rfactor_scale0(rfactor1, adm_norm_view_dist, adm_ref_display_height,
                                  adm_csf_mode, i_rfactor);
}

/* CSF-weight columns [j0, j1) of band `theta`; `offset` is the row offset. */
static inline void adm_csf_cols(const AdmCsfBands *b, const uint16_t i_rfactor[3], int theta,
                                ptrdiff_t offset, int j0, int j1)
{
    const int16_t *src_ptr = b->src[theta];
    int16_t *dst_ptr = b->dst[theta];
    int16_t *flt_ptr = b->flt[theta];

    for (int j = j0; j < j1; ++j) {
        const int32_t dst_val = i_rfactor[theta] * (int32_t)src_ptr[offset + j];
        const int16_t i16_dst_val =
            ((int16_t)((dst_val + adm_csf_shiftsadd[theta]) >> adm_csf_shifts[theta]));
        dst_ptr[offset + j] = i16_dst_val;
        flt_ptr[offset + j] =
            ((int16_t)(((ADM_FIX_ONE_BY_30 * abs((int32_t)i16_dst_val)) + 2048) >> 12));
    }
}

/* Slot of scale 1..3 in the per-scale tables of the 32-bit pipeline. The
 * extractor only passes 1..3; anything else takes slot 0 instead of reading
 * outside a table. */
static inline unsigned i4_scale_slot(int scale)
{
    return (scale >= 1 && scale <= 3) ? (unsigned)(scale - 1) : 0u;
}

/* Right-shift budgets of the 32-bit CSF outputs (scales 1..3). */
static const uint32_t i4_shift_dst[3] = {28, 28, 28};
static const uint32_t i4_shift_flt[3] = {32, 32, 32};
#define I4_ADM_FIX_ONE_BY_30 143165577u

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
static inline void i4_adm_round_terms(int32_t add_bef_shift_dst[3], int32_t add_bef_shift_flt[3])
{
    for (unsigned idx = 0; idx < 3; ++idx) {
        /* UBSan: cast unsigned shift result to int32_t explicitly; the wrap
         * for i4_shift_flt[idx]==32 is intentional per ADR-0155 (Netflix#955). */
        add_bef_shift_dst[idx] = (int32_t)(1u << (i4_shift_dst[idx] - 1));
        add_bef_shift_flt[idx] = (int32_t)(1u << (i4_shift_flt[idx] - 1));
    }
}

/* The 32-bit twin of AdmCsfBands plus the fixed-point terms of one scale. */
typedef struct I4AdmCsfCtx {
    const int32_t *src[3];
    int32_t *dst[3];
    int32_t *flt[3];
    uint32_t i_rfactor[3];
    int32_t add_bef_shift_dst;
    int32_t add_bef_shift_flt;
    uint32_t shift_dst;
    uint32_t shift_flt;
} I4AdmCsfCtx;

static inline void i4_adm_csf_ctx_init(I4AdmCsfCtx *c, const AdmBuffer *buf, int scale,
                                       double adm_norm_view_dist, int adm_ref_display_height,
                                       int adm_csf_mode, double adm_csf_scale,
                                       double adm_csf_diag_scale, bool measure_aim)
{
    const i4_adm_dwt_band_t *src = measure_aim ? &buf->i4_decouple_r : &buf->i4_decouple_a;
    const i4_adm_dwt_band_t *dst = measure_aim ? &buf->i4_csf_f : &buf->i4_csf_a;
    const i4_adm_dwt_band_t *flt = measure_aim ? &buf->i4_csf_a : &buf->i4_csf_f;
    c->src[0] = src->band_h;
    c->src[1] = src->band_v;
    c->src[2] = src->band_d;
    c->dst[0] = dst->band_h;
    c->dst[1] = dst->band_v;
    c->dst[2] = dst->band_d;
    c->flt[0] = flt->band_h;
    c->flt[1] = flt->band_v;
    c->flt[2] = flt->band_d;

    const AdmCsfFactors f = adm_csf_factors(scale, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
    (void)adm_csf_rfactor_s123(scale, rfactor1, adm_norm_view_dist, adm_ref_display_height,
                               adm_csf_mode, c->i_rfactor);

    int32_t add_bef_shift_dst[3];
    int32_t add_bef_shift_flt[3];
    i4_adm_round_terms(add_bef_shift_dst, add_bef_shift_flt);
    const unsigned slot = i4_scale_slot(scale);
    c->add_bef_shift_dst = add_bef_shift_dst[slot];
    c->add_bef_shift_flt = add_bef_shift_flt[slot];
    c->shift_dst = i4_shift_dst[slot];
    c->shift_flt = i4_shift_flt[slot];
}

/* CSF-weight columns [j0, j1) of band `theta` of the 32-bit pipeline. */
static inline void i4_adm_csf_cols(const I4AdmCsfCtx *c, int theta, ptrdiff_t offset, int j0,
                                   int j1)
{
    const int32_t *src_ptr = c->src[theta];
    int32_t *dst_ptr = c->dst[theta];
    int32_t *flt_ptr = c->flt[theta];

    for (int j = j0; j < j1; ++j) {
        const int32_t dst_val = (int32_t)(((c->i_rfactor[theta] * (int64_t)src_ptr[offset + j]) +
                                           c->add_bef_shift_dst) >>
                                          c->shift_dst);
        dst_ptr[offset + j] = dst_val;
        flt_ptr[offset + j] =
            (int32_t)((((int64_t)I4_ADM_FIX_ONE_BY_30 * abs(dst_val)) + c->add_bef_shift_flt) >>
                      c->shift_flt);
    }
}

/* ------------------------------------------------------------------------- */
/* Denominator (reference-energy) reductions                                 */
/* ------------------------------------------------------------------------- */

/* Cube-root finalisation shared by both denominator reductions: the per-band
 * CSF energies are converted to floating point and offset by the noise floor
 * of the reduced area. */
static inline float adm_den_scale_finalise(const double csf[3], int area, double adm_noise_weight)
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
typedef struct AdmDenCtx {
    float rfactor[3];
    AdmBorder b;
    int area;
    int32_t shift_accum;
    int32_t add_shift_accum;
} AdmDenCtx;

static inline void adm_csf_den_ctx_init(AdmDenCtx *c, int w, int h, double adm_norm_view_dist,
                                        int adm_ref_display_height, int adm_csf_mode,
                                        double adm_csf_scale, double adm_csf_diag_scale)
{
    const AdmCsfFactors f = adm_csf_factors(0, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    c->rfactor[0] = f.factor1;
    c->rfactor[1] = f.factor1;
    c->rfactor[2] = f.factor2;

    /* The computation of the denominator scales is not required for the regions
     * which lie outside the frame borders */
    c->b = adm_border(w, h);
    c->area = (c->b.bottom - c->b.top) * (c->b.right - c->b.left);

    int32_t shift_accum = (int32_t)ceil(log2(c->area) - 20);
    shift_accum = shift_accum > 0 ? shift_accum : 0;
    c->shift_accum = shift_accum;
    c->add_shift_accum = shift_accum > 0 ? (1 << (shift_accum - 1)) : 0;
}

/* Add the cubes of columns [j0, j1) of one row to the row accumulators. */
static inline void adm_csf_den_cols(const int16_t *src_h, const int16_t *src_v,
                                    const int16_t *src_d, int j0, int j1, uint64_t inner[3])
{
    for (int j = j0; j < j1; ++j) {
        const uint16_t h_abs = (uint16_t)abs(src_h[j]);
        const uint16_t v_abs = (uint16_t)abs(src_v[j]);
        const uint16_t d_abs = (uint16_t)abs(src_d[j]);

        inner[0] += ((uint64_t)h_abs * h_abs) * h_abs;
        inner[1] += ((uint64_t)v_abs * v_abs) * v_abs;
        inner[2] += ((uint64_t)d_abs * d_abs) * d_abs;
    }
}

/* Fold one row's cube sums into the frame accumulators and clear them. */
static inline void adm_csf_den_fold(uint64_t inner[3], uint64_t accum[3], uint32_t add_shift_accum,
                                    uint32_t shift_accum)
{
    for (int k = 0; k < 3; ++k) {
        accum[k] += adm_csf_den_round_row_total(inner[k], add_shift_accum, shift_accum);
        inner[k] = 0;
    }
}

static inline float adm_csf_den_result(const AdmDenCtx *c, const uint64_t accum[3],
                                       double adm_noise_weight)
{
    const double shift_csf = pow(2, (18 - c->shift_accum));
    const double csf[3] = {(double)(accum[0] / shift_csf) * pow(c->rfactor[0], 3),
                           (double)(accum[1] / shift_csf) * pow(c->rfactor[1], 3),
                           (double)(accum[2] / shift_csf) * pow(c->rfactor[2], 3)};

    return adm_den_scale_finalise(csf, c->area, adm_noise_weight);
}

/* Rounded ((x^2 + add_sq) >> shift_sq) * x, then rounded and shifted by
 * shift_cub: the per-sample cube term of the 32-bit denominator. */
static inline uint64_t i4_cube_term(uint32_t x_abs, uint32_t add_shift_sq, uint32_t shift_sq,
                                    uint32_t add_shift_cub, uint32_t shift_cub)
{
    return ((((((uint64_t)x_abs * x_abs) + add_shift_sq) >> shift_sq) * x_abs) + add_shift_cub) >>
           shift_cub;
}

/* Scales 1..3 denominator state. */
typedef struct I4AdmDenCtx {
    float rfactor[3];
    AdmBorder b;
    uint32_t shift_sq;
    uint32_t add_shift_sq;
    uint32_t shift_cub;
    uint32_t add_shift_cub;
    uint32_t shift_accum;
    uint32_t add_shift_accum;
    uint32_t accum_convert_float;
} I4AdmDenCtx;

static inline void i4_adm_csf_den_ctx_init(I4AdmDenCtx *c, int scale, int w, int h,
                                           double adm_norm_view_dist, int adm_ref_display_height,
                                           int adm_csf_mode, double adm_csf_scale,
                                           double adm_csf_diag_scale)
{
    const AdmCsfFactors f = adm_csf_factors(scale, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    c->rfactor[0] = f.factor1;
    c->rfactor[1] = f.factor1;
    c->rfactor[2] = f.factor2;

    const uint32_t shift_sq[3] = {31, 30, 31};
    const uint32_t accum_convert_float[3] = {32, 27, 23};
    const uint32_t add_shift_sq[3] = {1u << shift_sq[0], 1u << shift_sq[1], 1u << shift_sq[2]};
    const unsigned slot = i4_scale_slot(scale);
    c->shift_sq = shift_sq[slot];
    c->add_shift_sq = add_shift_sq[slot];
    c->accum_convert_float = accum_convert_float[slot];

    /* The computation of the denominator scales is not required for the regions
     * which lie outside the frame borders */
    c->b = adm_border(w, h);

    c->shift_cub = (uint32_t)ceil(log2(c->b.right - c->b.left));
    c->add_shift_cub = adm_half_shift(c->shift_cub);
    c->shift_accum = (uint32_t)ceil(log2(c->b.bottom - c->b.top));
    c->add_shift_accum = adm_half_shift(c->shift_accum);
}

/* Add the cube terms of columns [j0, j1) of one row to the row accumulators. */
static inline void i4_adm_csf_den_cols(const I4AdmDenCtx *c, const int32_t *src_h,
                                       const int32_t *src_v, const int32_t *src_d, int j0, int j1,
                                       uint64_t inner[3])
{
    for (int j = j0; j < j1; ++j) {
        const uint32_t h_abs = (uint32_t)abs(src_h[j]);
        const uint32_t v_abs = (uint32_t)abs(src_v[j]);
        const uint32_t d_abs = (uint32_t)abs(src_d[j]);

        inner[0] +=
            i4_cube_term(h_abs, c->add_shift_sq, c->shift_sq, c->add_shift_cub, c->shift_cub);
        inner[1] +=
            i4_cube_term(v_abs, c->add_shift_sq, c->shift_sq, c->add_shift_cub, c->shift_cub);
        inner[2] +=
            i4_cube_term(d_abs, c->add_shift_sq, c->shift_sq, c->add_shift_cub, c->shift_cub);
    }
}

/**
 * All the results are converted to floating-point to calculate the scores
 * For all scales the final shift is 3*shifts from dwt - total shifts done here
 */
static inline float i4_adm_csf_den_result(const I4AdmDenCtx *c, const uint64_t accum[3],
                                          double adm_noise_weight)
{
    const double shift_csf = pow(2, (c->accum_convert_float - c->shift_accum - c->shift_cub));
    const double csf[3] = {(double)(accum[0] / shift_csf) * pow(c->rfactor[0], 3),
                           (double)(accum[1] / shift_csf) * pow(c->rfactor[1], 3),
                           (double)(accum[2] / shift_csf) * pow(c->rfactor[2], 3)};

    return adm_den_scale_finalise(csf, (c->b.bottom - c->b.top) * (c->b.right - c->b.left),
                                  adm_noise_weight);
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
 *
 * The centre tap stays in int32. It reaches 69904 for a coefficient of
 * magnitude 32768; narrowing it to int16, as the first revision of upstream
 * PR #1602 did, wraps every tap above 32767 (|coefficient| > 15359) negative
 * and lets an isolated coefficient unmask itself (ADR-1402).
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
        sum += ((ONE_BY_15 * abs((int32_t)src_ptr[j])) + 2048) >> 12;
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
 * (upstream ADM_CM_ACCUM_ROUND). The excess over the threshold is clamped
 * to [0, INT32_MAX] in int64 (adm_cm_excess_s0()). */
static inline int64_t adm_cm_accum_round(int32_t x, int32_t thr, const AdmCmBand *p)
{
    const int32_t v = adm_cm_excess_s0(x, thr, (uint32_t)p->shift_sub);
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
        accum[k] += adm_cm_round_row_total(inner[k], add_shift_inner_accum, shift_inner_accum);
        inner[k] = 0;
    }
}

/* p-norm of the accumulated contrast plus the noise floor of the area. */
static inline float adm_num_scale(float f_accum, int area, double adm_noise_weight,
                                  float p_norm_exp)
{
    return powf(f_accum, p_norm_exp) + powf(area * adm_noise_weight, p_norm_exp);
}

/* Rows and columns a contrast-masking reduction visits. The border is
 * symmetric, so either both the first and the last column lie inside it
 * (`left_edge` and `right_edge`) or neither does. */
typedef struct AdmCmBounds {
    AdmBorder b;
    bool left_edge;
    bool right_edge;
    int start_col;
    int end_col;
    int start_row;
    int end_row;
} AdmCmBounds;

/* The computation of the scales is not required for the regions which lie
 * outside the frame borders */
static inline AdmCmBounds adm_cm_bounds(int w, int h)
{
    AdmCmBounds bd;
    bd.b = adm_border(w, h);
    bd.left_edge = bd.b.left <= 0;
    bd.right_edge = bd.b.right > (w - 1);
    bd.start_col = (bd.b.left > 1) ? bd.b.left : 1;
    bd.end_col = (bd.b.right < (w - 1)) ? bd.b.right : (w - 1);
    bd.start_row = (bd.b.top > 1) ? bd.b.top : 1;
    bd.end_row = (bd.b.bottom < (h - 1)) ? bd.b.bottom : (h - 1);
    return bd;
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
    uint32_t normalization_shift;
    AdmCmBand band[3];
    uint32_t shift_inner_accum;
    uint32_t add_shift_inner_accum;
    /* Frame-wide operands of the interior-row callback handed to
     * adm_cm_rows(): the vector kernels keep their broadcast constants here.
     * NULL for the scalar rows. */
    const void *row_data;
} AdmCmCtx;

static inline void adm_cm_ctx_init(AdmCmCtx *c, AdmBuffer *buf, int w, int h, int src_stride,
                                   int csf_a_stride, double adm_norm_view_dist,
                                   int adm_ref_display_height, int adm_csf_mode,
                                   double adm_csf_scale, double adm_csf_diag_scale,
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
    // NOLINTNEXTLINE(modernize-use-nullptr) — C header; MSVC's C has no nullptr (ADR-1138)
    c->row_data = NULL;

    c->normalization_shift =
        adm_csf_i_rfactor(adm_norm_view_dist, adm_ref_display_height, adm_csf_mode, adm_csf_scale,
                          adm_csf_diag_scale, c->i_rfactor);

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
static inline void adm_cm_row(const AdmCmCtx *c, int i, const AdmCmBounds *bd, int64_t inner[3])
{
    if (bd->left_edge) {
        adm_cm_accum_px(c, i, 0, inner);
    }
    for (int j = bd->start_col; j < bd->end_col; ++j) {
        adm_cm_accum_px(c, i, j, inner);
    }
    if (bd->right_edge) {
        adm_cm_accum_px(c, i, c->w - 1, inner);
    }
}

static inline float adm_cm_restore_accum(int64_t accum, int base_exp, uint32_t normalization_shift,
                                         uint32_t shift_cub, uint32_t shift_inner_accum)
{
    const int divisor_exp =
        base_exp - 3 * (int)normalization_shift - (int)shift_cub - (int)shift_inner_accum;
    return (float)(accum / pow(2, divisor_exp));
}

/**
 * For h and v total shifts pending from last stage is 6 rfactor[0,1] has 21 shifts
 * => after cubing (6+21)*3=81 after squaring shifted by 29
 * hence pending is 52-shift's done based on width and height
 *
 * For d total shifts pending from last stage is 6 rfactor[2] has 23 shifts
 * => after cubing (6+23)*3=87 after squaring shifted by 30
 * hence pending is 57-shift's done based on width and height
 */
static inline float adm_cm_result(const AdmCmCtx *c, const AdmCmBounds *bd, const int64_t accum[3],
                                  double adm_noise_weight, double adm_p_norm)
{
    const float f_accum_h = adm_cm_restore_accum(accum[0], 52, c->normalization_shift,
                                                 c->band[0].shift_cub, c->shift_inner_accum);
    const float f_accum_v = adm_cm_restore_accum(accum[1], 52, c->normalization_shift,
                                                 c->band[1].shift_cub, c->shift_inner_accum);
    const float f_accum_d = adm_cm_restore_accum(accum[2], 57, c->normalization_shift,
                                                 c->band[2].shift_cub, c->shift_inner_accum);

    const float p_norm_exp = 1.0f / (float)adm_p_norm;
    const int area = (bd->b.bottom - bd->b.top) * (bd->b.right - bd->b.left);
    const float num_scale_h = adm_num_scale(f_accum_h, area, adm_noise_weight, p_norm_exp);
    const float num_scale_v = adm_num_scale(f_accum_v, area, adm_noise_weight, p_norm_exp);
    const float num_scale_d = adm_num_scale(f_accum_d, area, adm_noise_weight, p_norm_exp);

    return (num_scale_h + num_scale_v + num_scale_d);
}

/* One interior row of a contrast-masking reduction: the scalar reference
 * passes adm_cm_row(), a SIMD twin its vector row. */
typedef void (*AdmCmRowFn)(const AdmCmCtx *c, int i, const AdmCmBounds *bd, int64_t inner[3]);

/* Rows of the scale-0 reduction. The first and last row need the mirrored /
 * clamped neighbourhood and always take the scalar kernel. */
static inline void adm_cm_rows(const AdmCmCtx *c, const AdmCmBounds *bd, AdmCmRowFn interior_row,
                               int64_t accum[3])
{
    int64_t inner[3] = {0, 0, 0};

    /* i=0 */
    if (bd->b.top <= 0) {
        adm_cm_row(c, 0, bd, inner);
    }
    adm_cm_fold(inner, accum, c->add_shift_inner_accum, c->shift_inner_accum);
    /* 0 < i < h-1 */
    for (int i = bd->start_row; i < bd->end_row; ++i) {
        interior_row(c, i, bd, inner);
        adm_cm_fold(inner, accum, c->add_shift_inner_accum, c->shift_inner_accum);
    }
    /* i=h-1 */
    if (bd->b.bottom > (c->h - 1)) {
        adm_cm_row(c, c->h - 1, bd, inner);
    }
    adm_cm_fold(inner, accum, c->add_shift_inner_accum, c->shift_inner_accum);
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
    int scale;
    uint32_t rfactor[3];
    uint32_t normalization_shift;
    int32_t add_bef_shift_dst;
    int32_t add_bef_shift_flt;
    uint32_t shift_dst;
    uint32_t shift_flt;
    AdmCmBand band;
    uint32_t shift_inner_accum;
    uint32_t add_shift_inner_accum;
} I4AdmCmCtx;

static inline void i4_adm_cm_ctx_init(I4AdmCmCtx *c, AdmBuffer *buf, int w, int h, int src_stride,
                                      int csf_a_stride, int scale, double adm_norm_view_dist,
                                      int adm_ref_display_height, int adm_csf_mode,
                                      double adm_csf_scale, double adm_csf_diag_scale,
                                      bool measure_aim)
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
    c->scale = scale;

    const AdmCsfFactors f = adm_csf_factors(scale, adm_norm_view_dist, adm_ref_display_height,
                                            adm_csf_mode, adm_csf_scale, adm_csf_diag_scale);
    const float rfactor1[3] = {f.factor1, f.factor1, f.factor2};
    c->normalization_shift = adm_csf_rfactor_s123(scale, rfactor1, adm_norm_view_dist,
                                                  adm_ref_display_height, adm_csf_mode, c->rfactor);

    /* Netflix#955 / ADR-0155: second occurrence of the same overflow —
     * see i4_adm_round_terms. Preserved for Netflix-golden bit-exactness. */
    int32_t add_bef_shift_dst[3];
    int32_t add_bef_shift_flt[3];
    i4_adm_round_terms(add_bef_shift_dst, add_bef_shift_flt);
    const unsigned slot = i4_scale_slot(scale);
    c->add_bef_shift_dst = add_bef_shift_dst[slot];
    c->add_bef_shift_flt = add_bef_shift_flt[slot];
    c->shift_dst = i4_shift_dst[slot];
    c->shift_flt = i4_shift_flt[slot];

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

static inline void i4_adm_cm_row(const I4AdmCmCtx *c, int i, const AdmCmBounds *bd,
                                 int64_t inner[3])
{
    if (bd->left_edge) {
        i4_adm_cm_accum_px(c, i, 0, inner);
    }
    for (int j = bd->start_col; j < bd->end_col; ++j) {
        i4_adm_cm_accum_px(c, i, j, inner);
    }
    if (bd->right_edge) {
        i4_adm_cm_accum_px(c, i, c->w - 1, inner);
    }
}

/**
 * Converted to floating-point for calculating the final scores
 * Final shifts is calculated from 3*(shifts_from_previous_stage(i.e src comes from dwt)+32)-total_shifts_done_in_this_function
 */
static inline float i4_adm_cm_result(const I4AdmCmCtx *c, const AdmCmBounds *bd,
                                     const int64_t accum[3], double adm_noise_weight,
                                     double adm_p_norm)
{
    const int restored_bits = 3 * (int)c->normalization_shift;
    const float final_shift[3] = {
        pow(2, (45 - restored_bits - (int)c->band.shift_cub - (int)c->shift_inner_accum)),
        pow(2, (39 - restored_bits - (int)c->band.shift_cub - (int)c->shift_inner_accum)),
        pow(2, (36 - restored_bits - (int)c->band.shift_cub - (int)c->shift_inner_accum))};
    const unsigned slot = i4_scale_slot(c->scale);
    const float f_accum_h = (float)(accum[0] / final_shift[slot]);
    const float f_accum_v = (float)(accum[1] / final_shift[slot]);
    const float f_accum_d = (float)(accum[2] / final_shift[slot]);

    const float p_norm_exp = 1.0f / (float)adm_p_norm;
    const int area = (bd->b.bottom - bd->b.top) * (bd->b.right - bd->b.left);
    const float num_scale_h = adm_num_scale(f_accum_h, area, adm_noise_weight, p_norm_exp);
    const float num_scale_v = adm_num_scale(f_accum_v, area, adm_noise_weight, p_norm_exp);
    const float num_scale_d = adm_num_scale(f_accum_d, area, adm_noise_weight, p_norm_exp);

    return (num_scale_h + num_scale_v + num_scale_d);
}

typedef void (*I4AdmCmRowFn)(const I4AdmCmCtx *c, int i, const AdmCmBounds *bd, int64_t inner[3]);

/* Rows of a scale 1..3 reduction; see adm_cm_rows(). */
static inline void i4_adm_cm_rows(const I4AdmCmCtx *c, const AdmCmBounds *bd,
                                  I4AdmCmRowFn interior_row, int64_t accum[3])
{
    int64_t inner[3] = {0, 0, 0};

    /* i=0 */
    if (bd->b.top <= 0) {
        i4_adm_cm_row(c, 0, bd, inner);
    }
    adm_cm_fold(inner, accum, c->add_shift_inner_accum, c->shift_inner_accum);
    /* 0 < i < h-1 */
    for (int i = bd->start_row; i < bd->end_row; ++i) {
        interior_row(c, i, bd, inner);
        adm_cm_fold(inner, accum, c->add_shift_inner_accum, c->shift_inner_accum);
    }
    /* i=h-1 */
    if (bd->b.bottom > (c->h - 1)) {
        i4_adm_cm_row(c, c->h - 1, bd, inner);
    }
    adm_cm_fold(inner, accum, c->add_shift_inner_accum, c->shift_inner_accum);
}

/* ------------------------------------------------------------------------- */
/* Daubechies-2 DWT                                                          */
/* ------------------------------------------------------------------------- */

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

/* Vertical pass over columns [j0, j1) of output row `i` of an 8-bit source:
 * low-pass into `tmplo` and, when `tmphi` is given, high-pass into `tmphi`.
 * Normalizing subtracts the coefficient sum so the (0..N) range maps to
 * (-N/2..N/2). */
static inline void adm_dwt2_vpass_8(const uint8_t *src, int *const *ind_y, int i, int src_stride,
                                    int j0, int j1, int16_t *tmplo, int16_t *tmphi)
{
    const int16_t shift_VP = 8;
    const int32_t add_shift_VP = 128;

    for (int j = j0; j < j1; ++j) {
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
 * input bit depth, and the response is formed in int64 because a bright
 * 16-bit column overflows int32 (adm_dwt2_vpass16_tap4). */
static inline void adm_dwt2_vpass_16(const uint16_t *src, int *const *ind_y, int i, int src_stride,
                                     int j0, int j1, int inp_size_bits, int16_t *tmplo,
                                     int16_t *tmphi)
{
    const int shift_VP = inp_size_bits;
    const int32_t add_shift_VP = 1 << (inp_size_bits - 1);

    for (int j = j0; j < j1; ++j) {
        const uint16_t u_s0 = src[ind_y[0][i] * src_stride + j];
        const uint16_t u_s1 = src[ind_y[1][i] * src_stride + j];
        const uint16_t u_s2 = src[ind_y[2][i] * src_stride + j];
        const uint16_t u_s3 = src[ind_y[3][i] * src_stride + j];

        tmplo[j] = (int16_t)adm_dwt2_vpass16_tap4(dwt2_db2_coeffs_lo, dwt2_db2_coeffs_lo_sum, u_s0,
                                                  u_s1, u_s2, u_s3, add_shift_VP, shift_VP);
        if (tmphi) {
            tmphi[j] =
                (int16_t)adm_dwt2_vpass16_tap4(dwt2_db2_coeffs_hi, dwt2_db2_coeffs_hi_sum, u_s0,
                                               u_s1, u_s2, u_s3, add_shift_VP, shift_VP);
        }
    }
}

/* Horizontal pass of output columns [j0, j1) of output row `i`: low-pass of
 * `tmplo` into band_a and, when `tmphi` is given, the remaining three bands. */
static inline void adm_dwt2_hpass(const int16_t *tmplo, const int16_t *tmphi,
                                  const adm_dwt_band_t *dst, int *const *ind_x, int i, int j0,
                                  int j1, int dst_stride)
{
    const int16_t shift_HP = 16;
    const int32_t add_shift_HP = 32768;
    const ptrdiff_t row = (ptrdiff_t)i * dst_stride;

    for (int j = j0; j < j1; ++j) {
        const int jx0 = ind_x[0][j];
        const int jx1 = ind_x[1][j];
        const int jx2 = ind_x[2][j];
        const int jx3 = ind_x[3][j];

        int32_t accum =
            adm_dwt2_tap4(dwt2_db2_coeffs_lo, tmplo[jx0], tmplo[jx1], tmplo[jx2], tmplo[jx3]);
        dst->band_a[row + j] = (accum + add_shift_HP) >> shift_HP;

        if (!tmphi) {
            continue;
        }
        accum = adm_dwt2_tap4(dwt2_db2_coeffs_hi, tmplo[jx0], tmplo[jx1], tmplo[jx2], tmplo[jx3]);
        dst->band_v[row + j] = (accum + add_shift_HP) >> shift_HP;

        accum = adm_dwt2_tap4(dwt2_db2_coeffs_lo, tmphi[jx0], tmphi[jx1], tmphi[jx2], tmphi[jx3]);
        dst->band_h[row + j] = (accum + add_shift_HP) >> shift_HP;

        accum = adm_dwt2_tap4(dwt2_db2_coeffs_hi, tmphi[jx0], tmphi[jx1], tmphi[jx2], tmphi[jx3]);
        dst->band_d[row + j] = (accum + add_shift_HP) >> shift_HP;
    }
}

/* Per-scale rounding of the 32-bit DWT (scales 1..3). */
typedef struct I4Dwt2Round {
    int32_t add_vp;
    int32_t add_hp;
    int16_t shift_vp;
    int16_t shift_hp;
} I4Dwt2Round;

static inline I4Dwt2Round i4_dwt2_round(int scale)
{
    const int32_t add_bef_shift_round_VP[3] = {0, 32768, 32768};
    const int32_t add_bef_shift_round_HP[3] = {16384, 32768, 16384};
    const int16_t shift_VerticalPass[3] = {0, 16, 16};
    const int16_t shift_HorizontalPass[3] = {15, 16, 15};
    const unsigned slot = i4_scale_slot(scale);
    const I4Dwt2Round r = {add_bef_shift_round_VP[slot], add_bef_shift_round_HP[slot],
                           shift_VerticalPass[slot], shift_HorizontalPass[slot]};
    return r;
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

/* Vertical pass of columns [j0, j1) of output row `i` for the reference and
 * distorted planes together; `tmp` holds tmplo_ref, tmphi_ref, tmplo_dis,
 * tmphi_dis (w each). */
static inline void i4_dwt2_vpass(const int32_t *i4_ref_scale, const int32_t *i4_curr_dis,
                                 int *const *ind_y, int i, int ref_stride, int dis_stride, int w,
                                 int j0, int j1, int32_t *tmp, int32_t add, int16_t shift)
{
    int32_t *tmplo_ref = tmp;
    int32_t *tmphi_ref = tmplo_ref + w;
    int32_t *tmplo_dis = tmphi_ref + w;
    int32_t *tmphi_dis = tmplo_dis + w;

    for (int j = j0; j < j1; ++j) {
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

/* Horizontal pass of output columns [j0, j1) of output row `i`, both planes. */
static inline void i4_dwt2_hpass(const int32_t *tmp, const i4_adm_dwt_band_t *i4_ref_dwt2,
                                 const i4_adm_dwt_band_t *i4_dis_dwt2, int *const *ind_x, int i,
                                 int w, int j0, int j1, int dst_stride, int32_t add, int16_t shift)
{
    const int32_t *tmplo_ref = tmp;
    const int32_t *tmphi_ref = tmplo_ref + w;
    const int32_t *tmplo_dis = tmphi_ref + w;
    const int32_t *tmphi_dis = tmplo_dis + w;

    for (int j = j0; j < j1; ++j) {
        const int jx[4] = {ind_x[0][j], ind_x[1][j], ind_x[2][j], ind_x[3][j]};
        const ptrdiff_t out = (ptrdiff_t)i * dst_stride + j;
        i4_dwt2_hpass_bands(tmplo_ref, tmphi_ref, i4_ref_dwt2, jx, out, add, shift);
        i4_dwt2_hpass_bands(tmplo_dis, tmphi_dis, i4_dis_dwt2, jx, out, add, shift);
    }
}

#endif /* FEATURE_INTEGER_ADM_KERNELS_H_ */
