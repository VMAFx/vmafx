/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernel for float_psnr (T8-1d / ADR-0421).
 *
 *  The sums are integers so that they are exact (ADR-1498; the design of the
 *  CUDA twin, ADR-1455, and of the SYCL and HIP twins, ADR-1450, ADR-1440).
 *  float_psnr.c adds `(double)(diff * diff)` per row and the rows in double:
 *  every term is a float and a multiple of 1 / scaler^2 (scaler =
 *  2^(bpc - 8)), so its running sum is exact and is the exact sum of its
 *  terms. Each thread forms the CPU's term as an integer in that unit
 *  (vmaf_mtl_fpsnr_term(), metal_float_psnr_math.h: one fp32 product of the
 *  raw sample difference, below 2^32); the threadgroup adds its 256 terms in
 *  64 bits and stores one ulong per threadgroup; the host adds those in
 *  uint64 and divides by scaler^2 and the pixel count
 *  (float_psnr_metal.mm::float_psnr_noise()).
 *
 *  A fp32 threadgroup sum, which this kernel had, is exact only at 8 bits: at
 *  10, 12 and 16 bits it rounds once a group's differences are large. MSL has
 *  no 64-bit SIMD reduction (simd_sum excludes long and ulong, Metal Shading
 *  Language Specification 4.1, section 6.10.2) and no 64-bit atomic the
 *  backend may use, so every thread publishes its term to threadgroup memory
 *  and thread 0 adds the 256 values. Threads outside the frame publish 0 and
 *  still reach the barrier.
 *
 *  Buffer bindings (both kernels, host must match float_psnr_metal.mm):
 *   [[buffer(0)]] ref      — const uchar *  (packed rows of the luma plane)
 *   [[buffer(1)]] dis      — const uchar *
 *   [[buffer(2)]] partials — ulong *        (grid_w × grid_h group sums)
 *   [[buffer(3)]] strides  — uint2          (ref_stride_bytes, dis_stride_bytes)
 *   [[buffer(4)]] dim      — uint2          (width, height)
 */

#include <metal_stdlib>
using namespace metal;

#include "metal_float_psnr_math.h"

/* The 16 x 16 threadgroup the host dispatches. */
#define FPSNR_THREADS_PER_GROUP 256u

/* The threadgroup's sum of `mine` over its threads, at partials[group]. The
 * sum is of integers, so its order cannot change it. */
inline void fpsnr_store_group_sum(ulong mine, uint lid, uint group, threadgroup ulong *scratch,
                                  device ulong *partials)
{
    scratch[lid] = mine;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0u) {
        ulong total = 0ul;
        for (uint i = 0u; i < FPSNR_THREADS_PER_GROUP; ++i) {
            total += scratch[i];
        }
        partials[group] = total;
    }
}

/* ------------------------------------------------------------------ */
/*  8 bpc kernel                                                        */
/* ------------------------------------------------------------------ */
kernel void float_psnr_kernel_8bpc(
    const device uchar  *ref      [[buffer(0)]],
    const device uchar  *dis      [[buffer(1)]],
    device       ulong  *partials [[buffer(2)]],
    constant     uint2  &strides  [[buffer(3)]],
    constant     uint2  &dim      [[buffer(4)]],
    uint2  gid         [[thread_position_in_grid]],
    uint2  bid         [[threadgroup_position_in_grid]],
    uint2  grid_groups [[threadgroups_per_grid]],
    uint   lid         [[thread_index_in_threadgroup]])
{
    ulong my_noise = 0ul;
    if (gid.x < dim.x && gid.y < dim.y) {
        const int r = (int)ref[gid.y * strides.x + gid.x];
        const int d = (int)dis[gid.y * strides.y + gid.x];
        my_noise = (ulong)vmaf_mtl_fpsnr_term(r, d);
    }

    threadgroup ulong scratch[FPSNR_THREADS_PER_GROUP];
    fpsnr_store_group_sum(my_noise, lid, bid.y * grid_groups.x + bid.x, scratch, partials);
}

/* ------------------------------------------------------------------ */
/*  10 / 12 / 16 bpc kernel: native ushort samples, terms in units of  */
/*  1 / scaler^2 (the host divides).                                    */
/* ------------------------------------------------------------------ */
kernel void float_psnr_kernel_16bpc(
    const device uchar  *ref      [[buffer(0)]],
    const device uchar  *dis      [[buffer(1)]],
    device       ulong  *partials [[buffer(2)]],
    constant     uint2  &strides  [[buffer(3)]],
    constant     uint2  &dim      [[buffer(4)]],
    uint2  gid         [[thread_position_in_grid]],
    uint2  bid         [[threadgroup_position_in_grid]],
    uint2  grid_groups [[threadgroups_per_grid]],
    uint   lid         [[thread_index_in_threadgroup]])
{
    ulong my_noise = 0ul;
    if (gid.x < dim.x && gid.y < dim.y) {
        const device ushort *ref_row = (const device ushort *)(ref + gid.y * strides.x);
        const device ushort *dis_row = (const device ushort *)(dis + gid.y * strides.y);
        const int r = (int)ref_row[gid.x];
        const int d = (int)dis_row[gid.x];
        my_noise = (ulong)vmaf_mtl_fpsnr_term(r, d);
    }

    threadgroup ulong scratch[FPSNR_THREADS_PER_GROUP];
    fpsnr_store_group_sum(my_noise, lid, bid.y * grid_groups.x + bid.x, scratch, partials);
}
