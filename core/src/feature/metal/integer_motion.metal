/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernel for integer_motion (v1) (T8-1i / ADR-0421).
 *  Mirrors `libvmaf/src/feature/vulkan/shaders/motion.comp`.
 *
 *  Algorithm (must match CPU libvmaf/src/feature/integer_motion.c,
 *  separable Gaussian blur V→H + SAD):
 *    1. Vertical filter:
 *         v[i,j] = (sum_k FILTER[k] * src[mirror(i-2+k), j] + (1<<(bpc-1))) >> bpc
 *    2. Horizontal filter:
 *         b[i,j] = (sum_k FILTER[k] * v[i, mirror(j-2+k)] + 32768) >> 16
 *    3. SAD += |b[i,j] - prev_b[i,j]|  (per-pixel)
 *    4. Host: score = sad / 256.0 / (W * H)
 *       motion2 = min(prev, cur); motion3 from host-side post-processing.
 *
 *  Filter (sum = 65536):  {3571, 16004, 26386, 16004, 3571}
 *
 *  Mirror padding: skip-boundary (2*(sup-1)-idx for idx >= sup).
 *
 *  Reduction: per-WG uint SAD partial (host accumulates in double).
 *  Note: max per-pixel SAD = 255 (8bpc) or 65535 (16bpc). For a
 *  16×16 = 256-thread WG: max WG sum = 256 × 65535 ≈ 16.7M — fits
 *  comfortably in uint32. Host double-precision accumulation.
 *
 *  Buffer bindings:
 *   [[buffer(0)]] ref          — const uchar * (current frame)
 *   [[buffer(1)]] prev_blurred — uint16 *  (prev blurred, uint16 per pixel)
 *   [[buffer(2)]] cur_blurred  — uint16 *  (output current blurred)
 *   [[buffer(3)]] sad_parts    — uint *    (grid_w × grid_h)
 *   [[buffer(4)]] strides      — uint4 (.x=ref_stride_bytes,
 *                                        .y=blur_stride_ushorts,
 *                                        .z=bpc, .w=compute_sad 0/1)
 *   [[buffer(5)]] dim          — uint2 (width, height)
 */

#include <metal_stdlib>
using namespace metal;

constant uint FILTER[5] = {3571u, 16004u, 26386u, 16004u, 3571u};

#define HALF_FW 2
#define TILE_W  20   /* 16 + 2*HALF_FW */
#define TILE_H  20   /* same for vertical pass */
#define TILE_PITCH 20

static inline int skip_mirror(int idx, int sup) {
    if (idx < 0) { return -idx; }
    if (idx >= sup) { return 2 * (sup - 1) - idx; }
    return idx;
}

/* ------------------------------------------------------------------ */
/*  8 bpc kernel                                                        */
/* ------------------------------------------------------------------ */
kernel void integer_motion_kernel_8bpc(
    const device uchar   *ref          [[buffer(0)]],
    const device ushort  *prev_blurred [[buffer(1)]],
    device       ushort  *cur_blurred  [[buffer(2)]],
    device       uint    *sad_parts    [[buffer(3)]],
    constant     uint4   &strides      [[buffer(4)]],
    constant     uint2   &dim          [[buffer(5)]],
    uint2  gid         [[thread_position_in_grid]],
    uint2  bid         [[threadgroup_position_in_grid]],
    uint2  grid_groups [[threadgroups_per_grid]],
    uint2  lid2        [[thread_position_in_threadgroup]],
    uint   lid         [[thread_index_in_threadgroup]],
    uint   simd_lane   [[thread_index_in_simdgroup]],
    uint   simd_id     [[simdgroup_index_in_threadgroup]],
    uint   simd_count  [[simdgroups_per_threadgroup]])
{
    const int width       = (int)dim.x;
    const int height      = (int)dim.y;
    const uint bpc        = strides.z;
    const int compute_sad = (int)strides.w;

    /* --- Phase 1: load 20×20 src tile. */
    threadgroup uint s_tile[TILE_H * TILE_PITCH];
    {
        const int wg_ox = (int)bid.x * 16 - HALF_FW;
        const int wg_oy = (int)bid.y * 16 - HALF_FW;
        const int n_elems = TILE_W * TILE_H;
        const int wg_size = 16 * 16;
        for (int i = (int)lid; i < n_elems; i += wg_size) {
            const int ty = i / TILE_W;
            const int tx = i % TILE_W;
            const int sy = skip_mirror(wg_oy + ty, height);
            const int sx = skip_mirror(wg_ox + tx, width);
            s_tile[ty * TILE_PITCH + tx] = (uint)ref[sy * (int)strides.x + sx];
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* --- Phase 2: vertical filter on inner rows (with halo). */
    threadgroup uint s_vert[TILE_H * TILE_PITCH];
    {
        const int wg_oy = (int)bid.y * 16 - HALF_FW;
        for (int i = (int)lid; i < TILE_W * TILE_H; i += 16 * 16) {
            const int ty = i / TILE_W;
            const int tx = i % TILE_W;
            uint acc = 0u;
            for (int k = 0; k < 5; ++k) {
                /* tile row for filter tap k: ty+k-HALF_FW relative to halo origin */
                const int src_ty = ty + k - HALF_FW;
                /* src_ty is relative to tile; if out-of-halo clamp with mirror */
                const int abs_row = wg_oy + ty + (k - HALF_FW);
                const int mir_row = skip_mirror(abs_row, height);
                /* Map back to tile — use mir_row - wg_oy clamped to tile */
                int tr = mir_row - wg_oy;
                tr = max(0, min(tr, TILE_H - 1));
                (void)src_ty;
                acc += FILTER[k] * s_tile[tr * TILE_PITCH + tx];
            }
            /* Round and shift: +round >> bpc */
            const uint rounding = 1u << (bpc - 1u);
            s_vert[ty * TILE_PITCH + tx] = (acc + rounding) >> bpc;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* --- Phase 3: horizontal filter + write blurred + SAD. */
    uint my_sad = 0u;
    if ((int)gid.x < width && (int)gid.y < height) {
        const int ty = (int)lid2.y + HALF_FW;
        const int tx = (int)lid2.x + HALF_FW;
        uint acc = 0u;
        for (int k = 0; k < 5; ++k) {
            acc += FILTER[k] * s_vert[ty * TILE_PITCH + (tx + k - HALF_FW)];
        }
        /* Round and shift: +32768 >> 16 */
        const ushort blurred = (ushort)((acc + 32768u) >> 16u);
        const int flat = (int)gid.y * (int)strides.y + (int)gid.x;
        cur_blurred[flat] = blurred;
        if (compute_sad != 0) {
            const uint pb = (uint)prev_blurred[flat];
            const uint cb = (uint)blurred;
            my_sad = (pb > cb) ? (pb - cb) : (cb - pb);
        }
    }

    threadgroup uint sg_sad[8];
    const uint lane_sum = simd_sum(my_sad);
    if (simd_lane == 0) { sg_sad[simd_id] = lane_sum; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        uint gs = 0u;
        for (uint i = 0; i < simd_count; ++i) { gs += sg_sad[i]; }
        sad_parts[bid.y * grid_groups.x + bid.x] = gs;
    }
}

/* ------------------------------------------------------------------ */
/*  16 bpc kernel                                                       */
/* ------------------------------------------------------------------ */
kernel void integer_motion_kernel_16bpc(
    const device uchar   *ref          [[buffer(0)]],
    const device ushort  *prev_blurred [[buffer(1)]],
    device       ushort  *cur_blurred  [[buffer(2)]],
    device       uint    *sad_parts    [[buffer(3)]],
    constant     uint4   &strides      [[buffer(4)]],
    constant     uint2   &dim          [[buffer(5)]],
    uint2  gid         [[thread_position_in_grid]],
    uint2  bid         [[threadgroup_position_in_grid]],
    uint2  grid_groups [[threadgroups_per_grid]],
    uint2  lid2        [[thread_position_in_threadgroup]],
    uint   lid         [[thread_index_in_threadgroup]],
    uint   simd_lane   [[thread_index_in_simdgroup]],
    uint   simd_id     [[simdgroup_index_in_threadgroup]],
    uint   simd_count  [[simdgroups_per_threadgroup]])
{
    const int width       = (int)dim.x;
    const int height      = (int)dim.y;
    const uint bpc        = strides.z;
    const int compute_sad = (int)strides.w;

    threadgroup uint s_tile[TILE_H * TILE_PITCH];
    {
        const int wg_ox = (int)bid.x * 16 - HALF_FW;
        const int wg_oy = (int)bid.y * 16 - HALF_FW;
        const int n_elems = TILE_W * TILE_H;
        const int wg_size = 16 * 16;
        for (int i = (int)lid; i < n_elems; i += wg_size) {
            const int ty = i / TILE_W;
            const int tx = i % TILE_W;
            const int sy = skip_mirror(wg_oy + ty, height);
            const int sx = skip_mirror(wg_ox + tx, width);
            const device ushort *row_ptr =
                (const device ushort *)(ref + sy * (int)strides.x);
            s_tile[ty * TILE_PITCH + tx] = (uint)row_ptr[sx];
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    threadgroup uint s_vert[TILE_H * TILE_PITCH];
    {
        const int wg_oy = (int)bid.y * 16 - HALF_FW;
        for (int i = (int)lid; i < TILE_W * TILE_H; i += 16 * 16) {
            const int ty = i / TILE_W;
            const int tx = i % TILE_W;
            uint acc = 0u;
            for (int k = 0; k < 5; ++k) {
                const int abs_row = wg_oy + ty + (k - HALF_FW);
                const int mir_row = skip_mirror(abs_row, height);
                int tr = mir_row - wg_oy;
                tr = max(0, min(tr, TILE_H - 1));
                acc += FILTER[k] * s_tile[tr * TILE_PITCH + tx];
            }
            const uint rounding = 1u << (bpc - 1u);
            s_vert[ty * TILE_PITCH + tx] = (acc + rounding) >> bpc;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    uint my_sad = 0u;
    if ((int)gid.x < width && (int)gid.y < height) {
        const int ty = (int)lid2.y + HALF_FW;
        const int tx = (int)lid2.x + HALF_FW;
        uint acc = 0u;
        for (int k = 0; k < 5; ++k) {
            acc += FILTER[k] * s_vert[ty * TILE_PITCH + (tx + k - HALF_FW)];
        }
        const ushort blurred = (ushort)((acc + 32768u) >> 16u);
        const int flat = (int)gid.y * (int)strides.y + (int)gid.x;
        cur_blurred[flat] = blurred;
        if (compute_sad != 0) {
            const uint pb = (uint)prev_blurred[flat];
            const uint cb = (uint)blurred;
            my_sad = (pb > cb) ? (pb - cb) : (cb - pb);
        }
    }

    threadgroup uint sg_sad[8];
    const uint lane_sum = simd_sum(my_sad);
    if (simd_lane == 0) { sg_sad[simd_id] = lane_sum; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        uint gs = 0u;
        for (uint i = 0; i < simd_count; ++i) { gs += sg_sad[i]; }
        sad_parts[bid.y * grid_groups.x + bid.x] = gs;
    }
}
