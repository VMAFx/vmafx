/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernel for the float_motion feature extractor
 *  (T7-23 / batch 3 part 4b — ADR-0192 / ADR-0196). CUDA twin of
 *  float_motion_vulkan. Float-domain blur + SAD.
 *
 *  Algorithm (must match CPU float_motion):
 *   1. Convert ref pixel: `val = (raw / scaler) - 128.0`.
 *   2. 5x5 separable Gaussian blur with FILTER_5_s (sums to ~1.0).
 *   3. Frame > 0: SAD += |b - prev_b|.
 *   Mirror padding: skip-boundary `2 * (sup - 1) - idx`.
 *
 *  Layout: 16x16 WG with 2-pixel halo (5-tap), produces float
 *  per-pixel blurred output and float per-block SAD partial.
 *  Host accumulates partials in `double` and divides by (w*h).
 */

#include "cuda_helper.cuh"
#include "common.h"

#define FM_BX 16
#define FM_BY 16
#define FM_RADIUS 2
#define FM_TILE_W (FM_BX + 2 * FM_RADIUS)
#define FM_TILE_H (FM_BY + 2 * FM_RADIUS)

__device__ static const float FM_FILT[5] = {
    0.054488685f, 0.244201342f, 0.402619947f, 0.244201342f, 0.054488685f,
};

__device__ __forceinline__ int fm_mirror(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * (sup - 1) - idx;
    return idx;
}

__device__ __forceinline__ float fm_warp_reduce(float v)
{
    for (int off = 16; off > 0; off >>= 1)
        v += __shfl_down_sync(0xffffffff, v, off);
    return v;
}

extern "C" {

__global__ void float_motion_kernel_8bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                         float *__restrict__ cur_blur,
                                         const float *__restrict__ prev_blur, VmafCudaBuffer sad,
                                         unsigned width, unsigned height, unsigned compute_sad)
{
    __shared__ float s_tile[FM_TILE_H][FM_TILE_W];

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
        s_tile[tr][tc] = (float)ref[gy * ref_stride + gx] - 128.0f;
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
            float prev = prev_blur[off];
            float diff = blurred - prev;
            abs_diff = diff < 0.0f ? -diff : diff;
        }
    }

    if (compute_sad != 0u) {
        __shared__ float s_warp[FM_BX * FM_BY / 32];
        float w = fm_warp_reduce(abs_diff);
        const int lane = lid % 32;
        const int warp_id = lid / 32;
        if (lane == 0)
            s_warp[warp_id] = w;
        __syncthreads();
        if (lid == 0) {
            float total = 0.0f;
#pragma unroll
            for (int i = 0; i < FM_BX * FM_BY / 32; i++)
                total += s_warp[i];
            const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
            reinterpret_cast<float *>(sad.data)[block_idx] = total;
        }
    } else {
        if (lid == 0) {
            const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
            reinterpret_cast<float *>(sad.data)[block_idx] = 0.0f;
        }
    }
}

__global__ void float_motion_kernel_16bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                          float *__restrict__ cur_blur,
                                          const float *__restrict__ prev_blur, VmafCudaBuffer sad,
                                          unsigned width, unsigned height, unsigned bpc,
                                          unsigned compute_sad)
{
    __shared__ float s_tile[FM_TILE_H][FM_TILE_W];

    float scaler = 1.0f;
    if (bpc == 10)
        scaler = 4.0f;
    else if (bpc == 12)
        scaler = 16.0f;
    else if (bpc == 16)
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
            float prev = prev_blur[off];
            float diff = blurred - prev;
            abs_diff = diff < 0.0f ? -diff : diff;
        }
    }

    if (compute_sad != 0u) {
        __shared__ float s_warp[FM_BX * FM_BY / 32];
        float w = fm_warp_reduce(abs_diff);
        const int lane = lid % 32;
        const int warp_id = lid / 32;
        if (lane == 0)
            s_warp[warp_id] = w;
        __syncthreads();
        if (lid == 0) {
            float total = 0.0f;
#pragma unroll
            for (int i = 0; i < FM_BX * FM_BY / 32; i++)
                total += s_warp[i];
            const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
            reinterpret_cast<float *>(sad.data)[block_idx] = total;
        }
    } else {
        if (lid == 0) {
            const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
            reinterpret_cast<float *>(sad.data)[block_idx] = 0.0f;
        }
    }
}

} /* extern "C" */
