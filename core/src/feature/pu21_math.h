/**
 *
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

/*
 * PU21 (Perceptually Uniform encoding, 2021) — closed-form transfer function
 * + SMPTE ST.2084 (PQ) inverse-EOTF, for the pu21 HDR feature extractor.
 *
 * The PU21 encoder maps absolute display luminance (cd/m^2, a.k.a. nits) onto
 * a perceptually-uniform scale on which the existing SDR full-reference
 * metrics (PSNR, SSIM) become perceptually meaningful for HDR content.
 *
 * The 4 variant coefficient tables and the encoding formula are taken verbatim
 * from the official gfxdisp/pu21 reference (matlab/pu21_encoder.m, BSD
 * 3-Clause, University of Cambridge 2021) and were independently cross-checked
 * against the FFmpeg pu21 filter C port — all 28 coefficients (4 variants x 7
 * params) match byte-for-byte across both sources.
 *
 * References:
 *   - Mantiuk & Azimi, "PU21: A novel perceptually uniform encoding for
 *     adapting existing quality metrics for HDR", QoMEX/PCS 2021.
 *   - https://github.com/gfxdisp/pu21  (matlab/pu21_encoder.m)
 *
 * Math note (verified, fp64): the inner term reuses Y^p[3] in BOTH the
 * numerator and the denominator; the outer exponent is p[4]; the scale is
 * p[6]; the offset is p[5]; the final value is clamped to >= 0:
 *
 *     Yp = pow(Y, p[3]);
 *     V  = max( p[6] * ( pow((p[0] + p[1]*Yp) / (1.0 + p[2]*Yp), p[4]) - p[5] ), 0 );
 *
 * All math is performed in double precision (the reference is MATLAB double).
 */

#ifndef __VMAF_SRC_FEATURE_PU21_MATH_H__
#define __VMAF_SRC_FEATURE_PU21_MATH_H__

#include <math.h>

/* PU21 valid input domain (cd/m^2), per pu21_encoder.m. */
#define PU21_L_MIN 0.005
#define PU21_L_MAX 10000.0

/** @brief PU21 encoder coefficient-table variants. */
enum Pu21Variant {
    PU21_VARIANT_BANDING = 0,
    PU21_VARIANT_BANDING_GLARE, /**< canonical default */
    PU21_VARIANT_PEAKS,
    PU21_VARIANT_PEAKS_GLARE,
    PU21_VARIANT_COUNT,
};

/*
 * The 7-parameter coefficient table for each variant. CROSS-CHECKED VERBATIM
 * between the official gfxdisp/pu21 matlab/pu21_encoder.m and the FFmpeg pu21
 * filter C port (pu21_params[4][7]); identical to full printed precision in
 * both.
 */
static const double pu21_params[PU21_VARIANT_COUNT][7] = {
    /* banding */
    {1.070275272, 0.4088273932, 0.153224308, 0.2520326168, 1.063512885, 1.14115047, 521.4527484},
    /* banding_glare (canonical default) */
    {0.353487901, 0.3734658629, 8.277049286e-05, 0.9062562627, 0.09150303166, 0.9099517204,
     596.3148142},
    /* peaks */
    {1.043882782, 0.6459495343, 0.3194584211, 0.374025247, 1.114783422, 1.095360363, 384.9217577},
    /* peaks_glare */
    {816.885024, 1479.463946, 0.001253215609, 0.9329636822, 0.06746643971, 1.573435413,
     419.6006374},
};

/**
 * @brief Clamp absolute luminance to the PU21 valid input domain.
 * @param y luminance in cd/m^2
 * @return clamped luminance in [PU21_L_MIN, PU21_L_MAX]
 */
static inline double pu21_clamp_luminance(double y)
{
    if (y < PU21_L_MIN)
        return PU21_L_MIN;
    if (y > PU21_L_MAX)
        return PU21_L_MAX;
    return y;
}

/**
 * @brief Encode absolute luminance (cd/m^2) onto the PU21 perceptual scale.
 *
 * Caller is responsible for clamping @p y to the valid domain via
 * pu21_clamp_luminance() (the encoder does NOT re-clamp so the test vector can
 * exercise exact in-domain points). The returned value spans ~0..600.
 *
 * @param y  luminance in cd/m^2 (already clamped to [PU21_L_MIN, PU21_L_MAX])
 * @param p  7-element coefficient row (e.g. pu21_params[PU21_VARIANT_*])
 * @return PU21-encoded value, >= 0
 */
static inline double pu21_encode(double y, const double *p)
{
    const double yp = pow(y, p[3]);
    const double inner = (p[0] + p[1] * yp) / (1.0 + p[2] * yp);
    const double v = p[6] * (pow(inner, p[4]) - p[5]);
    return v > 0.0 ? v : 0.0;
}

/*
 * SMPTE ST.2084 (PQ) constants. The EOTF returns a NORMALISED [0,1] display
 * signal which must be scaled by 10000 to obtain absolute cd/m^2 (the PQ peak
 * code value maps to 10000 nits). These constants are standard and
 * double-confirmed.
 */
#define PU21_PQ_M1 0.1593017578125
#define PU21_PQ_M2 78.84375
#define PU21_PQ_C1 0.8359375
#define PU21_PQ_C2 18.8515625
#define PU21_PQ_C3 18.6875
#define PU21_PQ_PEAK_NITS 10000.0

/**
 * @brief Decode a normalised PQ (ST.2084) signal to absolute luminance.
 *
 * @param ep normalised non-linear PQ signal in [0,1]
 * @return absolute display luminance in cd/m^2 (in [0, 10000])
 */
static inline double pu21_pq_eotf_nits(double ep)
{
    if (ep <= 0.0)
        return 0.0;
    if (ep > 1.0)
        ep = 1.0;
    const double ep_m2 = pow(ep, 1.0 / PU21_PQ_M2);
    double num = ep_m2 - PU21_PQ_C1;
    if (num < 0.0)
        num = 0.0;
    const double den = PU21_PQ_C2 - PU21_PQ_C3 * ep_m2;
    /* den is > 0 across the whole [0,1] domain (C2 - C3 = 0.1640625 at ep=1),
     * but guard against a pathological zero to keep pow() in-domain. */
    if (den <= 0.0)
        return PU21_PQ_PEAK_NITS;
    const double y = pow(num / den, 1.0 / PU21_PQ_M1);
    return y * PU21_PQ_PEAK_NITS;
}

#endif /* __VMAF_SRC_FEATURE_PU21_MATH_H__ */
