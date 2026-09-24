/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernel for float_moment (T8-1e / ADR-0421).
 *  Emits float_moment_{ref,dis}{1st,2nd} from exact integer workgroup sums.
 *
 *  The CPU normalises high-bit-depth samples by a power-of-two scaler before
 *  accumulation. This kernel accumulates the raw integer values and squares;
 *  the host divides the first moments by scaler and second moments by scaler².
 *  MSL has no 64-bit SIMD reduction or atomic, so all 256 lanes publish ulong
 *  values to threadgroup memory and lane 0 sums them without losing carries.
 *  Each uint64 result is then exported as a uint32 lo/hi pair.
 *
 *  Buffer bindings:
 *   [[buffer(0)]]  ref     — const uchar *
 *   [[buffer(1)]]  dis     — const uchar *
 *   [[buffer(2)]]  r1_lo   — uint * (grid_w × grid_h)
 *   [[buffer(3)]]  r1_hi   — uint *
 *   [[buffer(4)]]  d1_lo   — uint *
 *   [[buffer(5)]]  d1_hi   — uint *
 *   [[buffer(6)]]  r2_lo   — uint *
 *   [[buffer(7)]]  r2_hi   — uint *
 *   [[buffer(8)]]  d2_lo   — uint *
 *   [[buffer(9)]]  d2_hi   — uint *
 *   [[buffer(10)]] strides — uint2 for 8 bpc, uint4 for >8 bpc
 *   [[buffer(11)]] dim     — uint2 (width, height)
 */

#include <metal_stdlib>
using namespace metal;

#define FM_THREADS_PER_GROUP 256u

static inline void write_u64(device uint *lo, device uint *hi, uint idx, ulong value)
{
    lo[idx] = (uint)(value & 0xFFFFFFFFuL);
    hi[idx] = (uint)(value >> 32uL);
}

static inline void reduce_and_store(ulong my_r1, ulong my_d1, ulong my_r2, ulong my_d2,
                                    device uint *r1_lo, device uint *r1_hi, device uint *d1_lo,
                                    device uint *d1_hi, device uint *r2_lo, device uint *r2_hi,
                                    device uint *d2_lo, device uint *d2_hi, uint idx, uint lid,
                                    threadgroup ulong *tg_r1, threadgroup ulong *tg_d1,
                                    threadgroup ulong *tg_r2, threadgroup ulong *tg_d2)
{
    tg_r1[lid] = my_r1;
    tg_d1[lid] = my_d1;
    tg_r2[lid] = my_r2;
    tg_d2[lid] = my_d2;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (lid == 0) {
        ulong wg_r1 = 0uL;
        ulong wg_d1 = 0uL;
        ulong wg_r2 = 0uL;
        ulong wg_d2 = 0uL;
        for (uint i = 0; i < FM_THREADS_PER_GROUP; ++i) {
            wg_r1 += tg_r1[i];
            wg_d1 += tg_d1[i];
            wg_r2 += tg_r2[i];
            wg_d2 += tg_d2[i];
        }
        write_u64(r1_lo, r1_hi, idx, wg_r1);
        write_u64(d1_lo, d1_hi, idx, wg_d1);
        write_u64(r2_lo, r2_hi, idx, wg_r2);
        write_u64(d2_lo, d2_hi, idx, wg_d2);
    }
}

kernel void float_moment_kernel_8bpc(
    const device uchar *ref [[buffer(0)]], const device uchar *dis [[buffer(1)]],
    device uint *r1_lo [[buffer(2)]], device uint *r1_hi [[buffer(3)]],
    device uint *d1_lo [[buffer(4)]], device uint *d1_hi [[buffer(5)]],
    device uint *r2_lo [[buffer(6)]], device uint *r2_hi [[buffer(7)]],
    device uint *d2_lo [[buffer(8)]], device uint *d2_hi [[buffer(9)]],
    constant uint2 &strides [[buffer(10)]], constant uint2 &dim [[buffer(11)]],
    uint2 gid [[thread_position_in_grid]], uint2 bid [[threadgroup_position_in_grid]],
    uint2 grid_groups [[threadgroups_per_grid]], uint lid [[thread_index_in_threadgroup]])
{
    const int width = (int)dim.x;
    const int height = (int)dim.y;
    ulong my_r1 = 0uL;
    ulong my_d1 = 0uL;
    ulong my_r2 = 0uL;
    ulong my_d2 = 0uL;
    if ((int)gid.x < width && (int)gid.y < height) {
        const ulong rv = (ulong)ref[(int)gid.y * (int)strides.x + (int)gid.x];
        const ulong dv = (ulong)dis[(int)gid.y * (int)strides.y + (int)gid.x];
        my_r1 = rv;
        my_d1 = dv;
        my_r2 = rv * rv;
        my_d2 = dv * dv;
    }

    threadgroup ulong tg_r1[FM_THREADS_PER_GROUP];
    threadgroup ulong tg_d1[FM_THREADS_PER_GROUP];
    threadgroup ulong tg_r2[FM_THREADS_PER_GROUP];
    threadgroup ulong tg_d2[FM_THREADS_PER_GROUP];
    const uint idx = bid.y * grid_groups.x + bid.x;
    reduce_and_store(my_r1, my_d1, my_r2, my_d2, r1_lo, r1_hi, d1_lo, d1_hi, r2_lo, r2_hi, d2_lo,
                     d2_hi, idx, lid, tg_r1, tg_d1, tg_r2, tg_d2);
}

kernel void float_moment_kernel_16bpc(
    const device uchar *ref [[buffer(0)]], const device uchar *dis [[buffer(1)]],
    device uint *r1_lo [[buffer(2)]], device uint *r1_hi [[buffer(3)]],
    device uint *d1_lo [[buffer(4)]], device uint *d1_hi [[buffer(5)]],
    device uint *r2_lo [[buffer(6)]], device uint *r2_hi [[buffer(7)]],
    device uint *d2_lo [[buffer(8)]], device uint *d2_hi [[buffer(9)]],
    constant uint4 &strides [[buffer(10)]], constant uint2 &dim [[buffer(11)]],
    uint2 gid [[thread_position_in_grid]], uint2 bid [[threadgroup_position_in_grid]],
    uint2 grid_groups [[threadgroups_per_grid]], uint lid [[thread_index_in_threadgroup]])
{
    const int width = (int)dim.x;
    const int height = (int)dim.y;
    ulong my_r1 = 0uL;
    ulong my_d1 = 0uL;
    ulong my_r2 = 0uL;
    ulong my_d2 = 0uL;
    if ((int)gid.x < width && (int)gid.y < height) {
        const device ushort *ref_row = (const device ushort *)(ref + (int)gid.y * (int)strides.x);
        const device ushort *dis_row = (const device ushort *)(dis + (int)gid.y * (int)strides.y);
        const ulong rv = (ulong)ref_row[(int)gid.x];
        const ulong dv = (ulong)dis_row[(int)gid.x];
        my_r1 = rv;
        my_d1 = dv;
        my_r2 = rv * rv;
        my_d2 = dv * dv;
    }

    threadgroup ulong tg_r1[FM_THREADS_PER_GROUP];
    threadgroup ulong tg_d1[FM_THREADS_PER_GROUP];
    threadgroup ulong tg_r2[FM_THREADS_PER_GROUP];
    threadgroup ulong tg_d2[FM_THREADS_PER_GROUP];
    const uint idx = bid.y * grid_groups.x + bid.x;
    reduce_and_store(my_r1, my_d1, my_r2, my_d2, r1_lo, r1_hi, d1_lo, d1_hi, r2_lo, r2_hi, d2_lo,
                     d2_hi, idx, lid, tg_r1, tg_d1, tg_r2, tg_d2);
}
