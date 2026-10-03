/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernel for float_moment (T8-1e / ADR-0421).
 *  Emits float_moment_{ref,dis}{1st,2nd} from exact integer workgroup sums.
 *
 *  The CPU normalises high-bit-depth samples by a power-of-two scaler before
 *  accumulation. This kernel accumulates the raw integer values and the CPU's
 *  squares in units of 1 / scaler^2; the host divides the first moments by
 *  scaler and the second moments by scaler².
 *
 *  moment.c forms each square in float. At 8 bits that is the integer square.
 *  The 10/12/16-bit kernel adds vmaf_mtl_moment_float_square()
 *  (metal_float_moment_math.h): one fp32 product of the sample with itself,
 *  an integer below 2^32, which at 16 bits is the integer square rounded to
 *  24 bits as the CPU's float is (ADR-1498; the design of ADR-1453, ADR-1449,
 *  ADR-1447). An exact integer square, which this kernel added, is another
 *  number at 16 bits.
 *  MSL has no 64-bit SIMD reduction or atomic, so all 256 lanes publish ulong
 *  values to threadgroup memory and lane 0 sums them without losing carries.
 *  Each uint64 result is then exported as a uint32 lo/hi pair.
 *
 *  A 16-bit frame of more than 2^21 pixels can pass 2^53 units, where the CPU's
 *  running double rounds as it adds and the exact sum is another number
 *  (ADR-1497). For such a frame the host (vmaf_mtl_msum_may_round()) enqueues
 *  five more kernels on the same encoder, after the frame kernel and before
 *  its one wait, the arithmetic of metal_float_moment_sum.h (the CUDA / HIP
 *  float_moment_sum_gpu.h laid out over threadgroups of 256 lanes):
 *   float_moment_plane_sums     the four exact frame sums from the partials;
 *   float_moment_row_totals     each row's exact sum of float squares;
 *   float_moment_row_plans      a plan per row from the prefix of those sums;
 *   float_moment_row_units      each planned row's increments, composed in
 *                               pixel order over the lanes' runs and an
 *                               ordered tree;
 *   float_moment_ordered_totals one walk per plane over the rows: the CPU's
 *                               rounded second-moment sum, which replaces the
 *                               exact one in sums[2 + plane].
 *  Each of the last four returns at once while the plane's exact sum is at
 *  most 2^53 units: it is the CPU's sum then.
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

#include "metal_float_moment_math.h"
#include "metal_float_moment_sum.h"

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
        const uint rv = (uint)ref_row[(int)gid.x];
        const uint dv = (uint)dis_row[(int)gid.x];
        my_r1 = (ulong)rv;
        my_d1 = (ulong)dv;
        my_r2 = (ulong)vmaf_mtl_moment_float_square(rv);
        my_d2 = (ulong)vmaf_mtl_moment_float_square(dv);
    }

    threadgroup ulong tg_r1[FM_THREADS_PER_GROUP];
    threadgroup ulong tg_d1[FM_THREADS_PER_GROUP];
    threadgroup ulong tg_r2[FM_THREADS_PER_GROUP];
    threadgroup ulong tg_d2[FM_THREADS_PER_GROUP];
    const uint idx = bid.y * grid_groups.x + bid.x;
    reduce_and_store(my_r1, my_d1, my_r2, my_d2, r1_lo, r1_hi, d1_lo, d1_hi, r2_lo, r2_hi, d2_lo,
                     d2_hi, idx, lid, tg_r1, tg_d1, tg_r2, tg_d2);
}

/* ---- ADR-1497: the CPU's rounded second-moment sum past 2^53 units ---- */

/* The four exact frame sums from the workgroup partials, one threadgroup of
 * FM_THREADS_PER_GROUP lanes: sums[0..3] = ref1, dis1, ref2, dis2. */
kernel void float_moment_plane_sums(
    const device uint *r1_lo [[buffer(0)]], const device uint *r1_hi [[buffer(1)]],
    const device uint *d1_lo [[buffer(2)]], const device uint *d1_hi [[buffer(3)]],
    const device uint *r2_lo [[buffer(4)]], const device uint *r2_hi [[buffer(5)]],
    const device uint *d2_lo [[buffer(6)]], const device uint *d2_hi [[buffer(7)]],
    device ulong *sums [[buffer(8)]], constant uint &count [[buffer(9)]],
    uint lid [[thread_index_in_threadgroup]])
{
    threadgroup ulong tg[4u * FM_THREADS_PER_GROUP];
    ulong a0 = 0uL;
    ulong a1 = 0uL;
    ulong a2 = 0uL;
    ulong a3 = 0uL;
    for (uint i = lid; i < count; i += FM_THREADS_PER_GROUP) {
        a0 += ((ulong)r1_hi[i] << 32uL) | (ulong)r1_lo[i];
        a1 += ((ulong)d1_hi[i] << 32uL) | (ulong)d1_lo[i];
        a2 += ((ulong)r2_hi[i] << 32uL) | (ulong)r2_lo[i];
        a3 += ((ulong)d2_hi[i] << 32uL) | (ulong)d2_lo[i];
    }
    tg[lid] = a0;
    tg[FM_THREADS_PER_GROUP + lid] = a1;
    tg[(2u * FM_THREADS_PER_GROUP) + lid] = a2;
    tg[(3u * FM_THREADS_PER_GROUP) + lid] = a3;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint step = FM_THREADS_PER_GROUP / 2u; step > 0u; step >>= 1u) {
        if (lid < step) {
            for (uint k = 0u; k < 4u; ++k) {
                tg[(k * FM_THREADS_PER_GROUP) + lid] += tg[(k * FM_THREADS_PER_GROUP) + lid + step];
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lid == 0u) {
        for (uint k = 0u; k < 4u; ++k) {
            sums[k] = tg[k * FM_THREADS_PER_GROUP];
        }
    }
}

/* Each row's exact sum: threadgroup x = row, y = plane; dim = (width, height,
 * stride in bytes, 0). */
kernel void float_moment_row_totals(
    const device uchar *ref [[buffer(0)]], const device uchar *dis [[buffer(1)]],
    const device ulong *sums [[buffer(2)]], device ulong *row_totals [[buffer(3)]],
    constant uint4 &dim [[buffer(4)]], uint2 bid [[threadgroup_position_in_grid]],
    uint lane [[thread_index_in_threadgroup]])
{
    threadgroup ulong totals[VMAF_MTL_MSUM_LANES];
    const uint row = bid.x;
    const uint plane = bid.y;
    if (sums[2u + plane] <= VMAF_MTL_MSUM_EXACT_END)
        return;
    const device uchar *luma = plane == 0u ? ref : dis;
    totals[lane] =
        vmaf_mtl_msum_lane_total(vmaf_mtl_msum_line(luma, (size_t)dim.z, row), dim.x, lane);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint step = VMAF_MTL_MSUM_LANES / 2u; step > 0u; step >>= 1u) {
        if (lane < step)
            totals[lane] += totals[lane + step];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lane == 0u)
        row_totals[((size_t)plane * dim.y) + row] = totals[0];
}

/* The plan of every row of one plane, threadgroup x = plane: the lanes stage
 * a batch of row sums, lane 0 follows their prefix, the lanes store the plans. */
kernel void float_moment_row_plans(
    const device ulong *sums [[buffer(0)]], const device ulong *row_totals [[buffer(1)]],
    device int *plans [[buffer(2)]], constant uint4 &dim [[buffer(3)]],
    uint plane [[threadgroup_position_in_grid]], uint lane [[thread_index_in_threadgroup]])
{
    threadgroup ulong staged_totals[VMAF_MTL_MSUM_BATCH];
    threadgroup int staged_plans[VMAF_MTL_MSUM_BATCH];
    if (sums[2u + plane] <= VMAF_MTL_MSUM_EXACT_END)
        return;
    const uint height = dim.y;
    const size_t base = (size_t)plane * height;
    ulong prefix = 0uL;
    for (uint first = 0u; first < height; first += VMAF_MTL_MSUM_BATCH) {
        const uint count = height - first < VMAF_MTL_MSUM_BATCH ? height - first
                                                                : VMAF_MTL_MSUM_BATCH;
        const bool staged = lane < count && lane < VMAF_MTL_MSUM_BATCH;
        if (staged)
            staged_totals[lane] = row_totals[base + first + lane];
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (lane == 0u)
            vmaf_mtl_msum_plan_batch(&prefix, staged_totals, staged_plans, count);
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (staged)
            plans[base + first + lane] = staged_plans[lane];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
}

/* Each planned row's increments under its plan: a lane composes its run, then
 * the lanes are composed in order. Threadgroup x = row, y = plane. A row
 * without a binade plan gets zeros, which the walk never uses, and its samples
 * are not read. */
kernel void float_moment_row_units(
    const device uchar *ref [[buffer(0)]], const device uchar *dis [[buffer(1)]],
    const device ulong *sums [[buffer(2)]], const device int *plans [[buffer(3)]],
    device long *row_units [[buffer(4)]], constant uint4 &dim [[buffer(5)]],
    uint2 bid [[threadgroup_position_in_grid]], uint lane [[thread_index_in_threadgroup]])
{
    threadgroup long units[2u * VMAF_MTL_MSUM_LANES];
    const uint row = bid.x;
    const uint plane = bid.y;
    if (sums[2u + plane] <= VMAF_MTL_MSUM_EXACT_END)
        return;
    const size_t at = ((size_t)plane * dim.y) + row;
    const int plan = plans[at];
    if (!vmaf_mtl_msum_plan_is_binade(plan)) {
        if (lane == 0u) {
            row_units[(size_t)2u * at] = 0;
            row_units[((size_t)2u * at) + 1u] = 0;
        }
        return;
    }
    const device uchar *luma = plane == 0u ? ref : dis;
    vmaf_mtl_msum_lane_units(vmaf_mtl_msum_line(luma, (size_t)dim.z, row), dim.x, plan, lane,
                             units);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint step = 1u; step < VMAF_MTL_MSUM_LANES; step <<= 1u) {
        vmaf_mtl_msum_tree_step(units, lane, step);
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lane == 0u) {
        row_units[(size_t)2u * at] = units[0];
        row_units[((size_t)2u * at) + 1u] = units[1];
    }
}

/* The CPU's second-moment sum of one plane, threadgroup x = plane. Lane 0
 * walks the rows, a staged batch at a time. When a row must be added as runs,
 * every lane computes one into threadgroup memory and lane 0 adds them in
 * order. Every round stages a batch or consumes a row, which bounds the loop. */
kernel void float_moment_ordered_totals(
    const device uchar *ref [[buffer(0)]], const device uchar *dis [[buffer(1)]],
    device ulong *sums [[buffer(2)]], const device ulong *row_totals [[buffer(3)]],
    const device int *plans [[buffer(4)]], const device long *row_units [[buffer(5)]],
    constant uint4 &dim [[buffer(6)]], uint plane [[threadgroup_position_in_grid]],
    uint lane [[thread_index_in_threadgroup]])
{
    threadgroup int staged_plans[VMAF_MTL_MSUM_BATCH];
    threadgroup ulong staged_totals[VMAF_MTL_MSUM_BATCH];
    threadgroup long staged_units[2u * VMAF_MTL_MSUM_BATCH];
    threadgroup ulong run_totals[VMAF_MTL_MSUM_LANES];
    threadgroup long run_low[2u * VMAF_MTL_MSUM_LANES];
    threadgroup long run_high[2u * VMAF_MTL_MSUM_LANES];
    threadgroup uint command;
    threadgroup uint operand;
    threadgroup ulong walked;
    if (sums[2u + plane] <= VMAF_MTL_MSUM_EXACT_END)
        return;
    const uint height = dim.y;
    const size_t base = (size_t)plane * height;
    const device uchar *luma = plane == 0u ? ref : dis;
    if (lane == 0u) {
        command = VMAF_MTL_MSUM_WALK_LOAD;
        operand = 0u;
        walked = 0uL;
    }
    ulong sum = 0uL;
    uint row = 0u;
    uint first = 0u;
    const uint rounds = height + height / VMAF_MTL_MSUM_BATCH + 2u;
    for (uint round = 0u; round < rounds; round++) {
        threadgroup_barrier(mem_flags::mem_threadgroup);
        const uint todo = command;
        const uint what = operand;
        if (todo == VMAF_MTL_MSUM_WALK_DONE)
            break;
        if (todo == VMAF_MTL_MSUM_WALK_LOAD) {
            vmaf_mtl_msum_walk_stage(plans + base, row_totals + base, row_units + (2u * base),
                                     height, what * VMAF_MTL_MSUM_BATCH, lane, staged_plans,
                                     staged_totals, staged_units);
        } else {
            vmaf_mtl_msum_walk_run(vmaf_mtl_msum_line(luma, (size_t)dim.z, what), dim.x, walked,
                                   lane, run_totals, run_low, run_high);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (lane != 0u)
            continue;
        if (todo == VMAF_MTL_MSUM_WALK_RUNS) {
            sum = vmaf_mtl_msum_walk_row_runs(vmaf_mtl_msum_line(luma, (size_t)dim.z, what),
                                              dim.x, sum, run_totals, run_low, run_high);
            row = what + 1u;
        } else {
            first = what * VMAF_MTL_MSUM_BATCH;
        }
        uint next = 0u;
        command = vmaf_mtl_msum_walk_next(height, first, &sum, &row, staged_plans, staged_totals,
                                          staged_units, &next);
        operand = next;
        walked = sum;
    }
    if (lane == 0u)
        sums[2u + plane] = sum;
}
