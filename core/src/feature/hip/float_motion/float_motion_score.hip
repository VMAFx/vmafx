/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  HIP compute kernels for the float_motion feature extractor
 *  (T7-10b batch-2 / ADR-0373). Direct port of the CUDA twin at
 *  `libvmaf/src/feature/cuda/float_motion/float_motion_score.cu`.
 *
 *  Algorithm (must match CPU float_motion):
 *    1. Convert ref pixel: `val = (raw / scaler) - 128.0`.
 *    2. 5x5 separable Gaussian blur with FM_FILT (sums to ~1.0).
 *    3. Frame > 0: SAD += |cur_blurred - prev_blurred|.
 *    Mirror padding: skip-boundary `2 * (sup - 1) - idx`.
 *
 *  Layout: 16x16 work-group with 2-pixel halo (5-tap), produces a float
 *  per-pixel blurred output and one float per-block SAD partial.
 *  Host accumulates partials in `double` and divides by (w*h).
 *
 *  HIP adaptation from CUDA:
 *  - GCN/RDNA1 warp size is 64; RDNA2+ (gfx1030/gfx1100/gfx1101) is 32.
 *    The shared-memory array for warp partial sums is sized for the minimum
 *    warp size (32) so it is large enough on wave32: FM_BX*FM_BY/32 = 8 slots.
 *    At runtime `warpSize` is used for lane/warp_id and loop bounds.
 *  - `__shfl_down` used without mask (HIP convention; CUDA uses
 *    `__shfl_down_sync(0xffffffff, ...)`).
 *  - `VmafCudaBuffer sad` replaced by plain `float *partials` since the
 *    HIP host passes `rb.device` as a raw pointer.
 *  - Entry-point names are `float_motion_hip_kernel_{8,16}bpc` (not
 *    `float_motion_kernel_{8,16}bpc`) so the Module API lookup in
 *    `float_motion_hip.c:fm_hip_module_load()` resolves correctly.
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#define FM_BX 16
#define FM_BY 16
#define FM_RADIUS 2
#define FM_TILE_W (FM_BX + 2 * FM_RADIUS)
#define FM_TILE_H (FM_BY + 2 * FM_RADIUS)

/* Minimum HIP warp size across supported AMD targets (wave32 = RDNA2+).
 * Array sized for the minimum so it is large enough on wave32 hardware.
 * Runtime `warpSize` is used for the reduction loop and lane/warp_id. */
#define FM_MIN_WARP_SIZE   32
#define FM_WARPS_PER_BLOCK (FM_BX * FM_BY / FM_MIN_WARP_SIZE)

/* 5-tap Gaussian filter (same constants as the CUDA twin). */
__device__ static const float FM_FILT[5] = {
    0.054488685f, 0.244201342f, 0.402619947f, 0.244201342f, 0.054488685f,
};

/* Skip-boundary mirror — matches the CUDA twin's `fm_mirror`. */
__device__ __forceinline__ int fm_mirror(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * (sup - 1) - idx;
    return idx;
}

/* HIP warp-level float reduction — uses warpSize at runtime to handle
 * both wave64 (GCN/RDNA1, warpSize=64) and wave32 (RDNA2+, warpSize=32). */
__device__ __forceinline__ float fm_warp_reduce(float v)
{
    for (int off = (int)warpSize / 2; off > 0; off >>= 1)
        v += __shfl_down(v, off);
    return v;
}

extern "C" {

/**
 * 8-bpc kernel: ref is uint8_t row-major with ref_stride bytes per row.
 *
 * Computes per-pixel 5x5 separable Gaussian blur of the ref luma plane
 * (written to `cur_blur`) and, when `compute_sad != 0`, accumulates
 * per-pixel `|cur_blur - prev_blur|` partial sums into one float per
 * block in `partials[block_idx]`.
 */
__global__ void float_motion_hip_kernel_8bpc(const uint8_t *__restrict__ ref,
                                              ptrdiff_t ref_stride,
                                              float *__restrict__ cur_blur,
                                              const float *__restrict__ prev_blur,
                                              float *__restrict__ partials,
                                              unsigned width,
                                              unsigned height,
                                              unsigned compute_sad)
{
    __shared__ float s_tile[FM_TILE_H][FM_TILE_W];

    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    const int tile_ox = blockIdx.x * FM_BX - FM_RADIUS;
    const int tile_oy = blockIdx.y * FM_BY - FM_RADIUS;
    const unsigned tile_elems = FM_TILE_W * FM_TILE_H;
    const unsigned wg_size = FM_BX * FM_BY;

    /* Collaborative tile load with mirror padding. */
    for (unsigned i = lid; i < tile_elems; i += wg_size) {
        const unsigned tr = i / FM_TILE_W;
        const unsigned tc = i % FM_TILE_W;
        const int gx = fm_mirror(tile_ox + (int)tc, (int)width);
        const int gy = fm_mirror(tile_oy + (int)tr, (int)height);
        s_tile[tr][tc] = (float)ref[gy * ref_stride + gx] - 128.0f;
    }
    __syncthreads();

    float abs_diff = 0.0f;
    if (x < (int)width && y < (int)height) {
        const unsigned lx = threadIdx.x + FM_RADIUS;
        const unsigned ly = threadIdx.y + FM_RADIUS;

        /* 5x5 separable Gaussian blur (column-then-row). */
        float blurred = 0.0f;
#pragma unroll
        for (int xf = 0; xf < 5; xf++) {
            float v = 0.0f;
#pragma unroll
            for (int yf = 0; yf < 5; yf++)
                v += FM_FILT[yf] * s_tile[ly - FM_RADIUS + yf][lx - FM_RADIUS + xf];
            blurred += FM_FILT[xf] * v;
        }
        const size_t off = (size_t)y * width + (size_t)x;
        cur_blur[off] = blurred;

        if (compute_sad != 0u) {
            const float prev = prev_blur[off];
            const float diff = blurred - prev;
            abs_diff = diff < 0.0f ? -diff : diff;
        }
    }

    if (compute_sad != 0u) {
        /* Warp reduce, then accumulate warp sums in shared memory. */
        __shared__ float s_warp[FM_WARPS_PER_BLOCK];
        const float w = fm_warp_reduce(abs_diff);
        const unsigned ws      = (unsigned)warpSize;
        const unsigned lane    = lid % ws;
        const unsigned warp_id = lid / ws;
        if (lane == 0)
            s_warp[warp_id] = w;
        __syncthreads();
        if (lid == 0) {
            float total = 0.0f;
            const unsigned warps_active = (FM_BX * FM_BY) / ws;
            for (unsigned i = 0; i < warps_active; i++)
                total += s_warp[i];
            const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
            partials[block_idx] = total;
        }
    } else {
        if (lid == 0) {
            const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
            partials[block_idx] = 0.0f;
        }
    }
}

/**
 * 10/12/16-bpc kernel: ref is byte-addressed; each pixel is uint16_t.
 * Scale factors: 10bpc=4, 12bpc=16, 16bpc=256.
 * All other algorithm details are identical to the 8-bpc kernel.
 */
__global__ void float_motion_hip_kernel_16bpc(const uint8_t *__restrict__ ref,
                                               ptrdiff_t ref_stride,
                                               float *__restrict__ cur_blur,
                                               const float *__restrict__ prev_blur,
                                               float *__restrict__ partials,
                                               unsigned width,
                                               unsigned height,
                                               unsigned bpc,
                                               unsigned compute_sad)
{
    __shared__ float s_tile[FM_TILE_H][FM_TILE_W];

    float scaler = 1.0f;
    if (bpc == 10u)
        scaler = 4.0f;
    else if (bpc == 12u)
        scaler = 16.0f;
    else if (bpc == 16u)
        scaler = 256.0f;
    const float inv_scaler = 1.0f / scaler;

    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    const int tile_ox = blockIdx.x * FM_BX - FM_RADIUS;
    const int tile_oy = blockIdx.y * FM_BY - FM_RADIUS;
    const unsigned tile_elems = FM_TILE_W * FM_TILE_H;
    const unsigned wg_size = FM_BX * FM_BY;

    for (unsigned i = lid; i < tile_elems; i += wg_size) {
        const unsigned tr = i / FM_TILE_W;
        const unsigned tc = i % FM_TILE_W;
        const int gx = fm_mirror(tile_ox + (int)tc, (int)width);
        const int gy = fm_mirror(tile_oy + (int)tr, (int)height);
        const uint16_t r = reinterpret_cast<const uint16_t *>(ref + gy * ref_stride)[gx];
        s_tile[tr][tc] = (float)r * inv_scaler - 128.0f;
    }
    __syncthreads();

    float abs_diff = 0.0f;
    if (x < (int)width && y < (int)height) {
        const unsigned lx = threadIdx.x + FM_RADIUS;
        const unsigned ly = threadIdx.y + FM_RADIUS;

        float blurred = 0.0f;
#pragma unroll
        for (int xf = 0; xf < 5; xf++) {
            float v = 0.0f;
#pragma unroll
            for (int yf = 0; yf < 5; yf++)
                v += FM_FILT[yf] * s_tile[ly - FM_RADIUS + yf][lx - FM_RADIUS + xf];
            blurred += FM_FILT[xf] * v;
        }
        const size_t off = (size_t)y * width + (size_t)x;
        cur_blur[off] = blurred;

        if (compute_sad != 0u) {
            const float prev = prev_blur[off];
            const float diff = blurred - prev;
            abs_diff = diff < 0.0f ? -diff : diff;
        }
    }

    if (compute_sad != 0u) {
        __shared__ float s_warp[FM_WARPS_PER_BLOCK];
        const float w = fm_warp_reduce(abs_diff);
        const unsigned ws      = (unsigned)warpSize;
        const unsigned lane    = lid % ws;
        const unsigned warp_id = lid / ws;
        if (lane == 0)
            s_warp[warp_id] = w;
        __syncthreads();
        if (lid == 0) {
            float total = 0.0f;
            const unsigned warps_active = (FM_BX * FM_BY) / ws;
            for (unsigned i = 0; i < warps_active; i++)
                total += s_warp[i];
            const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
            partials[block_idx] = total;
        }
    } else {
        if (lid == 0) {
            const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
            partials[block_idx] = 0.0f;
        }
    }
}

} /* extern "C" */
