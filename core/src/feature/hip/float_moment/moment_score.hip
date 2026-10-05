/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP compute kernels for the float_moment feature extractor
 *  (T7-10b batch-3 / ADR-0374). Direct port of the CUDA twin at
 *  `core/src/feature/cuda/integer_moment/moment_score.cu`.
 *
 *  Algorithm (mirrors core/src/feature/float_moment.c::extract):
 *      for each pixel:
 *          ref1 += ref;        ref2 += ref * ref;
 *          dis1 += dis;        dis2 += dis * dis;
 *      host divides each accumulator by w*h.
 *
 *  Reduction strategy:
 *    1. Each thread computes its pixel's four contributions.
 *    2. Warp shuffle reduces 64 threads (HIP GCN/RDNA warp) → 1
 *       (uint64 via two uint32 shuffles, mirrors CUDA twin).
 *    3. Lane 0 of each warp atomicAdd's its four warp sums to
 *       the four global counters.
 *
 *  HIP adaptation from CUDA:
 *  - Warp size is 64 on GCN/RDNA; 32 in CUDA. `__shfl_down` is used
 *    without a mask (HIP convention; CUDA uses `__shfl_down_sync`).
 *  - VmafCudaBuffer / VmafPicture replaced by raw pointers since the
 *    HIP host passes `rb.device` directly.
 *  - The sums array is `[ref1st, dis1st, ref2nd, dis2nd]` (same order
 *    as the CUDA twin so the host collect() code is identical).
 *
 *  Bit-exactness contract (ADR-1447): the four sums are the CPU's.
 *  moment.c adds the samples, and their squares, into one double per
 *  output. The first moments' terms are exact, so their integer sum is the
 *  CPU's sum. For the second moments the CPU forms each square in float
 *  (`const float term = pic_ * pic_`) and adds the floats: up to 12 bits a
 *  square has at most 24 significant bits and the float is the integer
 *  square, but at 16 bits it is the square rounded to 24 bits. The kernel
 *  therefore adds the float square (moment_float_square()), an integer below
 *  2^32, so the uint64 sum is exact and equals the CPU's double sum while
 *  that sum is below 2^53 units, which holds for every frame of up to 2^21
 *  pixels.
 *
 *  Past 2^53 units (ADR-1497) the CPU's own running sum rounds as it adds.
 *  On a frame that can get there four more kernels, shared with the CUDA
 *  twin (feature/float_moment_sum_gpu.h), replace the two second-moment
 *  sums with the CPU's: each row's exact sum, a plan per row, each planned
 *  row's increments composed in pixel order, and one walk per plane that adds
 *  a row from its increments or as its runs, and a run that crosses a binade
 *  term by term. They return at once while a plane's exact sum is at most
 *  2^53 units.
 */

#include <hip/hip_runtime.h>
#include <cstdint>

#define MOMENT_BX 16
#define MOMENT_BY 16

/* uint64 warp reduction via two uint32 shuffles — same technique as
 * the CUDA twin (moment_score.cu::warp_reduce_u64). HIP __shfl_down
 * has the same signature as CUDA __shfl_down_sync without the mask.
 * Uses `warpSize` at runtime to support both wave64 (GCN/RDNA1) and
 * wave32 (RDNA2+: gfx1030/gfx1100/gfx1101) hardware. */
namespace
{

__device__ __forceinline__ uint64_t moment_warp_reduce_u64(uint64_t v)
{
    uint32_t lo = (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32u);
    for (unsigned off = (unsigned)warpSize / 2u; off > 0u; off >>= 1u) {
        const uint32_t tlo = __shfl_down(lo, off);
        const uint32_t thi = __shfl_down(hi, off);
        const uint64_t other = ((uint64_t)thi << 32u) | tlo;
        const uint64_t self = ((uint64_t)hi << 32u) | lo;
        const uint64_t sum = self + other;
        lo = (uint32_t)sum;
        hi = (uint32_t)(sum >> 32u);
    }
    return ((uint64_t)hi << 32u) | lo;
}

/* The term of moment.c::compute_2nd_moment() for the raw sample `v` of a 10-,
 * 12- or 16-bit plane, in units of 1 / scaler^2: picture_copy() divides the
 * sample by the scaler (a power of two, exact) and the square is one fp32
 * product, rounded to nearest even. Scaling by a power of two does not change
 * which bits are rounded away, so the float square of the raw sample has the
 * same significand. Its value is an integer below 2^32. */
__device__ __forceinline__ uint64_t moment_float_square(uint64_t v)
{
    const float sample = (float)v;
    const float square = sample * sample;
    return (uint64_t)square;
}

} /* namespace */

/* The CPU's second-moment sums past 2^53 units run in device code
 * (ADR-1497): integers only, and a sample's term is moment_float_square(). */
#define VMAF_ORDSUM_FUNC static __device__ __forceinline__
#define VMAF_ORDSUM_NO_FP64
#define VMAF_MOMENT_SQUARE(v) moment_float_square(v)
#include "float_moment_sum.h"
#include "float_moment_sum_gpu.h"

extern "C" {

/**
 * 8-bpc kernel: ref/dis are uint8_t row-major arrays with ref_stride /
 * dis_stride bytes per row. Accumulates four uint64 sums
 * [ref1st, dis1st, ref2nd, dis2nd] into `sums[0..3]` via atomicAdd.
 */
__global__ void calculate_moment_hip_kernel_8bpc(const uint8_t *__restrict__ ref,
                                                 const uint8_t *__restrict__ dis,
                                                 ptrdiff_t ref_stride, ptrdiff_t dis_stride,
                                                 unsigned long long *__restrict__ sums,
                                                 unsigned width, unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    uint64_t r1 = 0;
    uint64_t r2 = 0;
    uint64_t d1 = 0;
    uint64_t d2 = 0;
    if (x < (int)width && y < (int)height) {
        const uint64_t r = (uint64_t)ref[(ptrdiff_t)y * ref_stride + x];
        const uint64_t d = (uint64_t)dis[(ptrdiff_t)y * dis_stride + x];
        r1 = r;
        r2 = r * r;
        d1 = d;
        d2 = d * d;
    }

    r1 = moment_warp_reduce_u64(r1);
    r2 = moment_warp_reduce_u64(r2);
    d1 = moment_warp_reduce_u64(d1);
    d2 = moment_warp_reduce_u64(d2);

    const unsigned lane = lid % (unsigned)warpSize;
    if (lane == 0) {
        atomicAdd(&sums[0], (unsigned long long)r1);
        atomicAdd(&sums[1], (unsigned long long)d1);
        atomicAdd(&sums[2], (unsigned long long)r2);
        atomicAdd(&sums[3], (unsigned long long)d2);
    }
}

/**
 * 10/12/16-bpc kernel: ref/dis are byte-addressed; each pixel is
 * a native uint16_t. The samples are accumulated raw and the squares as the
 * CPU's float squares (moment_float_square()); the HOST divides the exact
 * integer sums by the bit-depth scaler (4/16/256, squared for the second
 * moments) and by w*h to recover the moment the CPU reference computes
 * (ADR-1212, ADR-1447). Same layout as 8bpc.
 */
__global__ void calculate_moment_hip_kernel_16bpc(const uint8_t *__restrict__ ref,
                                                  const uint8_t *__restrict__ dis,
                                                  ptrdiff_t ref_stride, ptrdiff_t dis_stride,
                                                  unsigned long long *__restrict__ sums,
                                                  unsigned width, unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    uint64_t r1 = 0;
    uint64_t r2 = 0;
    uint64_t d1 = 0;
    uint64_t d2 = 0;
    if (x < (int)width && y < (int)height) {
        const uint64_t r =
            (uint64_t)reinterpret_cast<const uint16_t *>(ref + (ptrdiff_t)y * ref_stride)[x];
        const uint64_t d =
            (uint64_t)reinterpret_cast<const uint16_t *>(dis + (ptrdiff_t)y * dis_stride)[x];
        r1 = r;
        r2 = moment_float_square(r);
        d1 = d;
        d2 = moment_float_square(d);
    }

    r1 = moment_warp_reduce_u64(r1);
    r2 = moment_warp_reduce_u64(r2);
    d1 = moment_warp_reduce_u64(d1);
    d2 = moment_warp_reduce_u64(d2);

    const unsigned lane = lid % (unsigned)warpSize;
    if (lane == 0) {
        atomicAdd(&sums[0], (unsigned long long)r1);
        atomicAdd(&sums[1], (unsigned long long)d1);
        atomicAdd(&sums[2], (unsigned long long)r2);
        atomicAdd(&sums[3], (unsigned long long)d2);
    }
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
