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
 *      bands scaled by 2^32.
 *
 *  Storage is not the binding limit. The contrast-masking reduction squares
 *  the weighted sample and narrows the square to int32, so a weight is in
 *  budget only when the largest wavelet coefficient of its scale, weighted,
 *  still has a square that fits (ADR-1472; the bounds are below).
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
 *  See ADR-1191, ADR-1325, ADR-1472, ADR-1917, and
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

/* Storage bound of the scale-0 weights: uint16_t i_rfactor. */
#define ADM_CSF_SCALE0_LIMIT (65536.0)
#define ADM_MIN_VIEWING_GEOMETRY (3240.0) /* 1080p at 3H */

/*
 * Arithmetic budget of the contrast-masking cube (ADR-1472).
 *
 * Per sample the reduction forms the excess `v` of the weighted coefficient
 * over its masking threshold, then `v_sq = (int32_t)((v * v + round) >> s)`
 * and `v_sq * v` (adm_cm_accum_round(), i4_adm_cm_accum_round(), and their
 * SIMD and device twins). The square fits int32 only up to
 *
 *   s = 29 (scale 0, horizontal / vertical):   v <= 2^30 - 1
 *   s = 30 (scale 0 diagonal, scales 1..3):    v <= 1518500249
 *
 * A larger excess wraps: the cube turns negative or small, the score is wrong,
 * and a negative accumulator ends as NaN in the p-norm.
 *
 * The excess is at most the weighted coefficient, plus 28 at scales 1..3
 * (the threshold there can be as low as -27, ADR-0155's rounding term, and
 * the weighting rounds). The weighted coefficient is `band * weight` at
 * scale 0 and `(band * weight) >> 28` at scales 1..3. So the weight budget
 * of a scale is the excess budget divided by the largest coefficient the
 * wavelet can produce there.
 *
 * That coefficient is bounded by the filter taps, whatever the picture: a
 * detail band is a linear function of the centred pixel p / 2^bpc - 1/2,
 * which lies in [-1/2, 1/2), so its magnitude is at most half the absolute
 * sum of the composite filter, in the band's fixed-point format (2^14 at
 * scale 0, 2^29, 2^27 and 2^26 at scales 1, 2 and 3):
 *
 *   scale 0:   22929.4      scale 2:   751508000
 *   scale 1:   1448980000   scale 3:   742509000
 *
 * (the larger of the horizontal / vertical and the diagonal band; a frame
 * built from the sign pattern of the composite filter reaches 99.6 % of each
 * at 8 bits). The constants below round those up by about half a percent,
 * which covers the pipeline's rounding and the decouple stage's reciprocal
 * table. core/test/test_integer_adm_cm_budget.c derives the bounds again
 * from the taps and holds them against these constants.
 */
#define ADM_CM_EXCESS_MAX_SQ29 (1073741823.0)
#define ADM_CM_EXCESS_MAX_SQ30 (1518500249.0)
#define ADM_I4_CM_EXCESS_SLACK (28.0)
#define ADM_DWT_BAND_MAX_SCALE0 (23040.0)
#define ADM_DWT_BAND_MAX_SCALE1 (1456000000.0)
#define ADM_DWT_BAND_MAX_SCALE2 (755000000.0)
#define ADM_DWT_BAND_MAX_SCALE3 (746000000.0)
#define ADM_I4_CM_WEIGHT_SHIFT (268435456.0) /* 2^28, i4_shift_dst */

/*
 * The scale-0 CSF stage binds the horizontal and vertical weight tighter than
 * the cube does (ADR-1917). It stores the weighted band
 * `i16 = (w * band + 2^14) >> 15` in int16 and its 1/30 magnitude
 * `(ADM_FIX_ONE_BY_30 * |i16| + 2048) >> 12` in int16 as well (adm_csf_cols(),
 * csf_block_avx2(), csf_block_avx512() and the device twins). The magnitude
 * passes INT16_MAX from |i16| = 30720, ADM_CSF_FLT_I16_LIMIT.
 *
 * The integer DWT reaches a scale-0 band of 22930: the taps' bound 22929.4
 * (above) plus at most 0.92 of rounding, half a unit from the second pass and
 * half a unit from the first carried through the second (gain 0.84: the
 * taps' absolute sum of 1.67 in Q15 against the pass's shift of 16). The
 * artifact band the stage weights is never larger than the band.
 * A weight w keeps the magnitude in int16 while w * 22930 + 2^14 < 30720 * 2^15,
 * that is w < 43899.55, and the stage truncates the weight to an integer, so
 * every fixed weight below 43900 does (adm_csf_fixed_limit()). The cube
 * alone allows 46603.4. The diagonal band shifts by 17, so its magnitude is
 * at most 12229 under any uint16_t weight.
 */
#define ADM_DWT_BAND_REACH_SCALE0 (22930.0)
#define ADM_CSF_FLT_I16_LIMIT (30720.0)

/**
 * Exclusive upper bound of the fixed-point CSF weight of `band` (0 and 1:
 * horizontal and vertical, 2: diagonal) at `scale`. A converted weight equal
 * to the bound is outside the budget, so comparisons are strict.
 *
 * Scale 0: the horizontal and vertical weights are bound by the int16 1/30
 * magnitude of the CSF stage (43900, derived above), which is
 * tighter than the cube (46603.4); the diagonal weight by its uint16_t
 * storage (65535 * 23040 is still below the s = 30 excess budget). Scales
 * 1..3: bound by the cube; every bound is below 2^30, so the uint32_t storage
 * and the signed products hold as well.
 */
static inline double adm_csf_fixed_limit(int scale, int band)
{
    if (scale == 0) {
        if (band == 2) {
            return ADM_CSF_SCALE0_LIMIT;
        }
        const double cube = ADM_CM_EXCESS_MAX_SQ29 / ADM_DWT_BAND_MAX_SCALE0;
        const double magnitude =
            floor(((ADM_CSF_FLT_I16_LIMIT * 32768.0) - 16384.0) / ADM_DWT_BAND_REACH_SCALE0) + 1.0;
        return magnitude < cube ? magnitude : cube;
    }
    const double band_max = scale == 1 ?
                                ADM_DWT_BAND_MAX_SCALE1 :
                                (scale == 2 ? ADM_DWT_BAND_MAX_SCALE2 : ADM_DWT_BAND_MAX_SCALE3);
    return (ADM_CM_EXCESS_MAX_SQ30 - ADM_I4_CM_EXCESS_SLACK) * ADM_I4_CM_WEIGHT_SHIFT / band_max;
}

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
static inline int adm_viewing_geometry_check(const char *extractor, double adm_norm_view_dist,
                                             int adm_ref_display_height)
{
    const double product = adm_norm_view_dist * (double)adm_ref_display_height;
    if (product < ADM_MIN_VIEWING_GEOMETRY) {
        if (extractor) {
            vmaf_log(VMAF_LOG_LEVEL_ERROR,
                     "%s: viewing geometry adm_norm_view_dist (%g) * adm_ref_display_height (%d) = "
                     "%g is below the 3240 floor (1080p at 3H); float_adm is the extractor that "
                     "accepts it.\n",
                     extractor, adm_norm_view_dist, adm_ref_display_height, product);
        }
        return -EINVAL;
    }
    return 0;
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
    /* NOLINTBEGIN(modernize-use-nullptr): C header shared with C++; MSVC C mode has no nullptr. ADR-1138. */
    const int geometry_err =
        adm_viewing_geometry_check(NULL, adm_norm_view_dist, adm_ref_display_height);
    /* NOLINTEND(modernize-use-nullptr) */
    if (geometry_err) {
        return geometry_err;
    }

    if (scale == 0) {
        adm_csf_scale0_fixed(rfactor1, adm_norm_view_dist, adm_ref_display_height, adm_csf_mode,
                             fixed);
    } else {
        for (int band = 0; band < 3; ++band) {
            fixed[band] = (double)rfactor1[band] * pow(2, ADM_CSF_S123_EXP);
        }
    }
    const double limit[3] = {adm_csf_fixed_limit(scale, 0), adm_csf_fixed_limit(scale, 1),
                             adm_csf_fixed_limit(scale, 2)};

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
    while (fixed[0] >= limit[0] || fixed[1] >= limit[1] || fixed[2] >= limit[2]) {
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

/* Smallest frame dimension the ADM pipelines accept, fixed-point and float.
 * Each DWT scale halves the band, rounding up: below 17 pixels the scale-3
 * band is a single sample, which the DWT reads past. In the fixed-point
 * pipeline the scale-0 horizontal and vertical cube shift,
 * ceil(log2(band) - 4), also goes negative there. */
#define ADM_MIN_FRAME_DIM (17u)

/**
 * 0 when a `w` x `h` frame is inside the ADM range, otherwise -EINVAL after
 * logging which extractor refused it. The fixed-point CPU, CUDA, HIP and SYCL
 * extractors call it from init(), and Metal has the same check inline, so every
 * backend refuses the same frames. The CPU `float_adm` calls it too.
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
