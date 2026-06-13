/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  HIP compute kernels for the PSNR feature extractor (T7-10b batch-1 /
 *  ADR-0372). Direct port of the CUDA twin at
 *  `libvmaf/src/feature/cuda/integer_psnr/psnr_score.cu`.
 *
 *  Algorithm (mirrors libvmaf/src/feature/integer_psnr.c::sse_line):
 *      diff = (int64)ref - (int64)dis;
 *      sse  += diff * diff;
 *
 *  Reduction: per-pixel squared error accumulated via a per-lane warp
 *  shuffle, then lane-0 of each warp issues an atomicAdd to a single
 *  global uint64 counter. Bit-exact SSE with scalar reference.
 *
 *  HIP adaptation from CUDA:
 *  - Warp size varies: 64 on GCN/RDNA1 (wave64), 32 on RDNA2+ (wave32).
 *    Runtime `warpSize` is used in all reduction loops. The uint64 shuffle
 *    is split into two 32-bit `__shfl_down` calls (HIP does not expose a
 *    native uint64 shuffle). CUDA uses `__shfl_down_sync(0xffffffff, lo/hi, off)`.
 *  - `VmafPicture` / `VmafCudaBuffer` types replaced by plain byte
 *    pointers + explicit stride/plane parameters — the HIP host copies
 *    the luma plane into a tightly-pitched device buffer at submit time,
 *    so the kernel receives a single (ref*, dis*, stride, width, height)
 *    argument set. This matches the float_psnr_hip pattern.
 *  - `atomicAdd` for `unsigned long long` is available under HIP with
 *    the same semantics as CUDA.
 *
 *  v1: luma-only. Mirrors the v1 CUDA twin before the chroma extension.
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#define BLOCK_X 16
#define BLOCK_Y 16

/* Runtime `warpSize` is used in all reduction loops and lane-detection
 * expressions below so the same binary runs on wave64 (GCN/RDNA1) and
 * wave32 (RDNA2+). This constant is kept only for documentation. */
/* #define WARP_SIZE 64  -- replaced by runtime warpSize */

extern "C" {

/**
 * 8-bpc kernel: ref/dis are uint8_t row-major arrays (stride bytes per row).
 * Computes per-pixel (ref - dis)^2 in uint64 and atomicAdds the per-warp
 * sum into sse[0].
 */
__global__ void calculate_psnr_hip_kernel_8bpc(const uint8_t *__restrict__ ref,
                                               const uint8_t *__restrict__ dis,
                                               ptrdiff_t ref_stride,
                                               ptrdiff_t dis_stride,
                                               unsigned long long *__restrict__ sse,
                                               unsigned width,
                                               unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    uint64_t my_se = 0;
    if (x < (int)width && y < (int)height) {
        const int64_t r = (int64_t)ref[(ptrdiff_t)y * ref_stride + x];
        const int64_t d = (int64_t)dis[(ptrdiff_t)y * dis_stride + x];
        const int64_t diff = r - d;
        my_se = (uint64_t)(diff * diff);
    }

    /* Warp-reduce my_se via two uint32 shuffles — uses runtime warpSize
     * to handle wave64 (GCN/RDNA1) and wave32 (RDNA2+). */
    uint32_t lo = (uint32_t)my_se;
    uint32_t hi = (uint32_t)(my_se >> 32);
    for (int off = (int)warpSize / 2; off > 0; off >>= 1) {
        const uint32_t tlo = __shfl_down(lo, off);
        const uint32_t thi = __shfl_down(hi, off);
        const uint64_t other = ((uint64_t)thi << 32) | tlo;
        const uint64_t self = ((uint64_t)hi << 32) | lo;
        const uint64_t sum = self + other;
        lo = (uint32_t)sum;
        hi = (uint32_t)(sum >> 32);
    }

    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    const unsigned lane = lid % (unsigned)warpSize;
    if (lane == 0) {
        const uint64_t warp_sum = ((uint64_t)hi << 32) | lo;
        atomicAdd(sse, (unsigned long long)warp_sum);
    }
}

/**
 * 16-bpc kernel: ref/dis are byte-addressed uint16 arrays (stride bytes
 * per row). Raw uint16 values are diffed in int64 to avoid overflow.
 */
__global__ void calculate_psnr_hip_kernel_16bpc(const uint8_t *__restrict__ ref,
                                                const uint8_t *__restrict__ dis,
                                                ptrdiff_t ref_stride,
                                                ptrdiff_t dis_stride,
                                                unsigned long long *__restrict__ sse,
                                                unsigned width,
                                                unsigned height)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;

    uint64_t my_se = 0;
    if (x < (int)width && y < (int)height) {
        const uint16_t r = reinterpret_cast<const uint16_t *>(
            ref + (ptrdiff_t)y * ref_stride)[x];
        const uint16_t d = reinterpret_cast<const uint16_t *>(
            dis + (ptrdiff_t)y * dis_stride)[x];
        const int64_t diff = (int64_t)r - (int64_t)d;
        my_se = (uint64_t)(diff * diff);
    }

    uint32_t lo = (uint32_t)my_se;
    uint32_t hi = (uint32_t)(my_se >> 32);
    for (int off = (int)warpSize / 2; off > 0; off >>= 1) {
        const uint32_t tlo = __shfl_down(lo, off);
        const uint32_t thi = __shfl_down(hi, off);
        const uint64_t other = ((uint64_t)thi << 32) | tlo;
        const uint64_t self = ((uint64_t)hi << 32) | lo;
        const uint64_t sum = self + other;
        lo = (uint32_t)sum;
        hi = (uint32_t)(sum >> 32);
    }

    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    const unsigned lane = lid % (unsigned)warpSize;
    if (lane == 0) {
        const uint64_t warp_sum = ((uint64_t)hi << 32) | lo;
        atomicAdd(sse, (unsigned long long)warp_sum);
    }
}

} /* extern "C" */
