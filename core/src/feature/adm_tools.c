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

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "mem.h"
#include "adm_options.h"
#include "adm_tools.h"

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795028841971693993751
#endif

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

#ifdef __SSE2__
#ifdef ADM_OPT_RECIP_DIVISION

#include <emmintrin.h>

static float rcp_s(float x)
{
    float xi = _mm_cvtss_f32(_mm_rcp_ss(_mm_load_ss(&x)));
    return xi + xi * (1.0f - x * xi);
}

#define DIVS(n, d) ((n) * rcp_s(d))
#endif //ADM_OPT_RECIP_DIVISION
#else
#define DIVS(n, d) ((n) / (d))
#endif // __SSE2__

static const float dwt2_db2_coeffs_lo_s[4] = {0.482962913144690, 0.836516303737469,
                                              0.224143868041857, -0.129409522550921};
static const float dwt2_db2_coeffs_hi_s[4] = {-0.129409522550921, -0.224143868041857,
                                              0.836516303737469, -0.482962913144690};

static const double dwt2_db2_coeffs_lo_d[4] = {0.482962913144690, 0.836516303737469,
                                               0.224143868041857, -0.129409522550921};
static const double dwt2_db2_coeffs_hi_d[4] = {-0.129409522550921, -0.224143868041857,
                                               0.836516303737469, -0.482962913144690};

#ifndef FLOAT_ONE_BY_30
#define FLOAT_ONE_BY_30 0.0333333351
#endif

#ifndef FLOAT_ONE_BY_15
#define FLOAT_ONE_BY_15 0.0666666701
#endif

static float get_noise_constant(int w, int h, double weight, double adm_p_norm)
{
    return powf(w * h * weight, 1.0f / adm_p_norm);
}

/* ------------------------------------------------------------------------- */
/* Shared prologues                                                          */
/* ------------------------------------------------------------------------- */

typedef struct AdmBorderS {
    int left;
    int top;
    int right;
    int bottom;
} AdmBorderS;

/* Region that takes part in the reductions: `border_factor` of each frame
 * edge is excluded. */
static AdmBorderS adm_border_s(int w, int h, double border_factor)
{
    AdmBorderS b;
    b.left = (int)(w * border_factor - 0.5);
    b.top = (int)(h * border_factor - 0.5);
    b.right = w - b.left;
    b.bottom = h - b.top;
    return b;
}

/* The same region widened by one filter tap on each side (-1 / +2) and
 * clamped to the frame, for the decouple and CSF stages. */
static AdmBorderS adm_border_filt_s(int w, int h, double border_factor)
{
    AdmBorderS b;
    b.left = (int)(w * border_factor - 0.5 - 1); // -1 for filter tap
    b.top = (int)(h * border_factor - 0.5 - 1);
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

/* Per-scale user override of the CSF weights (`adm_f1sN` / `adm_f2sN`,
 * negative = keep the model value). */
static void adm_csf_factor_overrides_s(int scale, double f1s0, double f1s1, double f1s2,
                                       double f1s3, double f2s0, double f2s1, double f2s2,
                                       double f2s3, float *factor1, float *factor2)
{
    double f1 = -1.0;
    double f2 = -1.0;
    if (scale == 0) {
        f1 = f1s0;
        f2 = f2s0;
    } else if (scale == 1) {
        f1 = f1s1;
        f2 = f2s1;
    } else if (scale == 2) {
        f1 = f1s2;
        f2 = f2s2;
    } else {
        f1 = f1s3;
        f2 = f2s3;
    }
    if (f1 >= 0) {
        *factor1 = f1;
    }
    if (f2 >= 0) {
        *factor2 = f2;
    }
}

/* CSF weights of DWT scale `scale` for the (h, v) bands (rfactor[0..1]) and
 * the (d) band (rfactor[2]). For ADM, scales go from 0 to 3 while the noise
 * floor paper numbers them 1 to 4 (finest to coarsest). */
static void adm_csf_rfactor_s(int scale, double adm_norm_view_dist, int adm_ref_display_height,
                              int adm_csf_mode, double luminance_level, double adm_csf_scale,
                              double adm_csf_diag_scale, double adm_f1s0, double adm_f1s1,
                              double adm_f1s2, double adm_f1s3, double adm_f2s0, double adm_f2s1,
                              double adm_f2s2, double adm_f2s3, float rfactor[3])
{
    float factor1;
    float factor2;
    if (adm_csf_mode == ADM_CSF_MODE_BARTEN) {
        factor1 = barten_csf(scale, adm_norm_view_dist, adm_ref_display_height, luminance_level,
                             adm_csf_scale);
        factor2 = barten_csf(scale, adm_norm_view_dist, adm_ref_display_height, luminance_level,
                             adm_csf_diag_scale);
    } else if (adm_csf_mode == ADM_CSF_MODE_ADM) {
        factor1 = adm_native_csf(scale, adm_norm_view_dist, adm_ref_display_height, 0);
        factor2 = adm_native_csf(scale, adm_norm_view_dist, adm_ref_display_height, 45);
    } else {
        factor1 = 1.0f / dwt_quant_step(&dwt_7_9_YCbCr_threshold[0], scale, 1, adm_norm_view_dist,
                                        adm_ref_display_height);
        factor2 = 1.0f / dwt_quant_step(&dwt_7_9_YCbCr_threshold[0], scale, 2, adm_norm_view_dist,
                                        adm_ref_display_height);
    }
    adm_csf_factor_overrides_s(scale, adm_f1s0, adm_f1s1, adm_f1s2, adm_f1s3, adm_f2s0, adm_f2s1,
                               adm_f2s2, adm_f2s3, &factor1, &factor2);
    rfactor[0] = factor1;
    rfactor[1] = factor1;
    rfactor[2] = factor2;
}

/* Fold a row accumulator (h, v, d) into the frame accumulator and reset it.
 * Float accumulators are upstream's golden-gated arithmetic: widening them
 * to double changes the Netflix scores (ADR-0418 widened only
 * adm_sum_cube_s, whose result survives at places=4); ADR-1141 keeps every
 * other ADM reduction bit-exact. */
static inline void adm_fold3_s(float inner[3], float accum[3])
{
    for (int k = 0; k < 3; ++k) {
        accum[k] += inner[k];
        inner[k] = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Reductions                                                                */
/* ------------------------------------------------------------------------- */

float adm_sum_cube_s(const float *x, int w, int h, int stride, double border_factor,
                     double adm_p_norm)
{
    const int px_stride = stride / sizeof(float);
    const AdmBorderS b = adm_border_s(w, h, border_factor);

    /* ADR-0418: outer accumulator in `double` to satisfy fork semgrep
     * rule `vmaf-no-double-precision-loss-in-reduction` (cubed-float
     * reduction drifts between scalar / SIMD paths). Upstream Netflix
     * ships these as `float`; bumping to `double` is fork-local and
     * does not change the final `powf(accum, 1.0f / adm_p_norm)` cast
     * back to float at places=4 precision. */
    double accum = 0;

    for (int i = b.top; i < b.bottom; ++i) {
        double accum_inner = 0;

        for (int j = b.left; j < b.right; ++j) {
            const float val = fabsf(x[i * px_stride + j]);
            if (adm_p_norm == 3.0) {
                accum_inner += (double)val * val * val;
            } else {
                accum_inner += powf(val, adm_p_norm);
            }
        }

        accum += accum_inner;
    }

    return powf((float)accum, 1.0f / adm_p_norm) +
           powf((b.bottom - b.top) * (b.right - b.left) / 32.0f, 1.0f / adm_p_norm);
}

/* ------------------------------------------------------------------------- */
/* Decouple                                                                  */
/* ------------------------------------------------------------------------- */

/* Determine if angle between (oh,ov) and (th,tv) is less than 1 degree. */
static inline int adm_angle_flag_s(float oh, float ov, float th, float tv, float cos_1deg_sq,
                                   float eps)
{
#ifdef ADM_OPT_AVOID_ATAN
    /* Given that u is the angle (oh,ov) and v is the angle (th,tv), this can
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
     */
    (void)eps;
    const float ot_dp = oh * th + ov * tv;
    const float o_mag_sq = oh * oh + ov * ov;
    const float t_mag_sq = th * th + tv * tv;

    return (ot_dp >= 0.0f) && (ot_dp * ot_dp >= cos_1deg_sq * o_mag_sq * t_mag_sq);
#else
    (void)cos_1deg_sq;
    float oa = atanf(DIVS(ov, oh + eps));
    float ta = atanf(DIVS(tv, th + eps));

    if (oh < 0.0f) {
        oa += (float)M_PI;
    }
    if (th < 0.0f) {
        ta += (float)M_PI;
    }

    const float diff = fabsf(oa - ta) * 180.0f / M_PI;
    return diff < 1.0f;
#endif
}

/* One band of the decouple: restore `o` toward `t` with the clipped ratio
 * k = t / o, then bound the enhancement gain (the fork's modification of
 * upstream's `if (angle_flag) rst = t;`). */
static inline float adm_decouple_band_s(float o, float t, float eps, int angle_flag,
                                        double adm_enhn_gain_limit)
{
    float k = DIVS(t, o + eps);
    k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
    float rst = k * o;

    if (angle_flag && (rst > 0.0)) {
        rst = MIN(rst * adm_enhn_gain_limit, t);
    }
    if (angle_flag && (rst < 0.0)) {
        rst = MAX(rst * adm_enhn_gain_limit, t);
    }
    return rst;
}

void adm_decouple_s(const adm_dwt_band_t_s *ref, const adm_dwt_band_t_s *dis,
                    const adm_dwt_band_t_s *r, const adm_dwt_band_t_s *a, int w, int h,
                    int ref_stride, int dis_stride, int r_stride, int a_stride,
                    double border_factor, double adm_enhn_gain_limit)
{
    const float cos_1deg_sq = cos(1.0 * M_PI / 180.0) * cos(1.0 * M_PI / 180.0);
    const float eps = 1e-30;

    const int ref_px_stride = ref_stride / sizeof(float);
    const int dis_px_stride = dis_stride / sizeof(float);
    const int r_px_stride = r_stride / sizeof(float);
    const int a_px_stride = a_stride / sizeof(float);

    /* The computation of the score is not required for the regions which lie outside the frame borders */
    const AdmBorderS b = adm_border_filt_s(w, h, border_factor);

    for (int i = b.top; i < b.bottom; ++i) {
        for (int j = b.left; j < b.right; ++j) {
            const ptrdiff_t ref_idx = (ptrdiff_t)i * ref_px_stride + j;
            const ptrdiff_t dis_idx = (ptrdiff_t)i * dis_px_stride + j;
            const float oh = ref->band_h[ref_idx];
            const float ov = ref->band_v[ref_idx];
            const float od = ref->band_d[ref_idx];
            const float th = dis->band_h[dis_idx];
            const float tv = dis->band_v[dis_idx];
            const float td = dis->band_d[dis_idx];

            const int angle_flag = adm_angle_flag_s(oh, ov, th, tv, cos_1deg_sq, eps);

            const float rst_h = adm_decouple_band_s(oh, th, eps, angle_flag, adm_enhn_gain_limit);
            const float rst_v = adm_decouple_band_s(ov, tv, eps, angle_flag, adm_enhn_gain_limit);
            const float rst_d = adm_decouple_band_s(od, td, eps, angle_flag, adm_enhn_gain_limit);

            const ptrdiff_t r_idx = (ptrdiff_t)i * r_px_stride + j;
            const ptrdiff_t a_idx = (ptrdiff_t)i * a_px_stride + j;
            r->band_h[r_idx] = rst_h;
            r->band_v[r_idx] = rst_v;
            r->band_d[r_idx] = rst_d;

            a->band_h[a_idx] = th - rst_h;
            a->band_v[a_idx] = tv - rst_v;
            a->band_d[a_idx] = td - rst_d;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Contrast sensitivity filtering                                            */
/* ------------------------------------------------------------------------- */

void adm_csf_s(const adm_dwt_band_t_s *src, const adm_dwt_band_t_s *dst,
               const adm_dwt_band_t_s *flt, int orig_h, int scale, int w, int h, int src_stride,
               int dst_stride, double border_factor, double adm_norm_view_dist,
               int adm_ref_display_height, int adm_csf_mode, double luminance_level,
               double adm_csf_scale, double adm_csf_diag_scale, double adm_f1s0, double adm_f1s1,
               double adm_f1s2, double adm_f1s3, double adm_f2s0, double adm_f2s1, double adm_f2s2,
               double adm_f2s3)
{
    (void)orig_h;

    const float *const src_angles[3] = {src->band_h, src->band_v, src->band_d};
    float *const dst_angles[3] = {dst->band_h, dst->band_v, dst->band_d};
    float *const flt_angles[3] = {flt->band_h, flt->band_v, flt->band_d};

    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

    float rfactor[3];
    adm_csf_rfactor_s(scale, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                      luminance_level, adm_csf_scale, adm_csf_diag_scale, adm_f1s0, adm_f1s1,
                      adm_f1s2, adm_f1s3, adm_f2s0, adm_f2s1, adm_f2s2, adm_f2s3, rfactor);

    /* The computation of the csf values is not required for the regions which lie outside the frame borders */
    const AdmBorderS b = adm_border_filt_s(w, h, border_factor);

    for (int theta = 0; theta < 3; ++theta) {
        const float *src_ptr = src_angles[theta];
        float *dst_ptr = dst_angles[theta];
        float *flt_ptr = flt_angles[theta];

        for (int i = b.top; i < b.bottom; ++i) {
            const ptrdiff_t src_offset = (ptrdiff_t)i * src_px_stride;
            const ptrdiff_t dst_offset = (ptrdiff_t)i * dst_px_stride;

            for (int j = b.left; j < b.right; ++j) {
                const float dst_val = rfactor[theta] * src_ptr[src_offset + j];
                dst_ptr[dst_offset + j] = dst_val;
                flt_ptr[dst_offset + j] = FLOAT_ONE_BY_30 * fabsf(dst_val);
            }
        }
    }
}

/* Combination of adm_csf_s and adm_sum_cube_s for csf_o based den_scale */
float adm_csf_den_scale_s(const adm_dwt_band_t_s *src, int orig_h, int scale, int w, int h,
                          int src_stride, double border_factor, double adm_norm_view_dist,
                          int adm_ref_display_height, int adm_csf_mode, double luminance_level,
                          double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                          double adm_p_norm, double adm_f1s0, double adm_f1s1, double adm_f1s2,
                          double adm_f1s3, double adm_f2s0, double adm_f2s1, double adm_f2s2,
                          double adm_f2s3)
{
    (void)orig_h;

    const int src_px_stride = src_stride / sizeof(float);

    float rfactor[3];
    adm_csf_rfactor_s(scale, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                      luminance_level, adm_csf_scale, adm_csf_diag_scale, adm_f1s0, adm_f1s1,
                      adm_f1s2, adm_f1s3, adm_f2s0, adm_f2s1, adm_f2s2, adm_f2s3, rfactor);

    /* (h, v, d) frame and per-row accumulators; see adm_fold3_s. */
    float accum[3] = {0, 0, 0};
    float inner[3] = {0, 0, 0};

    /* The computation of the denominator scales is not required for the regions which lie outside the frame borders */
    const AdmBorderS b = adm_border_s(w, h, border_factor);

    for (int i = b.top; i < b.bottom; ++i) {
        const float *src_h = src->band_h + (ptrdiff_t)i * src_px_stride;
        const float *src_v = src->band_v + (ptrdiff_t)i * src_px_stride;
        const float *src_d = src->band_d + (ptrdiff_t)i * src_px_stride;
        for (int j = b.left; j < b.right; ++j) {
            const float abs_csf_o_val_h = fabsf(rfactor[0] * src_h[j]);
            const float abs_csf_o_val_v = fabsf(rfactor[1] * src_v[j]);
            const float abs_csf_o_val_d = fabsf(rfactor[2] * src_d[j]);

            if (adm_p_norm == 3.0) {
                inner[0] += abs_csf_o_val_h * abs_csf_o_val_h * abs_csf_o_val_h;
                inner[1] += abs_csf_o_val_v * abs_csf_o_val_v * abs_csf_o_val_v;
                inner[2] += abs_csf_o_val_d * abs_csf_o_val_d * abs_csf_o_val_d;
            } else {
                inner[0] += powf(abs_csf_o_val_h, adm_p_norm);
                inner[1] += powf(abs_csf_o_val_v, adm_p_norm);
                inner[2] += powf(abs_csf_o_val_d, adm_p_norm);
            }
        }
        adm_fold3_s(inner, accum);
    }

    const float den_scale_h =
        powf(accum[0], 1.0f / adm_p_norm) +
        get_noise_constant(b.right - b.left, b.bottom - b.top, adm_noise_weight, adm_p_norm);
    const float den_scale_v =
        powf(accum[1], 1.0f / adm_p_norm) +
        get_noise_constant(b.right - b.left, b.bottom - b.top, adm_noise_weight, adm_p_norm);
    const float den_scale_d =
        powf(accum[2], 1.0f / adm_p_norm) +
        get_noise_constant(b.right - b.left, b.bottom - b.top, adm_noise_weight, adm_p_norm);

    return (den_scale_h + den_scale_v + den_scale_d);
}

/* ------------------------------------------------------------------------- */
/* Contrast masking                                                          */
/* ------------------------------------------------------------------------- */

/**
 * Masking threshold at (i, j): the 3x3 neighbourhood sum of the CSF-filtered
 * bands, with the centre tap taken from the unfiltered band scaled by 1/15.
 *
 * The row / column before the first edge mirrors to index 1 and the row /
 * column past the last edge clamps to the last index. This is the closed
 * form of the nine ADM_CM_THRESH_S_{0_0, 0_J, 0_W_M_1, I_0, I_J, I_W_M_1,
 * H_M_1_0, H_M_1_J, H_M_1_W_M_1} corner / edge / interior macro variants in
 * adm_tools.h; the float summation order matches them one-to-one.
 */
static inline float adm_cm_thresh3x3_s(const float *const *angles, const float *const *flt_angles,
                                       int src_px_stride, int w, int h, int i, int j)
{
    const int i_m1 = (i == 0) ? 1 : i - 1;
    const int i_p1 = (i == h - 1) ? h - 1 : i + 1;
    const int j_m1 = (j == 0) ? 1 : j - 1;
    const int j_p1 = (j == w - 1) ? w - 1 : j + 1;
    float accum = 0;

    for (int theta = 0; theta < 3; ++theta) {
        const float *src_ptr = angles[theta] + (ptrdiff_t)i * src_px_stride;
        const float *flt_m1 = flt_angles[theta] + (ptrdiff_t)i_m1 * src_px_stride;
        const float *flt_0 = flt_angles[theta] + (ptrdiff_t)i * src_px_stride;
        const float *flt_p1 = flt_angles[theta] + (ptrdiff_t)i_p1 * src_px_stride;
        float sum = 0;
        sum += flt_m1[j_m1];
        sum += flt_m1[j];
        sum += flt_m1[j_p1];
        sum += flt_0[j_m1];
        sum += FLOAT_ONE_BY_15 * fabsf(src_ptr[j]);
        sum += flt_0[j_p1];
        sum += flt_p1[j_m1];
        sum += flt_p1[j];
        sum += flt_p1[j_p1];
        accum += sum;
    }
    return accum;
}

/* Contrast-masking state of one scale. */
typedef struct AdmCmCtxS {
    const adm_dwt_band_t_s *src;
    const float *angles[3];
    const float *flt_angles[3];
    int src_px_stride;
    int csf_px_stride;
    int w;
    int h;
    float rfactor[3];
    int adm_bypass_cm;
    double adm_p_norm;
} AdmCmCtxS;

/* Accumulate the three bands of one sample into the row accumulator. */
static inline void adm_cm_accum_px_s(const AdmCmCtxS *c, int i, int j, float inner[3])
{
    const ptrdiff_t idx = (ptrdiff_t)i * c->src_px_stride + j;
    float xh = c->src->band_h[idx] * c->rfactor[0];
    float xv = c->src->band_v[idx] * c->rfactor[1];
    float xd = c->src->band_d[idx] * c->rfactor[2];
    float thr = 0.0;

    if (c->adm_bypass_cm == 0) {
        thr = adm_cm_thresh3x3_s(c->angles, c->flt_angles, c->csf_px_stride, c->w, c->h, i, j);
    }

    xh = fabsf(xh) - thr;
    xv = fabsf(xv) - thr;
    xd = fabsf(xd) - thr;

    xh = xh < 0.0f ? 0.0f : xh;
    xv = xv < 0.0f ? 0.0f : xv;
    xd = xd < 0.0f ? 0.0f : xd;

    if (c->adm_p_norm == 3.0) {
        inner[0] += (xh * xh * xh);
        inner[1] += (xv * xv * xv);
        inner[2] += (xd * xd * xd);
    } else {
        inner[0] += powf(xh, c->adm_p_norm);
        inner[1] += powf(xv, c->adm_p_norm);
        inner[2] += powf(xd, c->adm_p_norm);
    }
}

/* One row: the optional first / last column (when the border region reaches
 * the frame edge) plus the interior columns. */
static void adm_cm_row_s(const AdmCmCtxS *c, int i, bool left_edge, bool right_edge, int start_col,
                         int end_col, float inner[3])
{
    if (left_edge) {
        adm_cm_accum_px_s(c, i, 0, inner);
    }
    for (int j = start_col; j < end_col; ++j) {
        adm_cm_accum_px_s(c, i, j, inner);
    }
    if (right_edge) {
        adm_cm_accum_px_s(c, i, c->w - 1, inner);
    }
}

float adm_cm_s(const adm_dwt_band_t_s *src, const adm_dwt_band_t_s *csf_f,
               const adm_dwt_band_t_s *csf_a, int w, int h, int src_stride, int flt_stride,
               int csf_a_stride, double border_factor, int scale, double adm_norm_view_dist,
               int adm_ref_display_height, int adm_csf_mode, double luminance_level,
               double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
               int adm_bypass_cm, double adm_p_norm, double adm_f1s0, double adm_f1s1,
               double adm_f1s2, double adm_f1s3, double adm_f2s0, double adm_f2s1, double adm_f2s2,
               double adm_f2s3)
{
    (void)flt_stride;

    AdmCmCtxS c;
    c.src = src;
    c.angles[0] = csf_a->band_h;
    c.angles[1] = csf_a->band_v;
    c.angles[2] = csf_a->band_d;
    c.flt_angles[0] = csf_f->band_h;
    c.flt_angles[1] = csf_f->band_v;
    c.flt_angles[2] = csf_f->band_d;
    c.src_px_stride = src_stride / sizeof(float);
    c.csf_px_stride = csf_a_stride / sizeof(float);
    c.w = w;
    c.h = h;
    c.adm_bypass_cm = adm_bypass_cm;
    c.adm_p_norm = adm_p_norm;
    adm_csf_rfactor_s(scale, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                      luminance_level, adm_csf_scale, adm_csf_diag_scale, adm_f1s0, adm_f1s1,
                      adm_f1s2, adm_f1s3, adm_f2s0, adm_f2s1, adm_f2s2, adm_f2s3, c.rfactor);

    /* The computation of the scales is not required for the regions which lie outside the frame borders */
    const AdmBorderS b = adm_border_s(w, h, border_factor);
    const bool left_edge = b.left <= 0;
    const bool right_edge = b.right > (w - 1);
    const int start_col = (b.left > 1) ? b.left : 1;
    const int end_col = (b.right < (w - 1)) ? b.right : (w - 1);
    const int start_row = (b.top > 1) ? b.top : 1;
    const int end_row = (b.bottom < (h - 1)) ? b.bottom : (h - 1);

    float accum[3] = {0, 0, 0};
    float inner[3] = {0, 0, 0};

    /* i=0 */
    if (b.top <= 0) {
        adm_cm_row_s(&c, 0, left_edge, right_edge, start_col, end_col, inner);
    }
    adm_fold3_s(inner, accum);
    /* 0 < i < h-1 */
    for (int i = start_row; i < end_row; ++i) {
        adm_cm_row_s(&c, i, left_edge, right_edge, start_col, end_col, inner);
        adm_fold3_s(inner, accum);
    }
    /* i=h-1 */
    if (b.bottom > (h - 1)) {
        adm_cm_row_s(&c, h - 1, left_edge, right_edge, start_col, end_col, inner);
    }
    adm_fold3_s(inner, accum);

    const float num_scale_h =
        powf(accum[0], 1.0f / adm_p_norm) +
        get_noise_constant(b.right - b.left, b.bottom - b.top, adm_noise_weight, adm_p_norm);
    const float num_scale_v =
        powf(accum[1], 1.0f / adm_p_norm) +
        get_noise_constant(b.right - b.left, b.bottom - b.top, adm_noise_weight, adm_p_norm);
    const float num_scale_d =
        powf(accum[2], 1.0f / adm_p_norm) +
        get_noise_constant(b.right - b.left, b.bottom - b.top, adm_noise_weight, adm_p_norm);

    return (num_scale_h + num_scale_v + num_scale_d);
}

/* ------------------------------------------------------------------------- */
/* DWT                                                                       */
/* ------------------------------------------------------------------------- */

/* Symmetric-extension source indices of the four Daubechies-2 taps for every
 * output sample of one dimension: `ind[k][i]` is the input index read by tap
 * `k` when producing output `i` (Index = 2 * i - 1 + k). */
static void dwt2_src_indices_1d_s(int *const *ind, int n)
{
    for (int i = 0; i < (n + 1) / 2; ++i) {
        int ind0 = 2 * i - 1;
        ind0 = (ind0 < 0) ? -ind0 : ((ind0 >= n) ? (2 * n - ind0 - 1) : ind0);
        int ind1 = 2 * i;
        if (ind1 >= n) {
            ind1 = (2 * n - ind1 - 1);
        }
        int ind2 = 2 * i + 1;
        if (ind2 >= n) {
            ind2 = (2 * n - ind2 - 1);
        }
        int ind3 = 2 * i + 2;
        if (ind3 >= n) {
            ind3 = (2 * n - ind3 - 1);
        }
        ind[0][i] = ind0;
        ind[1][i] = ind1;
        ind[2][i] = ind2;
        ind[3][i] = ind3;
    }
}

// This function stores the imgcoeff values used in adm_dwt2_s in buffers, which reduces the control code cycles.
void dwt2_src_indices_filt_s(int **src_ind_y, int **src_ind_x, int w, int h)
{
    /* Vertical pass */
    dwt2_src_indices_1d_s(src_ind_y, h);
    /* Horizontal pass */
    dwt2_src_indices_1d_s(src_ind_x, w);
}

/* The float-ADM DWT2 feeds immutable Netflix golden scores.  Keep its four-tap
 * accumulation on an explicit multiply-then-add contract on every supported
 * compiler without changing any other arithmetic in this translation unit.
 * The earlier file-scope Clang pragma changed unrelated ADM reductions; the
 * function-scoped pragma and GCC attribute deliberately constrain only DWT2.
 * See ADR-1057's 2026-08-31 correction.
 *
 * The body stays a single function on purpose: splitting the vertical and
 * horizontal passes into helpers would require replicating the
 * `optimize("-ffp-contract=off")` attribute on each of them and relying on
 * GCC's cross-attribute inlining behaviour to keep the contraction contract
 * — an ARM-only effect that the x86 golden gate cannot observe. ADR-0141 §2
 * load-bearing invariant (ADR-1057 fp-contract bracket); see ADR-1141. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("-ffp-contract=off")))
#endif
// NOLINTNEXTLINE(readability-function-size) — ADR-1057 / ADR-1141
int adm_dwt2_s(const float *src, const adm_dwt_band_t_s *dst, int **ind_y, int **ind_x, int w,
               int h, int src_stride, int dst_stride)
{
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const float *filter_lo = dwt2_db2_coeffs_lo_s;
    const float *filter_hi = dwt2_db2_coeffs_hi_s;

    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

    float *tmplo = aligned_malloc(ALIGN_CEIL(sizeof(float) * w), MAX_ALIGN);
    if (!tmplo) {
        return -ENOMEM;
    }
    float *tmphi = aligned_malloc(ALIGN_CEIL(sizeof(float) * w), MAX_ALIGN);
    if (!tmphi) {
        aligned_free(tmplo);
        return -ENOMEM;
    }

    for (int i = 0; i < (h + 1) / 2; ++i) {
        /* Vertical pass. */
        for (int j = 0; j < w; ++j) {
            const float s0 = src[ind_y[0][i] * src_px_stride + j];
            const float s1 = src[ind_y[1][i] * src_px_stride + j];
            const float s2 = src[ind_y[2][i] * src_px_stride + j];
            const float s3 = src[ind_y[3][i] * src_px_stride + j];

            float accum = 0;
            accum += filter_lo[0] * s0;
            accum += filter_lo[1] * s1;
            accum += filter_lo[2] * s2;
            accum += filter_lo[3] * s3;
            tmplo[j] = accum;

            accum = 0;
            accum += filter_hi[0] * s0;
            accum += filter_hi[1] * s1;
            accum += filter_hi[2] * s2;
            accum += filter_hi[3] * s3;
            tmphi[j] = accum;
        }

        /* Horizontal pass (lo and hi). */
        for (int j = 0; j < (w + 1) / 2; ++j) {
            const int j0 = ind_x[0][j];
            const int j1 = ind_x[1][j];
            const int j2 = ind_x[2][j];
            const int j3 = ind_x[3][j];
            float s0 = tmplo[j0];
            float s1 = tmplo[j1];
            float s2 = tmplo[j2];
            float s3 = tmplo[j3];

            float accum = 0;
            accum += filter_lo[0] * s0;
            accum += filter_lo[1] * s1;
            accum += filter_lo[2] * s2;
            accum += filter_lo[3] * s3;
            dst->band_a[i * dst_px_stride + j] = accum;

            accum = 0;
            accum += filter_hi[0] * s0;
            accum += filter_hi[1] * s1;
            accum += filter_hi[2] * s2;
            accum += filter_hi[3] * s3;
            dst->band_v[i * dst_px_stride + j] = accum;
            s0 = tmphi[j0];
            s1 = tmphi[j1];
            s2 = tmphi[j2];
            s3 = tmphi[j3];

            accum = 0;
            accum += filter_lo[0] * s0;
            accum += filter_lo[1] * s1;
            accum += filter_lo[2] * s2;
            accum += filter_lo[3] * s3;
            dst->band_h[i * dst_px_stride + j] = accum;

            accum = 0;
            accum += filter_hi[0] * s0;
            accum += filter_hi[1] * s1;
            accum += filter_hi[2] * s2;
            accum += filter_hi[3] * s3;
            dst->band_d[i * dst_px_stride + j] = accum;
        }
    }

    aligned_free(tmplo);
    aligned_free(tmphi);
    return 0;
}

int adm_dwt2_lo_s(const float *src, const adm_dwt_band_t_s *dst, int **ind_y, int **ind_x, int w,
                  int h, int src_stride, int dst_stride)
{
    const float *filter_lo = dwt2_db2_coeffs_lo_s;

    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

    float *tmplo = aligned_malloc(ALIGN_CEIL(sizeof(float) * w), MAX_ALIGN);
    if (!tmplo) {
        return -ENOMEM;
    }

    for (int i = 0; i < (h + 1) / 2; ++i) {
        /* Vertical pass. */
        for (int j = 0; j < w; ++j) {
            const float s0 = src[ind_y[0][i] * src_px_stride + j];
            const float s1 = src[ind_y[1][i] * src_px_stride + j];
            const float s2 = src[ind_y[2][i] * src_px_stride + j];
            const float s3 = src[ind_y[3][i] * src_px_stride + j];

            float accum = 0;
            accum += filter_lo[0] * s0;
            accum += filter_lo[1] * s1;
            accum += filter_lo[2] * s2;
            accum += filter_lo[3] * s3;
            tmplo[j] = accum;
        }

        /* Horizontal pass (lo). */
        for (int j = 0; j < (w + 1) / 2; ++j) {
            const float s0 = tmplo[ind_x[0][j]];
            const float s1 = tmplo[ind_x[1][j]];
            const float s2 = tmplo[ind_x[2][j]];
            const float s3 = tmplo[ind_x[3][j]];

            float accum = 0;
            accum += filter_lo[0] * s0;
            accum += filter_lo[1] * s1;
            accum += filter_lo[2] * s2;
            accum += filter_lo[3] * s3;
            dst->band_a[i * dst_px_stride + j] = accum;
        }
    }

    aligned_free(tmplo);
    return 0;
}

/* Four-tap double-precision filter response, accumulated tap by tap. */
static inline double adm_dwt2_tap4_d(const double *filter, double s0, double s1, double s2,
                                     double s3)
{
    double accum = 0;
    accum += filter[0] * s0;
    accum += filter[1] * s1;
    accum += filter[2] * s2;
    accum += filter[3] * s3;
    return accum;
}

/* Horizontal pass of output row `i` of the double-precision DWT. */
static inline void adm_dwt2_hpass_d(const double *tmplo, const double *tmphi,
                                    const adm_dwt_band_t_d *dst, int *const *ind_x, int i, int w,
                                    int dst_px_stride)
{
    const ptrdiff_t row = (ptrdiff_t)i * dst_px_stride;

    for (int j = 0; j < (w + 1) / 2; ++j) {
        const int j0 = ind_x[0][j];
        const int j1 = ind_x[1][j];
        const int j2 = ind_x[2][j];
        const int j3 = ind_x[3][j];

        dst->band_a[row + j] =
            adm_dwt2_tap4_d(dwt2_db2_coeffs_lo_d, tmplo[j0], tmplo[j1], tmplo[j2], tmplo[j3]);
        dst->band_v[row + j] =
            adm_dwt2_tap4_d(dwt2_db2_coeffs_hi_d, tmplo[j0], tmplo[j1], tmplo[j2], tmplo[j3]);
        dst->band_h[row + j] =
            adm_dwt2_tap4_d(dwt2_db2_coeffs_lo_d, tmphi[j0], tmphi[j1], tmphi[j2], tmphi[j3]);
        dst->band_d[row + j] =
            adm_dwt2_tap4_d(dwt2_db2_coeffs_hi_d, tmphi[j0], tmphi[j1], tmphi[j2], tmphi[j3]);
    }
}

int adm_dwt2_d(const double *src, const adm_dwt_band_t_d *dst, int **ind_y, int **ind_x, int w,
               int h, int src_stride, int dst_stride)
{
    const int src_px_stride = src_stride / sizeof(double);
    const int dst_px_stride = dst_stride / sizeof(double);

    double *tmplo = aligned_malloc(ALIGN_CEIL(sizeof(double) * w), MAX_ALIGN);
    if (!tmplo) {
        return -ENOMEM;
    }
    double *tmphi = aligned_malloc(ALIGN_CEIL(sizeof(double) * w), MAX_ALIGN);
    if (!tmphi) {
        aligned_free(tmplo);
        return -ENOMEM;
    }

    for (int i = 0; i < (h + 1) / 2; ++i) {
        /* Vertical pass. */
        for (int j = 0; j < w; ++j) {
            const double s0 = src[ind_y[0][i] * src_px_stride + j];
            const double s1 = src[ind_y[1][i] * src_px_stride + j];
            const double s2 = src[ind_y[2][i] * src_px_stride + j];
            const double s3 = src[ind_y[3][i] * src_px_stride + j];

            tmplo[j] = adm_dwt2_tap4_d(dwt2_db2_coeffs_lo_d, s0, s1, s2, s3);
            tmphi[j] = adm_dwt2_tap4_d(dwt2_db2_coeffs_hi_d, s0, s1, s2, s3);
        }

        /* Horizontal pass (lo and hi). */
        adm_dwt2_hpass_d(tmplo, tmphi, dst, ind_x, i, w, dst_px_stride);
    }

    aligned_free(tmplo);
    aligned_free(tmphi);
    return 0;
}

/* planner probe */
