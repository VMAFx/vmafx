/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  The motion SAD kernel of the CUDA backend, shared by motion_cuda and
 *  motion_v2_cuda through integer_motion_sad_cuda.c (ADR-1372; the kernel
 *  was motion_v2's alone until motion_cuda moved to the CPU's order,
 *  T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29).
 *
 *  Arithmetic (must match CPU integer_motion.c / integer_motion_v2.c,
 *  motion_score_pipeline_8 / _16):
 *
 *    1. Per pixel: diff = prev[i,j] - cur[i,j]   (signed)
 *    2. Vertical filter on diff:
 *         v[i,j] = (sum_k filter[k] * diff[mirror(i-2+k), j]
 *                    + (1 << (bpc - 1))) >> bpc
 *    3. Horizontal filter on v:
 *         h[i,j] = (sum_k filter[k] * v[i, mirror(j-2+k)]
 *                    + 32768) >> 16
 *    4. SAD: atomic-add |h[i,j]| into a single 64-bit accumulator
 *
 *  Differencing first and rounding after each pass is what makes the result
 *  the CPU's: blurring each frame and differencing the blurred frames is the
 *  same sum only without rounding. Integer arithmetic throughout, so the
 *  order of the atomic adds cannot change the result.
 *
 *  Mirror padding: reflect-101 (`2 * size - idx - 2` for idx >= size),
 *  matching CPU mirror(). Padding threads past the plane load tile samples
 *  no output consumes; those indices are clamped (cuda_tile_index.h) so a
 *  plane smaller than the tile never reads outside its buffer.
 */

#include "cuda_helper.cuh"
#include "common.h"
#include "cuda/cuda_tile_index.h"

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

namespace
{

typedef int32_t DiffTile[MV2_TILE_H][MV2_TILE_PITCH];

/* Sample `x` of row `y` of a packed plane of T. */
template <typename T>
__device__ __forceinline__ int load_sample(const uint8_t *__restrict__ plane, ptrdiff_t stride,
                                           int y, int x)
{
    return (int)__ldg(reinterpret_cast<const T *>(plane + ((ptrdiff_t)y * stride)) + x);
}

/* Stage prev - cur for the block's outputs and the two-sample halo, in that
 * order: the arithmetic shift rounds a negative sum towards minus infinity,
 * so the sign convention is the CPU's. */
template <typename T>
__device__ __forceinline__ void stage_diff(DiffTile &s_diff, const uint8_t *__restrict__ prev,
                                           const uint8_t *__restrict__ cur, ptrdiff_t prev_stride,
                                           ptrdiff_t cur_stride, unsigned width, unsigned height)
{
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    const int tile_origin_x = (int)(blockIdx.x * MV2_BLOCK_X) - MV2_RADIUS;
    const int tile_origin_y = (int)(blockIdx.y * MV2_BLOCK_Y) - MV2_RADIUS;
    const unsigned tile_elems = MV2_TILE_W * MV2_TILE_H;
    const unsigned wg_size = MV2_BLOCK_X * MV2_BLOCK_Y;

    for (unsigned i = lid; i < tile_elems; i += wg_size) {
        const unsigned ty = i / MV2_TILE_W;
        const unsigned tx = i % MV2_TILE_W;
        const int gx = vmaf_cuda_tile_index(
            vmaf_cuda_reflect_101(tile_origin_x + (int)tx, (int)width), (int)width);
        const int gy = vmaf_cuda_tile_index(
            vmaf_cuda_reflect_101(tile_origin_y + (int)ty, (int)height), (int)height);
        s_diff[ty][tx] =
            load_sample<T>(prev, prev_stride, gy, gx) - load_sample<T>(cur, cur_stride, gy, gx);
    }
}

/* |h| at this thread's pixel from the staged difference. VAcc is the
 * vertical accumulator: int32 holds 65536 * (2^8 - 1) for 8-bit input, and
 * 10- to 16-bit input takes int64, as the CPU's 16-bit pipeline does. */
template <typename VAcc>
__device__ __forceinline__ int64_t filtered_abs(const DiffTile &s_diff, unsigned bpc)
{
    const int shift_y = (int)bpc;
    const VAcc round_y = (VAcc)1 << (shift_y - 1);
    constexpr int shift_x = 16;
    constexpr int64_t round_x = (int64_t)1 << 15;
    const unsigned lx = threadIdx.x + MV2_RADIUS;
    const unsigned ly = threadIdx.y + MV2_RADIUS;

    int64_t blurred = 0;
#pragma unroll
    for (int xf = 0; xf < 5; ++xf) {
        VAcc blurred_y = 0;
#pragma unroll
        for (int yf = 0; yf < 5; ++yf) {
            blurred_y +=
                (VAcc)mv2_filter_d[yf] * (VAcc)s_diff[ly - MV2_RADIUS + yf][lx - MV2_RADIUS + xf];
        }
        const int32_t v = (int32_t)((blurred_y + round_y) >> shift_y);
        blurred += (int64_t)mv2_filter_d[xf] * (int64_t)v;
    }
    const int64_t h = (blurred + round_x) >> shift_x;
    return h < 0 ? -h : h;
}

/* Add the block's |h| values into *sad: a warp shuffle, then one atomic per
 * warp. Every thread of the block reaches this point, so the full mask is
 * exact. */
__device__ __forceinline__ void add_block_sad(int64_t abs_h, unsigned long long *__restrict__ sad)
{
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 16);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 8);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 4);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 2);
    abs_h += __shfl_down_sync(0xffffffff, abs_h, 1);
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    if ((lid & 31u) == 0u) {
        atomicAdd(sad, static_cast<unsigned long long>(abs_h));
    }
}

/* sum |blur(prev - cur)| over this block's outputs into *sad. T is the
 * sample type, VAcc the vertical accumulator (filtered_abs). */
template <typename T, typename VAcc>
__device__ __forceinline__ void
motion_sad(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur, ptrdiff_t prev_stride,
           ptrdiff_t cur_stride, unsigned long long *__restrict__ sad, unsigned width,
           unsigned height, unsigned bpc)
{
    /* Shared tile holds the signed diff (prev - cur) so the nested
     * separable filter operates on a single dataset. Inner dim is
     * MV2_TILE_PITCH (= MV2_TILE_W + 1) for bank-conflict padding. */
    __shared__ DiffTile s_diff;
    stage_diff<T>(s_diff, prev, cur, prev_stride, cur_stride, width, height);
    __syncthreads();

    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const int64_t abs_h = (x < width && y < height) ? filtered_abs<VAcc>(s_diff, bpc) : 0;
    add_block_sad(abs_h, sad);
}

} // namespace

/* Both kernels take the same parameters, so the host passes one argument
 * array (ADR-1215: cuLaunchKernel checks neither count nor order). */
extern "C" {

__launch_bounds__(MV2_BLOCK_X *MV2_BLOCK_Y, 8) __global__
    void motion_v2_kernel_8bpc(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur,
                               ptrdiff_t prev_stride, ptrdiff_t cur_stride,
                               unsigned long long *__restrict__ sad, unsigned width,
                               unsigned height, unsigned bpc)
{
    motion_sad<uint8_t, int32_t>(prev, cur, prev_stride, cur_stride, sad, width, height, bpc);
}

__launch_bounds__(MV2_BLOCK_X *MV2_BLOCK_Y, 8) __global__
    void motion_v2_kernel_16bpc(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur,
                                ptrdiff_t prev_stride, ptrdiff_t cur_stride,
                                unsigned long long *__restrict__ sad, unsigned width,
                                unsigned height, unsigned bpc)
{
    /* For bpc=16 the per-tap product can reach 26386 * 65535 ≈ 1.7e9 and
     * the 5-tap sum overflows int32 — int64 in the vertical accumulator. */
    motion_sad<uint16_t, int64_t>(prev, cur, prev_stride, cur_stride, sad, width, height, bpc);
}

} /* extern "C" */
