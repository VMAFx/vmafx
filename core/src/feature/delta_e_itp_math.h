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

#ifndef DELTA_E_ITP_MATH_H_
#define DELTA_E_ITP_MATH_H_

#include <math.h>

/*
 * Deterministic SMPTE ST-2084 (PQ) transfer functions used by the
 * ΔE-ITP (Delta E ITP) colour-difference extractor.
 *
 * ΔE-ITP (ITU-R BT.2124-0 Annex 1) measures the perceptual visibility
 * of colour differences in HDR/WCG video.  The PQ EOTF decodes the
 * nonlinear PQ signal to display-referred luminance (cd/m^2); the PQ
 * inverse EOTF re-encodes LMS luminance into the nonlinear domain that
 * the ICtCp space is defined on.
 *
 * Constants are taken verbatim from ITU-R BT.2124-0 §2 Step 2 (which
 * cites SMPTE ST-2084:2014), triple-confirmed against the colour-science
 * `st_2084.py` reference and the SMPTE standard:
 *   m1 = 2610/16384      = 0.1593017578125
 *   m2 = 2523/4096 * 128 = 78.84375
 *   c1 = 3424/4096       = 0.8359375     (= c3 - c2 + 1)
 *   c2 = 2413/4096 * 32  = 18.8515625
 *   c3 = 2392/4096 * 32  = 18.6875
 *
 * Precision: all math is double precision (matching the ciede.c
 * precedent for per-pixel colour metrics).  libm `pow` differs by at
 * most ~1 ulp across glibc/musl/macOS libSystem; the resulting drift
 * on the linear luminance is < ~1e-9 relative, negligible against the
 * x720 JND scale of ΔE-ITP and well within the test's places=4 bound.
 * A committed deterministic LUT (cf. ssimulacra2_math.h) is deferred to
 * the SIMD / snapshot follow-up, mirroring the SSIMULACRA 2 staging.
 */

#define VMAF_PQ_M1 (2610.0 / 16384.0)
#define VMAF_PQ_M2 (2523.0 / 4096.0 * 128.0)
#define VMAF_PQ_C1 (3424.0 / 4096.0)
#define VMAF_PQ_C2 (2413.0 / 4096.0 * 32.0)
#define VMAF_PQ_C3 (2392.0 / 4096.0 * 32.0)

/* PQ peak luminance: the EOTF maps the nonlinear range [0, 1] onto
 * [0, 10000] cd/m^2 (ITU-R BT.2124-0 §2 Step 2 / BT.2100 Table 4). */
#define VMAF_PQ_PEAK_LUMINANCE 10000.0

/* PQ EOTF: nonlinear E' in [0, 1] -> display luminance in cd/m^2.
 *
 * Used to decode the input PQ R'G'B' signal to linear, display-referred
 * RGB before the RGB->LMS step (ITU-R BT.2124-0 §2 Step 2, input decode
 * direction).  Negative or out-of-domain inputs are clamped to 0 because
 * a nonlinear PQ code value is non-negative by construction; the
 * out-of-gamut "do not clamp" rule (BT.2124 Annex 4) applies to the LMS
 * / ICtCp chain, not to the input nonlinear signal. */
static inline double vmaf_pq_eotf(double ep)
{
    if (ep <= 0.0) {
        return 0.0;
    }
    const double e_pow = pow(ep, 1.0 / VMAF_PQ_M2);
    const double num = e_pow - VMAF_PQ_C1;
    const double den = VMAF_PQ_C2 - VMAF_PQ_C3 * e_pow;
    if (num <= 0.0 || den <= 0.0) {
        return 0.0;
    }
    return VMAF_PQ_PEAK_LUMINANCE * pow(num / den, 1.0 / VMAF_PQ_M1);
}

/* PQ inverse EOTF: display luminance in cd/m^2 -> nonlinear E' in [0, 1].
 *
 * Used to re-encode the LMS luminance values into the nonlinear domain
 * the ICtCp space is defined on (ITU-R BT.2124-0 §2 Step 2, encode
 * direction).  Negative inputs are NOT clamped — they preserve the sign
 * via an odd-power continuation so out-of-gamut measurements remain
 * meaningful (BT.2124 Annex 4 note). */
static inline double vmaf_pq_eotf_inv(double y)
{
    /* Odd extension for negative inputs: the rational base (num/den) can be
     * negative when y_pow < 0, making pow(num/den, M2) return NaN for a
     * fractional exponent.  Recurse on the positive value (which is always
     * well-defined) and negate the result.  This preserves the BT.2124
     * Annex 4 "no clamp / out-of-gamut" contract without NaN. */
    if (y < 0.0) {
        return -vmaf_pq_eotf_inv(-y);
    }
    const double yn = y / VMAF_PQ_PEAK_LUMINANCE;
    const double y_pow = pow(yn, VMAF_PQ_M1);
    const double num = VMAF_PQ_C1 + VMAF_PQ_C2 * y_pow;
    const double den = 1.0 + VMAF_PQ_C3 * y_pow;
    return pow(num / den, VMAF_PQ_M2);
}

#endif /* DELTA_E_ITP_MATH_H_ */
