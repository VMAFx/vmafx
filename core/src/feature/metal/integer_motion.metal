/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernel for integer_motion (v1) (T8-1i / ADR-0421).
 *
 *  Differences first (ADR-1498; the design of the SYCL and CUDA motion
 *  twins, ADR-1371, ADR-1372), as core/src/feature/integer_motion.c does since
 *  Netflix a4a1492d:
 *    1. d[i,j] = prev[i,j] - cur[i,j]
 *    2. v[i,j] = (sum_k FILTER[k] * d[mirror(i-2+k), j] + (1<<(bpc-1))) >> bpc
 *    3. h[i,j] = (sum_k FILTER[k] * v[i, mirror(j-2+k)] + 32768) >> 16
 *    4. SAD = sum |h[i,j]|
 *  The arithmetic of steps 2 and 3 and the mirror are metal_integer_motion_math.h,
 *  which test_metal_integer_motion_math runs on the host against the CPU. The
 *  kernel blurred each frame and differenced the blurred frames, which rounds
 *  each frame on its own: another sum (T-METAL-MOTION-BLUR-THEN-DIFF-2026-09-29).
 *
 *  One thread per output pixel in a 16x16 threadgroup. The threadgroup stages
 *  prev - cur of its pixels plus the two-sample halo (20x20, mirrored rows and
 *  columns), filters it vertically at the 16 rows and 20 columns the
 *  horizontal pass needs, and each thread filters its pixel horizontally. The
 *  256 |h| values are below 2^16 each, so their sum fits a uint exactly; thread
 *  0 adds them and stores one uint per threadgroup, and the host adds those in
 *  uint64 (integer_motion_metal.mm). Integer arithmetic throughout.
 *
 *  Buffer bindings (both kernels, host must match integer_motion_metal.mm):
 *   [[buffer(0)]] prev      — const uchar * (packed luma plane, the frame the
 *                             SAD is taken against: n-1, or n-2 with
 *                             motion_five_frame_window)
 *   [[buffer(1)]] cur       — const uchar * (packed luma plane, frame n)
 *   [[buffer(2)]] sad_parts — uint *        (grid_w × grid_h group sums)
 *   [[buffer(3)]] params    — uint2         (.x = bpc, .y = unused)
 *   [[buffer(4)]] dim       — uint2         (width, height)
 */

#include <metal_stdlib>
using namespace metal;

#include "metal_integer_motion_math.h"

#define IM_GROUP 16
#define IM_HALF 2
#define IM_TILE 20 /* IM_GROUP + 2 * IM_HALF */
#define IM_THREADS 256u

/* Sample `offset` of a packed plane of uchar or ushort samples. */
inline int im_sample(const device uchar *plane, uint offset, bool hbd)
{
    if (hbd) {
        return (int)((const device ushort *)plane)[offset];
    }
    return (int)plane[offset];
}

/* prev - cur over the threadgroup's 20x20 tile, rows and columns mirrored. */
inline void im_load_diff(const device uchar *prev, const device uchar *cur, bool hbd, uint2 bid,
                         uint lid, int width, int height, threadgroup int *diff)
{
    const int oy = (int)bid.y * IM_GROUP - IM_HALF;
    const int ox = (int)bid.x * IM_GROUP - IM_HALF;
    for (uint i = lid; i < (uint)(IM_TILE * IM_TILE); i += IM_THREADS) {
        const int y = vmaf_mtl_motion_mirror(oy + (int)(i / IM_TILE), height);
        const int x = vmaf_mtl_motion_mirror(ox + (int)(i % IM_TILE), width);
        const uint offset = (uint)(y * width + x);
        diff[i] = im_sample(prev, offset, hbd) - im_sample(cur, offset, hbd);
    }
}

/* The vertical pass at the 16 output rows and all 20 tile columns. */
inline void im_vertical(const threadgroup int *diff, uint lid, uint bpc, threadgroup int *vert)
{
    for (uint i = lid; i < (uint)(IM_GROUP * IM_TILE); i += IM_THREADS) {
        const uint c = i % IM_TILE;
        const uint top = (i / IM_TILE) * IM_TILE + c;
        vert[i] = vmaf_mtl_motion_vertical(diff[top], diff[top + IM_TILE],
                                           diff[top + 2 * IM_TILE], diff[top + 3 * IM_TILE],
                                           diff[top + 4 * IM_TILE], bpc);
    }
}

/* The threadgroup's sum of `mine` at sad_parts[group]: at most 256 values
 * below 2^16, exact in a uint. */
inline void im_store_group_sum(uint mine, uint lid, uint group, threadgroup uint *scratch,
                               device uint *sad_parts)
{
    scratch[lid] = mine;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0u) {
        uint total = 0u;
        for (uint i = 0u; i < IM_THREADS; ++i) {
            total += scratch[i];
        }
        sad_parts[group] = total;
    }
}

/* |h| of this thread's pixel; 0 outside the frame. */
inline uint im_pixel(const threadgroup int *vert, uint2 gid, uint2 lid2, uint2 dim)
{
    if (gid.x >= dim.x || gid.y >= dim.y) {
        return 0u;
    }
    const uint b = lid2.y * IM_TILE + lid2.x;
    return vmaf_mtl_motion_abs_h(vert[b], vert[b + 1u], vert[b + 2u], vert[b + 3u], vert[b + 4u]);
}

/* ------------------------------------------------------------------ */
/*  8 bpc kernel                                                        */
/* ------------------------------------------------------------------ */
kernel void integer_motion_kernel_8bpc(
    const device uchar *prev      [[buffer(0)]],
    const device uchar *cur       [[buffer(1)]],
    device       uint  *sad_parts [[buffer(2)]],
    constant     uint2 &params    [[buffer(3)]],
    constant     uint2 &dim       [[buffer(4)]],
    uint2 gid         [[thread_position_in_grid]],
    uint2 bid         [[threadgroup_position_in_grid]],
    uint2 grid_groups [[threadgroups_per_grid]],
    uint2 lid2        [[thread_position_in_threadgroup]],
    uint  lid         [[thread_index_in_threadgroup]])
{
    threadgroup int diff[IM_TILE * IM_TILE];
    threadgroup int vert[IM_GROUP * IM_TILE];
    threadgroup uint scratch[IM_THREADS];

    im_load_diff(prev, cur, false, bid, lid, (int)dim.x, (int)dim.y, diff);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    im_vertical(diff, lid, params.x, vert);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const uint mine = im_pixel(vert, gid, lid2, dim);
    im_store_group_sum(mine, lid, bid.y * grid_groups.x + bid.x, scratch, sad_parts);
}

/* ------------------------------------------------------------------ */
/*  10 / 12 / 16 bpc kernel: native ushort samples                      */
/* ------------------------------------------------------------------ */
kernel void integer_motion_kernel_16bpc(
    const device uchar *prev      [[buffer(0)]],
    const device uchar *cur       [[buffer(1)]],
    device       uint  *sad_parts [[buffer(2)]],
    constant     uint2 &params    [[buffer(3)]],
    constant     uint2 &dim       [[buffer(4)]],
    uint2 gid         [[thread_position_in_grid]],
    uint2 bid         [[threadgroup_position_in_grid]],
    uint2 grid_groups [[threadgroups_per_grid]],
    uint2 lid2        [[thread_position_in_threadgroup]],
    uint  lid         [[thread_index_in_threadgroup]])
{
    threadgroup int diff[IM_TILE * IM_TILE];
    threadgroup int vert[IM_GROUP * IM_TILE];
    threadgroup uint scratch[IM_THREADS];

    im_load_diff(prev, cur, true, bid, lid, (int)dim.x, (int)dim.y, diff);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    im_vertical(diff, lid, params.x, vert);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const uint mine = im_pixel(vert, gid, lid2, dim);
    im_store_group_sum(mine, lid, bid.y * grid_groups.x + bid.x, scratch, sad_parts);
}
