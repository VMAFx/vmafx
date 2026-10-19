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
 *          ref1 += ref;        ref2 += (float)ref * (float)ref;
 *          dis1 += dis;        dis2 += (float)dis * (float)dis;
 *      host divides each accumulator by the bit-depth scaler and by w*h.
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
 *  Bit-exactness contract (ADR-1453): the four sums are the CPU's.
 *  moment.c adds the samples, and their squares, into one double per
 *  output. The first moments' terms are exact, so their integer sum is the
 *  CPU's sum. For the second moments the CPU forms each square in float
 *  (`const float term = pic_ * pic_`) and adds the floats: up to 12 bits a
 *  square has at most 24 significant bits and the float is the integer
 *  square, but at 16 bits it is the square rounded to 24 bits. The 16bpc
 *  kernel therefore adds the float square (moment_float_square()), an
 *  integer below 2^32, so the uint64 sum is exact, its order cannot change
 *  it, and it equals the CPU's double sum while that sum is below 2^53
 *  units, which holds for every frame of up to 2^21 pixels.
 *
 *  Past 2^53 units (ADR-1497) the CPU's own running sum rounds as it adds.
 *  On a frame that can get there (vmaf_moment_sum_may_round()) four more
 *  kernels replace the two second-moment sums with the CPU's, from rows,
 *  with feature/float_moment_sum.h:
 *
 *    moment_row_totals      each row's exact sum of float squares;
 *    moment_row_plans       a plan per row from the prefix of those sums;
 *    moment_row_units       each planned row's increments, composed in pixel
 *                           order over 256 runs and an ordered tree;
 *    moment_ordered_totals  one walk per plane over the rows. A row whose
 *                           plan does not hold at the CPU's sum is added as
 *                           its 256 runs, and a run that crosses a binade
 *                           term by term, in pixel order.
 *
 *  Each of them returns at once when the exact sum is at most 2^53 units:
 *  then it is the CPU's sum.
 */

#include "cuda_helper.cuh"
#include "cuda/integer_moment_cuda.h"
#include "common.h"
#include "cuda_device_ptr.cuh"

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
    for (unsigned long long &sum : m.v) {
        for (int off = 16; off > 0; off >>= 1)
            sum += __shfl_down_sync(0xffffffffu, sum, off);
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

/* The term of moment.c::compute_2nd_moment() for the raw sample `v` of a 10-,
 * 12- or 16-bit plane, in units of 1 / scaler^2: picture_copy() divides the
 * sample by the scaler (a power of two, exact) and the square is one fp32
 * product, rounded to nearest even. Scaling by a power of two does not change
 * which bits are rounded away, so the float square of the raw sample has the
 * same significand. Its value is an integer below 2^32. */
__device__ __forceinline__ unsigned long long moment_float_square(unsigned long long v)
{
    const float sample = (float)v;
    const float square = __fmul_rn(sample, sample);
    return (unsigned long long)square;
}

/* The square a T sample contributes. An 8-bit square has at most 16
 * significant bits, so the CPU's float square is the integer square. */
template <typename T> __device__ __forceinline__ unsigned long long sample_square(T v);

template <> __device__ __forceinline__ unsigned long long sample_square<uint8_t>(uint8_t v)
{
    return (unsigned long long)v * (unsigned long long)v;
}

template <> __device__ __forceinline__ unsigned long long sample_square<uint16_t>(uint16_t v)
{
    return moment_float_square(v);
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
            const T r = __ldg(&ref_row[x]);
            const T d = __ldg(&dis_row[x]);
            m.v[0] += r;
            m.v[1] += d;
            m.v[2] += sample_square<T>(r);
            m.v[3] += sample_square<T>(d);
        }
    }
    return m;
}

} // namespace

/* The CPU's second-moment sums past 2^53 units run in device code
 * (ADR-1497): integers only, and a sample's term is moment_float_square(). */
#define VMAF_ORDSUM_FUNC static __device__ __forceinline__
#define VMAF_ORDSUM_NO_FP64
#define VMAF_MOMENT_SQUARE(v) moment_float_square(v)
#include "feature/float_moment_sum.h"
#include "feature/float_moment_sum_gpu.h"

extern "C" {

__global__ void calculate_moment_kernel_8bpc(const VmafPicture ref, const VmafPicture dis,
                                             VmafCudaBuffer sums, unsigned width, unsigned height)
{
    add_block_sums(thread_sums<uint8_t>(ref, dis, width, height),
                   VMAF_CUDA_DPTR(unsigned long long, sums.data));
}

__global__ void calculate_moment_kernel_16bpc(const VmafPicture ref, const VmafPicture dis,
                                              VmafCudaBuffer sums, unsigned width, unsigned height)
{
    add_block_sums(thread_sums<uint16_t>(ref, dis, width, height),
                   VMAF_CUDA_DPTR(unsigned long long, sums.data));
}

/* The four kernels of the CPU's second-moment sums past 2^53 units
 * (feature/float_moment_sum_gpu.h, ADR-1497). */
__global__ void __launch_bounds__(VMAF_MOMENT_SUM_LANES)
    moment_row_totals(const VmafMomentSumArgs a)
{
    moment_row_totals_body(a);
}

__global__ void __launch_bounds__(VMAF_MOMENT_SUM_BATCH) moment_row_plans(const VmafMomentSumArgs a)
{
    moment_row_plans_body(a);
}

__global__ void __launch_bounds__(VMAF_MOMENT_SUM_LANES) moment_row_units(const VmafMomentSumArgs a)
{
    moment_row_units_body(a);
}

__global__ void __launch_bounds__(VMAF_MOMENT_SUM_LANES)
    moment_ordered_totals(const VmafMomentSumArgs a)
{
    moment_ordered_totals_body(a);
}

} /* extern "C" */
