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
 *    4. SAD: sum |h[i,j]| per block, one atomic add per block into a
 *       single 64-bit accumulator
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
#define MV2_TILE_PITCH (MV2_TILE_W + 1)            /* 21 */
#define MV2_WARPS (MV2_BLOCK_X * MV2_BLOCK_Y / 32) /* 8 */

namespace
{

typedef int32_t DiffTile[MV2_TILE_H][MV2_TILE_PITCH];
/* The vertical pass: the block's 16 output rows over all 20 tile columns. */
typedef int32_t VTile[MV2_BLOCK_Y][MV2_TILE_PITCH];

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

/* The vertical pass of the block's outputs, rounded like the CPU:
 *   v[r][c] = (sum_k filter[k] * diff[r + k][c] + 2^(bpc-1)) >> bpc
 * for the 16 output rows and all 20 tile columns. Each v feeds five
 * horizontally adjacent outputs, so computing it once per block instead of
 * once per output cuts the multiply-adds from 25 to 10 per output; the
 * values are the ones the per-output loop computed, integer for integer.
 * VAcc is the vertical accumulator: int32 holds 65536 * (2^8 - 1) for 8-bit
 * input, and 10- to 16-bit input takes int64, as the CPU's 16-bit pipeline
 * does. */
template <typename VAcc>
__device__ __forceinline__ void vertical_pass(const DiffTile &s_diff, VTile &s_v, unsigned bpc)
{
    const int shift_y = (int)bpc;
    const VAcc round_y = (VAcc)1 << (shift_y - 1);
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    for (unsigned i = lid; i < MV2_BLOCK_Y * MV2_TILE_W; i += MV2_BLOCK_X * MV2_BLOCK_Y) {
        const unsigned r = i / MV2_TILE_W;
        const unsigned c = i % MV2_TILE_W;
        VAcc blurred_y = 0;
#pragma unroll
        for (int k = 0; k < 5; ++k)
            blurred_y += (VAcc)mv2_filter_d[k] * (VAcc)s_diff[r + k][c];
        s_v[r][c] = (int32_t)((blurred_y + round_y) >> shift_y);
    }
}

/* |h| at this thread's pixel from the vertical pass:
 *   h = (sum_k filter[k] * v[ty][tx + k] + 2^15) >> 16 */
__device__ __forceinline__ int64_t horizontal_abs(const VTile &s_v)
{
    constexpr int shift_x = 16;
    constexpr int64_t round_x = (int64_t)1 << 15;
    int64_t blurred = 0;
#pragma unroll
    for (int k = 0; k < 5; ++k)
        blurred += (int64_t)mv2_filter_d[k] * (int64_t)s_v[threadIdx.y][threadIdx.x + k];
    const int64_t h = (blurred + round_x) >> shift_x;
    return h < 0 ? -h : h;
}

/* Add the block's |h| values into *sad with one atomic per block: a warp
 * shuffle, the eight warp sums through shared memory, then the first warp.
 * One atomic per warp to the frame's single accumulator serialised the
 * kernel on the L2 atomic unit (8x the atomics for the same sum). Every
 * thread of the block reaches this point, so the full masks are exact, and
 * the sum is integer, so its order cannot change the result. */
__device__ __forceinline__ void add_block_sad(int64_t abs_h, unsigned long long *__restrict__ sad)
{
    __shared__ unsigned long long s_warp[MV2_WARPS];
    unsigned long long v = static_cast<unsigned long long>(abs_h);
    for (int off = 16; off > 0; off >>= 1)
        v += __shfl_down_sync(0xffffffffu, v, off);
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    if ((lid & 31u) == 0u)
        s_warp[lid >> 5] = v;
    __syncthreads();
    if (lid < 32u) {
        v = (lid < MV2_WARPS) ? s_warp[lid] : 0ull;
        for (int off = 16; off > 0; off >>= 1)
            v += __shfl_down_sync(0xffffffffu, v, off);
        if (lid == 0u)
            atomicAdd(sad, v);
    }
}

/* sum |blur(prev - cur)| over this block's outputs into *sad. T is the
 * sample type, VAcc the vertical accumulator (vertical_pass). */
template <typename T, typename VAcc>
__device__ __forceinline__ void
motion_sad(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur, ptrdiff_t prev_stride,
           ptrdiff_t cur_stride, unsigned long long *__restrict__ sad, unsigned width,
           unsigned height, unsigned bpc)
{
    /* Shared tile holds the signed diff (prev - cur) so the separable
     * filter operates on a single dataset. Inner dim is MV2_TILE_PITCH
     * (= MV2_TILE_W + 1) for bank-conflict padding. */
    __shared__ DiffTile s_diff;
    __shared__ VTile s_v;
    stage_diff<T>(s_diff, prev, cur, prev_stride, cur_stride, width, height);
    __syncthreads();
    vertical_pass<VAcc>(s_diff, s_v, bpc);
    __syncthreads();

    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    const int64_t abs_h = (x < width && y < height) ? horizontal_abs(s_v) : 0;
    add_block_sad(abs_h, sad);
}

} // namespace

/* Both kernels take the same parameters, so the host passes one argument
 * array (ADR-1215: cuLaunchKernel checks neither count nor order).
 *
 * __launch_bounds__(256, MV2_MIN_BLOCKS): six resident 256-thread blocks
 * are 1536 threads, the most an SM of sm_86 / 89 / 120 holds, so the hint
 * asks for full occupancy there. The former minimum of 8 blocks (2048
 * threads) exceeded that limit: ptxas ignored it with a `.minnctapersm`
 * warning on those targets and capped the registers at 32 on sm_80 / 90. */
#define MV2_MIN_BLOCKS 6
extern "C" {

__launch_bounds__(MV2_BLOCK_X *MV2_BLOCK_Y, MV2_MIN_BLOCKS) __global__
    void motion_v2_kernel_8bpc(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur,
                               ptrdiff_t prev_stride, ptrdiff_t cur_stride,
                               unsigned long long *__restrict__ sad, unsigned width,
                               unsigned height, unsigned bpc)
{
    motion_sad<uint8_t, int32_t>(prev, cur, prev_stride, cur_stride, sad, width, height, bpc);
}

__launch_bounds__(MV2_BLOCK_X *MV2_BLOCK_Y, MV2_MIN_BLOCKS) __global__
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
