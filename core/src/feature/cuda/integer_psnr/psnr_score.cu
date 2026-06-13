/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernel for the PSNR feature extractor (T7-23 / batch 1b).
 *  Per-pixel squared-error reduction with int64 accumulation on the
 *  device. Mirrors the Vulkan psnr.comp shipped in PR #125 (ADR-0182).
 *
 *  Algorithm (mirrors libvmaf/src/feature/integer_psnr.c::sse_line_{8,16}):
 *      diff = (int64)ref - (int64)dis;
 *      sse  += diff * diff;             // per-pixel
 *
 *  Reduction strategy:
 *    1. Each thread computes one pixel's squared error (uint64).
 *    2. Warp shuffle reduction collapses 32 threads → 1.
 *    3. Lane 0 of each warp atomicAdd's to a single global uint64
 *       counter. (Same pattern as motion_score.cu's SAD reduction.)
 *
 *  Bit-exactness contract: byte-equal int64 SSE accumulation with the
 *  scalar reference ⇒ places=4 cross-backend gate clears trivially.
 *
 *  v1: luma-only ("psnr_y" output), matching psnr_vulkan.c's scope.
 *      Chroma is a focused follow-up (the picture_cuda upload path
 *      is luma-only today).
 */

#include "cuda_helper.cuh"
#include "cuda/integer_psnr_cuda.h"
#include "common.h"

#define BLOCK_X 16
#define BLOCK_Y 16

extern "C" {

__global__ void calculate_psnr_kernel_8bpc(const VmafPicture ref, const VmafPicture dis,
                                           VmafCudaBuffer sse, unsigned width, unsigned height,
                                           unsigned plane)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    uint64_t my_se = 0;
    if (x < (int)width && y < (int)height) {
        const uint8_t *ref_row =
            reinterpret_cast<const uint8_t *>(ref.data[plane]) + y * ref.stride[plane];
        const uint8_t *dis_row =
            reinterpret_cast<const uint8_t *>(dis.data[plane]) + y * dis.stride[plane];
        const int64_t diff = (int64_t)ref_row[x] - (int64_t)dis_row[x];
        my_se = (uint64_t)(diff * diff);
    }

    /* Warp-reduce my_se (uint64 via two uint32 shuffles). */
    uint32_t lo = (uint32_t)my_se;
    uint32_t hi = (uint32_t)(my_se >> 32);
    for (int off = 16; off > 0; off >>= 1) {
        uint32_t tlo = __shfl_down_sync(0xffffffff, lo, off);
        uint32_t thi = __shfl_down_sync(0xffffffff, hi, off);
        uint64_t other = ((uint64_t)thi << 32) | tlo;
        uint64_t self = ((uint64_t)hi << 32) | lo;
        uint64_t sum = self + other;
        lo = (uint32_t)sum;
        hi = (uint32_t)(sum >> 32);
    }

    const int lane = (threadIdx.y * blockDim.x + threadIdx.x) % 32;
    if (lane == 0) {
        const uint64_t warp_sum = ((uint64_t)hi << 32) | lo;
        atomicAdd(reinterpret_cast<unsigned long long *>(sse.data),
                  static_cast<unsigned long long>(warp_sum));
    }
}

__global__ void calculate_psnr_kernel_16bpc(const VmafPicture ref, const VmafPicture dis,
                                            VmafCudaBuffer sse, unsigned width, unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    uint64_t my_se = 0;
    if (x < (int)width && y < (int)height) {
        const uint16_t *ref_row = reinterpret_cast<const uint16_t *>(
            reinterpret_cast<const uint8_t *>(ref.data[0]) + y * ref.stride[0]);
        const uint16_t *dis_row = reinterpret_cast<const uint16_t *>(
            reinterpret_cast<const uint8_t *>(dis.data[0]) + y * dis.stride[0]);
        const int64_t diff = (int64_t)ref_row[x] - (int64_t)dis_row[x];
        my_se = (uint64_t)(diff * diff);
    }

    uint32_t lo = (uint32_t)my_se;
    uint32_t hi = (uint32_t)(my_se >> 32);
    for (int off = 16; off > 0; off >>= 1) {
        uint32_t tlo = __shfl_down_sync(0xffffffff, lo, off);
        uint32_t thi = __shfl_down_sync(0xffffffff, hi, off);
        uint64_t other = ((uint64_t)thi << 32) | tlo;
        uint64_t self = ((uint64_t)hi << 32) | lo;
        uint64_t sum = self + other;
        lo = (uint32_t)sum;
        hi = (uint32_t)(sum >> 32);
    }

    const int lane = (threadIdx.y * blockDim.x + threadIdx.x) % 32;
    if (lane == 0) {
        const uint64_t warp_sum = ((uint64_t)hi << 32) | lo;
        atomicAdd(reinterpret_cast<unsigned long long *>(sse.data),
                  static_cast<unsigned long long>(warp_sum));
    }
}

} /* extern "C" */
