/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernel for the PSNR feature extractor (T7-23 / batch 1b).
 *  Per-pixel squared-error reduction with int64 accumulation on the
 *  device. Mirrors the Vulkan psnr.comp shipped in PR #125 (ADR-0182).
 *
 *  Algorithm (mirrors core/src/feature/integer_psnr.c::sse_line_{8,16}):
 *      diff = (int64)ref - (int64)dis;
 *      sse  += diff * diff;             // per-pixel
 *
 *  Reduction strategy:
 *    1. Each thread sums the squared errors of PSNR_COLS_PER_THREAD pixels
 *       of one row (uint64), PSNR_BLOCK_X apart, so a warp's loads are
 *       contiguous.
 *    2. Warp shuffle reduction collapses 32 threads -> 1, the warp sums go
 *       through shared memory, and the first warp adds them up.
 *    3. One atomicAdd per block to the plane's uint64 accumulator. One
 *       atomic per warp, one pixel per thread, serialised the kernel on
 *       the L2 atomic unit: about 390,000 atomics to one address per 4K
 *       4:2:0 frame, against about 6,200 now.
 *
 *  Bit-exactness contract: byte-equal int64 SSE accumulation with the
 *  scalar reference ⇒ places=4 cross-backend gate clears trivially.
 *
 *  The host launches the kernel once per plane (Y, Cb, Cr) with that
 *  plane's size; `plane` selects data[] and stride[].
 */

#include "cuda_helper.cuh"
#include "cuda/integer_psnr_cuda.h"
#include "common.h"

namespace
{

/* Sum `se` over the block and add it to *sse with one atomic. Every thread
 * of the block reaches this point, so the full masks are exact; the sum is
 * integer, so its order cannot change the result. */
__device__ __forceinline__ void add_block_sse(uint64_t se, unsigned long long *__restrict__ sse)
{
    constexpr unsigned warps = (PSNR_BLOCK_X * PSNR_BLOCK_Y) / 32u;
    __shared__ unsigned long long s_warp[warps];
    unsigned long long v = static_cast<unsigned long long>(se);
    for (int off = 16; off > 0; off >>= 1)
        v += __shfl_down_sync(0xffffffffu, v, off);
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    if ((lid & 31u) == 0u)
        s_warp[lid >> 5] = v;
    __syncthreads();
    if (lid < 32u) {
        v = (lid < warps) ? s_warp[lid] : 0ull;
        for (int off = 16; off > 0; off >>= 1)
            v += __shfl_down_sync(0xffffffffu, v, off);
        if (lid == 0u)
            atomicAdd(sse, v);
    }
}

/* Row `y` of `plane` of `pic` as T samples. The plane is selected with
 * constant indices: indexing the by-value VmafPicture kernel parameter with
 * the runtime `plane` made nvcc copy both pictures (192 bytes) to each
 * thread's stack. */
template <typename T>
__device__ __forceinline__ const T *plane_row(const VmafPicture &pic, unsigned plane, unsigned y)
{
    const void *data = pic.data[0];
    ptrdiff_t stride = pic.stride[0];
    if (plane == 1u) {
        data = pic.data[1];
        stride = pic.stride[1];
    } else if (plane == 2u) {
        data = pic.data[2];
        stride = pic.stride[2];
    }
    return reinterpret_cast<const T *>(static_cast<const uint8_t *>(data) + (size_t)y * stride);
}

/* Squared error of this thread's pixels of `plane`; T is the sample type.
 * (int64)ref - (int64)dis, squared, as integer_psnr.c::sse_line_{8,16}. */
template <typename T>
__device__ __forceinline__ uint64_t thread_sse(const VmafPicture &ref, const VmafPicture &dis,
                                               unsigned width, unsigned height, unsigned plane)
{
    const unsigned y = blockIdx.y * PSNR_BLOCK_Y + threadIdx.y;
    if (y >= height)
        return 0;
    const T *ref_row = plane_row<T>(ref, plane, y);
    const T *dis_row = plane_row<T>(dis, plane, y);
    const unsigned x0 = blockIdx.x * PSNR_BLOCK_COLS + threadIdx.x;
    uint64_t se = 0;
#pragma unroll
    for (unsigned k = 0; k < PSNR_COLS_PER_THREAD; k++) {
        const unsigned x = x0 + k * PSNR_BLOCK_X;
        if (x < width) {
            const int64_t diff = (int64_t)__ldg(&ref_row[x]) - (int64_t)__ldg(&dis_row[x]);
            se += (uint64_t)(diff * diff);
        }
    }
    return se;
}

} // namespace

extern "C" {

__global__ void calculate_psnr_kernel_8bpc(const VmafPicture ref, const VmafPicture dis,
                                           VmafCudaBuffer sse, unsigned width, unsigned height,
                                           unsigned plane)
{
    add_block_sse(thread_sse<uint8_t>(ref, dis, width, height, plane),
                  reinterpret_cast<unsigned long long *>(sse.data));
}

/* ADR-1215: `plane` selects the Y/Cb/Cr plane exactly as in the 8-bpc kernel.
 * The host has always passed it (psnr_cuda_dispatch's kernelParams), but this
 * kernel had no parameter for it and read `data[0]` / `stride[0]`, so every
 * high-bit-depth chroma dispatch measured a chroma-sized top-left window of
 * the LUMA plane: psnr_cb == psnr_cr == a luma value. */
__global__ void calculate_psnr_kernel_16bpc(const VmafPicture ref, const VmafPicture dis,
                                            VmafCudaBuffer sse, unsigned width, unsigned height,
                                            unsigned plane)
{
    add_block_sse(thread_sse<uint16_t>(ref, dis, width, height, plane),
                  reinterpret_cast<unsigned long long *>(sse.data));
}

} /* extern "C" */
