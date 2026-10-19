/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP compute kernel for the float_psnr feature extractor (T7-10b /
 *  ADR-0254). Direct port of the CUDA twin at
 *  `core/src/feature/cuda/float_psnr/float_psnr_score.cu`.
 *
 *  Per-pixel `(ref - dis)^2` as float_psnr.c forms it, added per wave and
 *  per block as an integer, one block per FPSNR_BX pixels of one row; the
 *  host forms each row's exact sum from its blocks, adds the rows in order as
 *  the CPU does (feature/float_psnr_rows.h, ADR-1499) and applies the CPU
 *  log10 formula.
 *
 *  The sums are integers so that they are exact (ADR-1440). float_psnr.c
 *  adds `(double)(diff * diff)` per row and the rows in double: every term is
 *  a float, the running sum is exact, and its value is the exact sum of the
 *  terms. With `scaler` = 2^(bpc - 8), `diff` is the sample difference over
 *  `scaler`, and its float square is the float square of the sample
 *  difference over `scaler`^2. So the kernel squares the sample difference in
 *  float, as the CPU does, and adds that value as an integer:
 *    - up to 12 bits the square is below 2^24, exact in float, and 256 of
 *      them fit a uint32 (256 * 4095^2 < 2^32);
 *    - at 16 bits the square is rounded to 24 bits by the float multiply, as
 *      on the CPU, and is below 2^32; its low and high 16 bits are added
 *      separately, each sum below 2^24.
 *  Thread 0 puts the two halves together into one uint64 per block. The
 *  host adds a row's blocks exactly, the rows into a double in the CPU's
 *  order, and divides by `scaler`^2, a power of two: the CPU's sum, rounded
 *  where the CPU rounds past 2^53 units (ADR-1499). A fp32 block
 *  sum, which this kernel had, is exact only at 8 bits: at 10, 12 and 16 bits
 *  it rounded once the block's rms difference reached 256 code values, which
 *  put the score up to 7.6e-8 dB from the CPU's. fp64 sums are exact too and
 *  cost twice the frame time on a gfx1036 (a double shuffle is two).
 *
 *  HIP warp size varies: 64 on GCN/RDNA1 (wave64), 32 on RDNA2+ (wave32).
 *  The shared memory array for warp partial sums is sized for the minimum
 *  warp size (32 = FPSNR_MIN_WARP_SIZE) so it is large enough on wave32
 *  hardware (8 slots) and wave64 uses only the first 4 of those slots.
 *  Runtime `warpSize` is used in all reduction loops and lane detection so
 *  the same binary runs correctly on both.
 *
 *  Compilation: `hipcc --offload-arch=gfx90a --emit-llvm -S` (or
 *  equivalent `--genco` for HSACO fat binary). The meson.build
 *  `enable_hipcc=true` path calls `hipcc --genco` and embeds the result
 *  via `xxd -i`; identical to the CUDA nvcc + bin2c approach in
 *  `core/src/meson.build`.
 */

#include <hip/hip_runtime.h>
#include <cstdint>

/* One block per FPSNR_BX pixels of one row (float_psnr_hip.c uses the same
 * numbers), so that each block sum is a segment of a single row. */
#define FPSNR_BX 256
#define FPSNR_BY 1

/* Array sized for the minimum AMD warp size (wave32 = RDNA2+, 32 lanes) so
 * it is large enough on wave32 hardware: BX*BY/32 = 8 slots needed.
 * On wave64 hardware only BX*BY/64 = 4 of the 8 slots are populated. */
#define FPSNR_MIN_WARP_SIZE 32
/* Number of warps per block at minimum warp size = (BX * BY) / MIN_WARP_SIZE = 8. */
#define FPSNR_WARPS_PER_BLOCK (FPSNR_BX * FPSNR_BY / FPSNR_MIN_WARP_SIZE)

namespace
{

__device__ __forceinline__ uint32_t fpsnr_warp_reduce(uint32_t v)
{
    /* Use warpSize at runtime to handle wave64 (GCN/RDNA1) and wave32 (RDNA2+). */
    for (unsigned off = (unsigned)warpSize / 2u; off > 0u; off >>= 1u)
        v += __shfl_down(v, off);
    return v;
}

/* The block's sum of `mine` over all threads, valid in thread 0. `s_warps` is
 * the block's shared scratch array. Threads outside the frame pass 0 and
 * still take part: every thread of the block must reach the barriers. */
__device__ __forceinline__ uint32_t fpsnr_block_sum(uint32_t *s_warps, uint32_t mine)
{
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    const uint32_t w = fpsnr_warp_reduce(mine);
    const unsigned ws = (unsigned)warpSize;
    if (lid % ws == 0u)
        s_warps[lid / ws] = w;
    __syncthreads();

    uint32_t total = 0u;
    if (lid == 0u) {
        const unsigned warps_active = (FPSNR_BX * FPSNR_BY) / ws;
        for (unsigned i = 0; i < warps_active; i++)
            total += s_warps[i];
    }
    return total;
}

/* The float square of a sample difference: float_psnr.c's `diff * diff`
 * times `scaler`^2. Both factors are exact (the difference is below 2^16 in
 * magnitude) and the product is an integer below 2^32, rounded to 24 bits
 * above 2^24 as the CPU's is. */
__device__ __forceinline__ uint32_t fpsnr_square(int ref, int dis)
{
    const float diff = (float)(ref - dis);
    return (uint32_t)(diff * diff);
}

} /* namespace */

extern "C" {

/**
 * 8-bpc kernel: ref/dis are uint8_t arrays (row-major, ref_stride bytes
 * per row). Adds the squared differences of each block and stores the sum
 * at partials[2 * block]; partials[2 * block + 1], the high half, is 0.
 */
__global__ void float_psnr_kernel_8bpc(const uint8_t *__restrict__ ref,
                                       const uint8_t *__restrict__ dis, ptrdiff_t ref_stride,
                                       ptrdiff_t dis_stride,
                                       unsigned long long *__restrict__ partials, unsigned width,
                                       unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    uint32_t my_noise = 0u;
    if (x < (int)width && y < (int)height) {
        my_noise =
            fpsnr_square(ref[(ptrdiff_t)y * ref_stride + x], dis[(ptrdiff_t)y * dis_stride + x]);
    }

    __shared__ uint32_t s_warps[FPSNR_WARPS_PER_BLOCK];
    const uint32_t total = fpsnr_block_sum(s_warps, my_noise);
    if (threadIdx.x == 0u && threadIdx.y == 0u) {
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        partials[block_idx] = (unsigned long long)total;
    }
}

/**
 * 10/12/16-bpc kernel: ref/dis are uint8_t arrays whose elements are
 * interpreted as native uint16_t (little-endian, row-major). The squared
 * differences are added in units of 1 / scaler^2 (scaler = 2^(bpc - 8)); the
 * host divides. Up to 12 bits a block's sum fits partials[2 * block]; at 16
 * bits the low 16 bits of each square go there and the rest to
 * partials[2 * block + 1].
 */
__global__ void float_psnr_kernel_16bpc(const uint8_t *__restrict__ ref,
                                        const uint8_t *__restrict__ dis, ptrdiff_t ref_stride,
                                        ptrdiff_t dis_stride,
                                        unsigned long long *__restrict__ partials, unsigned width,
                                        unsigned height, unsigned bpc)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const bool split = bpc > 12u;

    uint32_t my_lo = 0u;
    uint32_t my_hi = 0u;
    if (x < (int)width && y < (int)height) {
        /* ref/dis are byte-addressed; cast the row pointer to uint16_t*
         * for each pixel access (matches the CUDA twin's pattern). */
        const uint16_t rv = reinterpret_cast<const uint16_t *>(ref + (ptrdiff_t)y * ref_stride)[x];
        const uint16_t dv = reinterpret_cast<const uint16_t *>(dis + (ptrdiff_t)y * dis_stride)[x];
        const uint32_t square = fpsnr_square(rv, dv);
        my_lo = split ? (square & 0xFFFFu) : square;
        my_hi = split ? (square >> 16u) : 0u;
    }

    __shared__ uint32_t s_warps[FPSNR_WARPS_PER_BLOCK];
    const uint32_t total_lo = fpsnr_block_sum(s_warps, my_lo);
    uint32_t total_hi = 0u;
    /* `bpc` is the same for every thread, so every thread takes the same
     * branch and reaches the same barriers. */
    if (split) {
        /* The array is free for the second sum once thread 0 has read it. */
        __syncthreads();
        total_hi = fpsnr_block_sum(s_warps, my_hi);
    }
    if (threadIdx.x == 0u && threadIdx.y == 0u) {
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        partials[block_idx] = (unsigned long long)total_lo + ((unsigned long long)total_hi << 16u);
    }
}

} /* extern "C" */
