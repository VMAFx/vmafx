/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The arithmetic of the RGB to Y'CbCr conversion of an imported frame (RC4
 * WP13, ADR-2146): one pixel's Y', Cb' or Cr' from its R', G', B' samples,
 * in 64-bit integers, as a Q30 dot product with a constant term and one
 * rounding (half up) to an integer code value clipped to [0, 2^bpc - 1]. No
 * floating-point operation, no division, no table: the plan carries every
 * constant, so the CPU reference (rgb_convert.c), the CUDA and HIP kernels
 * (import_convert_kernels.h) and the SYCL kernels evaluate the same
 * expression and return the same integers. A change here changes every one of
 * them in the same PR (core/test/test_vmafx_rgb_convert.c and the device
 * bit-exactness tests hold them together).
 *
 * Freestanding C: the device compilers include it with VMAFX_RGB_FN set to
 * their function qualifier. Nothing in it indexes an array with a run-time
 * index (a SYCL kernel must stay free of scratch memory, ADR-1395).
 */

#ifndef VMAF_SRC_VMAFX_RGB_MATH_H_
#define VMAF_SRC_VMAFX_RGB_MATH_H_

#include <stdint.h>

#ifndef VMAFX_RGB_FN
#define VMAFX_RGB_FN static inline
#endif

/* Fixed-point bits of the coefficients (rgb_coefficients_gen.h checks it). */
#define VMAFX_RGB_SHIFT 30u

/* One conversion: coefficients of R, G, B for the rows Y', Cb', Cr' and the
 * constant of each (offset, rounding half), the largest code value, and where
 * R, G, B are in a pixel of `elems` elements of `in_bytes` bytes. Passed to
 * a kernel by value. */
typedef struct VmafxRgbPlan {
    int64_t coef[3][3];
    int64_t add[3];
    uint32_t max;
    uint32_t elems;
    uint32_t in_bytes;
    uint32_t pos[3];
} VmafxRgbPlan;

/* Y' (plane 0), Cb' (1) or Cr' (2) of the pixel (r, g, b). */
VMAFX_RGB_FN uint32_t vmafx_rgb_value(const VmafxRgbPlan *plan, uint32_t plane, uint32_t r,
                                      uint32_t g, uint32_t b)
{
    const int64_t c_r =
        plane == 0u ? plan->coef[0][0] : (plane == 1u ? plan->coef[1][0] : plan->coef[2][0]);
    const int64_t c_g =
        plane == 0u ? plan->coef[0][1] : (plane == 1u ? plan->coef[1][1] : plan->coef[2][1]);
    const int64_t c_b =
        plane == 0u ? plan->coef[0][2] : (plane == 1u ? plan->coef[1][2] : plan->coef[2][2]);
    const int64_t add = plane == 0u ? plan->add[0] : (plane == 1u ? plan->add[1] : plan->add[2]);
    const int64_t t = c_r * (int64_t)r + c_g * (int64_t)g + c_b * (int64_t)b + add;
    const uint64_t v = t < 0 ? 0u : (uint64_t)t >> VMAFX_RGB_SHIFT;
    return v > plan->max ? plan->max : (uint32_t)v;
}

#endif /* VMAF_SRC_VMAFX_RGB_MATH_H_ */
