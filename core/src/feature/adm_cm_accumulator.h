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
    /* The arithmetic shift of the signed row total is the CPU rounding the twins reproduce bit
     * for bit; an unsigned shift would move its bits. */
    // NOLINTNEXTLINE(bugprone-signed-bitwise): arithmetic shift of the signed total, ADR-1416
    return (row_total + rounding) >> shift;
}

/**
 * Apply the denominator rounding shift to one complete row total.
 *
 * The denominator reductions (adm_csf_den_scale(), adm_csf_den_s123()) add
 * the cube terms of a row into an unsigned accumulator and fold the row once.
 * The same rule as above applies: sum every partial of the row first. A twin
 * that folds each warp, thread or tile of a row on its own gets a different
 * accumulator, and on frames with little reference detail a different score
 * (ADR-1416).
 */
static VMAF_ADM_CM_HOST_DEVICE inline uint64_t
adm_csf_den_round_row_total(uint64_t row_sum, uint32_t add_shift_accum, uint32_t shift_accum)
{
    return (row_sum + add_shift_accum) >> shift_accum;
}

/**
 * Scale-0 masking excess `clamp(|x| - thr * 2^shift, 0, INT32_MAX)`.
 *
 * `thr` is the masking threshold: the 3x3 sum of the filtered neighbours with
 * a centre tap of up to 69904 per band, so the product with `2^shift` can
 * leave int32. The excess is therefore formed in int64 and clamped, as
 * upstream Netflix/vmaf does since the second revision of its PR #1602
 * (ADR-1402). A threshold above the magnitude masks the sample completely
 * (0); a negative threshold, which only hand-built buffers produce, raises
 * the excess and saturates at INT32_MAX.
 *
 * Every implementation returns this value bit for bit: the AVX2 / AVX-512
 * vector loops (`cm_excess_avx2()` / `cm_excess_avx512()`), the CUDA and HIP
 * kernels (which call this function), SYCL `adm_dev_cm_excess_s0()` and the
 * Metal twin `adm_cm_excess_s0()` in `integer_adm.metal`.
 *
 * `shift` must be in [1, 30]; the callers pass 10 (horizontal / vertical
 * band) and 12 (diagonal band).
 */
static VMAF_ADM_CM_HOST_DEVICE inline int32_t adm_cm_excess_s0(int32_t x, int32_t thr,
                                                               uint32_t shift)
{
    const int64_t magnitude = (x < 0) ? -(int64_t)x : (int64_t)x;
    const int64_t excess = magnitude - ((int64_t)thr * (int64_t)((uint64_t)1 << shift));
    /* Two selects, not an early return: whether a sample is masked is as good
     * as random on noisy content, and a branch on it mispredicts. */
    const int64_t floored = (excess < 0) ? 0 : excess;
    return (int32_t)((floored > INT32_MAX) ? INT32_MAX : floored);
}

#undef VMAF_ADM_CM_HOST_DEVICE

#endif /* VMAF_FEATURE_ADM_CM_ACCUMULATOR_H_ */
