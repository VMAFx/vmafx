/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Internal integer-ADM contrast-masking row accumulator primitive.
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

#undef VMAF_ADM_CM_HOST_DEVICE

#endif /* VMAF_FEATURE_ADM_CM_ACCUMULATOR_H_ */
