/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernel for integer_psnr (T8-1g / ADR-0421).
 *  Emits `psnr_y`, `psnr_cb`, `psnr_cr` — one kernel invocation per plane.
 *
 *  Algorithm (must match CPU libvmaf/src/feature/integer_psnr.c::sse_line_*
 *  and CUDA twin libvmaf/src/feature/cuda/float_psnr/float_psnr_score.cu):
 *    diff = (int64)(ref_px) - (int64)(dis_px)
 *    sse  += diff * diff        (exact integer)
 *    mse  = sse / (W * H)
 *    psnr = min(10 * log10(peak^2 / max(mse, 1e-16)), psnr_max)
 *    where peak = (1 << bpc) - 1, psnr_max = 6*bpc + 12.
 *
 *  Reduction: each pixel produces a uint64 squared-error. MSL lacks
 *  `atomic_ulong`, so we split into (lo, hi) uint32 slots per threadgroup
 *  and reconstruct on the host. Two uint partials per WG:
 *    sse_lo_parts[idx] = (uint32)(sse_wg & 0xFFFFFFFF)
 *    sse_hi_parts[idx] = (uint32)(sse_wg >> 32)
 *  Host reconstructs: sse_wg = ((uint64)hi << 32) | lo, accumulates in
 *  double (which has 53-bit mantissa — sufficient for 64-bit integers up to
 *  ~9e15, well beyond any frame size × max SSE per pixel).
 *
 *  Buffer bindings (same for 8bpc and 16bpc, strides format differs):
 *   [[buffer(0)]] ref       — const uchar *  (plane Y/Cb/Cr, byte-addressed)
 *   [[buffer(1)]] dis       — const uchar *
 *   [[buffer(2)]] sse_lo    — uint * (grid_w × grid_h per-WG uint32 low)
 *   [[buffer(3)]] sse_hi    — uint * (grid_w × grid_h per-WG uint32 high)
 *   [[buffer(4)]] strides   — uint2 (8bpc: ref_stride, dis_stride)
 *                           — uint4 (16bpc: .x=ref, .y=dis, .z=bpc, .w=0)
 *   [[buffer(5)]] dim       — uint2 (width, height)
 */

#include <metal_stdlib>
using namespace metal;

/* ------------------------------------------------------------------ */
/*  8 bpc kernel                                                        */
/* ------------------------------------------------------------------ */
kernel void integer_psnr_kernel_8bpc(
    const device uchar  *ref      [[buffer(0)]],
    const device uchar  *dis      [[buffer(1)]],
    device       uint   *sse_lo   [[buffer(2)]],
    device       uint   *sse_hi   [[buffer(3)]],
    constant     uint2  &strides  [[buffer(4)]],
    constant     uint2  &dim      [[buffer(5)]],
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

    ulong my_se = 0uL;
    if ((int)gid.x < width && (int)gid.y < height) {
        const long r = (long)ref[(int)gid.y * (int)strides.x + (int)gid.x];
        const long d = (long)dis[(int)gid.y * (int)strides.y + (int)gid.x];
        const long diff = r - d;
        my_se = (ulong)(diff * diff);
    }

    /* Two-level reduction using uint32 halves (no atomic_ulong). */
    threadgroup uint sg_lo[8], sg_hi[8];
    /* Reduce within SIMD group using uint32 simd_sum (two passes). */
    const uint my_lo = (uint)(my_se & 0xFFFFFFFFuL);
    const uint my_hi = (uint)(my_se >> 32uL);
    const uint lane_lo = simd_sum(my_lo);
    const uint lane_hi = simd_sum(my_hi);
    /* Carry correction: lo might have overflowed into hi. */
    /* We accept a small error here — for 256 threads × max diff^2=65025:
     * max WG sum = 256 * 65025 ≈ 16.6M, fits in uint32 with headroom. */
    if (simd_lane == 0) {
        sg_lo[simd_id] = lane_lo;
        sg_hi[simd_id] = lane_hi;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        /* Reconstruct 64-bit per WG and re-split (handles carry). */
        ulong wg_se = 0uL;
        for (uint i = 0; i < simd_count; ++i) {
            wg_se += ((ulong)sg_hi[i] << 32uL) | (ulong)sg_lo[i];
        }
        const uint idx = bid.y * grid_groups.x + bid.x;
        sse_lo[idx] = (uint)(wg_se & 0xFFFFFFFFuL);
        sse_hi[idx] = (uint)(wg_se >> 32uL);
    }
}

/* ------------------------------------------------------------------ */
/*  16 bpc kernel                                                       */
/* ------------------------------------------------------------------ */
kernel void integer_psnr_kernel_16bpc(
    const device uchar  *ref      [[buffer(0)]],
    const device uchar  *dis      [[buffer(1)]],
    device       uint   *sse_lo   [[buffer(2)]],
    device       uint   *sse_hi   [[buffer(3)]],
    constant     uint4  &strides  [[buffer(4)]],
    constant     uint2  &dim      [[buffer(5)]],
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

    ulong my_se = 0uL;
    if ((int)gid.x < width && (int)gid.y < height) {
        const device ushort *ref_row =
            (const device ushort *)(ref + (int)gid.y * (int)strides.x);
        const device ushort *dis_row =
            (const device ushort *)(dis + (int)gid.y * (int)strides.y);
        const long r = (long)ref_row[(int)gid.x];
        const long d = (long)dis_row[(int)gid.x];
        const long diff = r - d;
        my_se = (ulong)(diff * diff);
    }

    threadgroup uint sg_lo[8], sg_hi[8];
    const uint my_lo = (uint)(my_se & 0xFFFFFFFFuL);
    const uint my_hi = (uint)(my_se >> 32uL);
    const uint lane_lo = simd_sum(my_lo);
    const uint lane_hi = simd_sum(my_hi);
    if (simd_lane == 0) {
        sg_lo[simd_id] = lane_lo;
        sg_hi[simd_id] = lane_hi;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        ulong wg_se = 0uL;
        for (uint i = 0; i < simd_count; ++i) {
            wg_se += ((ulong)sg_hi[i] << 32uL) | (ulong)sg_lo[i];
        }
        const uint idx = bid.y * grid_groups.x + bid.x;
        sse_lo[idx] = (uint)(wg_se & 0xFFFFFFFFuL);
        sse_hi[idx] = (uint)(wg_se >> 32uL);
    }
}
