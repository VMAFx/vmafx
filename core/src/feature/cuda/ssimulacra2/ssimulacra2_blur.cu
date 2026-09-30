/**
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  CUDA kernels for the ssimulacra2 separable FastGaussian IIR blur: the
 *  libjxl Charalampidis 2016 3-pole recursive Gaussian (k = {1, 3, 5},
 *  sigma = 1.5, zero-padded boundaries) of
 *  core/src/feature/ssimulacra2.c::fast_gaussian_1d / blur_plane.
 *
 *  Each launch runs the five blurs of a scale (ssimulacra2_cuda.h,
 *  enum ss2c_blur_job) for all three channels: blockIdx.y is the job,
 *  blockIdx.z the channel. The products ref^2, dis^2 and ref*dis are formed
 *  as the horizontal pass loads its input, one rounding each, as
 *  ssimulacra2.c::multiply_3plane does.
 *
 *  The IIR is sequential along its axis (ADR-1391):
 *    - Horizontal pass: one warp per block, one row per lane. The warp loads
 *      32-column tiles of its 32 rows with coalesced reads into shared
 *      memory, each lane walks its row through the tile, and the warp stores
 *      the 32 output columns coalesced. The next tile is loaded into
 *      registers while the current one is walked.
 *    - Vertical pass: one column per thread on the row-major output of the
 *      horizontal pass; neighbouring threads read neighbouring columns, so
 *      every load and store is coalesced without a transpose.
 *
 *  Bit-exactness: every step is `n2 * sum - d1 * prev1 - prev2` with the
 *  products rounded before the subtractions, as in the CPU extractor. The
 *  fatbin is built with --fmad=false so nvcc cannot fuse them.
 */

#include "cuda/ssimulacra2_cuda.h"

namespace
{

/* State of the three poles of one 1D IIR. */
struct Ss2cIir {
    float prev1[3];
    float prev2[3];
};

/* One step of ssimulacra2.c::fast_gaussian_1d; returns o0 + o1 + o2. */
__device__ __forceinline__ float ss2c_iir_step(Ss2cIir &st, const Ss2cBlurArgs &a, float lv,
                                               float rv)
{
    const float sum = lv + rv;
    float o[3];
#pragma unroll
    for (int k = 0; k < 3; k++) {
        const float ns = a.n2[k] * sum;
        const float dp = a.d1[k] * st.prev1[k];
        const float t = ns - dp;
        o[k] = t - st.prev2[k];
        st.prev2[k] = st.prev1[k];
        st.prev1[k] = o[k];
    }
    const float o01 = o[0] + o[1];
    return o01 + o[2];
}

/* Input sample `idx` of blur job `job`: the XYB value or the product of two,
 * rounded once (ssimulacra2.c::multiply_3plane). */
__device__ __forceinline__ float ss2c_blur_input(const Ss2cBlurArgs &a, unsigned job, size_t idx)
{
    switch (job) {
    case SS2C_MU1:
        return a.ref[idx];
    case SS2C_MU2:
        return a.dis[idx];
    case SS2C_S11: {
        const float r = a.ref[idx];
        return r * r;
    }
    case SS2C_S22: {
        const float d = a.dis[idx];
        return d * d;
    }
    default: {
        const float r = a.ref[idx];
        const float d = a.dis[idx];
        return r * d;
    }
    }
}

/* Column `col` of rows row0 .. row0 + 31 into registers; zero outside the
 * plane (the IIR's zero padding on the right, and rows past the end). */
__device__ __forceinline__ void ss2c_load_tile_column(const Ss2cBlurArgs &a, unsigned job,
                                                      size_t base, unsigned row0, unsigned col,
                                                      float column[SS2C_BLUR_TILE])
{
#pragma unroll
    for (unsigned r = 0; r < SS2C_BLUR_TILE; r++) {
        const unsigned row = row0 + r;
        column[r] = (col < a.width && row < a.height) ?
                        ss2c_blur_input(a, job, base + (size_t)row * a.width + col) :
                        0.0f;
    }
}

} // namespace

extern "C" {

/* Horizontal pass: grid (ceil(height / 32), SS2C_BLUR_JOBS, 3), 32 threads.
 * Chunk c walks the 32 IIR steps n = 32c - (N - 1) .. 32c - (N - 1) + 31.
 * Their right-hand input n + N - 1 is column k of tile c, their left-hand
 * input n - N - 1 lies in tile c - 1 or c (N <= SS2C_BLUR_MAX_RADIUS), so two
 * tile slots suffice. */
__global__ void __launch_bounds__(SS2C_BLUR_TILE) ssimulacra2_blur_h(const Ss2cBlurArgs a)
{
    __shared__ float tile[2][SS2C_BLUR_TILE][SS2C_BLUR_TILE + 1];
    __shared__ float outs[SS2C_BLUR_TILE][SS2C_BLUR_TILE + 1];
    const unsigned lane = threadIdx.x;
    const unsigned row0 = blockIdx.x * SS2C_BLUR_TILE;
    const unsigned job = blockIdx.y;
    const size_t plane = (size_t)a.width * a.height;
    const size_t base = (size_t)blockIdx.z * plane;
    const int w = (int)a.width;
    const int lead = a.radius - 1;

    float column[SS2C_BLUR_TILE];
    ss2c_load_tile_column(a, job, base, row0, lane, column);
#pragma unroll
    for (unsigned r = 0; r < SS2C_BLUR_TILE; r++) {
        tile[0][r][lane] = column[r];
        tile[1][r][lane] = 0.0f; /* tile -1: left of column 0 */
    }
    __syncwarp();

    Ss2cIir st = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    float *pass = a.pass[job] + base;
    for (unsigned chunk = 0; (int)(chunk * SS2C_BLUR_TILE) - lead < w; chunk++) {
        const int first = (int)(chunk * SS2C_BLUR_TILE) - lead;
        ss2c_load_tile_column(a, job, base, row0, (chunk + 1u) * SS2C_BLUR_TILE + lane, column);
        for (int k = 0; k < SS2C_BLUR_TILE && first + k < w; k++) {
            const int n = first + k;
            const int left = n - a.radius - 1;
            const float lv = (left >= 0) ? tile[(left >> 5) & 1][lane][left & 31] : 0.0f;
            const float rv = tile[chunk & 1u][lane][k];
            const float o = ss2c_iir_step(st, a, lv, rv);
            if (n >= 0)
                outs[lane][k] = o;
        }
        __syncwarp();
        const int col = first + (int)lane;
        if (col >= 0 && col < w) {
            for (unsigned r = 0; r < SS2C_BLUR_TILE && row0 + r < a.height; r++)
                pass[(size_t)(row0 + r) * a.width + (unsigned)col] = outs[r][lane];
        }
#pragma unroll
        for (unsigned r = 0; r < SS2C_BLUR_TILE; r++)
            tile[(chunk + 1u) & 1u][r][lane] = column[r];
        __syncwarp();
    }
}

/* Vertical pass: grid (ceil(width / 64), SS2C_BLUR_JOBS, 3), 64 threads, one
 * column each. */
__global__ void __launch_bounds__(SS2C_BLUR_V_BLOCK) ssimulacra2_blur_v(const Ss2cBlurArgs a)
{
    const unsigned col = blockIdx.x * blockDim.x + threadIdx.x;
    if (col >= a.width)
        return;
    const unsigned job = blockIdx.y;
    const size_t offset = (size_t)blockIdx.z * a.width * a.height + col;
    const float *__restrict__ in = a.pass[job] + offset;
    float *__restrict__ out = a.out[job] + offset;
    const int h = (int)a.height;
    const size_t w = a.width;

    Ss2cIir st = {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
#pragma unroll 4
    for (int n = 1 - a.radius; n < h; n++) {
        const int left = n - a.radius - 1;
        const int right = n + a.radius - 1;
        const float lv = (left >= 0) ? in[(size_t)left * w] : 0.0f;
        const float rv = (right < h) ? in[(size_t)right * w] : 0.0f;
        const float o = ss2c_iir_step(st, a, lv, rv);
        if (n >= 0)
            out[(size_t)n * w] = o;
    }
}

} /* extern "C" */
