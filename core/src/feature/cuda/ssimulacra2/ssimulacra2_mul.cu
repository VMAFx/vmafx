/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA kernel for the ssimulacra2 elementwise plane multiply.
 *  Mirrors `multiply_3plane` in libvmaf/src/feature/ssimulacra2.c
 *  — operates on a contiguous 3-plane buffer; out[i] = a[i] * b[i].
 *
 *  Per-scale dispatch: grid = ceil(w / 16) * ceil(h / 8); each thread
 *  writes one (x, y, c) sample. The 3 channels are stored at offsets
 *  c * plane_stride (= full_w * full_h, kept constant across pyramid
 *  scales for layout consistency with the host buffers).
 *
 *  No reduction here — pure scalar multiply, FMA-free by definition.
 *  The ssim/edge_diff per-pixel double-precision combine is host-side
 *  (matches the Vulkan kernel; see ssimulacra2_cuda.c::extract).
 */

#include "cuda_helper.cuh"

extern "C" {

__global__ void ssimulacra2_mul3(const float *__restrict__ a, const float *__restrict__ b,
                                 float *__restrict__ out, unsigned width, unsigned height,
                                 unsigned plane_count, unsigned plane_stride)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;
    const unsigned base = y * width + x;
    for (unsigned c = 0u; c < plane_count; ++c) {
        const unsigned idx = base + c * plane_stride;
        out[idx] = a[idx] * b[idx];
    }
}

} /* extern "C" */
