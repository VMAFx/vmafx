/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  HIP compute kernels for the float_moment feature extractor
 *  (T7-10b batch-3 / ADR-0374). Direct port of the CUDA twin at
 *  `libvmaf/src/feature/cuda/integer_moment/moment_score.cu`.
 *
 *  Algorithm (mirrors libvmaf/src/feature/float_moment.c::extract):
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
 *  Bit-exactness contract: int64 sum is exact on integer YUV inputs
 *  — places=4 cross-backend gate clears trivially (0/48 mismatches,
 *  matching the Vulkan twin).
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#define MOMENT_BX 16
#define MOMENT_BY 16

/* uint64 warp reduction via two uint32 shuffles — same technique as
 * the CUDA twin (moment_score.cu::warp_reduce_u64). HIP __shfl_down
 * has the same signature as CUDA __shfl_down_sync without the mask.
 * Uses `warpSize` at runtime to support both wave64 (GCN/RDNA1) and
 * wave32 (RDNA2+: gfx1030/gfx1100/gfx1101) hardware. */
__device__ __forceinline__ uint64_t moment_warp_reduce_u64(uint64_t v)
{
    uint32_t lo = (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32);
    for (int off = (int)warpSize / 2; off > 0; off >>= 1) {
        const uint32_t tlo = __shfl_down(lo, off);
        const uint32_t thi = __shfl_down(hi, off);
        const uint64_t other = ((uint64_t)thi << 32) | tlo;
        const uint64_t self  = ((uint64_t)hi  << 32) | lo;
        const uint64_t sum   = self + other;
        lo = (uint32_t)sum;
        hi = (uint32_t)(sum >> 32);
    }
    return ((uint64_t)hi << 32) | lo;
}

extern "C" {

/**
 * 8-bpc kernel: ref/dis are uint8_t row-major arrays with ref_stride /
 * dis_stride bytes per row. Accumulates four uint64 sums
 * [ref1st, dis1st, ref2nd, dis2nd] into `sums[0..3]` via atomicAdd.
 */
__global__ void calculate_moment_hip_kernel_8bpc(const uint8_t *__restrict__ ref,
                                                  const uint8_t *__restrict__ dis,
                                                  ptrdiff_t ref_stride,
                                                  ptrdiff_t dis_stride,
                                                  unsigned long long *__restrict__ sums,
                                                  unsigned width,
                                                  unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    uint64_t r1 = 0, r2 = 0, d1 = 0, d2 = 0;
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
 * a native uint16_t. Values are accumulated raw; the HOST divides the
 * exact integer sums by the bit-depth scaler (4/16/256) and by w*h to
 * recover the moment the CPU reference computes (ADR-1212). Same layout as 8bpc.
 */
__global__ void calculate_moment_hip_kernel_16bpc(const uint8_t *__restrict__ ref,
                                                   const uint8_t *__restrict__ dis,
                                                   ptrdiff_t ref_stride,
                                                   ptrdiff_t dis_stride,
                                                   unsigned long long *__restrict__ sums,
                                                   unsigned width,
                                                   unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    uint64_t r1 = 0, r2 = 0, d1 = 0, d2 = 0;
    if (x < (int)width && y < (int)height) {
        const uint64_t r = (uint64_t)reinterpret_cast<const uint16_t *>(
            ref + (ptrdiff_t)y * ref_stride)[x];
        const uint64_t d = (uint64_t)reinterpret_cast<const uint16_t *>(
            dis + (ptrdiff_t)y * dis_stride)[x];
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

} /* extern "C" */
