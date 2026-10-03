/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  The per-sample arithmetic of integer_motion_metal, valid as Metal Shading
 *  Language (integer_motion.metal) and as host C, where
 *  test_metal_integer_motion_math runs the kernel's tile layout on it and holds
 *  the SAD against the CPU `motion` extractor (ADR-1498; the diff-first design
 *  of the SYCL and CUDA twins, ADR-1371, ADR-1372).
 *
 *  integer_motion.c (since Netflix a4a1492d) blurs the frame difference: for
 *  each pixel the 5-tap filter of integer_motion.h runs down the column of
 *  prev - cur, rounded and shifted by bpc (`(accum + (1 << (bpc - 1))) >> bpc`,
 *  the 8-bit pipeline's `+ (1 << 7) >> 8` is the same at bpc 8), then along
 *  the row of those values, rounded and shifted by 16, and the absolute values
 *  are summed. Both passes take reflect-101 neighbours (mirror(): -1 -> 1,
 *  size -> size - 2). The arithmetic is integer, so the order of the sums
 *  cannot change the SAD. Blurring each frame and differencing the blurred
 *  frames, which the kernel did, rounds each frame on its own and is another
 *  sum.
 */

#ifndef VMAF_FEATURE_METAL_METAL_INTEGER_MOTION_MATH_H_
#define VMAF_FEATURE_METAL_METAL_INTEGER_MOTION_MATH_H_

#include "metal_portable.h"

/* integer_motion.h's filter[5] = {3571, 16004, 26386, 16004, 3571} (sum 2^16). */
#define VMAF_MTL_MOTION_TAP0 3571
#define VMAF_MTL_MOTION_TAP1 16004
#define VMAF_MTL_MOTION_TAP2 26386

/* The CPU's mirror() for an index within two samples of the frame
 * ([-2, size + 1], every index a 5-tap neighbourhood reaches; size >= 3, the
 * floor init() enforces). A threadgroup tile also loads positions further out
 * that feed no output pixel; those are first clamped into that range so every
 * load stays in the plane. */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_motion_mirror(vmaf_mtl_i32 idx, vmaf_mtl_i32 size)
{
    const vmaf_mtl_i32 c = (idx < -2) ? -2 : ((idx > size + 1) ? size + 1 : idx);
    if (c < 0) {
        return -c;
    }
    if (c >= size) {
        return (2 * size) - c - 2;
    }
    return c;
}

/* The vertical pass at one column: d0..d4 are prev - cur at rows i - 2 ..
 * i + 2 (mirrored). integer_motion.c's y_row[j]. */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_motion_vertical(vmaf_mtl_i32 d0, vmaf_mtl_i32 d1,
                                                    vmaf_mtl_i32 d2, vmaf_mtl_i32 d3,
                                                    vmaf_mtl_i32 d4, vmaf_mtl_u32 bpc)
{
    const vmaf_mtl_i64 sum =
        (VMAF_MTL_I64(VMAF_MTL_MOTION_TAP0) * d0) + (VMAF_MTL_I64(VMAF_MTL_MOTION_TAP1) * d1) +
        (VMAF_MTL_I64(VMAF_MTL_MOTION_TAP2) * d2) + (VMAF_MTL_I64(VMAF_MTL_MOTION_TAP1) * d3) +
        (VMAF_MTL_I64(VMAF_MTL_MOTION_TAP0) * d4);
    return (vmaf_mtl_i32)((sum + (VMAF_MTL_I64(1) << (bpc - 1u))) >> bpc);
}

/* The horizontal pass at one pixel and its absolute value: v0..v4 are the
 * vertical results at columns j - 2 .. j + 2 (mirrored). integer_motion.c's
 * abs(val). */
VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_motion_abs_h(vmaf_mtl_i32 v0, vmaf_mtl_i32 v1, vmaf_mtl_i32 v2,
                                                 vmaf_mtl_i32 v3, vmaf_mtl_i32 v4)
{
    const vmaf_mtl_i64 sum =
        (VMAF_MTL_I64(VMAF_MTL_MOTION_TAP0) * v0) + (VMAF_MTL_I64(VMAF_MTL_MOTION_TAP1) * v1) +
        (VMAF_MTL_I64(VMAF_MTL_MOTION_TAP2) * v2) + (VMAF_MTL_I64(VMAF_MTL_MOTION_TAP1) * v3) +
        (VMAF_MTL_I64(VMAF_MTL_MOTION_TAP0) * v4);
    const vmaf_mtl_i32 val = (vmaf_mtl_i32)((sum + 32768) >> 16);
    return (vmaf_mtl_u32)((val < 0) ? -val : val);
}

#endif /* VMAF_FEATURE_METAL_METAL_INTEGER_MOTION_MATH_H_ */
