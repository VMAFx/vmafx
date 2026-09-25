/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
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
 *      bands scaled by 2^32. The later signed multiply/cube stages require
 *      two headroom bits, so their usable arithmetic budget is 2^30.
 *
 *  Those budgets were sized for the Watson97 CSF, whose weights sit around
 *  1e-2. The fork-added `adm_csf_mode` option (integer_adm.h) also exposes
 *  the Barten CSF, whose weights are ~1.2 at scale 0 and ~27 at scale 3 with
 *  the default `adm_csf_scale` of 1.0 -- 38x to 155x past the uint16_t
 *  ceiling at scale 0 and past the uint32_t ceiling at every other scale.
 *  The narrowing conversions in the extractors silently wrapped, and the
 *  resulting scores were nonsense rather than an error.
 *
 *  This header centralises the bounds and the shared power-of-two
 *  normalisation used by every integer-ADM backend.  A single shift is used
 *  for all three bands of one scale, preserving their relative CSF weights;
 *  the contrast-masking reduction restores the removed exponent after its
 *  cube.  Invalid negative / non-finite table results are still rejected.
 *  See ADR-1191, ADR-1325, and
 *  docs/state.md :: T-UPSTREAM-1494-ADM-CSF-MODE-IRFACTOR-OVERFLOW-2026-09-03.
 *
 *  The frame-size bound and the rounding constant of the pipeline's right
 *  shifts, at the end of this file, are shared for the same reason. See
 *  docs/state.md :: T-ADM-AVX512-SMALL-WIDTH-SCALE0-2026-09-18 and
 *  T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18.
 */

#ifndef ADM_CSF_FIXED_POINT_H_
#define ADM_CSF_FIXED_POINT_H_

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "log.h"

/* Fixed-point exponents of the integer-ADM CSF weights. */
#define ADM_CSF_SCALE0_HV_EXP (21)
#define ADM_CSF_SCALE0_D_EXP (23)
#define ADM_CSF_S123_EXP (32)

/* Exclusive upper bounds of the corresponding fixed-point arithmetic. A
 * converted weight equal to the bound is outside the budget, so comparisons
 * are strict. Scale 0 is storage-limited; scales 1..3 keep two bits below
 * uint32_t's ceiling because their signed CSF/CM arithmetic and cube
 * accumulation need that headroom. */
#define ADM_CSF_SCALE0_LIMIT (65536.0)    /* 2^16, uint16_t i_rfactor */
#define ADM_CSF_S123_LIMIT (1073741824.0) /* 2^30, signed headroom */
#define ADM_MIN_VIEWING_GEOMETRY (3240.0) /* 1080p at 3H */

/**
 * Preserve the integer pipeline's minimum angular-frequency contract. The
 * reference CPU extractor has always rejected viewing geometries below
 * 1080p at 3H; every GPU twin must reject the same inputs before calculating
 * fixed-point CSF factors or claiming device resources.
 *
 * Cast the integral height before multiplication so the comparison cannot
 * overflow in integer arithmetic if the option range grows later. The
 * relational expression intentionally matches the CPU reference's handling
 * of non-finite doubles; the option parser owns those range checks.
 */
static inline int adm_viewing_geometry_check(double adm_norm_view_dist, int adm_ref_display_height)
{
    return adm_norm_view_dist * (double)adm_ref_display_height < ADM_MIN_VIEWING_GEOMETRY ?
               -EINVAL :
               0;
}

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
 * True when `value` is a valid input to the fixed-point normaliser. Negative
 * inputs are rejected: the
 * blended-CSF tables in barten_csf_tools.h return `-EINVAL` as a float when
 * asked for an (adm_norm_view_dist, adm_ref_display_height) pair they do not
 * tabulate, and converting a negative float to an unsigned integer type is
 * undefined behaviour (C17 6.3.1.4p1).
 */
static inline bool adm_csf_fixed_valid(double value)
{
    return isfinite(value) && value >= 0.0;
}

/**
 * Convert the CSF weights of one DWT scale to the fixed-point representation
 * used by the integer pipeline. When a weight would exceed its arithmetic
 * budget, divide every band on the scale by the same power of two until all
 * fit.
 * `normalization_shift` records that exponent for the contrast-masking cube
 * finalisation, which restores `3 * normalization_shift` bits.
 *
 * Returns 0 when the viewing geometry meets the fixed-point floor and every
 * generated weight is finite and non-negative. Geometry below the floor and
 * invalid weights return -EINVAL; in particular, the blended-CSF lookup uses
 * -EINVAL encoded as a float for unsupported display geometry.
 *
 * `scale` is 0 for the 16-bit pipeline and 1..3 for the 32-bit pipeline;
 * `rfactor1` is { factor1, factor1, factor2 }.
 */
static inline int adm_csf_fixed_scale(int scale, const float rfactor1[3], double adm_norm_view_dist,
                                      int adm_ref_display_height, int adm_csf_mode, double fixed[3],
                                      uint32_t *normalization_shift)
{
    const int geometry_err = adm_viewing_geometry_check(adm_norm_view_dist, adm_ref_display_height);
    if (geometry_err) {
        return geometry_err;
    }

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
        if (!adm_csf_fixed_valid(fixed[band])) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "integer_adm: adm_csf_mode=%d at adm_norm_view_dist=%g, "
                     "adm_ref_display_height=%d yields an invalid scale-%d band-%d "
                     "CSF weight of %g (expected a finite non-negative value).\n",
                     adm_csf_mode, adm_norm_view_dist, adm_ref_display_height, scale, band,
                     fixed[band]);
            return -EINVAL;
        }
    }

    *normalization_shift = 0u;
    while (fixed[0] >= limit || fixed[1] >= limit || fixed[2] >= limit) {
        fixed[0] *= 0.5;
        fixed[1] *= 0.5;
        fixed[2] *= 0.5;
        ++*normalization_shift;
    }
    return 0;
}

/* Validation-only wrapper used during extractor initialisation. */
static inline int adm_csf_check_scale(int scale, const float rfactor1[3], double adm_norm_view_dist,
                                      int adm_ref_display_height, int adm_csf_mode)
{
    double fixed[3];
    uint32_t normalization_shift;
    return adm_csf_fixed_scale(scale, rfactor1, adm_norm_view_dist, adm_ref_display_height,
                               adm_csf_mode, fixed, &normalization_shift);
}

/* Smallest frame dimension the integer-ADM pipeline accepts. Each DWT scale
 * halves the band, rounding up: below 17 pixels the scale-3 band is a single
 * sample, which the DWT reads past. The scale-0 horizontal and vertical cube
 * shift, ceil(log2(band) - 4), also goes negative there. */
#define ADM_MIN_FRAME_DIM (17u)

/**
 * 0 when a `w` x `h` frame is inside the integer-ADM range, otherwise -EINVAL
 * after logging which extractor refused it. The CPU, CUDA, HIP and SYCL
 * extractors call it from init(), and Metal has the same check inline, so every
 * backend refuses the same frames.
 */
static inline int adm_frame_size_check(const char *extractor, unsigned w, unsigned h)
{
    if (w >= ADM_MIN_FRAME_DIM && h >= ADM_MIN_FRAME_DIM) {
        return 0;
    }
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "%s requires width >= %u and height >= %u (got %ux%u)\n",
             extractor, ADM_MIN_FRAME_DIM, ADM_MIN_FRAME_DIM, w, h);
    return -EINVAL;
}

/**
 * Rounding constant of a right shift by `shift`: 2^(shift - 1), or 0 when
 * `shift` is 0. The scale-0 horizontal and vertical cube shift is exactly 0 for
 * frames 17 to 32 pixels wide. There the unguarded 2^(shift - 1) wraps to
 * 2^(2^32 - 1), which is undefined both as a shift and as a float-to-integer
 * conversion (T-ADM-AVX512-SMALL-WIDTH-SCALE0-2026-09-18).
 */
static inline uint32_t adm_half_shift(uint32_t shift)
{
    return (shift > 0u) ? ((uint32_t)1u << (shift - 1u)) : 0u;
}

#endif /* ADM_CSF_FIXED_POINT_H_ */
