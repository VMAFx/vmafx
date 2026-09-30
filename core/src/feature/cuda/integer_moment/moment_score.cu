/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernel for the float_moment feature extractor
 *  (T7-23 / batch 1d part 2). Mirrors the Vulkan moment.comp
 *  shipped in PR #133 (ADR-0182): emits all four metrics —
 *  float_moment_ref{1st,2nd} + float_moment_dis{1st,2nd} — in
 *  a single kernel pass via four uint64 atomic counters.
 *
 *  Algorithm (mirrors core/src/feature/float_moment.c::extract):
 *      for each pixel:
 *          ref1 += ref;        ref2 += ref * ref;
 *          dis1 += dis;        dis2 += dis * dis;
 *      host divides each accumulator by w*h.
 *
 *  Reduction strategy (ADR-1392, as psnr_score.cu):
 *    1. Each thread sums the four contributions of MOMENT_COLS_PER_THREAD
 *       pixels of one row, MOMENT_BLOCK_X apart, so a warp's loads are
 *       contiguous.
 *    2. A warp shuffle collapses 32 threads -> 1 per sum, and the warp
 *       sums go through shared memory.
 *    3. Threads 0..3 each add up one sum over the block's warps and add
 *       it to its accumulator: one atomic per accumulator per block. One
 *       atomic per warp and sum, one pixel per thread, serialised the
 *       kernel on the four addresses (T-CUDA-MOMENT-PER-WARP-ATOMICS-
 *       2026-10-01).
 *
 *  Bit-exactness contract: the sums are integers, so their order cannot
 *  change them; the host arithmetic is unchanged.
 */

#include "cuda_helper.cuh"
#include "cuda/integer_moment_cuda.h"
#include "common.h"

namespace
{

/* The frame's four sums in accumulator order: ref1, dis1, ref2, dis2. */
struct MomentSums {
    unsigned long long v[MOMENT_SUMS];
};

/* Sum `m` over the block and add each sum to its accumulator with one
 * atomic per block. Every thread of the block reaches this point, so the
 * full masks are exact. */
__device__ __forceinline__ void add_block_sums(MomentSums m, unsigned long long *__restrict__ acc)
{
    constexpr unsigned warps = (MOMENT_BLOCK_X * MOMENT_BLOCK_Y) / 32u;
    __shared__ unsigned long long s_warp[MOMENT_SUMS][warps];
#pragma unroll
    for (unsigned k = 0; k < MOMENT_SUMS; k++) {
        for (int off = 16; off > 0; off >>= 1)
            m.v[k] += __shfl_down_sync(0xffffffffu, m.v[k], off);
    }
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    if ((lid & 31u) == 0u) {
#pragma unroll
        for (unsigned k = 0; k < MOMENT_SUMS; k++)
            s_warp[k][lid >> 5] = m.v[k];
    }
    __syncthreads();
    if (lid < MOMENT_SUMS) {
        unsigned long long sum = 0ull;
#pragma unroll
        for (unsigned w = 0; w < warps; w++)
            sum += s_warp[lid][w];
        atomicAdd(&acc[lid], sum);
    }
}

/* Row `y` of the luma plane of `pic` as T samples. */
template <typename T>
__device__ __forceinline__ const T *luma_row(const VmafPicture &pic, unsigned y)
{
    return reinterpret_cast<const T *>(static_cast<const uint8_t *>(pic.data[0]) +
                                       (size_t)y * pic.stride[0]);
}

/* The four sums of this thread's pixels; T is the sample type. */
template <typename T>
__device__ __forceinline__ MomentSums thread_sums(const VmafPicture &ref, const VmafPicture &dis,
                                                  unsigned width, unsigned height)
{
    MomentSums m = {{0ull, 0ull, 0ull, 0ull}};
    const unsigned y = blockIdx.y * MOMENT_BLOCK_Y + threadIdx.y;
    if (y >= height)
        return m;
    const T *ref_row = luma_row<T>(ref, y);
    const T *dis_row = luma_row<T>(dis, y);
    const unsigned x0 = blockIdx.x * MOMENT_BLOCK_COLS + threadIdx.x;
#pragma unroll
    for (unsigned k = 0; k < MOMENT_COLS_PER_THREAD; k++) {
        const unsigned x = x0 + k * MOMENT_BLOCK_X;
        if (x < width) {
            const unsigned long long r = __ldg(&ref_row[x]);
            const unsigned long long d = __ldg(&dis_row[x]);
            m.v[0] += r;
            m.v[1] += d;
            m.v[2] += r * r;
            m.v[3] += d * d;
        }
    }
    return m;
}

} // namespace

extern "C" {

__global__ void calculate_moment_kernel_8bpc(const VmafPicture ref, const VmafPicture dis,
                                             VmafCudaBuffer sums, unsigned width, unsigned height)
{
    add_block_sums(thread_sums<uint8_t>(ref, dis, width, height),
                   reinterpret_cast<unsigned long long *>(sums.data));
}

__global__ void calculate_moment_kernel_16bpc(const VmafPicture ref, const VmafPicture dis,
                                              VmafCudaBuffer sums, unsigned width, unsigned height)
{
    add_block_sums(thread_sums<uint16_t>(ref, dis, width, height),
                   reinterpret_cast<unsigned long long *>(sums.data));
}

} /* extern "C" */
