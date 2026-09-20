/**
 *
 *  Copyright 2016-2023 Netflix, Inc.
 *  Copyright 2021 NVIDIA Corporation.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#include "cuda_helper.cuh"
#include "cuda/integer_motion_cuda.h"

#include "common.h"

__constant__ uint16_t filter_d[5] = {3571, 16004, 26386, 16004, 3571};
__constant__ int filter_width_d = sizeof(filter_d) / sizeof(filter_d[0]);

// 5-tap filter radius
#define RADIUS 2
// Block dimensions (must match host-side launch config: 16x16)
#define BLOCK_X 16
#define BLOCK_Y 16
// Shared memory tile dimensions: block size + 2*RADIUS halo
#define TILE_W (BLOCK_X + 2 * RADIUS) // 20
#define TILE_H (BLOCK_Y + 2 * RADIUS) // 20
// Stride pads TILE_W to break shared-memory bank conflicts: with
// TILE_W=20 and 32-bank shared memory, GCD(20, 32) = 4 forces
// rows to alias on a 4-bank-cycle, producing 2-way conflicts
// between (y=1, x=12..15) and (y=0, x=0..3). Padding to 21
// (GCD(21, 32) = 1) eliminates them at a cost of 64 extra
// uint32_t per block (1764 vs 1600 bytes — well under the
// 48 KB SM limit). Indexing must use TILE_PITCH for x, NOT
// TILE_W (cuda-reviewer 2026-05-09).
#define TILE_PITCH (TILE_W + 1) // 21

// Device function that mirrors an idx along its valid [0,sup) range.
// Skip-boundary convention matches CPU integer_motion's edge_8 / edge_16:
// idx=-1   -> 1   (skip row 0 in the reflection)
// idx=-2   -> 2
// idx=sup  -> sup-2  (skip row sup-1; the +2 below, NOT +1, is what
//                     enforces the skip semantics)
// idx=sup+1 -> sup-3
// The previous +1 reflection (idx=sup -> sup-1) repeated the boundary
// row instead of skipping it, producing a systematic ~2.6e-3 motion
// drift vs CPU on every frame after the first (T7-15). Fixed in PR #120.
__device__ __forceinline__ int mirror(const int idx, const int sup)
{
    int out = abs(idx);
    return (out < sup) ? out : (sup - (out - sup + 2));
}

__device__ __forceinline__ void load_motion_tile_8bpc(uint32_t tile[][TILE_PITCH],
                                                      const VmafPicture src, unsigned width,
                                                      unsigned height, unsigned lid)
{
    const int origin_x = blockIdx.x * BLOCK_X - RADIUS;
    const int origin_y = blockIdx.y * BLOCK_Y - RADIUS;
    for (unsigned i = lid; i < TILE_W * TILE_H; i += BLOCK_X * BLOCK_Y) {
        const unsigned ty = i / TILE_W;
        const unsigned tx = i % TILE_W;
        const int gx = mirror(origin_x + (int)tx, (int)width);
        const int gy = mirror(origin_y + (int)ty, (int)height);
        tile[ty][tx] = (reinterpret_cast<const uint8_t *>(src.data[0]) + gy * src.stride[0])[gx];
    }
}

__device__ __forceinline__ void load_motion_tile_16bpc(uint32_t tile[][TILE_PITCH],
                                                       const VmafPicture src, unsigned width,
                                                       unsigned height, unsigned lid)
{
    const int origin_x = blockIdx.x * BLOCK_X - RADIUS;
    const int origin_y = blockIdx.y * BLOCK_Y - RADIUS;
    for (unsigned i = lid; i < TILE_W * TILE_H; i += BLOCK_X * BLOCK_Y) {
        const unsigned ty = i / TILE_W;
        const unsigned tx = i % TILE_W;
        const int gx = mirror(origin_x + (int)tx, (int)width);
        const int gy = mirror(origin_y + (int)ty, (int)height);
        tile[ty][tx] = reinterpret_cast<const uint16_t *>(
            reinterpret_cast<const uint8_t *>(src.data[0]) + gy * src.stride[0])[gx];
    }
}

__device__ __forceinline__ uint32_t blur_motion_tile(const uint32_t tile[][TILE_PITCH],
                                                     unsigned shift_y, unsigned round_y)
{
    const unsigned lx = threadIdx.x + RADIUS;
    const unsigned ly = threadIdx.y + RADIUS;
    uint32_t blurred = 0u;
#pragma unroll
    for (int xf = 0; xf < filter_width_d; ++xf) {
        uint32_t blurred_y = 0u;
#pragma unroll
        for (int yf = 0; yf < filter_width_d; ++yf)
            blurred_y += filter_d[yf] * tile[ly - RADIUS + yf][lx - RADIUS + xf];
        blurred += filter_d[xf] * ((blurred_y + round_y) >> shift_y);
    }
    return (blurred + 32768u) >> 16u;
}

__device__ __forceinline__ void accumulate_motion_sad(uint32_t abs_dist, VmafCudaBuffer sad)
{
    abs_dist += __shfl_down_sync(0xffffffff, abs_dist, 16);
    abs_dist += __shfl_down_sync(0xffffffff, abs_dist, 8);
    abs_dist += __shfl_down_sync(0xffffffff, abs_dist, 4);
    abs_dist += __shfl_down_sync(0xffffffff, abs_dist, 2);
    abs_dist += __shfl_down_sync(0xffffffff, abs_dist, 1);
    const int lane = (threadIdx.y * blockDim.x + threadIdx.x) % 32;
    if (lane == 0)
        atomicAdd(reinterpret_cast<unsigned long long *>(sad.data),
                  static_cast<unsigned long long>(abs_dist));
}

extern "C" __launch_bounds__(BLOCK_X *BLOCK_Y, 8) __global__
    void calculate_motion_score_kernel_8bpc(const VmafPicture src, VmafCudaBuffer src_blurred,
                                            const VmafCudaBuffer prev_blurred, VmafCudaBuffer sad,
                                            unsigned width, unsigned height, ptrdiff_t src_stride,
                                            ptrdiff_t blurred_stride)
{
    __shared__ uint32_t s_tile[TILE_H][TILE_PITCH];
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    load_motion_tile_8bpc(s_tile, src, width, height, threadIdx.y * blockDim.x + threadIdx.x);
    __syncthreads();

    uint32_t abs_dist = 0u;
    if (x < (int)width && y < (int)height) {
        const uint32_t blurred = blur_motion_tile(s_tile, 8u, 128u);
        reinterpret_cast<uint16_t *>(src_blurred.data + y * blurred_stride)[x] =
            static_cast<uint16_t>(blurred);
        abs_dist = abs(static_cast<int>(blurred) - static_cast<int>(reinterpret_cast<uint16_t *>(
                                                       prev_blurred.data + y * blurred_stride)[x]));
    }
    accumulate_motion_sad(abs_dist, sad);
}

extern "C" __launch_bounds__(BLOCK_X *BLOCK_Y, 8) __global__
    void calculate_motion_score_kernel_16bpc(const VmafPicture src, VmafCudaBuffer src_blurred,
                                             const VmafCudaBuffer prev_blurred, VmafCudaBuffer sad,
                                             unsigned width, unsigned height, ptrdiff_t src_stride,
                                             ptrdiff_t blurred_stride)
{
    __shared__ uint32_t s_tile[TILE_H][TILE_PITCH];
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    load_motion_tile_16bpc(s_tile, src, width, height, threadIdx.y * blockDim.x + threadIdx.x);
    __syncthreads();

    uint32_t abs_dist = 0u;
    if (x < (int)width && y < (int)height) {
        const uint32_t blurred = blur_motion_tile(s_tile, src.bpc, 1u << (src.bpc - 1));
        reinterpret_cast<uint16_t *>(src_blurred.data + y * blurred_stride)[x] =
            static_cast<uint16_t>(blurred);
        abs_dist = abs(static_cast<int>(blurred) - static_cast<int>(reinterpret_cast<uint16_t *>(
                                                       prev_blurred.data + y * blurred_stride)[x]));
    }
    accumulate_motion_sad(abs_dist, sad);
}
