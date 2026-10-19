/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernels for integer_psnr (T8-1g / ADR-0421), twin of the CPU
 *  `psnr` (core/src/feature/integer_psnr.c). One dispatch per plane.
 *
 *  The CPU sums each plane's squared differences as integers
 *  (sse_line_8_c / sse_line_16_c: `e = ref - dis`, `sse += (uint64)e * e`)
 *  and derives every score from that SSE through psnr_score.h. These kernels
 *  return the same integer: each thread forms its squared difference in 64
 *  bits, the threadgroup's 256 values go into threadgroup memory and thread 0
 *  adds them in uint64, and the group's exact sum is stored in its slot of
 *  `sse_parts`. The host (integer_psnr_metal.mm) adds the slots in uint64 and
 *  calls psnr_score.h, so no floating-point value is formed on the device.
 *
 *  No simd_sum: a 16-bit squared difference reaches 65535^2 (just under
 *  2^32), so a 32-lane uint32 sum, or separate sums of the low and high
 *  halves, would drop carries (core/src/feature/metal/AGENTS.d/exact-designs.md, exact 64-bit
 *  reductions). No 64-bit atomics (Apple GPUs have none for ulong).
 *
 *  Buffer bindings (both kernels):
 *   [[buffer(0)]] ref       - const uchar *  (plane, byte-addressed rows)
 *   [[buffer(1)]] dis       - const uchar *
 *   [[buffer(2)]] sse_parts - ulong *        (one exact SSE per threadgroup)
 *   [[buffer(3)]] strides   - uint2          (ref, dis row pitch in bytes)
 *   [[buffer(4)]] dim       - uint2          (plane width, height)
 *  Threadgroup: 16 x 16 threads (PSNR_TG_THREADS); grid ceil(w/16) x ceil(h/16).
 */

#include <metal_stdlib>
using namespace metal;

#define PSNR_TG_THREADS 256u

/* Thread 0 adds the group's 256 squared differences in uint64 and stores the
 * exact sum. Every thread of the group calls this (the barrier needs all of
 * them); threads outside the plane contribute 0. */
inline void psnr_store_group_sse(ulong my_se, uint lid, uint slot, threadgroup ulong *tg_se,
                                 device ulong *sse_parts)
{
    tg_se[lid] = my_se;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0u) {
        ulong group_se = 0uL;
        for (uint i = 0u; i < PSNR_TG_THREADS; ++i) {
            group_se += tg_se[i];
        }
        sse_parts[slot] = group_se;
    }
}

kernel void integer_psnr_kernel_8bpc(
    const device uchar *ref [[buffer(0)]], const device uchar *dis [[buffer(1)]],
    device ulong *sse_parts [[buffer(2)]], constant uint2 &strides [[buffer(3)]],
    constant uint2 &dim [[buffer(4)]], uint2 gid [[thread_position_in_grid]],
    uint2 bid [[threadgroup_position_in_grid]], uint2 grid_groups [[threadgroups_per_grid]],
    uint lid [[thread_index_in_threadgroup]])
{
    threadgroup ulong tg_se[PSNR_TG_THREADS];

    ulong my_se = 0uL;
    if (gid.x < dim.x && gid.y < dim.y) {
        const long r = (long)ref[gid.y * strides.x + gid.x];
        const long d = (long)dis[gid.y * strides.y + gid.x];
        const long e = r - d;
        my_se = (ulong)(e * e);
    }
    psnr_store_group_sse(my_se, lid, bid.y * grid_groups.x + bid.x, tg_se, sse_parts);
}

kernel void integer_psnr_kernel_16bpc(
    const device uchar *ref [[buffer(0)]], const device uchar *dis [[buffer(1)]],
    device ulong *sse_parts [[buffer(2)]], constant uint2 &strides [[buffer(3)]],
    constant uint2 &dim [[buffer(4)]], uint2 gid [[thread_position_in_grid]],
    uint2 bid [[threadgroup_position_in_grid]], uint2 grid_groups [[threadgroups_per_grid]],
    uint lid [[thread_index_in_threadgroup]])
{
    threadgroup ulong tg_se[PSNR_TG_THREADS];

    ulong my_se = 0uL;
    if (gid.x < dim.x && gid.y < dim.y) {
        const device ushort *ref_row = (const device ushort *)(ref + gid.y * strides.x);
        const device ushort *dis_row = (const device ushort *)(dis + gid.y * strides.y);
        const long r = (long)ref_row[gid.x];
        const long d = (long)dis_row[gid.x];
        const long e = r - d;
        my_se = (ulong)(e * e);
    }
    psnr_store_group_sse(my_se, lid, bid.y * grid_groups.x + bid.x, tg_se, sse_parts);
}
