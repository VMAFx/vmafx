/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernel for float_psnr (T8-1d / ADR-0421).
 *  Translation of `libvmaf/src/feature/vulkan/shaders/float_psnr.comp`
 *  (same algorithm, MSL idioms).
 *
 *  Algorithm (must match CPU libvmaf/src/feature/float_psnr.c):
 *    1. Convert pixel to float:
 *         8bpc:       val = (float)raw          (peak = 255.0)
 *         10bpc:      val = (float)raw / 4.0    (peak = 255.75)
 *         12bpc:      val = (float)raw / 16.0   (peak = 255.9375)
 *         16bpc:      val = (float)raw / 256.0  (peak = 255.99609375)
 *    2. noise += (ref_val - dis_val)^2   (per-pixel float)
 *    3. Host (collect): mse = sum(partials) / (W * H)
 *                       score = min(10*log10(peak^2 / max(mse, 1e-10)), psnr_max)
 *
 *  Reduction: per-thread float → simd_sum into 8-slot threadgroup
 *  array → single float partial per threadgroup written to
 *  `partials[bid.y * grid_w + bid.x]`. No atomics (matches the
 *  integer_motion_v2 partial-sum pattern, ADR-0421).
 *
 *  Buffer bindings (8bpc kernel, host must match float_psnr_metal.mm):
 *   [[buffer(0)]] ref      — const uchar *  (planar Y, stride via strides.x)
 *   [[buffer(1)]] dis      — const uchar *  (planar Y, stride via strides.y)
 *   [[buffer(2)]] partials — float *        (grid_w × grid_h floats)
 *   [[buffer(3)]] strides  — uint2          (ref_stride_bytes, dis_stride_bytes)
 *   [[buffer(4)]] dim      — uint2          (width, height)
 *
 *  Buffer bindings (16bpc kernel, same as 8bpc except strides is uint4):
 *   [[buffer(3)]] strides  — uint4          (.x=ref_stride_bytes, .y=dis_stride_bytes,
 *                                            .z=bpc, .w=unused)
 */

#include <metal_stdlib>
using namespace metal;

/* ------------------------------------------------------------------ */
/*  8 bpc kernel                                                        */
/* ------------------------------------------------------------------ */
kernel void float_psnr_kernel_8bpc(
    const device uchar  *ref      [[buffer(0)]],
    const device uchar  *dis      [[buffer(1)]],
    device       float  *partials [[buffer(2)]],
    constant     uint2  &strides  [[buffer(3)]],
    constant     uint2  &dim      [[buffer(4)]],
    uint2  gid         [[thread_position_in_grid]],
    uint2  bid         [[threadgroup_position_in_grid]],
    uint2  grid_groups [[threadgroups_per_grid]],
    uint   lid         [[thread_index_in_threadgroup]],
    uint   simd_lane   [[thread_index_in_simdgroup]],
    uint   simd_id     [[simdgroup_index_in_threadgroup]],
    uint   simd_count  [[simdgroups_per_threadgroup]])
{
    const int width  = (int)dim.x;
    const int height = (int)dim.y;

    float my_noise = 0.0f;
    if ((int)gid.x < width && (int)gid.y < height) {
        const float r = (float)ref[(int)gid.y * (int)strides.x + (int)gid.x];
        const float d = (float)dis[(int)gid.y * (int)strides.y + (int)gid.x];
        const float diff = r - d;
        my_noise = diff * diff;
    }

    threadgroup float simd_partials[8];
    const float lane_sum = simd_sum(my_noise);
    if (simd_lane == 0) {
        simd_partials[simd_id] = lane_sum;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        float group_sum = 0.0f;
        for (uint i = 0; i < simd_count; ++i) {
            group_sum += simd_partials[i];
        }
        partials[bid.y * grid_groups.x + bid.x] = group_sum;
    }
}

/* ------------------------------------------------------------------ */
/*  16 bpc kernel                                                       */
/* ------------------------------------------------------------------ */
kernel void float_psnr_kernel_16bpc(
    const device uchar  *ref      [[buffer(0)]],
    const device uchar  *dis      [[buffer(1)]],
    device       float  *partials [[buffer(2)]],
    constant     uint4  &strides  [[buffer(3)]],
    constant     uint2  &dim      [[buffer(4)]],
    uint2  gid         [[thread_position_in_grid]],
    uint2  bid         [[threadgroup_position_in_grid]],
    uint2  grid_groups [[threadgroups_per_grid]],
    uint   lid         [[thread_index_in_threadgroup]],
    uint   simd_lane   [[thread_index_in_simdgroup]],
    uint   simd_id     [[simdgroup_index_in_threadgroup]],
    uint   simd_count  [[simdgroups_per_threadgroup]])
{
    const int width  = (int)dim.x;
    const int height = (int)dim.y;
    /* scaler = 1 << (bpc - 8): 4 for 10bpc, 16 for 12bpc, 256 for 16bpc */
    const float scaler = (float)(1u << ((uint)strides.z - 8u));

    float my_noise = 0.0f;
    if ((int)gid.x < width && (int)gid.y < height) {
        const device ushort *ref_row =
            (const device ushort *)(ref + (int)gid.y * (int)strides.x);
        const device ushort *dis_row =
            (const device ushort *)(dis + (int)gid.y * (int)strides.y);
        const float r = (float)ref_row[(int)gid.x] / scaler;
        const float d = (float)dis_row[(int)gid.x] / scaler;
        const float diff = r - d;
        my_noise = diff * diff;
    }

    threadgroup float simd_partials[8];
    const float lane_sum = simd_sum(my_noise);
    if (simd_lane == 0) {
        simd_partials[simd_id] = lane_sum;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        float group_sum = 0.0f;
        for (uint i = 0; i < simd_count; ++i) {
            group_sum += simd_partials[i];
        }
        partials[bid.y * grid_groups.x + bid.x] = group_sum;
    }
}
