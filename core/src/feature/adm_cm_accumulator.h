/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Internal integer-ADM contrast-masking accumulator primitives.
 */

#ifndef VMAF_FEATURE_ADM_CM_ACCUMULATOR_H_
#define VMAF_FEATURE_ADM_CM_ACCUMULATOR_H_

#include <stdint.h>

#if defined(__CUDACC__) || defined(__HIPCC__)
#define VMAF_ADM_CM_HOST_DEVICE __host__ __device__
#else
#define VMAF_ADM_CM_HOST_DEVICE
#endif

/**
 * Apply the inner-accumulator rounding shift to one complete row total.
 *
 * Callers must sum every pixel, lane, warp, subgroup, or threadgroup partial
 * belonging to the row before calling this function. Integer ADM cube terms
 * are non-negative, while `rounding` remains signed for ADR-0155's negative
 * CUDA i4 term; never reinterpret that term as unsigned. `shift` is bounded
 * by the accepted frame height. Keeping the operation at this seam makes its
 * raw int64 result observable before the later float conversion erases
 * one-unit placement errors.
 */
static VMAF_ADM_CM_HOST_DEVICE inline int64_t
adm_cm_round_row_total(int64_t row_total, int64_t rounding, uint32_t shift)
{
    return (row_total + rounding) >> shift;
}

/**
 * Scale-0 masking excess `|x| - (thr << shift)`, computed modulo 2^32.
 *
 * `thr` is the masking threshold. Its centre tap is narrowed to int16, so one
 * large coefficient among small neighbours makes the whole sum negative, and a
 * large enough sum can shift out of int32. Shifting a negative `int`, or
 * shifting a value out of `int`, is undefined in C. The AVX2 and AVX-512
 * vector loops (`_mm*_slli_epi32`, `_mm*_sub_epi32`) and the SYCL twin compute
 * this expression modulo 2^32, so unsigned arithmetic gives the scalar
 * reference the result those paths produce without the undefined shift. The
 * magnitude is taken the same way, which keeps `INT32_MIN` defined as well.
 *
 * The result is negative when the threshold exceeds the magnitude; the caller
 * clamps it to zero.
 */
static VMAF_ADM_CM_HOST_DEVICE inline int32_t adm_cm_excess_s0(int32_t x, int32_t thr,
                                                               uint32_t shift)
{
    const uint32_t magnitude = (x < 0) ? (0u - (uint32_t)x) : (uint32_t)x;
    return (int32_t)(magnitude - ((uint32_t)thr << shift));
}

#undef VMAF_ADM_CM_HOST_DEVICE

#endif /* VMAF_FEATURE_ADM_CM_ACCUMULATOR_H_ */
