/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Per-sample arithmetic of the integer VIF Metal kernels (integer_vif.metal),
 *  written on metal_portable.h so it compiles as Metal Shading Language in the
 *  kernels and as C on the host, where core/test/test_metal_integer_vif_math.c
 *  holds it against the CPU reference without a device (ADR-1498).
 */

#ifndef VMAF_FEATURE_METAL_METAL_INTEGER_VIF_MATH_H_
#define VMAF_FEATURE_METAL_METAL_INTEGER_VIF_MATH_H_

#include "metal_portable.h"

/*
 * Border index of a plane `sup` samples long. integer_vif.c reflects each
 * filter tap once about the edge sample, excluding the edge itself
 * (pad_top_and_bottom, PADDING_SQ_DATA): idx < 0 -> -idx, idx >= sup ->
 * 2 * (sup - 1) - idx. This folds over the period 2 * (sup - 1) instead, which
 * is that reflection for every index it brings into the plane
 * (-(sup - 1) <= idx <= 2 * (sup - 1)) and lands in [0, sup - 1] for any
 * other index too. Every tap an output reads is such an index from the
 * 16-pixel minimum on (vif_metal_min_dim(),
 * T-GPU-INTEGER-VIF-MIN-DIM-TWINS-2026-09-29); the compute kernels also load
 * tile samples beyond the plane that no output reads, and those stay inside
 * the buffers.
 */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_vif_mirror(vmaf_mtl_i32 idx, vmaf_mtl_i32 sup)
{
    if (sup <= 1) {
        return 0;
    }
    const vmaf_mtl_i32 period = 2 * (sup - 1);
    vmaf_mtl_i32 m = idx % period;
    if (m < 0) {
        m += period;
    }
    return (m < sup) ? m : period - m;
}

#endif /* VMAF_FEATURE_METAL_METAL_INTEGER_VIF_MATH_H_ */
