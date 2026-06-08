/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  HIP compute kernel for the float_psnr feature extractor (T7-10b /
 *  ADR-0254). Direct port of the CUDA twin at
 *  `libvmaf/src/feature/cuda/float_psnr/float_psnr_score.cu`.
 *
 *  Per-pixel `(ref - dis)^2` (float), warp-reduced + per-block partial
 *  written to a contiguous float buffer; host accumulates in `double`
 *  and applies the CPU log10 formula.
 *
 *  HIP warp size is 64 on GCN/RDNA (vs CUDA's 32). The `__shfl_down`
 *  intrinsic is available under HIP's `<hip/hip_runtime.h>` with the
 *  same signature as CUDA's `__shfl_down_sync` (HIP masks are 64-bit on
 *  RDNA). The shared memory array for warp partial sums is sized for the
 *  HIP warp size (64).
 *
 *  Compilation: `hipcc --offload-arch=gfx90a --emit-llvm -S` (or
 *  equivalent `--genco` for HSACO fat binary). The meson.build
 *  `enable_hipcc=true` path calls `hipcc --genco` and embeds the result
 *  via `xxd -i`; identical to the CUDA nvcc + bin2c approach in
 *  `libvmaf/src/meson.build`.
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#define FPSNR_BX 16
#define FPSNR_BY 16

/* Array sized for the minimum AMD warp size (wave32 = RDNA2+, 32 lanes) so
 * it is large enough on wave32 hardware: BX*BY/32 = 8 slots needed.
 * On wave64 hardware only BX*BY/64 = 4 of the 8 slots are populated. */
#define FPSNR_MIN_WARP_SIZE 32
/* Number of warps per block at minimum warp size = (BX * BY) / MIN_WARP_SIZE = 8. */
#define FPSNR_WARPS_PER_BLOCK (FPSNR_BX * FPSNR_BY / FPSNR_MIN_WARP_SIZE)

__device__ __forceinline__ float fpsnr_warp_reduce(float v)
{
    /* Use warpSize at runtime to handle wave64 (GCN/RDNA1) and wave32 (RDNA2+). */
    for (int off = (int)warpSize / 2; off > 0; off >>= 1)
        v += __shfl_down(v, off);
    return v;
}

extern "C" {

/**
 * 8-bpc kernel: ref/dis are uint8_t arrays (row-major, ref_stride bytes
 * per row). Computes per-pixel (ref - dis)^2 in float and accumulates one
 * partial sum per block into `partials[block_idx]`.
 */
__global__ void float_psnr_kernel_8bpc(const uint8_t *__restrict__ ref,
                                       const uint8_t *__restrict__ dis,
                                       ptrdiff_t ref_stride,
                                       ptrdiff_t dis_stride,
                                       float *__restrict__ partials,
                                       unsigned width,
                                       unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    float my_noise = 0.0f;
    if (x < (int)width && y < (int)height) {
        const float r = (float)ref[(ptrdiff_t)y * ref_stride + x];
        const float d = (float)dis[(ptrdiff_t)y * dis_stride + x];
        const float diff = r - d;
        my_noise = diff * diff;
    }

    /* Warp-level reduction. */
    __shared__ float s_warps[FPSNR_WARPS_PER_BLOCK];
    const float w = fpsnr_warp_reduce(my_noise);
    const unsigned ws      = (unsigned)warpSize;
    const unsigned lane    = lid % ws;
    const unsigned warp_id = lid / ws;
    if (lane == 0)
        s_warps[warp_id] = w;
    __syncthreads();

    /* Thread 0 aggregates warp partials and stores the block result. */
    if (lid == 0) {
        float total = 0.0f;
        const unsigned warps_active = (FPSNR_BX * FPSNR_BY) / ws;
        for (unsigned i = 0; i < warps_active; i++)
            total += s_warps[i];
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        partials[block_idx] = total;
    }
}

/**
 * 10/12/16-bpc kernel: ref/dis are uint8_t arrays whose elements are
 * interpreted as native uint16_t (little-endian, row-major). The raw
 * uint16 value is divided by `scaler` to convert to the [0,255]
 * float range that the CUDA twin uses.
 */
__global__ void float_psnr_kernel_16bpc(const uint8_t *__restrict__ ref,
                                        const uint8_t *__restrict__ dis,
                                        ptrdiff_t ref_stride,
                                        ptrdiff_t dis_stride,
                                        float *__restrict__ partials,
                                        unsigned width,
                                        unsigned height,
                                        unsigned bpc)
{
    float scaler = 1.0f;
    if (bpc == 10u)
        scaler = 4.0f;
    else if (bpc == 12u)
        scaler = 16.0f;
    else if (bpc == 16u)
        scaler = 256.0f;
    const float inv_scaler = 1.0f / scaler;

    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    float my_noise = 0.0f;
    if (x < (int)width && y < (int)height) {
        /* ref/dis are byte-addressed; cast the row pointer to uint16_t*
         * for each pixel access (matches the CUDA twin's pattern). */
        const uint16_t rv =
            reinterpret_cast<const uint16_t *>(ref + (ptrdiff_t)y * ref_stride)[x];
        const uint16_t dv =
            reinterpret_cast<const uint16_t *>(dis + (ptrdiff_t)y * dis_stride)[x];
        const float r = (float)rv * inv_scaler;
        const float d = (float)dv * inv_scaler;
        const float diff = r - d;
        my_noise = diff * diff;
    }

    __shared__ float s_warps[FPSNR_WARPS_PER_BLOCK];
    const float w = fpsnr_warp_reduce(my_noise);
    const unsigned ws      = (unsigned)warpSize;
    const unsigned lane    = lid % ws;
    const unsigned warp_id = lid / ws;
    if (lane == 0)
        s_warps[warp_id] = w;
    __syncthreads();

    if (lid == 0) {
        float total = 0.0f;
        const unsigned warps_active = (FPSNR_BX * FPSNR_BY) / ws;
        for (unsigned i = 0; i < warps_active; i++)
            total += s_warps[i];
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        partials[block_idx] = total;
    }
}

} /* extern "C" */
