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
 * ssim_accumulate_lane.h — single-lane scalar-double reduction helper.
 *
 * Encodes the ADR-0139 per-lane SSIM accumulation formula in one place
 * so all SIMD variants (AVX2, AVX-512, NEON) share identical scalar
 * type promotion. The SIMD callers pre-compute the float-valued
 * intermediates (`srsc`, `l_den`, `c_den`, `sv_f`) in vector float
 * — those are bit-exact by construction — spill them to an aligned
 * float buffer, then call this helper per lane with `C1` / `C2`
 * passed as the C `double` literals required for bit-identity with
 * `ssim_accumulate_default_scalar` in ssim_tools.c.
 *
 * The helper is `static inline`: no TU-boundary overhead, no symbol
 * collision, and the compiler can fully unroll the per-lane loop in
 * the caller.
 *
 * Fork-local; see ADR-0139 + ADR-0141.
 */

#ifndef VMAF_FEATURE_IQA_SSIM_ACCUMULATE_LANE_H_
#define VMAF_FEATURE_IQA_SSIM_ACCUMULATE_LANE_H_

#include <math.h>

/* Per-lane reduction. All input floats are single-rounded (no FMA).
 *
 *   lv = (2.0 * rm * cm + C1) / l_den     — double, matches scalar
 *   cv = (2.0 * srsc + C2)   / c_den      — double, matches scalar
 *   sv = (csb + C3) / (srsc + C3)         — float-computed, promoted
 *                                            to double on the `= sv_f`
 *                                            assignment below
 *   ssim += lv * cv * sv
 *
 * Why `C1` / `C2` are taken as `float`: their callers (the scalar
 * reference and the three SIMD variants) already hold them as
 * `float`; the `2.0 *` literal on the numerator is the C `double`
 * that drives the double promotion.
 */
static inline void ssim_accumulate_lane(float rm, float cm, float srsc, float l_den, float c_den,
                                        float sv_f, float C1, float C2, double *local_ssim,
                                        double *local_l, double *local_c, double *local_s)
{
    const double lv = (2.0 * rm * cm + C1) / l_den;
    const double cv = (2.0 * srsc + C2) / c_den;
    const double sv = sv_f;
    *local_ssim += lv * cv * sv;
    *local_l += lv;
    *local_c += cv;
    *local_s += sv;
}

/* Scalar-tail step shared by every SIMD accumulator. Computes the
 * float-valued intermediates in scalar (bit-exact to the vector
 * ones by construction) and delegates the per-lane reduction to
 * `ssim_accumulate_lane`. The vectorised block body in the SIMD
 * TUs uses the same helper after spilling its `__m*` vectors to
 * stack buffers — so both paths share one source of truth for the
 * ADR-0139 double-promotion contract. */
static inline void ssim_accumulate_scalar_step(float rm, float cm, float rs, float cs, float sb,
                                               float C1, float C2, float C3, double *local_ssim,
                                               double *local_l, double *local_c, double *local_s)
{
    const float srsc = sqrtf(rs * cs);
    const float l_den = rm * rm + cm * cm + C1;
    const float c_den = rs + cs + C2;
    const float csb = (sb < 0.0f && srsc <= 0.0f) ? 0.0f : sb;
    const float sv_f = (csb + C3) / (srsc + C3);
    ssim_accumulate_lane(rm, cm, srsc, l_den, c_den, sv_f, C1, C2, local_ssim, local_l, local_c,
                         local_s);
}

#endif /* VMAF_FEATURE_IQA_SSIM_ACCUMULATE_LANE_H_ */
