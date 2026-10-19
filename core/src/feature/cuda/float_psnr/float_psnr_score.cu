/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernel for the float_psnr feature extractor
 *  (T7-23 / batch 3 part 3b — ADR-0192 / ADR-0195). CUDA twin of
 *  float_psnr_vulkan.
 *
 *  Per-pixel `(ref - dis)^2` as float_psnr.c forms it, added per warp and
 *  per block as an integer, one block per FPSNR_BX pixels of one row; the
 *  host forms each row's exact sum from its blocks and adds the rows as the
 *  CPU does (feature/float_psnr_rows.h).
 *
 *  The sums are integers so that they are exact (ADR-1455). float_psnr.c
 *  adds `(double)(diff * diff)` per row and the rows in double: every term is
 *  a float, a row's running sum is exact, and its value is the exact sum of
 *  the row's terms. With `scaler` = 2^(bpc - 8), `diff` is the sample
 *  difference over `scaler`, and its float square is the float square of the
 *  sample difference over `scaler`^2. So the kernel squares the sample
 *  difference in float, as the CPU does, and adds that value as an integer:
 *    - up to 12 bits the square is below 2^24 and exact in float;
 *    - at 16 bits the square is rounded to 24 bits by the float multiply, as
 *      on the CPU, and is below 2^32.
 *  A block's 256 terms are below 2^40, so one uint64 per block holds their
 *  sum. The CPU's adds of the rows round once its sum passes 2^53 units; the
 *  host adds the rows' exact sums in the same order, so it rounds where the
 *  CPU rounds (ADR-1499). Blocks of 16x16 pixels, which this kernel had, mix
 *  rows: their total was exact but rounded once past 2^53.
 */

#include "cuda_helper.cuh"
#include "common.h"
#include "cuda/float_psnr_cuda.h"
#include "cuda_device_ptr.cuh"

#define FPSNR_WARPS (FPSNR_BX * FPSNR_BY / 32u)

namespace
{

/* The float square of a sample difference: float_psnr.c's `diff * diff`
 * times `scaler`^2. Both factors are exact (the difference is below 2^16 in
 * magnitude) and the product is an integer below 2^32, rounded to 24 bits
 * above 2^24 as the CPU's is. */
__device__ __forceinline__ unsigned long long fpsnr_square(int ref, int dis)
{
    const float diff = (float)(ref - dis);
    return (unsigned long long)__fmul_rn(diff, diff);
}

/* The block's sum of `mine` over all threads, valid in thread 0. Threads
 * outside the frame pass 0 and still take part: every thread of the block
 * reaches the barrier, so the full masks are exact. The sum is integer, so
 * its order cannot change it. */
__device__ __forceinline__ unsigned long long fpsnr_block_sum(unsigned long long mine)
{
    __shared__ unsigned long long s_warps[FPSNR_WARPS];
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    unsigned long long v = mine;
    for (int off = 16; off > 0; off >>= 1)
        v += __shfl_down_sync(0xffffffffu, v, off);
    if ((lid & 31u) == 0u)
        s_warps[lid >> 5] = v;
    __syncthreads();

    unsigned long long total = 0ull;
    if (lid == 0u) {
#pragma unroll
        for (const unsigned long long warp_sum : s_warps)
            total += warp_sum;
    }
    return total;
}

/* One block of T samples: the squared differences of its pixels, added, at
 * partials[block]. */
template <typename T>
__device__ __forceinline__ void
fpsnr_block(const uint8_t *__restrict__ ref, const uint8_t *__restrict__ dis, ptrdiff_t ref_stride,
            ptrdiff_t dis_stride, VmafCudaBuffer partials, unsigned width, unsigned height)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;

    unsigned long long my_noise = 0ull;
    if (x < width && y < height) {
        const T rv = reinterpret_cast<const T *>(ref + static_cast<ptrdiff_t>(y) * ref_stride)[x];
        const T dv = reinterpret_cast<const T *>(dis + static_cast<ptrdiff_t>(y) * dis_stride)[x];
        my_noise = fpsnr_square(rv, dv);
    }

    const unsigned long long total = fpsnr_block_sum(my_noise);
    if (threadIdx.x == 0u && threadIdx.y == 0u) {
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        VMAF_CUDA_DPTR(unsigned long long, partials.data)[block_idx] = total;
    }
}

} // namespace

extern "C" {

__global__ void float_psnr_kernel_8bpc(const uint8_t *__restrict__ ref,
                                       const uint8_t *__restrict__ dis, ptrdiff_t ref_stride,
                                       ptrdiff_t dis_stride, VmafCudaBuffer partials,
                                       unsigned width, unsigned height)
{
    fpsnr_block<uint8_t>(ref, dis, ref_stride, dis_stride, partials, width, height);
}

/* 10/12/16-bpc kernel: the samples are native uint16_t. The squared
 * differences are added in units of 1 / scaler^2 (scaler = 2^(bpc - 8)); the
 * host divides. */
__global__ void float_psnr_kernel_16bpc(const uint8_t *__restrict__ ref,
                                        const uint8_t *__restrict__ dis, ptrdiff_t ref_stride,
                                        ptrdiff_t dis_stride, VmafCudaBuffer partials,
                                        unsigned width, unsigned height)
{
    fpsnr_block<uint16_t>(ref, dis, ref_stride, dis_stride, partials, width, height);
}

} /* extern "C" */
