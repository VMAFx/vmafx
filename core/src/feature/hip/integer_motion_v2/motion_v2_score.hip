/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  The motion SAD kernel of the HIP backend, shared by motion_hip and
 *  motion_v2_hip through integer_motion_sad_hip.c (ADR-1377; the kernel was
 *  motion_v2's alone until motion_hip moved to the CPU's order,
 *  T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29).
 *
 *  Arithmetic (must match CPU integer_motion.c / integer_motion_v2.c,
 *  motion_score_pipeline_8 / _16):
 *
 *    1. Per pixel: diff = prev[i,j] - cur[i,j]   (signed, raw samples)
 *    2. Vertical filter on diff:
 *         v[i,j] = (sum_k filter[k] * diff[mirror(i-2+k), j]
 *                    + (1 << (bpc - 1))) >> bpc
 *    3. Horizontal filter on v:
 *         h[i,j] = (sum_k filter[k] * v[i, mirror(j-2+k)]
 *                    + 32768) >> 16
 *    4. SAD: sum |h[i,j]| into one 64-bit accumulator
 *
 *  Differencing first and rounding after each pass is what makes the result
 *  the CPU's: blurring each frame and differencing the blurred frames is the
 *  same sum only without rounding, and motion_hip drifted from the CPU that
 *  way (the SYCL twin measured 2e-4 at 17x17 and 1.3e-5 on the Netflix
 *  576x324 pair before ADR-1371). Integer arithmetic throughout, so the order
 *  of the atomic adds cannot change the result.
 *
 *  Mirror padding: reflect-101 (`2 * size - idx - 2` for idx >= size), the
 *  CPU's mirror(). A block loads a fixed 20x20 tile, halo included, for every
 *  thread, including the padding threads past the last row or column. One
 *  reflection keeps every sample a valid output consumes inside the plane
 *  (for planes of at least 3x3, which both extractors' init() enforces), but
 *  not the padding rows and columns: at 17x17 the last block's tile reflects
 *  row 33 to -1 and read one row before the device buffer. The reflected
 *  index goes through vmaf_hip_tile_index() (hip_tile_index.h), the identity
 *  for every consumed sample.
 *
 *  HIP adaptation from CUDA:
 *  - Warp size varies: 64 on GCN/CDNA (wave64), 32 on RDNA (wave32).
 *    Runtime `warpSize` is used in reduction loops and lane detection.
 *    Warp reduction uses `__shfl_down` (no mask).
 *  - `sad` accumulator is a plain `uint64_t *` (not VmafCudaBuffer).
 *  - TILE_PITCH = TILE_W + 1 retained (bank-conflict padding, same as CUDA).
 *  - Shift operations on int32/int64 are arithmetic (signed), matching
 *    the CPU reference and the CUDA twin (ADR-0138/0139 bit-exactness posture).
 *
 *  Kernel entry-point names (resolved by integer_motion_sad_hip.c):
 *    motion_v2_kernel_8bpc   uint8 samples, int32 vertical sum
 *    motion_v2_kernel_16bpc  uint16 samples (9 to 16 bits), int64 vertical sum
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include "../hip_tile_index.h"

#define MV2_RADIUS 2
#define MV2_BLOCK_X 16
#define MV2_BLOCK_Y 16
#define MV2_TILE_W (MV2_BLOCK_X + 2 * MV2_RADIUS) /* 20 */
#define MV2_TILE_H (MV2_BLOCK_Y + 2 * MV2_RADIUS) /* 20 */
/* Pad inner stride to break shared-memory bank conflicts (same as CUDA twin). */
#define MV2_TILE_PITCH (MV2_TILE_W + 1) /* 21 */

__constant__ int32_t mv2_filter_d[5] = {3571, 16004, 26386, 16004, 3571};

/* Device helpers are called only by the kernels of this translation unit,
 * so they live in an anonymous namespace (internal linkage). */
namespace
{

/* Sample `col` of one packed row of T. */
template <typename T> __device__ __forceinline__ int mv2_sample(const uint8_t *row, int col)
{
    return (int)reinterpret_cast<const T *>(row)[col];
}

/* Stage prev - cur for the block's outputs and halo. The CPU differences in
 * that order, and the order matters: the arithmetic shift rounds a negative
 * sum towards minus infinity. */
template <typename T>
__device__ __forceinline__ void
mv2_load_diff(int32_t (*s_diff)[MV2_TILE_PITCH], const uint8_t *prev, const uint8_t *cur,
              ptrdiff_t prev_stride, ptrdiff_t cur_stride, unsigned width, unsigned height)
{
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    const int tile_origin_x = (int)(blockIdx.x * MV2_BLOCK_X) - MV2_RADIUS;
    const int tile_origin_y = (int)(blockIdx.y * MV2_BLOCK_Y) - MV2_RADIUS;
    const unsigned tile_elems = MV2_TILE_W * MV2_TILE_H;
    const unsigned wg_size = MV2_BLOCK_X * MV2_BLOCK_Y;

    for (unsigned i = lid; i < tile_elems; i += wg_size) {
        const unsigned ty = i / MV2_TILE_W;
        const unsigned tx = i % MV2_TILE_W;
        const int gx = vmaf_hip_tile_index(
            vmaf_hip_reflect_101(tile_origin_x + (int)tx, (int)width), (int)width);
        const int gy = vmaf_hip_tile_index(
            vmaf_hip_reflect_101(tile_origin_y + (int)ty, (int)height), (int)height);
        s_diff[ty][tx] =
            mv2_sample<T>(prev + gy * prev_stride, gx) - mv2_sample<T>(cur + gy * cur_stride, gx);
    }
}

/* Vertical 5-tap filter of tile column `col` around output row `ly`, rounded
 * like the CPU. `VAcc` is int32 at 8 bits and int64 above: at 16 bits a
 * single tap reaches 26386 * 65535 and the sum overflows int32. */
template <typename VAcc>
__device__ __forceinline__ int32_t mv2_vertical_tap(const int32_t (*s_diff)[MV2_TILE_PITCH],
                                                    unsigned ly, unsigned col, int shift_y)
{
    VAcc sum = 0;
#pragma unroll
    for (int yf = 0; yf < 5; ++yf) {
        sum += (VAcc)mv2_filter_d[yf] * (VAcc)s_diff[ly - MV2_RADIUS + yf][col];
    }
    const VAcc round_y = (VAcc)1 << (shift_y - 1);
    /* Arithmetic right shift, as the CPU and the CUDA twin: ADR-0138/0139. */
    return (int32_t)((sum + round_y) >> shift_y);
}

/* |h| at this thread's pixel; 0 for the padding threads. */
template <typename VAcc>
__device__ __forceinline__ int64_t mv2_filtered_abs(const int32_t (*s_diff)[MV2_TILE_PITCH],
                                                    unsigned width, unsigned height, int shift_y)
{
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);
    if (x >= (int)width || y >= (int)height)
        return 0;
    const unsigned lx = threadIdx.x + MV2_RADIUS;
    const unsigned ly = threadIdx.y + MV2_RADIUS;

    int64_t blurred = 0;
#pragma unroll
    for (int xf = 0; xf < 5; ++xf) {
        const int32_t v = mv2_vertical_tap<VAcc>(s_diff, ly, lx - MV2_RADIUS + xf, shift_y);
        blurred += (int64_t)mv2_filter_d[xf] * (int64_t)v;
    }
    const int64_t h = (blurred + ((int64_t)1 << 15)) >> 16;
    return h < 0 ? -h : h;
}

/* Warp reduction with the runtime `warpSize` (wave32 on RDNA, wave64 on
 * GCN/CDNA), then one atomic per wave. */
__device__ __forceinline__ void mv2_reduce_sad(int64_t abs_h, uint64_t *sad)
{
    for (int off = (int)warpSize / 2; off > 0; off >>= 1)
        abs_h += __shfl_down(abs_h, off);
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    if ((lid % (unsigned)warpSize) == 0u) {
        atomicAdd(reinterpret_cast<unsigned long long *>(sad),
                  static_cast<unsigned long long>(abs_h));
    }
}

template <typename T, typename VAcc>
__device__ __forceinline__ void mv2_sad(const uint8_t *prev, const uint8_t *cur,
                                        ptrdiff_t prev_stride, ptrdiff_t cur_stride, uint64_t *sad,
                                        unsigned width, unsigned height, int shift_y)
{
    __shared__ int32_t s_diff[MV2_TILE_H][MV2_TILE_PITCH];
    mv2_load_diff<T>(s_diff, prev, cur, prev_stride, cur_stride, width, height);
    __syncthreads();
    mv2_reduce_sad(mv2_filtered_abs<VAcc>(s_diff, width, height, shift_y), sad);
}

} // namespace

extern "C" {

__launch_bounds__(MV2_BLOCK_X *MV2_BLOCK_Y, 8) __global__
    void motion_v2_kernel_8bpc(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur,
                               ptrdiff_t prev_stride, ptrdiff_t cur_stride,
                               uint64_t *__restrict__ sad, unsigned width, unsigned height)
{
    mv2_sad<uint8_t, int32_t>(prev, cur, prev_stride, cur_stride, sad, width, height, 8);
}

__launch_bounds__(MV2_BLOCK_X *MV2_BLOCK_Y, 8) __global__
    void motion_v2_kernel_16bpc(const uint8_t *__restrict__ prev, const uint8_t *__restrict__ cur,
                                ptrdiff_t prev_stride, ptrdiff_t cur_stride,
                                uint64_t *__restrict__ sad, unsigned width, unsigned height,
                                unsigned bpc)
{
    mv2_sad<uint16_t, int64_t>(prev, cur, prev_stride, cur_stride, sad, width, height, (int)bpc);
}

} /* extern "C" */
