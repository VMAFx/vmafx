/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
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

#include <math.h>
#include "common/macros.h"
#include <errno.h>

#ifndef BARTEN_CSF_TOOLS_H_
#define BARTEN_CSF_TOOLS_H_

static float linear_interpolate(float left_position, float left_value, float right_position,
                                float right_value, float sample_position)
{
    /* Upstream's expression: every operation in float (ADR-1489). */
    return left_value + ((right_value - left_value) / (right_position - left_position)) *
                            (sample_position - left_position);
}

/*
 * There were 6 luminance levels (in cd/m2) that were measured in the CSF experiments of HDR-VDP2
 * Note: HDR-VDP2 paper has 5 levels, but version 2.2.1 was updated with data for 6 levels:
 * 0.002, 0.02, 0.2, 2, 20, 150
 */
static const float barten_csf_param_anchors[6] = {0.002f, 0.02f, 0.2f, 2, 20, 150};

static const float barten_csf_params[6][5] = {{0.0160737f, 0.991265f, 3.74038f, 0.50722f, 4.46044f},
                                              {0.383873f, 0.800889f, 3.54104f, 0.682505f, 4.94958f},
                                              {0.929301f, 0.476505f, 4.37453f, 0.750315f, 5.28678f},
                                              {1.29776f, 0.405782f, 4.40602f, 0.935314f, 5.61425f},
                                              {1.49222f, 0.334278f, 3.79542f, 1.07327f, 6.4635f},
                                              {1.46213f, 0.394533f, 2.7755f, 1.16577f, 7.45665f}};

static const float barten_csf_sa[4] = {30.162f, 4.0627f, 1.6596f, 0.2712f};
static const float barten_mtf_params_a[4] = {0.424838596301290f, 0.572435103936480f,
                                             0.000167576239164937f, 0.00255872352306433f};
static const float barten_mtf_params_b[4] = {0.028f, 0.37f, 37, 360};

/* Luminance-dependent component of Barten's CSF */
static float barten_rod_cone_sens(float luminance_level)
{
    const float cvi_sens_drop = barten_csf_sa[1];   // p6 in the paper
    const float cvi_trans_slope = barten_csf_sa[2]; // p7 in the paper
    const float cvi_low_slope = barten_csf_sa[3];   // p8 in the paper
    /* Upstream's expression with its promotions written out: the quotient is
     * a float, and pow() takes and returns double. Written this way a C++
     * translation unit, where pow(float, float) is another function,
     * evaluates what a C one does (ADR-1489). */
    return (
        float)((double)barten_csf_sa[0] *
               pow(pow((double)(cvi_sens_drop / luminance_level), (double)cvi_trans_slope) + 1.0,
                   (double)-cvi_low_slope));
}

/*  MTF portion of Barten's CSF */
static float barten_mtf(float spatial_frequency)
{
    float mtf = 0.0;
    for (int i = 0; i <= 3; i++) {
        /* Upstream's expression: the argument of exp() is a float product,
         * promoted after it is formed (ADR-1489). */
        mtf = (float)(mtf + barten_mtf_params_a[i] *
                                exp((double)(-barten_mtf_params_b[i] * spatial_frequency)));
    }
    return mtf;
}

#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

/*
 * Simplified version of Barten's CSF used in HDR-VDP2 (ported from Matlab version HDR-VDP 2.2.1)
 * HDR-VDP-2: A calibrated visual metric for visibility and quality predictions in all luminance conditions
 * ACM Transactions on Graphics, Volume 30, Issue 4, July 2011, Article No.: 40, pp 1–14
 * Rafal Mantiuk, Kil Joong Kim, Allan G. Rempel, Wolfgang Heidrich
 *
 * The arithmetic is upstream's (Netflix/vmaf libvmaf/src/feature/barten_csf_tools.h,
 * barten_csf()): each float product and quotient is formed in float and its
 * result promoted for the math library; the three float factors of the return
 * value are multiplied in float, the double scale last. The casts name those
 * promotions and change no value in C; in a C++ translation unit they select
 * the double functions a C one calls (ADR-1489).
 */
static FORCE_INLINE float barten_csf(int lambda, double adm_norm_view_dist,
                                     int adm_ref_display_height, double adm_csf_lum_level,
                                     double adm_csf_scale)
{
    /* This is the display visual resolution (DVR), in pixels/degree of visual angle. It should be ~56.55. */
    const float r = (float)(adm_norm_view_dist * adm_ref_display_height * M_PI / 180.0);
    /* This is the nominal spatial frequency for each DWT level; first level (level = 0) is half of the DVR. */
    const float spatial_frequency = (float)(r / pow(2, lambda + 1));

    const double clamped_lum =
        CLAMP(adm_csf_lum_level, barten_csf_param_anchors[0], barten_csf_param_anchors[5]);

    int left_lum_index = 0;
    int right_lum_index = 0;
    int i = 0;
    while (i < 5) {
        if (clamped_lum >= barten_csf_param_anchors[i] &&
            clamped_lum <= barten_csf_param_anchors[i + 1]) {
            left_lum_index = i;
            right_lum_index = i + 1;
            break;
        }
        i++;
    }

    const float left_lum = barten_csf_param_anchors[left_lum_index];
    const float right_lum = barten_csf_param_anchors[right_lum_index];

    const float left_position = (float)log10((double)left_lum);
    const float right_position = (float)log10((double)right_lum);
    const float sample_position = (float)log10(clamped_lum);

    const float p_0 =
        linear_interpolate(left_position, barten_csf_params[left_lum_index][1], right_position,
                           barten_csf_params[right_lum_index][1], sample_position);
    const float p_1 =
        linear_interpolate(left_position, barten_csf_params[left_lum_index][2], right_position,
                           barten_csf_params[right_lum_index][2], sample_position);
    const float p_2 =
        linear_interpolate(left_position, barten_csf_params[left_lum_index][3], right_position,
                           barten_csf_params[right_lum_index][3], sample_position);
    const float p_3 =
        linear_interpolate(left_position, barten_csf_params[left_lum_index][4], right_position,
                           barten_csf_params[right_lum_index][4], sample_position);

    // these values can be derived by the matlab code:
    // metric_par = hdrvdp_parse_options({})
    // hdrvdp_ncsf(rho, lum, metric_par); (any rho works)
    const float a = (float)(1.0f + pow((double)(p_0 * spatial_frequency), (double)p_1));
    const float b =
        (float)(1.0f / pow(1 - exp(-pow((double)(spatial_frequency / 7), 2)), (double)p_2));

    /* neural contrast sensitivity function */
    const float csf = (float)(p_3 / pow((double)(a * b), 0.5));

    /* return entire CSF */
    return (float)((double)(csf * barten_mtf(spatial_frequency) *
                            barten_rod_cone_sens((float)adm_csf_lum_level)) *
                   adm_csf_scale);
}

static const float BLENDED_CSF_1080_3H[2][4] = {{0.01183f, 0.025026f, 0.04295f, 0.058621f},
                                                {0.004302f, 0.011778f, 0.023918f, 0.035901f}};

static const float BLENDED_CSF_1080_5H[2][4] = {{0.004212f, 0.014809f, 0.029642f, 0.047464f},
                                                {0.000984f, 0.005852f, 0.0146f, 0.027574f}};

static const float BLENDED_CSF_2160_3H[2][4] = {{0.00226f, 0.01183f, 0.025026f, 0.04295f},
                                                {0.000479f, 0.004302f, 0.011778f, 0.023918f}};

static const float BLENDED_CSF_2160_5H[2][4] = {{0.000092f, 0.004212f, 0.014809f, 0.029642f},
                                                {0.000050f, 0.000984f, 0.005852f, 0.0146f}};

static const float BLENDED_CSF_720_3H[2][4] = {{0.018715f, 0.035637f, 0.052798f, 0.061509f},
                                               {0.007999f, 0.018396f, 0.031851f, 0.037718f}};

static const float BLENDED_CSF_720_5H[2][4] = {{0.010144f, 0.022561f, 0.040309f, 0.05672f},
                                               {0.003463f, 0.010282f, 0.021839f, 0.034641f}};

static const float BLENDED_CSF_480_3H[2][4] = {{0.027961f, 0.045875f, 0.060275f, 0.056234f},
                                               {0.013572f, 0.026277f, 0.036959f, 0.034511f}};

static const float BLENDED_CSF_480_5H[2][4] = {{0.016781f, 0.032822f, 0.05032f, 0.061594f},
                                               {0.00691f, 0.016545f, 0.029917f, 0.037777f}};

/*
 * BLENDED_CSF coefficient arrays for v1.0.17+ models (with CSF bug fix)
 * These coefficients incorporate the L1 optimization scaling parameter fix
 */
static const float BLENDED_CSF_1080_3H_MAE[2][4] = {{0.011249f, 0.022606f, 0.035930f, 0.045673f},
                                                    {0.004097f, 0.010921f, 0.021430f, 0.031313f}};

static const float BLENDED_CSF_1080_5H_MAE[2][4] = {{0.004052f, 0.013939f, 0.026298f, 0.038833f},
                                                    {0.000927f, 0.005544f, 0.013415f, 0.024515f}};

static const float BLENDED_CSF_2160_3H_MAE[2][4] = {{0.002166f, 0.011249f, 0.022606f, 0.035930f},
                                                    {0.000447f, 0.004097f, 0.010921f, 0.021430f}};

static const float BLENDED_CSF_2160_5H_MAE[2][4] = {{0.000077f, 0.004052f, 0.013939f, 0.026298f},
                                                    {0.000045f, 0.000927f, 0.005544f, 0.013415f}};

static const float BLENDED_CSF_720_3H_MAE[2][4] = {{0.017329f, 0.030870f, 0.042134f, 0.047410f},
                                                   {0.007509f, 0.016707f, 0.028072f, 0.032722f}};

static const float BLENDED_CSF_720_5H_MAE[2][4] = {{0.009689f, 0.020577f, 0.034162f, 0.044523f},
                                                   {0.003302f, 0.009579f, 0.019661f, 0.030318f}};

static const float BLENDED_CSF_480_3H_MAE[2][4] = {{0.024969f, 0.037825f, 0.046676f, 0.043974f},
                                                   {0.012511f, 0.023424f, 0.032139f, 0.030166f}};

static const float BLENDED_CSF_480_5H_MAE[2][4] = {{0.015665f, 0.028766f, 0.040612f, 0.047483f},
                                                   {0.006514f, 0.015107f, 0.026476f, 0.032776f}};

/*
 * CSF function with CSF bug fix
 * Uses the corrected L1 optimization
 */
static FORCE_INLINE float barten_watson_blend_csf_mae(int scale, int theta,
                                                      double adm_norm_view_dist,
                                                      int adm_ref_display_height)
{
    if ((adm_ref_display_height == 1080 && adm_norm_view_dist == 3.0) ||
        (adm_ref_display_height == 2160 && adm_norm_view_dist == 1.5)) {
        // 2160@1.5H has the same PPD as 1080@3H (1.5*2160 == 3.0*1080 == 56.55 ppd)
        return BLENDED_CSF_1080_3H_MAE[theta][scale];
    } else if (adm_ref_display_height == 1080 && adm_norm_view_dist == 5.0) {
        return BLENDED_CSF_1080_5H_MAE[theta][scale];
    } else if (adm_ref_display_height == 2160 && adm_norm_view_dist == 3.0) {
        return BLENDED_CSF_2160_3H_MAE[theta][scale];
    } else if (adm_ref_display_height == 2160 && adm_norm_view_dist == 5.0) {
        return BLENDED_CSF_2160_5H_MAE[theta][scale];
    } else if (adm_ref_display_height == 720 && adm_norm_view_dist == 3.0) {
        return BLENDED_CSF_720_3H_MAE[theta][scale];
    } else if (adm_ref_display_height == 720 && adm_norm_view_dist == 5.0) {
        return BLENDED_CSF_720_5H_MAE[theta][scale];
    } else if (adm_ref_display_height == 480 && adm_norm_view_dist == 3.0) {
        return BLENDED_CSF_480_3H_MAE[theta][scale];
    } else if (adm_ref_display_height == 480 && adm_norm_view_dist == 5.0) {
        return BLENDED_CSF_480_5H_MAE[theta][scale];
    } else {
        return -EINVAL;
    }
}

/*
 * Legacy Blended CSF function (backward compatibility)
 * Always uses legacy coefficients for existing models
 */
static FORCE_INLINE float barten_watson_blend_csf(int scale, int theta, double adm_norm_view_dist,
                                                  int adm_ref_display_height)
{
    if ((adm_ref_display_height == 1080 && adm_norm_view_dist == 3.0) ||
        (adm_ref_display_height == 2160 && adm_norm_view_dist == 1.5)) {
        // 2160@1.5H has the same PPD as 1080@3H (1.5*2160 == 3.0*1080 == 56.55 ppd)
        return BLENDED_CSF_1080_3H[theta][scale];
    } else if (adm_ref_display_height == 1080 && adm_norm_view_dist == 5.0) {
        return BLENDED_CSF_1080_5H[theta][scale];
    } else if (adm_ref_display_height == 2160 && adm_norm_view_dist == 3.0) {
        return BLENDED_CSF_2160_3H[theta][scale];
    } else if (adm_ref_display_height == 2160 && adm_norm_view_dist == 5.0) {
        return BLENDED_CSF_2160_5H[theta][scale];
    } else if (adm_ref_display_height == 720 && adm_norm_view_dist == 3.0) {
        return BLENDED_CSF_720_3H[theta][scale];
    } else if (adm_ref_display_height == 720 && adm_norm_view_dist == 5.0) {
        return BLENDED_CSF_720_5H[theta][scale];
    } else if (adm_ref_display_height == 480 && adm_norm_view_dist == 3.0) {
        return BLENDED_CSF_480_3H[theta][scale];
    } else if (adm_ref_display_height == 480 && adm_norm_view_dist == 5.0) {
        return BLENDED_CSF_480_5H[theta][scale];
    } else {
        return -EINVAL;
    }
}

#endif /* BARTEN_CSF_TOOLS_H_ */
