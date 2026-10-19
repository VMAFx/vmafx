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

#ifndef ADM_TOOLS_H_
#define ADM_TOOLS_H_

#include <math.h>
#include "common/macros.h"
#include "barten_csf_tools.h"
#include "adm_csf_tools.h"

/* Upstream defines nine ADM_CM_THRESH_S_{0_0, 0_W_M_1, 0_J, H_M_1_0, H_M_1_W_M_1,
 * H_M_1_J, I_J, I_0, I_W_M_1} macros here, one per corner, edge and interior
 * case of the 3x3 masking-threshold sum. The fork has no expansion of them
 * since ADR-1141: adm_tools.c::adm_cm_thresh3x3_s() is their closed form, with
 * the nine terms in the macros' order. They are not carried as dead text. An
 * upstream change to that macro family is ported into adm_cm_thresh3x3_s()
 * (and its integer twins in integer_adm_kernels.h), see docs/rebase-notes.md. */

typedef struct adm_dwt_band_t_s {
    float *band_a; /* Low-pass V + low-pass H. */
    float *band_v; /* Low-pass V + high-pass H. */
    float *band_h; /* High-pass V + low-pass H. */
    float *band_d; /* High-pass V + high-pass H. */
} adm_dwt_band_t_s;

typedef struct adm_dwt_band_t_d {
    double *band_a; /* Low-pass V + low-pass H. */
    double *band_v; /* Low-pass V + high-pass H. */
    double *band_h; /* High-pass V + low-pass H. */
    double *band_d; /* High-pass V + high-pass H. */
} adm_dwt_band_t_d;

float adm_sum_cube_s(const float *x, int w, int h, int stride, double border_factor,
                     double adm_p_norm);
float adm_sum_cube_s_p3(const float *x, int w, int h, int stride, double border_factor);

void adm_decouple_s(const adm_dwt_band_t_s *ref, const adm_dwt_band_t_s *dis,
                    const adm_dwt_band_t_s *r, const adm_dwt_band_t_s *a, int w, int h,
                    int ref_stride, int dis_stride, int r_stride, int a_stride,
                    double border_factor, double adm_enhn_gain_limit);

/* One band of the CSF stage over a `w` x `h` plane: `dst = factor * src` and
 * `flt = one_by_30 * |dst|`, the second as a double product narrowed to
 * float. adm_csf_plane_s() is the reference; a SIMD kernel with this
 * signature returns its bits (x86/float_adm_avx2.h, x86/float_adm_avx512.h).
 * `one_by_30` is adm_tools.c's FLOAT_ONE_BY_30, passed by adm_csf_planes_s(). */
typedef void (*adm_csf_plane_fn)(const float *src, float *dst, float *flt, int w, int h,
                                 int src_stride, int dst_stride, float factor, double one_by_30);

void adm_csf_plane_s(const float *src_ptr, float *dst_ptr, float *flt_ptr, int w, int h,
                     int src_stride, int dst_stride, float factor, double one_by_30);

/* adm_csf_s() with the band kernel chosen by the caller. */
void adm_csf_planes_s(const adm_dwt_band_t_s *src, const adm_dwt_band_t_s *dst,
                      const adm_dwt_band_t_s *flt, int orig_h, int scale, int w, int h,
                      int src_stride, int dst_stride, double border_factor,
                      double adm_norm_view_dist, int adm_ref_display_height, int adm_csf_mode,
                      double luminance_level, double adm_csf_scale, double adm_csf_diag_scale,
                      double adm_f1s0, double adm_f1s1, double adm_f1s2, double adm_f1s3,
                      double adm_f2s0, double adm_f2s1, double adm_f2s2, double adm_f2s3,
                      adm_csf_plane_fn plane);

void adm_csf_s(const adm_dwt_band_t_s *src, const adm_dwt_band_t_s *dst,
               const adm_dwt_band_t_s *flt, int orig_h, int scale, int w, int h, int src_stride,
               int dst_stride, double border_factor, double adm_norm_view_dist,
               int adm_ref_display_height, int adm_csf_mode, double luminance_level,
               double adm_csf_scale, double adm_csf_diag_scale, double adm_f1s0, double adm_f1s1,
               double adm_f1s2, double adm_f1s3, double adm_f2s0, double adm_f2s1, double adm_f2s2,
               double adm_f2s3);

void adm_cm_thresh_s(const adm_dwt_band_t_s *src, float *dst, int w, int h, int src_stride,
                     int dst_stride);

float adm_csf_den_scale_s(const adm_dwt_band_t_s *src, int orig_h, int scale, int w, int h,
                          int src_stride, double border_factor, double adm_norm_view_dist,
                          int adm_ref_display_height, int adm_csf_mode, double luminance_level,
                          double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                          double adm_p_norm, double adm_f1s0, double adm_f1s1, double adm_f1s2,
                          double adm_f1s3, double adm_f2s0, double adm_f2s1, double adm_f2s2,
                          double adm_f2s3);
float adm_csf_den_scale_s_p3(const adm_dwt_band_t_s *src, int orig_h, int scale, int w, int h,
                             int src_stride, double border_factor, double adm_norm_view_dist,
                             int adm_ref_display_height, int adm_csf_mode, double luminance_level,
                             double adm_csf_scale, double adm_csf_diag_scale,
                             double adm_noise_weight, double adm_f1s0, double adm_f1s1,
                             double adm_f1s2, double adm_f1s3, double adm_f2s0, double adm_f2s1,
                             double adm_f2s2, double adm_f2s3);

float adm_cm_s(const adm_dwt_band_t_s *src, const adm_dwt_band_t_s *dst,
               const adm_dwt_band_t_s *csf_a, int w, int h, int src_stride, int dst_stride,
               int csf_a_stride, double border_factor, int scale, double adm_norm_view_dist,
               int adm_ref_display_height, int adm_csf_mode, double luminance_level,
               double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
               int adm_bypass_cm, double adm_p_norm, double adm_f1s0, double adm_f1s1,
               double adm_f1s2, double adm_f1s3, double adm_f2s0, double adm_f2s1, double adm_f2s2,
               double adm_f2s3);
float adm_cm_s_p3(const adm_dwt_band_t_s *src, const adm_dwt_band_t_s *dst,
                  const adm_dwt_band_t_s *csf_a, int w, int h, int src_stride, int dst_stride,
                  int csf_a_stride, double border_factor, int scale, double adm_norm_view_dist,
                  int adm_ref_display_height, int adm_csf_mode, double luminance_level,
                  double adm_csf_scale, double adm_csf_diag_scale, double adm_noise_weight,
                  int adm_bypass_cm, double adm_f1s0, double adm_f1s1, double adm_f1s2,
                  double adm_f1s3, double adm_f2s0, double adm_f2s1, double adm_f2s2,
                  double adm_f2s3);

void dwt2_src_indices_filt_s(int **src_ind_y, int **src_ind_x, int w, int h);

int adm_dwt2_s(const float *src, const adm_dwt_band_t_s *dst, int **ind_y, int **ind_x, int w,
               int h, int src_stride, int dst_stride);

int adm_dwt2_lo_s(const float *src, const adm_dwt_band_t_s *dst, int **ind_y, int **ind_x, int w,
                  int h, int src_stride, int dst_stride);

int adm_dwt2_d(const double *src, const adm_dwt_band_t_d *dst, int **ind_y, int **ind_x, int w,
               int h, int src_stride, int dst_stride);

/* ================= */
/* Noise floor model */
/* ================= */

/*
 * The following dwt visibility threshold parameters are taken from
 * "Visibility of Wavelet Quantization Noise"
 * by A. B. Watson, G. Y. Yang, J. A. Solomon and J. Villasenor
 * IEEE Trans. on Image Processing, Vol. 6, No 8, Aug. 1997
 * Page 1170, formula (7) and corresponding Table IV
 * Table IV has 2 entries for Cb and Cr thresholds
 * Chose those corresponding to subject "sfl" since they are lower
 * These thresholds were obtained and modeled for the 7-9 biorthogonal wavelet basis
 */
struct dwt_model_params {
    float a;
    float k;
    float f0;
    float g[4];
};

// 0 -> Y, 1 -> Cb, 2 -> Cr
static const struct dwt_model_params dwt_7_9_YCbCr_threshold[3] = {
    {.a = 0.495f, .k = 0.466f, .f0 = 0.401f, .g = {1.501f, 1.0f, 0.534f, 1.0f}},
    {.a = 1.633f, .k = 0.353f, .f0 = 0.209f, .g = {1.520f, 1.0f, 0.502f, 1.0f}},
    {.a = 0.944f, .k = 0.521f, .f0 = 0.404f, .g = {1.868f, 1.0f, 0.516f, 1.0f}}};

/*
 * The following dwt basis function amplitudes, A(lambda,theta), are taken from
 * "Visibility of Wavelet Quantization Noise"
 * by A. B. Watson, G. Y. Yang, J. A. Solomon and J. Villasenor
 * IEEE Trans. on Image Processing, Vol. 6, No 8, Aug. 1997
 * Page 1172, Table V
 * The table has been transposed, i.e. it can be used directly to obtain A[lambda][theta]
 * These amplitudes were calculated for the 7-9 biorthogonal wavelet basis
 */
static const float dwt_7_9_basis_function_amplitudes[6][4] = {
    {0.62171f, 0.67234f, 0.72709f, 0.67234f},     {0.34537f, 0.41317f, 0.49428f, 0.41317f},
    {0.18004f, 0.22727f, 0.28688f, 0.22727f},     {0.091401f, 0.11792f, 0.15214f, 0.11792f},
    {0.045943f, 0.059758f, 0.077727f, 0.059758f}, {0.023013f, 0.030018f, 0.039156f, 0.030018f}};

/*
 * lambda = 0 (finest scale), 1, 2, 3 (coarsest scale);
 * theta = 0 (ll), 1 (lh - vertical), 2 (hh - diagonal), 3(hl - horizontal).
 */
static FORCE_INLINE float dwt_quant_step(const struct dwt_model_params *params, int lambda,
                                         int theta, double adm_norm_view_dist,
                                         int adm_ref_display_height)
{
    // Formula (1), page 1165 - display visual resolution (DVR), in pixels/degree of visual angle. This should be 56.55
    float r = (float)(adm_norm_view_dist * adm_ref_display_height * M_PI / 180.0);

    // Formula (9), page 1171
    float temp = (float)log10(pow(2.0, lambda + 1) * params->f0 * params->g[theta] / r);
    /* Upstream's statements (Netflix/vmaf libvmaf/src/feature/adm_tools.h,
     * dwt_quant_step()): r and temp are rounded to float where they are
     * stored, and the exponent k * temp * temp is a float product whose
     * result alone is promoted for pow(). Keeping any of the three in double
     * changes every CSF weight in the last digits and with it every float_adm
     * score. The cast widens the product's result, as upstream's implicit
     * promotion does (ADR-1489). */
    float Q = (float)(2.0 * params->a * pow(10.0, (double)(params->k * temp * temp)) /
                      dwt_7_9_basis_function_amplitudes[lambda][theta]);

    return Q;
}

#endif /* ADM_TOOLS_H_ */
