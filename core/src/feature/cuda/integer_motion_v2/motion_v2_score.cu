/**
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

extern "C" {

__launch_bounds__(MV2_BLOCK_X *MV2_BLOCK_Y, 8) __global__
    void motion_v2_kernel_8bpc(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur,
                               ptrdiff_t prev_stride, ptrdiff_t cur_stride, VmafCudaBuffer sad,
                               unsigned width, unsigned height)
{
    /* Shared tile holds the signed diff (prev - cur) so the nested
     * separable filter operates on a single dataset. Inner dim is
     * MV2_TILE_PITCH (= MV2_TILE_W + 1) for bank-conflict padding. */
    __shared__ int32_t s_diff[MV2_TILE_H][MV2_TILE_PITCH];

    constexpr int shift_y = 8;
    constexpr int round_y = 1 << 7;
    constexpr int shift_x = 16;
    constexpr int round_x = 1 << 15;

    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    const int tile_origin_x = blockIdx.x * MV2_BLOCK_X - MV2_RADIUS;
    const int tile_origin_y = blockIdx.y * MV2_BLOCK_Y - MV2_RADIUS;
    const unsigned tile_elems = MV2_TILE_W * MV2_TILE_H;
    const unsigned wg_size = MV2_BLOCK_X * MV2_BLOCK_Y;

    for (unsigned i = lid; i < tile_elems; i += wg_size) {
        const unsigned ty = i / MV2_TILE_W;
        const unsigned tx = i % MV2_TILE_W;
        const int gx = mv2_mirror(tile_origin_x + (int)tx, (int)width);
        const int gy = mv2_mirror(tile_origin_y + (int)ty, (int)height);
        const int p = (int)prev[gy * prev_stride + gx];
        const int c = (int)cur[gy * cur_stride + gx];
        s_diff[ty][tx] = p - c;
    }
    __syncthreads();

    int64_t abs_h = 0;
    if (x < (int)width && y < (int)height) {
        const unsigned lx = threadIdx.x + MV2_RADIUS;
        const unsigned ly = threadIdx.y + MV2_RADIUS;

        int64_t blurred = 0;
#pragma unroll
        for (int xf = 0; xf < 5; ++xf) {
            int32_t blurred_y = 0;
#pragma unroll
            for (int yf = 0; yf < 5; ++yf) {
                blurred_y += mv2_filter_d[yf] * s_diff[ly - MV2_RADIUS + yf][lx - MV2_RADIUS + xf];
            }
            const int32_t v = (blurred_y + round_y) >> shift_y;
            blurred += (int64_t)mv2_filter_d[xf] * (int64_t)v;
        }
        const int64_t h = (blurred + round_x) >> shift_x;
        abs_h = h < 0 ? -h : h;
    }

    /* Warp reduction (lane 0 carries the warp's partial). */
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 16);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 8);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 4);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 2);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 1);
    const int lane = (threadIdx.y * blockDim.x + threadIdx.x) & 31;
    if (lane == 0) {
        atomicAdd(reinterpret_cast<unsigned long long *>(sad.data),
                  static_cast<unsigned long long>(abs_h));
    }
}

__launch_bounds__(MV2_BLOCK_X *MV2_BLOCK_Y, 8) __global__
    void motion_v2_kernel_16bpc(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur,
                                ptrdiff_t prev_stride, ptrdiff_t cur_stride, VmafCudaBuffer sad,
                                unsigned width, unsigned height, unsigned bpc)
{
    /* Inner dim is MV2_TILE_PITCH for bank-conflict padding (see
     * comment in the 8bpc kernel above). */
    __shared__ int32_t s_diff[MV2_TILE_H][MV2_TILE_PITCH];

    const int shift_y = (int)bpc;
    const int round_y = 1 << ((int)bpc - 1);
    constexpr int shift_x = 16;
    constexpr int round_x = 1 << 15;

    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    const int tile_origin_x = blockIdx.x * MV2_BLOCK_X - MV2_RADIUS;
    const int tile_origin_y = blockIdx.y * MV2_BLOCK_Y - MV2_RADIUS;
    const unsigned tile_elems = MV2_TILE_W * MV2_TILE_H;
    const unsigned wg_size = MV2_BLOCK_X * MV2_BLOCK_Y;

    for (unsigned i = lid; i < tile_elems; i += wg_size) {
        const unsigned ty = i / MV2_TILE_W;
        const unsigned tx = i % MV2_TILE_W;
        const int gx = mv2_mirror(tile_origin_x + (int)tx, (int)width);
        const int gy = mv2_mirror(tile_origin_y + (int)ty, (int)height);
        const int p = (int)reinterpret_cast<const uint16_t *>(prev + gy * prev_stride)[gx];
        const int c = (int)reinterpret_cast<const uint16_t *>(cur + gy * cur_stride)[gx];
        s_diff[ty][tx] = p - c;
    }
    __syncthreads();

    int64_t abs_h = 0;
    if (x < (int)width && y < (int)height) {
        const unsigned lx = threadIdx.x + MV2_RADIUS;
        const unsigned ly = threadIdx.y + MV2_RADIUS;

        int64_t blurred = 0;
#pragma unroll
        for (int xf = 0; xf < 5; ++xf) {
            /* For bpc=16 the per-tap product can reach 26386 * 65535
             * ≈ 1.7e9 and the 5-tap sum overflows int32 — int64 in
             * the inner accumulator. */
            int64_t blurred_y = 0;
#pragma unroll
            for (int yf = 0; yf < 5; ++yf) {
                blurred_y += (int64_t)mv2_filter_d[yf] *
                             (int64_t)s_diff[ly - MV2_RADIUS + yf][lx - MV2_RADIUS + xf];
            }
            const int32_t v = (int32_t)((blurred_y + (int64_t)round_y) >> shift_y);
            blurred += (int64_t)mv2_filter_d[xf] * (int64_t)v;
        }
        const int64_t h = (blurred + (int64_t)round_x) >> shift_x;
        abs_h = h < 0 ? -h : h;
    }

    abs_h += __shfl_down_sync(0xffffffff, abs_h, 16);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 8);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 4);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 2);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 1);
    const int lane = (threadIdx.y * blockDim.x + threadIdx.x) & 31;
    if (lane == 0) {
        atomicAdd(reinterpret_cast<unsigned long long *>(sad.data),
                  static_cast<unsigned long long>(abs_h));
    }
}

} /* extern "C" */
