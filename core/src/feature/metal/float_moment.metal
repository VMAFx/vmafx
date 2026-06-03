/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernel for float_moment (T8-1e / ADR-0421).
 *  Emits four per-frame statistics across ref and dis planes:
 *  float_moment_ref1st, float_moment_dis1st,
 *  float_moment_ref2nd, float_moment_dis2nd.
 *
 *  Algorithm (must match CPU libvmaf/src/feature/float_moment.c and
 *  all other GPU backends — CUDA/SYCL/Vulkan/HIP):
 *    For each plane in {ref, dis}:
 *      1st moment: sum  += val              (floating-point pixel)
 *      2nd moment: sum2 += val * val
 *    Host: divide each accumulated sum by (W * H).
 *
 *  Pixel conversion (same as float_psnr):
 *    8bpc:  val = (float)raw          (no scaling)
 *    >8bpc: val = (float)raw / scaler  (scaler = 1 << (bpc - 8))
 *
 *  Four float partials per threadgroup (interleaved in the
 *  partials buffer):
 *    partials[idx * 4 + 0] = ref_sum1
 *    partials[idx * 4 + 1] = dis_sum1
 *    partials[idx * 4 + 2] = ref_sum2
 *    partials[idx * 4 + 3] = dis_sum2
 *  where idx = bid.y * grid_groups.x + bid.x.
 *
 *  Buffer bindings:
 *   [[buffer(0)]] ref      — const uchar *
 *   [[buffer(1)]] dis      — const uchar *
 *   [[buffer(2)]] partials — float * (grid_w × grid_h × 4 floats)
 *   [[buffer(3)]] strides  — uint4 (.x=ref_stride, .y=dis_stride,
 *                                    .z=bpc 8bpc kernel unused, >8bpc=bpc)
 *   [[buffer(4)]] dim      — uint2 (width, height)
 */

#include <metal_stdlib>
using namespace metal;

/* ------------------------------------------------------------------ */
/*  8 bpc kernel                                                        */
/* ------------------------------------------------------------------ */
kernel void float_moment_kernel_8bpc(
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

    float r1 = 0.0f, d1 = 0.0f, r2 = 0.0f, d2 = 0.0f;
    if ((int)gid.x < width && (int)gid.y < height) {
        const float rv = (float)ref[(int)gid.y * (int)strides.x + (int)gid.x];
        const float dv = (float)dis[(int)gid.y * (int)strides.y + (int)gid.x];
        r1 = rv;
        d1 = dv;
        r2 = rv * rv;
        d2 = dv * dv;
    }

    threadgroup float sg_r1[8], sg_d1[8], sg_r2[8], sg_d2[8];
    const float sr1 = simd_sum(r1);
    const float sd1 = simd_sum(d1);
    const float sr2 = simd_sum(r2);
    const float sd2 = simd_sum(d2);
    if (simd_lane == 0) {
        sg_r1[simd_id] = sr1;
        sg_d1[simd_id] = sd1;
        sg_r2[simd_id] = sr2;
        sg_d2[simd_id] = sd2;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        float gr1 = 0.0f, gd1 = 0.0f, gr2 = 0.0f, gd2 = 0.0f;
        for (uint i = 0; i < simd_count; ++i) {
            gr1 += sg_r1[i];
            gd1 += sg_d1[i];
            gr2 += sg_r2[i];
            gd2 += sg_d2[i];
        }
        const uint base = (bid.y * grid_groups.x + bid.x) * 4u;
        partials[base + 0] = gr1;
        partials[base + 1] = gd1;
        partials[base + 2] = gr2;
        partials[base + 3] = gd2;
    }
}

/* ------------------------------------------------------------------ */
/*  16 bpc kernel                                                       */
/* ------------------------------------------------------------------ */
kernel void float_moment_kernel_16bpc(
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
    const float scaler = (float)(1u << ((uint)strides.z - 8u));

    float r1 = 0.0f, d1 = 0.0f, r2 = 0.0f, d2 = 0.0f;
    if ((int)gid.x < width && (int)gid.y < height) {
        const device ushort *ref_row =
            (const device ushort *)(ref + (int)gid.y * (int)strides.x);
        const device ushort *dis_row =
            (const device ushort *)(dis + (int)gid.y * (int)strides.y);
        const float rv = (float)ref_row[(int)gid.x] / scaler;
        const float dv = (float)dis_row[(int)gid.x] / scaler;
        r1 = rv;
        d1 = dv;
        r2 = rv * rv;
        d2 = dv * dv;
    }

    threadgroup float sg_r1[8], sg_d1[8], sg_r2[8], sg_d2[8];
    const float sr1 = simd_sum(r1);
    const float sd1 = simd_sum(d1);
    const float sr2 = simd_sum(r2);
    const float sd2 = simd_sum(d2);
    if (simd_lane == 0) {
        sg_r1[simd_id] = sr1;
        sg_d1[simd_id] = sd1;
        sg_r2[simd_id] = sr2;
        sg_d2[simd_id] = sd2;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        float gr1 = 0.0f, gd1 = 0.0f, gr2 = 0.0f, gd2 = 0.0f;
        for (uint i = 0; i < simd_count; ++i) {
            gr1 += sg_r1[i];
            gd1 += sg_d1[i];
            gr2 += sg_r2[i];
            gd2 += sg_d2[i];
        }
        const uint base = (bid.y * grid_groups.x + bid.x) * 4u;
        partials[base + 0] = gr1;
        partials[base + 1] = gd1;
        partials[base + 2] = gr2;
        partials[base + 3] = gd2;
    }
}
