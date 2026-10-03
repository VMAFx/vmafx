/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The second-moment term of float_moment_metal for a 10-, 12- or 16-bit
 *  sample, valid as Metal Shading Language (float_moment.metal) and as host C,
 *  where test_metal_float_moment_math holds it against moment.c (ADR-1498;
 *  the design of the CUDA, SYCL and HIP twins, ADR-1453, ADR-1449, ADR-1447).
 *
 *  moment.c::compute_2nd_moment() forms each square in float
 *  (`const float term = pic_ * pic_`, pic_ the sample picture_copy() divided
 *  by scaler = 2^(bpc - 8)) and adds the floats in double. Up to 12 bits the
 *  square has at most 24 significant bits and the float is the integer square;
 *  at 16 bits it is the integer square rounded to 24 bits. Dividing by a power
 *  of two does not change which bits are rounded away, so the float square of
 *  the raw sample, one fp32 product rounded to nearest even, has the CPU's
 *  significand. Its value is a whole number below 2^32 (the largest,
 *  65535^2 = 2^32 - 2^17 + 1, rounds to 2^32 - 2^17), so the conversion
 *  truncates nothing and the kernel's 64-bit sum of these terms is the exact
 *  sum of the CPU's terms in units of 1 / scaler^2.
 */

#ifndef VMAF_FEATURE_METAL_METAL_FLOAT_MOMENT_MATH_H_
#define VMAF_FEATURE_METAL_METAL_FLOAT_MOMENT_MATH_H_

#include "metal_portable.h"

/* moment.c's float square of the raw sample `v` (below 2^16), times
 * scaler^2. */
VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_moment_float_square(vmaf_mtl_u32 v)
{
    const float sample = (float)v;
    return (vmaf_mtl_u32)(sample * sample);
}

#endif /* VMAF_FEATURE_METAL_METAL_FLOAT_MOMENT_MATH_H_ */
