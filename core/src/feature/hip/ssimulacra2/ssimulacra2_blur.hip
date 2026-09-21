/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  HIP kernel for the ssimulacra2 separable FastGaussian IIR blur.
 *  Direct port of the CUDA twin at
 *  `libvmaf/src/feature/cuda/ssimulacra2/ssimulacra2_blur.cu`.
 *
 *  H pass: one thread per row, scans left to right.
 *  V pass: one thread per column, scans top to bottom.
 *
 *  HIP adaptation: warp-size differences are immaterial here (no
 *  shared-memory warp reduce). An earlier `__launch_bounds__(64, 32)` asked
 *  every target for 32 resident waves even though the kernels' register
 *  footprint permits only 8 or 16; omitting the impossible minimum lets the
 *  compiler choose the real occupancy without changing launch geometry.
 *  FMA suppression
 *  is achieved via hipcc `-fno-fused-madd` (the `hip_kernel_sources`
 *  build rule in meson.build passes it alongside `--genco`).
 */

#include <hip/hip_runtime.h>

typedef struct Ssimulacra2IirStateHip {
    float prev1_0;
    float prev1_1;
    float prev1_2;
    float prev2_0;
    float prev2_1;
    float prev2_2;
} Ssimulacra2IirStateHip;

__device__ __forceinline__ float ssimulacra2_iir_step_hip(Ssimulacra2IirStateHip *state, float sum,
                                                          float n2_0, float n2_1, float n2_2,
                                                          float d1_0, float d1_1, float d1_2)
{
    const float ns0 = n2_0 * sum;
    const float dp0 = d1_0 * state->prev1_0;
    const float t0 = ns0 - dp0;
    const float o0 = t0 - state->prev2_0;
    const float ns1 = n2_1 * sum;
    const float dp1 = d1_1 * state->prev1_1;
    const float t1 = ns1 - dp1;
    const float o1 = t1 - state->prev2_1;
    const float ns2 = n2_2 * sum;
    const float dp2 = d1_2 * state->prev1_2;
    const float t2 = ns2 - dp2;
    const float o2 = t2 - state->prev2_2;
    state->prev2_0 = state->prev1_0;
    state->prev2_1 = state->prev1_1;
    state->prev2_2 = state->prev1_2;
    state->prev1_0 = o0;
    state->prev1_1 = o1;
    state->prev1_2 = o2;
    const float sum01 = o0 + o1;
    return sum01 + o2;
}

extern "C" __global__ void ssimulacra2_blur_h(const float *__restrict__ in_buf,
                                              float *__restrict__ out_buf, unsigned width,
                                              unsigned height, float n2_0, float n2_1, float n2_2,
                                              float d1_0, float d1_1, float d1_2, int radius,
                                              unsigned in_offset, unsigned out_offset)
{
    const unsigned row = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= height)
        return;

    const int xsize = (int)width;
    const int N = radius;

    Ssimulacra2IirStateHip state = {0};

    const unsigned in_base = in_offset + row * width;
    const unsigned out_base = out_offset + row * width;

    for (int n = -N + 1; n < xsize; ++n) {
        const int left = n - N - 1;
        const int right = n + N - 1;
        const float lv = (left >= 0) ? in_buf[in_base + (unsigned)left] : 0.f;
        const float rv = (right < xsize) ? in_buf[in_base + (unsigned)right] : 0.f;
        const float sum = lv + rv;

        const float output =
            ssimulacra2_iir_step_hip(&state, sum, n2_0, n2_1, n2_2, d1_0, d1_1, d1_2);
        if (n >= 0)
            out_buf[out_base + (unsigned)n] = output;
    }
}

extern "C" __global__ void ssimulacra2_blur_v(const float *__restrict__ in_buf,
                                              float *__restrict__ out_buf, unsigned width,
                                              unsigned height, float n2_0, float n2_1, float n2_2,
                                              float d1_0, float d1_1, float d1_2, int radius,
                                              unsigned in_offset, unsigned out_offset)
{
    const unsigned col = blockIdx.x * blockDim.x + threadIdx.x;
    if (col >= width)
        return;

    const int ysize = (int)height;
    const int N = radius;

    Ssimulacra2IirStateHip state = {0};

    for (int n = -N + 1; n < ysize; ++n) {
        const int left = n - N - 1;
        const int right = n + N - 1;
        const float lv = (left >= 0) ? in_buf[in_offset + (unsigned)left * width + col] : 0.f;
        const float rv = (right < ysize) ? in_buf[in_offset + (unsigned)right * width + col] : 0.f;
        const float sum = lv + rv;

        const float output =
            ssimulacra2_iir_step_hip(&state, sum, n2_0, n2_1, n2_2, d1_0, d1_1, d1_2);
        if (n >= 0)
            out_buf[out_offset + (unsigned)n * width + col] = output;
    }
}
