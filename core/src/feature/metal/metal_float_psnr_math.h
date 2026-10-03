/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The per-sample term of float_psnr_metal, valid as Metal Shading Language
 *  (float_psnr.metal) and as host C, where test_metal_float_psnr_math holds it
 *  against float_psnr.c (ADR-1498; the design of the CUDA, SYCL and HIP twins,
 *  ADR-1455, ADR-1450, ADR-1440).
 *
 *  float_psnr.c adds `(double)(diff * diff)` per row and the rows in double,
 *  `diff` being the difference of two samples converted by picture_copy(), a
 *  raw sample over scaler = 2^(bpc - 8). That difference is exact, and its
 *  float square is the float square of the raw difference over scaler^2. So
 *  the term here is one fp32 product of the raw difference with itself,
 *  rounded to nearest even as the CPU's is, and converted to an integer in
 *  units of 1 / scaler^2:
 *    - up to 12 bits the square is below 2^24 and exact in float;
 *    - at 16 bits it is the integer square rounded to 24 bits, as on the CPU,
 *      and below 2^32 (the largest, 65535^2 = 2^32 - 2^17 + 1, rounds to
 *      2^32 - 2^17).
 *  Every term is a whole number, the conversion truncates nothing, and a
 *  32-bit unsigned integer holds it. The kernel and the host add the terms as
 *  64-bit integers, so the twin's sum is the exact sum of the CPU's terms; the
 *  host divides it by scaler^2 and the pixel count as the CPU does.
 */

#ifndef VMAF_FEATURE_METAL_METAL_FLOAT_PSNR_MATH_H_
#define VMAF_FEATURE_METAL_METAL_FLOAT_PSNR_MATH_H_

#include "metal_portable.h"

/* float_psnr.c's `diff * diff` times scaler^2, for the raw samples `ref` and
 * `dis` (each below 2^16). */
VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_fpsnr_term(vmaf_mtl_i32 ref, vmaf_mtl_i32 dis)
{
    const float diff = (float)(ref - dis);
    return (vmaf_mtl_u32)(diff * diff);
}

#endif /* VMAF_FEATURE_METAL_METAL_FLOAT_PSNR_MATH_H_ */
