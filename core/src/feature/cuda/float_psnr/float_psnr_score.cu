/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernel for the float_psnr feature extractor
 *  (T7-23 / batch 3 part 3b — ADR-0192 / ADR-0195). CUDA twin of
 *  float_psnr_vulkan.
 *
 *  Per-pixel `(ref - dis)²` (float), warp-reduced + per-block
 *  partial written to a contiguous float buffer; host accumulates
 *  in `double` and applies the CPU formula.
 */

#include "cuda_helper.cuh"
#include "common.h"

#define FPSNR_BX 16
#define FPSNR_BY 16

__device__ __forceinline__ float fpsnr_warp_reduce(float v)
{
    for (int off = 16; off > 0; off >>= 1)
        v += __shfl_down_sync(0xffffffff, v, off);
    return v;
}

extern "C" {

__global__ void float_psnr_kernel_8bpc(const uint8_t *__restrict__ ref,
                                       const uint8_t *__restrict__ dis, ptrdiff_t ref_stride,
                                       ptrdiff_t dis_stride, VmafCudaBuffer partials,
                                       unsigned width, unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    float my_noise = 0.0f;
    if (x < (int)width && y < (int)height) {
        const float r = (float)ref[y * ref_stride + x];
        const float d = (float)dis[y * dis_stride + x];
        const float diff = r - d;
        my_noise = diff * diff;
    }

    __shared__ float s_warps[FPSNR_BX * FPSNR_BY / 32];
    const float w = fpsnr_warp_reduce(my_noise);
    const int lane = lid % 32;
    const int warp_id = lid / 32;
    if (lane == 0)
        s_warps[warp_id] = w;
    __syncthreads();
    if (lid == 0) {
        float total = 0.0f;
#pragma unroll
        for (int i = 0; i < FPSNR_BX * FPSNR_BY / 32; i++)
            total += s_warps[i];
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        reinterpret_cast<float *>(partials.data)[block_idx] = total;
    }
}

__global__ void float_psnr_kernel_16bpc(const uint8_t *__restrict__ ref,
                                        const uint8_t *__restrict__ dis, ptrdiff_t ref_stride,
                                        ptrdiff_t dis_stride, VmafCudaBuffer partials,
                                        unsigned width, unsigned height, unsigned bpc)
{
    float scaler = 1.0f;
    if (bpc == 10)
        scaler = 4.0f;
    else if (bpc == 12)
        scaler = 16.0f;
    else if (bpc == 16)
        scaler = 256.0f;
    const float inv_scaler = 1.0f / scaler;

    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    float my_noise = 0.0f;
    if (x < (int)width && y < (int)height) {
        const uint16_t rv = reinterpret_cast<const uint16_t *>(ref + y * ref_stride)[x];
        const uint16_t dv = reinterpret_cast<const uint16_t *>(dis + y * dis_stride)[x];
        const float r = (float)rv * inv_scaler;
        const float d = (float)dv * inv_scaler;
        const float diff = r - d;
        my_noise = diff * diff;
    }

    __shared__ float s_warps[FPSNR_BX * FPSNR_BY / 32];
    const float w = fpsnr_warp_reduce(my_noise);
    const int lane = lid % 32;
    const int warp_id = lid / 32;
    if (lane == 0)
        s_warps[warp_id] = w;
    __syncthreads();
    if (lid == 0) {
        float total = 0.0f;
#pragma unroll
        for (int i = 0; i < FPSNR_BX * FPSNR_BY / 32; i++)
            total += s_warps[i];
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        reinterpret_cast<float *>(partials.data)[block_idx] = total;
    }
}

} /* extern "C" */
