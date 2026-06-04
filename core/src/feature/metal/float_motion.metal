/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  Metal compute kernel for float_motion (T8-1h / ADR-0421).
 *  Mirrors `libvmaf/src/feature/vulkan/shaders/float_motion.comp`.
 *
 *  Algorithm (must match CPU libvmaf/src/feature/motion_tools.c,
 *  FILTER_5_s path):
 *    1. Convert: val = (raw / scaler) - 128.0
 *       scaler: 1 for bpc=8, 4/16/256 for 10/12/16bpc.
 *    2. 5-tap Gaussian vertical pass → s_vert[TILE_W * WG_H].
 *    3. 5-tap Gaussian horizontal pass → blurred[x] (per-thread).
 *    4. Write blurred pixel to `cur_blurred[y*stride + x]`.
 *    5. If `compute_sad`: SAD += |blurred - prev_blurred[y*stride + x]|.
 *    6. Host: score = sad_sum / (W * H).
 *
 *  Mirror padding: SKIP-BOUNDARY reflective (2*(sup-1) - idx for idx >= sup).
 *  This DIVERGES from motion_v2's edge-replicating form — matches the CPU
 *  reference's `convolution_f32_c_s` path.
 *
 *  Reduction: per-WG float SAD partial. `compute_sad` is passed via push
 *  constant (uint2 dim = {width, height} for first dispatch; set
 *  compute_sad separately via strides.z).
 *
 *  Buffer bindings:
 *   [[buffer(0)]] ref          — const uchar *  (current frame)
 *   [[buffer(1)]] prev_blurred — float *         (previous blurred frame)
 *   [[buffer(2)]] cur_blurred  — float *         (output: current blurred)
 *   [[buffer(3)]] sad_parts    — float *         (grid_w × grid_h, 0 if frame 0)
 *   [[buffer(4)]] strides      — uint4 (.x=ref_stride_bytes, .y=blur_stride_floats,
 *                                        .z=bpc, .w=compute_sad 0/1)
 *   [[buffer(5)]] dim          — uint2 (width, height)
 */

#include <metal_stdlib>
using namespace metal;

constant float FILT[5] = {
    0.054488685f, 0.244201342f, 0.402619947f, 0.244201342f, 0.054488685f
};

#define HALF_FW 2
#define TILE_W  20   /* 16 + 2*HALF_FW */
#define TILE_H  20   /* 16 + 2*HALF_FW: vertical halo required for 5-tap filter */
/* s_vert: TILE_H × TILE_W. */
#define TILE_PITCH_V 20

static inline int skip_mirror(int idx, int sup) {
    if (idx < 0) { return -idx; }
    if (idx >= sup) { return 2 * (sup - 1) - idx; }
    return idx;
}

/* ------------------------------------------------------------------ */
/*  8 bpc kernel                                                        */
/* ------------------------------------------------------------------ */
kernel void float_motion_kernel_8bpc(
    const device uchar  *ref          [[buffer(0)]],
    const device float  *prev_blurred [[buffer(1)]],
    device       float  *cur_blurred  [[buffer(2)]],
    device       float  *sad_parts    [[buffer(3)]],
    constant     uint4  &strides      [[buffer(4)]],
    constant     uint2  &dim          [[buffer(5)]],
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
    const int compute_sad = (int)strides.w;

    /* --- Phase 1: load 20×20 ref tile into shared (halo on x and y). */
    threadgroup float s_tile[TILE_H * TILE_PITCH_V];
    {
        const int wg_ox = (int)bid.x * 16 - HALF_FW;
        const int wg_oy = (int)bid.y * 16 - HALF_FW;  /* 2-row halo for 5-tap vertical filter */
        const int n_elems = TILE_W * TILE_H;
        const int wg_size = 16 * 16;
        for (int i = (int)lid; i < n_elems; i += wg_size) {
            const int ty = i / TILE_W;
            const int tx = i % TILE_W;
            const int sy = skip_mirror(wg_oy + ty, height);
            const int sx = skip_mirror(wg_ox + tx, width);
            s_tile[ty * TILE_PITCH_V + tx] =
                (float)ref[sy * (int)strides.x + sx] - 128.0f;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* --- Phase 2: vertical filter (each thread computes its column). */
    threadgroup float s_vert[TILE_H * TILE_PITCH_V];
    {
        const int col = (int)lid2.x;  /* 0..15 */
        const int wg_oy = (int)bid.y * 16 - HALF_FW;  /* halo origin, same as tile load */
        /* Iterate over all TILE_H rows (output rows HALF_FW..TILE_H-1-HALF_FW are the
         * 16 WG rows needed for Phase 3; halo rows are consumed here but not written out). */
        for (int row = (int)lid2.y; row < TILE_H; row += 16) {
            const int tile_col = col + HALF_FW;  /* offset into tile */
            float acc = 0.0f;
            for (int k = 0; k < 5; ++k) {
                const int src_row = skip_mirror(wg_oy + row + (k - 2), height);
                /* src_row is absolute; map back to tile-relative (wg_oy is halo origin). */
                const int tile_row = src_row - wg_oy;
                /* Clamp to tile bounds. */
                const int tr = max(0, min(tile_row, TILE_H - 1));
                acc += FILT[k] * s_tile[tr * TILE_PITCH_V + tile_col];
            }
            s_vert[row * TILE_PITCH_V + tile_col] = acc;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    /* --- Phase 3: horizontal filter, write blurred, accumulate SAD.
     * The 16 output rows within the WG tile correspond to s_vert rows
     * HALF_FW .. TILE_H-1-HALF_FW (== 2..17); lid2.y addresses these as
     * ty = lid2.y + HALF_FW so that the horizontal pass reads from the
     * vertically-filtered rows that align with gid.y. */
    float my_sad = 0.0f;
    if ((int)gid.x < width && (int)gid.y < height) {
        const int ty = (int)lid2.y + HALF_FW;
        const int tx = (int)lid2.x + HALF_FW;
        float blurred = 0.0f;
        for (int k = 0; k < 5; ++k) {
            blurred += FILT[k] * s_vert[ty * TILE_PITCH_V + (tx + k - 2)];
        }
        const int flat = (int)gid.y * (int)strides.y + (int)gid.x;
        cur_blurred[flat] = blurred;
        if (compute_sad != 0) {
            my_sad = abs(blurred - prev_blurred[flat]);
        }
    }

    threadgroup float sg_sad[8];
    const float lane_sum = simd_sum(my_sad);
    if (simd_lane == 0) { sg_sad[simd_id] = lane_sum; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        float gs = 0.0f;
        for (uint i = 0; i < simd_count; ++i) { gs += sg_sad[i]; }
        sad_parts[bid.y * grid_groups.x + bid.x] = gs;
    }
}

/* ------------------------------------------------------------------ */
/*  16 bpc kernel                                                       */
/* ------------------------------------------------------------------ */
kernel void float_motion_kernel_16bpc(
    const device uchar  *ref          [[buffer(0)]],
    const device float  *prev_blurred [[buffer(1)]],
    device       float  *cur_blurred  [[buffer(2)]],
    device       float  *sad_parts    [[buffer(3)]],
    constant     uint4  &strides      [[buffer(4)]],
    constant     uint2  &dim          [[buffer(5)]],
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
    const float scaler    = (float)(1u << ((uint)strides.z - 8u));
    const int compute_sad = (int)strides.w;

    threadgroup float s_tile[TILE_H * TILE_PITCH_V];
    {
        const int wg_ox = (int)bid.x * 16 - HALF_FW;
        const int wg_oy = (int)bid.y * 16 - HALF_FW;  /* 2-row halo for 5-tap vertical filter */
        const int n_elems = TILE_W * TILE_H;
        const int wg_size = 16 * 16;
        for (int i = (int)lid; i < n_elems; i += wg_size) {
            const int ty = i / TILE_W;
            const int tx = i % TILE_W;
            const int sy = skip_mirror(wg_oy + ty, height);
            const int sx = skip_mirror(wg_ox + tx, width);
            const device ushort *row_ptr =
                (const device ushort *)(ref + sy * (int)strides.x);
            s_tile[ty * TILE_PITCH_V + tx] = (float)row_ptr[sx] / scaler - 128.0f;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    threadgroup float s_vert[TILE_H * TILE_PITCH_V];
    {
        const int col = (int)lid2.x;
        const int wg_oy = (int)bid.y * 16 - HALF_FW;  /* halo origin, same as tile load */
        for (int row = (int)lid2.y; row < TILE_H; row += 16) {
            const int tile_col = col + HALF_FW;
            float acc = 0.0f;
            for (int k = 0; k < 5; ++k) {
                const int src_row = skip_mirror(wg_oy + row + (k - 2), height);
                const int tile_row = src_row - wg_oy;
                const int tr = max(0, min(tile_row, TILE_H - 1));
                acc += FILT[k] * s_tile[tr * TILE_PITCH_V + tile_col];
            }
            s_vert[row * TILE_PITCH_V + tile_col] = acc;
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    float my_sad = 0.0f;
    if ((int)gid.x < width && (int)gid.y < height) {
        const int ty = (int)lid2.y + HALF_FW;  /* offset by halo to reach WG output rows */
        const int tx = (int)lid2.x + HALF_FW;
        float blurred = 0.0f;
        for (int k = 0; k < 5; ++k) {
            blurred += FILT[k] * s_vert[ty * TILE_PITCH_V + (tx + k - 2)];
        }
        const int flat = (int)gid.y * (int)strides.y + (int)gid.x;
        cur_blurred[flat] = blurred;
        if (compute_sad != 0) {
            my_sad = abs(blurred - prev_blurred[flat]);
        }
    }

    threadgroup float sg_sad[8];
    const float lane_sum = simd_sum(my_sad);
    if (simd_lane == 0) { sg_sad[simd_id] = lane_sum; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (lid == 0) {
        float gs = 0.0f;
        for (uint i = 0; i < simd_count; ++i) { gs += sg_sad[i]; }
        sad_parts[bid.y * grid_groups.x + bid.x] = gs;
    }
}
