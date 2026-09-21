/**
 *  Copyright 2016-2025 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernel for the integer_motion_v2 feature extractor
 *  (T7-23 / batch 3 part 1b — ADR-0192 / ADR-0193). CUDA twin of the
 *  Vulkan kernel landed in PR #146.
 *
 *  Algorithm (must match CPU integer_motion_v2.c — exploits convolution
 *  linearity so we can compute the score in one dispatch over
 *  (prev_ref - cur_ref) without storing blurred frames across submits):
 *
 *    1. Per pixel: diff = prev[i,j] - cur[i,j]   (signed)
 *    2. Vertical filter on diff:
 *         v[i,j] = (sum_k filter[k] * diff[mirror(i-2+k), j]
 *                    + (1 << (bpc - 1))) >> bpc
 *    3. Horizontal filter on v:
 *         h[i,j] = (sum_k filter[k] * v[i, mirror(j-2+k)]
 *                    + 32768) >> 16
 *    4. SAD: atomic-add |h[i,j]| into a single int64 accumulator
 *
 *  Final score on host: motion_v2_sad_score = SAD / 256.0 / (W*H)
 *  motion2_v2_score is the host-side min(cur, next) post-process in
 *  flush(); no GPU work needed.
 *
 *  Mirror padding: reflect-101 (`2 * size - idx - 2` for idx >= size),
 *  matching CPU `integer_motion_v2.c::mirror` and motion's CUDA kernel.
 */

#include "cuda_helper.cuh"
#include "common.h"

__constant__ int32_t mv2_filter_d[5] = {3571, 16004, 26386, 16004, 3571};

#define MV2_RADIUS 2
#define MV2_BLOCK_X 16
#define MV2_BLOCK_Y 16
#define MV2_TILE_W (MV2_BLOCK_X + 2 * MV2_RADIUS) /* 20 */
#define MV2_TILE_H (MV2_BLOCK_Y + 2 * MV2_RADIUS) /* 20 */
/* Pad inner stride to break shared-memory bank conflicts: GCD(20, 32)
 * = 4 produces 2-way conflicts on neighbouring rows. GCD(21, 32) = 1
 * eliminates them. Cost is +64 int32 per block (~256 bytes), well
 * under the 48 KB SM limit (cuda-reviewer 2026-05-09). */
#define MV2_TILE_PITCH (MV2_TILE_W + 1) /* 21 */

__device__ __forceinline__ int mv2_mirror(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * sup - idx - 2;
    return idx;
}

template <typename Pixel>
__device__ __forceinline__ void
load_motion_v2_diff(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur,
                    ptrdiff_t prev_stride, ptrdiff_t cur_stride,
                    int32_t diff[MV2_TILE_H][MV2_TILE_PITCH], unsigned width, unsigned height)
{
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    const int origin_x = blockIdx.x * MV2_BLOCK_X - MV2_RADIUS;
    const int origin_y = blockIdx.y * MV2_BLOCK_Y - MV2_RADIUS;
    constexpr unsigned tile_elems = MV2_TILE_W * MV2_TILE_H;
    constexpr unsigned group_size = MV2_BLOCK_X * MV2_BLOCK_Y;
    for (unsigned i = lid; i < tile_elems; i += group_size) {
        const unsigned ty = i / MV2_TILE_W;
        const unsigned tx = i % MV2_TILE_W;
        const int gx = mv2_mirror(origin_x + static_cast<int>(tx), static_cast<int>(width));
        const int gy = mv2_mirror(origin_y + static_cast<int>(ty), static_cast<int>(height));
        const auto *prev_row = reinterpret_cast<const Pixel *>(prev + gy * prev_stride);
        const auto *cur_row = reinterpret_cast<const Pixel *>(cur + gy * cur_stride);
        diff[ty][tx] = static_cast<int>(prev_row[gx]) - static_cast<int>(cur_row[gx]);
    }
}

__device__ __forceinline__ int64_t
blur_motion_v2_8bpc(const int32_t diff[MV2_TILE_H][MV2_TILE_PITCH], unsigned lx, unsigned ly)
{
    int64_t blurred = 0;
#pragma unroll
    for (int xf = 0; xf < 5; ++xf) {
        int32_t blurred_y = 0;
#pragma unroll
        for (int yf = 0; yf < 5; ++yf)
            blurred_y += mv2_filter_d[yf] * diff[ly - MV2_RADIUS + yf][lx - MV2_RADIUS + xf];
        const int32_t vertical = (blurred_y + (1 << 7)) >> 8;
        blurred += (int64_t)mv2_filter_d[xf] * (int64_t)vertical;
    }
    const int64_t horizontal = (blurred + (1 << 15)) >> 16;
    return horizontal < 0 ? -horizontal : horizontal;
}

__device__ __forceinline__ int64_t blur_motion_v2_16bpc(
    const int32_t diff[MV2_TILE_H][MV2_TILE_PITCH], unsigned lx, unsigned ly, unsigned bpc)
{
    int64_t blurred = 0;
#pragma unroll
    for (int xf = 0; xf < 5; ++xf) {
        int64_t blurred_y = 0;
#pragma unroll
        for (int yf = 0; yf < 5; ++yf) {
            blurred_y += (int64_t)mv2_filter_d[yf] *
                         (int64_t)diff[ly - MV2_RADIUS + yf][lx - MV2_RADIUS + xf];
        }
        const int32_t vertical = (int32_t)((blurred_y + (1LL << (bpc - 1))) >> bpc);
        blurred += (int64_t)mv2_filter_d[xf] * (int64_t)vertical;
    }
    const int64_t horizontal = (blurred + (1 << 15)) >> 16;
    return horizontal < 0 ? -horizontal : horizontal;
}

__device__ __forceinline__ void accumulate_motion_v2_sad(VmafCudaBuffer sad, int64_t value)
{
    value += __shfl_down_sync(0xffffffff, value, 16);
    value += __shfl_down_sync(0xffffffff, value, 8);
    value += __shfl_down_sync(0xffffffff, value, 4);
    value += __shfl_down_sync(0xffffffff, value, 2);
    value += __shfl_down_sync(0xffffffff, value, 1);
    const int lane = (threadIdx.y * blockDim.x + threadIdx.x) & 31;
    if (lane == 0) {
        atomicAdd(reinterpret_cast<unsigned long long *>(sad.data),
                  static_cast<unsigned long long>(value));
    }
}

/* CUDA 13.4 rejects the old eight-block occupancy hint: 8 * 256 threads
 * exceeds the per-SM thread limit on every compiled target, so ptxas ignored
 * it. Keep the truthful maximum block size and let ptxas choose occupancy. */
extern "C" __launch_bounds__(MV2_BLOCK_X *MV2_BLOCK_Y) __global__
    void motion_v2_kernel_8bpc(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur,
                               ptrdiff_t prev_stride, ptrdiff_t cur_stride, VmafCudaBuffer sad,
                               unsigned width, unsigned height)
{
    __shared__ int32_t s_diff[MV2_TILE_H][MV2_TILE_PITCH];
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    load_motion_v2_diff<uint8_t>(prev, cur, prev_stride, cur_stride, s_diff, width, height);
    __syncthreads();

    int64_t abs_h = 0;
    if (x < (int)width && y < (int)height) {
        const unsigned lx = threadIdx.x + MV2_RADIUS;
        const unsigned ly = threadIdx.y + MV2_RADIUS;
        abs_h = blur_motion_v2_8bpc(s_diff, lx, ly);
    }
    accumulate_motion_v2_sad(sad, abs_h);
}

extern "C" __launch_bounds__(MV2_BLOCK_X *MV2_BLOCK_Y) __global__
    void motion_v2_kernel_16bpc(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur,
                                ptrdiff_t prev_stride, ptrdiff_t cur_stride, VmafCudaBuffer sad,
                                unsigned width, unsigned height, unsigned bpc)
{
    __shared__ int32_t s_diff[MV2_TILE_H][MV2_TILE_PITCH];
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    load_motion_v2_diff<uint16_t>(prev, cur, prev_stride, cur_stride, s_diff, width, height);
    __syncthreads();

    int64_t abs_h = 0;
    if (x < (int)width && y < (int)height) {
        const unsigned lx = threadIdx.x + MV2_RADIUS;
        const unsigned ly = threadIdx.y + MV2_RADIUS;
        abs_h = blur_motion_v2_16bpc(s_diff, lx, ly, bpc);
    }
    accumulate_motion_v2_sad(sad, abs_h);
}
