/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Representability of the integer-ADM contrast-sensitivity weights.
 *
 *  The integer ADM pipeline stores the CSF weights of one DWT scale as
 *  fixed-point integers:
 *
 *    - scale 0 (16-bit pipeline): `uint16_t i_rfactor[3]`, with the
 *      horizontal / vertical bands scaled by 2^21 and the diagonal band
 *      by 2^23;
 *    - scales 1..3 (32-bit pipeline): `uint32_t i_rfactor[3]`, all three
 *      bands scaled by 2^32.
 *
 *  Those budgets were sized for the Watson97 CSF, whose weights sit around
 *  1e-2. The fork-added `adm_csf_mode` option (integer_adm.h) also exposes
 *  the Barten CSF, whose weights are ~1.2 at scale 0 and ~27 at scale 3 with
 *  the default `adm_csf_scale` of 1.0 -- 38x to 155x past the uint16_t
 *  ceiling at scale 0 and past the uint32_t ceiling at every other scale.
 *  The narrowing conversions in the extractors silently wrapped, and the
 *  resulting scores were nonsense rather than an error.
 *
 *  This header centralises the bounds so every integer-ADM backend (scalar,
 *  SIMD, CUDA, HIP, SYCL) rejects the same set of configurations up front.
 *  See ADR-1191 and
 *  docs/state.md :: T-UPSTREAM-1494-ADM-CSF-MODE-IRFACTOR-OVERFLOW-2026-09-03.
 */

#ifndef ADM_CSF_FIXED_POINT_H_
#define ADM_CSF_FIXED_POINT_H_

#include <errno.h>
#include <math.h>
#include <stdbool.h>

#include "log.h"

/* Fixed-point exponents of the integer-ADM CSF weights. */
#define ADM_CSF_SCALE0_HV_EXP (21)
#define ADM_CSF_SCALE0_D_EXP (23)
#define ADM_CSF_S123_EXP (32)

/* Exclusive upper bounds of the corresponding storage types. A converted
 * weight equal to the bound already wraps, so the comparisons below are
 * strict. */
#define ADM_CSF_SCALE0_LIMIT (65536.0)    /* 2^16, uint16_t i_rfactor  */
#define ADM_CSF_S123_LIMIT (4294967296.0) /* 2^32, uint32_t i_rfactor  */

/**
 * True when the scale-0 weights come from the upstream-tabulated constants
 * { 36453, 36453, 49417 } instead of a runtime conversion. Those constants
 * are in range by construction, so the representability check skips them --
 * and, more importantly, every backend must agree on when the fast path
 * applies so the check never rejects a configuration the extractor would
 * have served from the table.
 */
static inline bool adm_csf_scale0_tabulated(double adm_norm_view_dist, int adm_ref_display_height,
                                            int adm_csf_mode)
{
    /* ADM_CSF_MODE_WATSON97 is 0 in every enum that spells it (integer_adm.h,
     * adm_options.h); comparing against the literal keeps this header free of
     * a dependency on either. */
    return fabs(adm_norm_view_dist * (double)adm_ref_display_height - 3.0 * 1080.0) < 1.0e-8 &&
           adm_csf_mode == 0;
}

/**
 * Scale-0 CSF weights in fixed point, before the narrowing conversion to
 * `uint16_t`. `rfactor1` is { factor1, factor1, factor2 } as produced by the
 * backend's `adm_csf_factors()`; `fixed` receives the unnarrowed products so
 * a caller can both range-check them and convert them.
 *
 * The products are evaluated in `double` exactly as the pre-existing
 * `(uint16_t)(rfactor1[k] * pow(2, N))` expressions were -- `float * double`
 * promotes to `double` -- so routing the conversion through this helper is
 * bit-exact with the code it replaces.
 */
static inline void adm_csf_scale0_fixed(const float rfactor1[3], double adm_norm_view_dist,
                                        int adm_ref_display_height, int adm_csf_mode,
                                        double fixed[3])
{
    if (adm_csf_scale0_tabulated(adm_norm_view_dist, adm_ref_display_height, adm_csf_mode)) {
        fixed[0] = 36453.0;
        fixed[1] = 36453.0;
        fixed[2] = 49417.0;
        return;
    }
    fixed[0] = (double)rfactor1[0] * pow(2, ADM_CSF_SCALE0_HV_EXP);
    fixed[1] = (double)rfactor1[1] * pow(2, ADM_CSF_SCALE0_HV_EXP);
    fixed[2] = (double)rfactor1[2] * pow(2, ADM_CSF_SCALE0_D_EXP);
}

/**
 * True when `value` survives the narrowing conversion into a fixed-point
 * weight of `limit` (exclusive). Negative inputs are rejected as well: the
 * blended-CSF tables in barten_csf_tools.h return `-EINVAL` as a float when
 * asked for an (adm_norm_view_dist, adm_ref_display_height) pair they do not
 * tabulate, and converting a negative float to an unsigned integer type is
 * undefined behaviour (C17 6.3.1.4p1).
 */
static inline bool adm_csf_fixed_in_range(double value, double limit)
{
    return value >= 0.0 && value < limit;
}

/**
 * Range-check the CSF weights of one DWT scale against the storage the
 * integer pipeline uses for it. Returns 0 when the scale is representable
 * and -EINVAL (after logging which band overflowed) otherwise.
 *
 * `scale` is 0 for the 16-bit pipeline and 1..3 for the 32-bit pipeline;
 * `rfactor1` is { factor1, factor1, factor2 }.
 */
static inline int adm_csf_check_scale(int scale, const float rfactor1[3], double adm_norm_view_dist,
                                      int adm_ref_display_height, int adm_csf_mode)
{
    double fixed[3];
    double limit;

    if (scale == 0) {
        adm_csf_scale0_fixed(rfactor1, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                             fixed);
        limit = ADM_CSF_SCALE0_LIMIT;
    } else {
        for (int band = 0; band < 3; ++band) {
            fixed[band] = (double)rfactor1[band] * pow(2, ADM_CSF_S123_EXP);
        }
        limit = ADM_CSF_S123_LIMIT;
    }

    for (int band = 0; band < 3; ++band) {
        if (!adm_csf_fixed_in_range(fixed[band], limit)) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "integer_adm: adm_csf_mode=%d at adm_norm_view_dist=%g, "
                     "adm_ref_display_height=%d yields a scale-%d band-%d CSF weight of %g, "
                     "which the fixed-point pipeline cannot represent (needs 0 <= w < %g). "
                     "Use the float ADM extractor, or lower adm_csf_scale / "
                     "adm_csf_diag_scale.\n",
                     adm_csf_mode, adm_norm_view_dist, adm_ref_display_height, scale, band,
                     fixed[band], limit);
            return -EINVAL;
        }
    }
    return 0;
}

#endif /* ADM_CSF_FIXED_POINT_H_ */
