/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernel for integer_motion_v2 (T8-1c / ADR-0421).
 *  Translation of `libvmaf/src/feature/cuda/integer_motion_v2/
 *  motion_v2_score.cu` (ADR-0192 / ADR-0193) — same algorithm,
 *  threadgroup-shared tile, separable filter, atomic SAD reduction.
 *
 *  Algorithm parity (must match scalar `integer_motion_v2.c`):
 *    1. Per pixel: diff = prev[i,j] - cur[i,j]   (signed)
 *    2. Vertical filter on diff:
 *         v[i,j] = (sum_k filter[k] * diff[mirror(i-2+k), j]
 *                    + (1 << (bpc - 1))) >> bpc
 *    3. Horizontal filter on v:
 *         h[i,j] = (sum_k filter[k] * v[i, mirror(j-2+k)]
 *                    + 32768) >> 16
 *    4. SAD: atomic-add |h[i,j]| into a single ulong accumulator.
 *
 *  Mirror padding: edge-replicating reflective mirror
 *  (`2 * size - idx - 1` for idx >= size), matching the CUDA twin.
 *  DIFFERS from motion_v1's `2 * size - idx - 2`; see CUDA file
 *  header for the bring-up note.
 *
 *  Threadgroup layout: 16 × 16 threads per group, +2 pixel halo on
 *  each side → 20 × 20 shared tile. Inner pitch padded to 21 to
 *  break threadgroup-memory bank conflicts (mirrors the CUDA
 *  mitigation; Apple GPUs have a 32-bank threadgroup memory).
 *
 *  Reduction: per-thread `abs(h)` (uint) → SIMD-group sum via
 *  `simd_sum` (MSL 2.4+) → threadgroup sum via a 16-slot
 *  threadgroup-memory accumulator → one uint per threadgroup
 *  written to `partials[bid.y * grid_w + bid.x]`. Host reads the
 *  partials array via `[buf contents]` and sums in double
 *  precision. NO ATOMICS — Apple's MSL `atomic_fetch_add_explicit`
 *  doesn't support `ulong` even with `atomic_ulong` declared
 *  (CI run 25685703780 / job 75408804495), and the partial-sum
 *  pattern is cleaner anyway (one global memory write per
 *  threadgroup, no contention, deterministic order under
 *  unified-memory host reduction).
 *
 *  Bit-exactness gate: `places=4` cross-backend-diff against scalar
 *  (per ADR-0214). Validation pending Apple Silicon hardware run.
 */

#include <metal_stdlib>
using namespace metal;

constant int MV2_FILTER[5] = {3571, 16004, 26386, 16004, 3571};

/* Reflect-101 index fold, iterated -- TWO defects fixed here.
 *
 * (1) CONVENTION. This helper used `2 * sup - idx - 1`, which reflects
 *     idx == sup back to sup - 1 and therefore REPEATS the boundary row.
 *     Reflect-101 skips it: `2 * (sup - 1) - idx`. Every other backend
 *     already carries the corrected form -- CPU
 *     (integer_motion_v2.c::mirror), CUDA (fixed in PR #120 / T7-15),
 *     SYCL (integer_motion_sycl.cpp::dev_mirror_motion) and HIP
 *     (integer_motion_v2/motion_v2_score.hip) -- and the SYCL fix records
 *     the measured impact of the `- 1` form as a systematic ~2.6e-3 motion
 *     drift vs CPU on every frame after the first. Metal was the last
 *     backend still diverging (cross-backend parity, ADR-0214 places=4).
 *
 * (2) OUT OF BOUNDS. The kernels load a 20x20 tile at origin
 *     `tile_origin - 2`, so this helper sees indices far outside the 5-tap
 *     neighbourhood, and a single bounce only lands in range when
 *     `idx <= 2 * (sup - 1)`. Enumerated over the real tile span the
 *     single-bounce form read out of bounds for every dimension in 1..9 and
 *     for exactly 17. Iterating is bit-identical wherever one bounce already
 *     sufficed, so this costs no in-contract score movement -- only the
 *     `- 1` -> `- 2` convention change in (1) moves borders, and it moves
 *     them onto the CPU reference.
 */
inline int mv2_mirror(int idx, int sup)
{
    if (sup <= 1) return 0;
    while (idx < 0 || idx >= sup)
        idx = (idx < 0) ? -idx : 2 * (sup - 1) - idx;
    return idx;
}

/*
 * 8 bpc kernel — packed `uint8_t` ref + dist samples.
 *
 * Buffer bindings (host side must match `integer_motion_v2_metal.mm`):
 *   [[buffer(0)]] prev       — const uchar * (planar Y, row pitch via `strides.x`)
 *   [[buffer(1)]] cur        — const uchar * (planar Y, row pitch via `strides.y`)
 *   [[buffer(2)]] sad        — atomic_ulong, single accumulator
 *   [[buffer(3)]] strides    — packed (prev_stride, cur_stride) in bytes (uint2)
 *   [[buffer(4)]] dim        — packed (width, height) (uint2)
 *   [[threadgroup(0)]] tile  — int32[20 * 21]
 */
kernel void motion_v2_kernel_8bpc(
    const device uchar       *prev    [[buffer(0)]],
    const device uchar       *cur     [[buffer(1)]],
    device   uint            *partials [[buffer(2)]],
    constant uint2           &strides [[buffer(3)]],
    constant uint2           &dim     [[buffer(4)]],
    threadgroup int          *s_diff  [[threadgroup(0)]],
    uint2  gid               [[thread_position_in_grid]],
    uint2  tid               [[thread_position_in_threadgroup]],
    uint2  bid               [[threadgroup_position_in_grid]],
    uint2  grid_groups       [[threadgroups_per_grid]],
    uint   lid               [[thread_index_in_threadgroup]],
    uint   simd_lane         [[thread_index_in_simdgroup]],
    uint   simd_id           [[simdgroup_index_in_threadgroup]],
    uint   simd_count        [[simdgroups_per_threadgroup]])
{
    constexpr int MV2_RADIUS    = 2;
    constexpr int MV2_BLOCK_X   = 16;
    constexpr int MV2_BLOCK_Y   = 16;
    constexpr int MV2_TILE_W    = MV2_BLOCK_X + 2 * MV2_RADIUS; /* 20 */
    constexpr int MV2_TILE_H    = MV2_BLOCK_Y + 2 * MV2_RADIUS; /* 20 */
    constexpr int MV2_TILE_PITCH = MV2_TILE_W + 1;              /* 21 */

    const int width  = (int)dim.x;
    const int height = (int)dim.y;
    const int prev_stride = (int)strides.x;
    const int cur_stride  = (int)strides.y;

    const int shift_y = 8;
    const int round_y = 1 << 7;
    const int shift_x = 16;
    const int round_x = 1 << 15;

    const int tile_origin_x = (int)bid.x * MV2_BLOCK_X - MV2_RADIUS;
    const int tile_origin_y = (int)bid.y * MV2_BLOCK_Y - MV2_RADIUS;
    const int tile_elems    = MV2_TILE_W * MV2_TILE_H;
    const int wg_size       = MV2_BLOCK_X * MV2_BLOCK_Y;

    for (int i = (int)lid; i < tile_elems; i += wg_size) {
        const int ty = i / MV2_TILE_W;
        const int tx = i % MV2_TILE_W;
        const int gx = mv2_mirror(tile_origin_x + tx, width);
        const int gy = mv2_mirror(tile_origin_y + ty, height);
        const int p  = (int)prev[gy * prev_stride + gx];
        const int c  = (int)cur [gy * cur_stride  + gx];
        s_diff[ty * MV2_TILE_PITCH + tx] = p - c;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* Per-thread abs(h). For 8bpc max |h| fits in uint comfortably:
     * max blurred ≈ 65461 × 65461 × 5 × 5 ≈ 1.1e10 → shifted by 16
     * = ~1.6e5, well under UINT_MAX. */
    uint abs_h = 0;
    if ((int)gid.x < width && (int)gid.y < height) {
        const int lx = (int)tid.x + MV2_RADIUS;
        const int ly = (int)tid.y + MV2_RADIUS;

        long blurred = 0;
        for (int xf = 0; xf < 5; ++xf) {
            int blurred_y = 0;
            for (int yf = 0; yf < 5; ++yf) {
                blurred_y += MV2_FILTER[yf] *
                             s_diff[(ly - MV2_RADIUS + yf) * MV2_TILE_PITCH +
                                    (lx - MV2_RADIUS + xf)];
            }
            const int v = (blurred_y + round_y) >> shift_y;
            blurred += (long)MV2_FILTER[xf] * (long)v;
        }
        const long h = (blurred + round_x) >> shift_x;
        abs_h = (uint)(h < 0 ? -h : h);
    }

    /* Two-level reduction: SIMD-group → threadgroup → one uint per
     * group written to `partials`. No atomics. */
    threadgroup uint simd_partials[8]; /* max 8 simdgroups in a 256-thread group */
    const uint lane_sum = simd_sum(abs_h);
    if (simd_lane == 0) {
        simd_partials[simd_id] = lane_sum;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        uint group_sum = 0;
        for (uint i = 0; i < simd_count; ++i) {
            group_sum += simd_partials[i];
        }
        partials[bid.y * grid_groups.x + bid.x] = group_sum;
    }
}

/*
 * 16 bpc kernel — `uint16_t` samples (row pitch in bytes). The
 * y-conv inner accumulator widens to long because
 * 26386 × 65535 × 5 ≈ 8.6e9 overflows int32. The final per-pixel
 * |h| can reach ~1.6e8 at 16bpc, still under UINT_MAX.
 *
 * `bpc` arrives via the strides buffer's z component (host passes
 * uint4 with the bpc in .z; see `integer_motion_v2_metal.mm`).
 */
kernel void motion_v2_kernel_16bpc(
    const device uchar       *prev    [[buffer(0)]],
    const device uchar       *cur     [[buffer(1)]],
    device   uint            *partials [[buffer(2)]],
    constant uint4           &strides [[buffer(3)]],
    constant uint2           &dim     [[buffer(4)]],
    threadgroup int          *s_diff  [[threadgroup(0)]],
    uint2  gid               [[thread_position_in_grid]],
    uint2  tid               [[thread_position_in_threadgroup]],
    uint2  bid               [[threadgroup_position_in_grid]],
    uint2  grid_groups       [[threadgroups_per_grid]],
    uint   lid               [[thread_index_in_threadgroup]],
    uint   simd_lane         [[thread_index_in_simdgroup]],
    uint   simd_id           [[simdgroup_index_in_threadgroup]],
    uint   simd_count        [[simdgroups_per_threadgroup]])
{
    constexpr int MV2_RADIUS    = 2;
    constexpr int MV2_BLOCK_X   = 16;
    constexpr int MV2_BLOCK_Y   = 16;
    constexpr int MV2_TILE_W    = MV2_BLOCK_X + 2 * MV2_RADIUS; /* 20 */
    constexpr int MV2_TILE_H    = MV2_BLOCK_Y + 2 * MV2_RADIUS; /* 20 */
    constexpr int MV2_TILE_PITCH = MV2_TILE_W + 1;              /* 21 */

    const int width  = (int)dim.x;
    const int height = (int)dim.y;
    const int prev_stride = (int)strides.x; /* bytes */
    const int cur_stride  = (int)strides.y; /* bytes */
    const int bpc         = (int)strides.z;

    const int shift_y = bpc;
    const int round_y = 1 << (bpc - 1);
    const int shift_x = 16;
    const int round_x = 1 << 15;

    const int tile_origin_x = (int)bid.x * MV2_BLOCK_X - MV2_RADIUS;
    const int tile_origin_y = (int)bid.y * MV2_BLOCK_Y - MV2_RADIUS;
    const int tile_elems    = MV2_TILE_W * MV2_TILE_H;
    const int wg_size       = MV2_BLOCK_X * MV2_BLOCK_Y;

    for (int i = (int)lid; i < tile_elems; i += wg_size) {
        const int ty = i / MV2_TILE_W;
        const int tx = i % MV2_TILE_W;
        const int gx = mv2_mirror(tile_origin_x + tx, width);
        const int gy = mv2_mirror(tile_origin_y + ty, height);
        const device ushort *prev_row =
            (const device ushort *)(prev + gy * prev_stride);
        const device ushort *cur_row  =
            (const device ushort *)(cur  + gy * cur_stride);
        const int p = (int)prev_row[gx];
        const int c = (int)cur_row [gx];
        s_diff[ty * MV2_TILE_PITCH + tx] = p - c;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    uint abs_h = 0;
    if ((int)gid.x < width && (int)gid.y < height) {
        const int lx = (int)tid.x + MV2_RADIUS;
        const int ly = (int)tid.y + MV2_RADIUS;

        long blurred = 0;
        for (int xf = 0; xf < 5; ++xf) {
            long blurred_y = 0;
            for (int yf = 0; yf < 5; ++yf) {
                blurred_y += (long)MV2_FILTER[yf] *
                             (long)s_diff[(ly - MV2_RADIUS + yf) * MV2_TILE_PITCH +
                                          (lx - MV2_RADIUS + xf)];
            }
            const int v = (int)((blurred_y + (long)round_y) >> shift_y);
            blurred += (long)MV2_FILTER[xf] * (long)v;
        }
        const long h = (blurred + (long)round_x) >> shift_x;
        abs_h = (uint)(h < 0 ? -h : h);
    }

    threadgroup uint simd_partials[8];
    const uint lane_sum = simd_sum(abs_h);
    if (simd_lane == 0) {
        simd_partials[simd_id] = lane_sum;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        uint group_sum = 0;
        for (uint i = 0; i < simd_count; ++i) {
            group_sum += simd_partials[i];
        }
        partials[bid.y * grid_groups.x + bid.x] = group_sum;
    }
}
