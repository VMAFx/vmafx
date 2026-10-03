/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernels for float_motion (T8-1h / ADR-0421), on the design
 *  of float_motion_hip (ADR-1404, ADR-1419) and the arithmetic of
 *  metal_float_motion_math.h (ADR-1498).
 *
 *  Algorithm (CPU core/src/feature/float_motion.c):
 *    1. picture_copy() of every sample: raw / scaler - 128.
 *    2. convolution_f32_c_s() with the filter motion_filter_size selects
 *       (motion_blur_plane()): FILTER_5_s by default, FILTER_3_s for 3,
 *       FILTER_5_NO_OP_s for 1; reflect-101 edges (`float_motion_blur`).
 *    3. From the second frame on, |cur - prev| of every sample, stored by
 *       the blur kernel transposed in groups of VMAF_MTL_FM_ROW_GROUP rows,
 *       and one fp32 sum per row, the row's differences added left to right
 *       (`float_motion_row_sum`, one thread per row): float_sad_line().
 *    4. `motion_add_scale1`: the same over both blurred frames scaled to half
 *       size with motion.c::motion_scale_bilinear()
 *       (`float_motion_scale1_diff`, then `float_motion_row_sum`).
 *    The host adds the row sums of each plane top to bottom and divides in
 *    fp32 (feature/float_motion_sad.h); `motion_add_uv` runs steps 1 to 4 on
 *    the U and V planes too.
 *
 *  Numerical contract: every value is computed by a function of
 *  metal_float_motion_math.h, which core/test/test_metal_float_motion_math.cpp
 *  holds against the CPU's own functions on the host. The kernels build with
 *  -fno-fast-math -ffp-contract=off (core/src/metal/meson.build): fp32
 *  + - * / are correctly rounded and nothing is fused, as on the CPU. No
 *  kernel reduces across threads: a running fp32 sum rounds at every step,
 *  and only one thread per row walking the row in order gives the CPU's sum.
 *
 *  Layout: the blur runs 16x16 threadgroups over a 20x20 tile with a
 *  2-sample halo (every thread loads its share, then blurs its sample from
 *  the tile); the tile indices are folded into the plane with
 *  vmaf_mtl_fm_reflect101() for every thread, padding threads included, so
 *  no load leaves the plane whatever its size. The row kernel runs
 *  VMAF_MTL_FM_ROW_GROUP threads per threadgroup.
 *
 *  Buffer bindings:
 *   float_motion_blur
 *    [[buffer(0)]] ref        — const uchar *: the raw plane, packed rows
 *                               (uint16 samples above 8 bits)
 *    [[buffer(1)]] cur_blur   — float *: this frame's blurred plane
 *    [[buffer(2)]] prev_blur  — const float *: the previous frame's
 *    [[buffer(3)]] diff       — float *: |cur - prev|, transposed
 *    [[buffer(4)]] args       — VmafMtlFmBlurArgs
 *   float_motion_scale1_diff
 *    [[buffer(0)]] cur_blur, [[buffer(1)]] prev_blur — const float *
 *    [[buffer(2)]] diff       — float *: scale-1 |cur - prev|, transposed
 *    [[buffer(3)]] args       — VmafMtlFmScale1Args
 *   float_motion_row_sum
 *    [[buffer(0)]] diff       — const float *: a transposed plane
 *    [[buffer(1)]] row_sad    — float *: the read-back of every row sum
 *    [[buffer(2)]] args       — VmafMtlFmRowArgs
 */

#include <metal_stdlib>

#include "metal_float_motion_math.h"

using namespace metal;

/* picture_copy() of the sample at `idx` of the packed plane. */
inline float fm_load_sample(const device uchar *ref, uint hbd, uint idx, float inv_scaler)
{
    const uint raw = (hbd != 0u) ? (uint)((const device ushort *)ref)[idx] : (uint)ref[idx];
    return vmaf_mtl_fm_sample(raw, inv_scaler);
}

/* The four samples of `plane` (`width` columns) motion_bilinear_interp()
 * weighs for one half-size sample. */
inline VmafMtlFmCorners fm_corners(const device float *plane, uint width, VmafMtlFmBilinearAt at)
{
    const uint row1 = (uint)at.y1 * width;
    const uint row2 = (uint)at.y2 * width;
    VmafMtlFmCorners s;
    s.s11 = plane[row1 + (uint)at.x1];
    s.s12 = plane[row1 + (uint)at.x2];
    s.s21 = plane[row2 + (uint)at.x1];
    s.s22 = plane[row2 + (uint)at.x2];
    return s;
}

/* Blurs the plane into `cur_blur` and, when `args.compute_sad` is set,
 * stores |cur_blur - prev_blur| of every sample in the transposed plane
 * `diff` for float_motion_row_sum. */
kernel void float_motion_blur(const device uchar *ref [[buffer(0)]],
                              device float *cur_blur [[buffer(1)]],
                              const device float *prev_blur [[buffer(2)]],
                              device float *diff [[buffer(3)]],
                              constant VmafMtlFmBlurArgs &args [[buffer(4)]],
                              uint2 gid [[thread_position_in_grid]],
                              uint2 bid [[threadgroup_position_in_grid]],
                              uint2 lid2 [[thread_position_in_threadgroup]],
                              uint lid [[thread_index_in_threadgroup]])
{
    threadgroup float tile[VMAF_MTL_FM_TILE * VMAF_MTL_FM_TILE];

    const int width = (int)args.width;
    const int height = (int)args.height;
    const uint hbd = (args.bpc > 8u) ? 1u : 0u;
    const float inv_scaler = vmaf_mtl_fm_inv_scaler(args.bpc);
    const int tile_ox = (int)(bid.x * VMAF_MTL_FM_BLOCK) - VMAF_MTL_FM_RADIUS;
    const int tile_oy = (int)(bid.y * VMAF_MTL_FM_BLOCK) - VMAF_MTL_FM_RADIUS;
    for (uint i = lid; i < VMAF_MTL_FM_TILE * VMAF_MTL_FM_TILE;
         i += VMAF_MTL_FM_BLOCK * VMAF_MTL_FM_BLOCK) {
        const int sy = vmaf_mtl_fm_reflect101(tile_oy + (int)(i / VMAF_MTL_FM_TILE), height);
        const int sx = vmaf_mtl_fm_reflect101(tile_ox + (int)(i % VMAF_MTL_FM_TILE), width);
        tile[i] = fm_load_sample(ref, hbd, (uint)(sy * width + sx), inv_scaler);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (gid.x >= args.width || gid.y >= args.height) {
        return;
    }
    VmafMtlFmWindow win;
    for (uint r = 0u; r < (uint)VMAF_MTL_FM_TAPS; r++) {
        for (uint c = 0u; c < (uint)VMAF_MTL_FM_TAPS; c++) {
            win.v[r * (uint)VMAF_MTL_FM_TAPS + c] =
                tile[(lid2.y + r) * VMAF_MTL_FM_TILE + lid2.x + c];
        }
    }
    const float blurred = vmaf_mtl_fm_blur(vmaf_mtl_fm_taps(args.filter_size), win);
    const uint off = gid.y * args.width + gid.x;
    cur_blur[off] = blurred;
    if (args.compute_sad != 0u) {
        diff[vmaf_mtl_fm_diff_index(gid.x, gid.y, args.width)] =
            vmaf_mtl_fm_abs_diff(blurred, prev_blur[off]);
    }
}

/* `motion_add_scale1`: the differences of the scale-1 term of
 * motion.c::vmaf_image_sad_c(). Both blurred frames are scaled to
 * `args.scaled_width` x `args.scaled_height` with motion_scale_bilinear(),
 * one thread per half-size sample, and |cur - prev| is stored in the
 * transposed plane `diff`. Runs after the blur kernel of the same frame. */
kernel void float_motion_scale1_diff(const device float *cur_blur [[buffer(0)]],
                                     const device float *prev_blur [[buffer(1)]],
                                     device float *diff [[buffer(2)]],
                                     constant VmafMtlFmScale1Args &args [[buffer(3)]],
                                     uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= args.scaled_width || gid.y >= args.scaled_height) {
        return;
    }
    const VmafMtlFmBilinearAt at = vmaf_mtl_fm_bilinear_at(args.width, args.height, args.ratio_x,
                                                           args.ratio_y, gid.x, gid.y);
    const float cur = vmaf_mtl_fm_bilinear(fm_corners(cur_blur, args.width, at), at.dx, at.dy);
    const float prev = vmaf_mtl_fm_bilinear(fm_corners(prev_blur, args.width, at), at.dx, at.dy);
    diff[vmaf_mtl_fm_diff_index(gid.x, gid.y, args.scaled_width)] =
        vmaf_mtl_fm_abs_diff(cur, prev);
}

/* float_sad_line() of every row of an `args.width` x `args.height` plane:
 * one thread per row adds the row's absolute differences, left to right,
 * into one fp32 accumulator. `diff` is the transposed plane the blur or the
 * scale-1 kernel wrote; the sums land at `args.first_row` of `row_sad`. */
kernel void float_motion_row_sum(const device float *diff [[buffer(0)]],
                                 device float *row_sad [[buffer(1)]],
                                 constant VmafMtlFmRowArgs &args [[buffer(2)]],
                                 uint y [[thread_position_in_grid]])
{
    if (y >= args.height) {
        return;
    }
    const uint base = vmaf_mtl_fm_diff_index(0u, y, args.width);
    float accum = 0.0f;
    for (uint j = 0u; j < args.width; j++) {
        accum += diff[base + j * VMAF_MTL_FM_ROW_GROUP];
    }
    row_sad[args.first_row + y] = accum;
}
