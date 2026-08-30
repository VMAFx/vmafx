/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernel for the float_moment feature extractor
 *  (T7-23 / batch 1d part 2). Mirrors the Vulkan moment.comp
 *  shipped in PR #133 (ADR-0182): emits all four metrics —
 *  float_moment_ref{1st,2nd} + float_moment_dis{1st,2nd} — in
 *  a single kernel pass via four uint64 atomic counters.
 *
 *  Algorithm (mirrors libvmaf/src/feature/float_moment.c::extract):
 *      for each pixel:
 *          ref1 += ref;        ref2 += ref * ref;
 *          dis1 += dis;        dis2 += dis * dis;
 *      host divides each accumulator by w*h.
 *
 *  Reduction strategy:
 *    1. Each thread computes its pixel's four contributions.
 *    2. Warp shuffle reduces 32 threads → 1 (uint64 via two
 *       uint32 shuffles, same trick as psnr_score.cu).
 *    3. Lane 0 of each warp atomicAdd's its four warp sums to
 *       the four global counters.
 *
 *  Bit-exactness contract: int64 sum is exact on integer YUV
 *  inputs ⇒ places=4 cross-backend gate clears trivially
 *  (matches the empirical result on Vulkan: 0/48 mismatches).
 */

#include "cuda_helper.cuh"
#include "cuda/integer_moment_cuda.h"
#include "common.h"

#define BLOCK_X 16
#define BLOCK_Y 16

extern "C" {

__device__ static inline uint64_t warp_reduce_u64(uint64_t v)
{
    uint32_t lo = (uint32_t)v;
    uint32_t hi = (uint32_t)(v >> 32);
    for (int off = 16; off > 0; off >>= 1) {
        uint32_t tlo = __shfl_down_sync(0xffffffff, lo, off);
        uint32_t thi = __shfl_down_sync(0xffffffff, hi, off);
        uint64_t other = ((uint64_t)thi << 32) | tlo;
        uint64_t self = ((uint64_t)hi << 32) | lo;
        uint64_t sum = self + other;
        lo = (uint32_t)sum;
        hi = (uint32_t)(sum >> 32);
    }
    return ((uint64_t)hi << 32) | lo;
}

__global__ void calculate_moment_kernel_8bpc(const VmafPicture ref, const VmafPicture dis,
                                             VmafCudaBuffer sums, unsigned width, unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    uint64_t r1 = 0, r2 = 0, d1 = 0, d2 = 0;
    if (x < (int)width && y < (int)height) {
        const uint8_t *ref_row = reinterpret_cast<const uint8_t *>(ref.data[0]) + y * ref.stride[0];
        const uint8_t *dis_row = reinterpret_cast<const uint8_t *>(dis.data[0]) + y * dis.stride[0];
        const uint64_t r = (uint64_t)ref_row[x];
        const uint64_t d = (uint64_t)dis_row[x];
        r1 = r;
        r2 = r * r;
        d1 = d;
        d2 = d * d;
    }

    r1 = warp_reduce_u64(r1);
    r2 = warp_reduce_u64(r2);
    d1 = warp_reduce_u64(d1);
    d2 = warp_reduce_u64(d2);

    const int lane = (threadIdx.y * blockDim.x + threadIdx.x) % 32;
    if (lane == 0) {
        unsigned long long *acc = reinterpret_cast<unsigned long long *>(sums.data);
        atomicAdd(&acc[0], (unsigned long long)r1);
        atomicAdd(&acc[1], (unsigned long long)d1);
        atomicAdd(&acc[2], (unsigned long long)r2);
        atomicAdd(&acc[3], (unsigned long long)d2);
    }
}

__global__ void calculate_moment_kernel_16bpc(const VmafPicture ref, const VmafPicture dis,
                                              VmafCudaBuffer sums, unsigned width, unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    uint64_t r1 = 0, r2 = 0, d1 = 0, d2 = 0;
    if (x < (int)width && y < (int)height) {
        const uint16_t *ref_row = reinterpret_cast<const uint16_t *>(
            reinterpret_cast<const uint8_t *>(ref.data[0]) + y * ref.stride[0]);
        const uint16_t *dis_row = reinterpret_cast<const uint16_t *>(
            reinterpret_cast<const uint8_t *>(dis.data[0]) + y * dis.stride[0]);
        const uint64_t r = (uint64_t)ref_row[x];
        const uint64_t d = (uint64_t)dis_row[x];
        r1 = r;
        r2 = r * r;
        d1 = d;
        d2 = d * d;
    }

    r1 = warp_reduce_u64(r1);
    r2 = warp_reduce_u64(r2);
    d1 = warp_reduce_u64(d1);
    d2 = warp_reduce_u64(d2);

    const int lane = (threadIdx.y * blockDim.x + threadIdx.x) % 32;
    if (lane == 0) {
        unsigned long long *acc = reinterpret_cast<unsigned long long *>(sums.data);
        atomicAdd(&acc[0], (unsigned long long)r1);
        atomicAdd(&acc[1], (unsigned long long)d1);
        atomicAdd(&acc[2], (unsigned long long)r2);
        atomicAdd(&acc[3], (unsigned long long)d2);
    }
}

} /* extern "C" */
