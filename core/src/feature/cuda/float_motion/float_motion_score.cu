/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernels for the float_motion feature extractor
 *  (T7-23 / batch 3 part 4b — ADR-0192 / ADR-0196). Float-domain blur + SAD.
 *
 *  Algorithm (float_motion.c, default options):
 *   1. Convert the reference pixel: `val = (raw / scaler) - 128.0`.
 *   2. 5x5 separable Gaussian blur with FILTER_5_s, vertical pass then
 *      horizontal pass, mirror padding `2 * (sup - 1) - idx`.
 *   3. Frame > 0: sum of absolute differences against the previous blur.
 *
 *  Numerical contract (ADR-1403, ADR-1409). Both steps are the CPU's
 *  arithmetic, so the twin returns the CPU extractor's score bit for bit:
 *   - the blur is convolution_f32_c_s(): each tap one rounded fp32 multiply
 *     and one rounded fp32 add, taps in order. The fatbin builds with
 *     --fmad=false, so nothing is fused.
 *   - the SAD is compute_motion_simd(): float_sad_line() adds the absolute
 *     differences of one row, left to right, into one fp32 accumulator, and
 *     the rows are added top to bottom into another. The result depends on
 *     that order (at 1920x1080 it is up to 1.4e-4 from the exact sum), so
 *     float_motion_row_sad runs one thread per row over the whole row, and
 *     the host adds the rows (feature/float_motion_sad.h). A per-block
 *     partial sum, which this file used to produce, cannot give that value.
 */

#include "cuda_helper.cuh"
#include "common.h"
#include "cuda/cuda_tile_index.h"

#define FM_BX 16
#define FM_BY 16
#define FM_RADIUS 2
#define FM_TILE_W (FM_BX + 2 * FM_RADIUS)
#define FM_TILE_H (FM_BY + 2 * FM_RADIUS)
#define FM_ROW_THREADS 128

namespace
{

__device__ const float FM_FILT[5] = {
    0.054488685f, 0.244201342f, 0.402619947f, 0.244201342f, 0.054488685f,
};

/* Tile-load index: the CPU's skip-boundary mirror (reflect-101), clamped into
 * the plane for the padding loads no output consumes. A plane narrower or
 * shorter than one tile (3 to 9 samples, or 17) reflected some of those loads
 * to a negative index, a read before the plane; the HIP twin clamps them the
 * same way (fm_tile_index()). */
__device__ __forceinline__ int fm_mirror(int idx, int sup)
{
    return vmaf_cuda_tile_index(vmaf_cuda_reflect_101(idx, sup), sup);
}

/* picture_copy() of an 8-bit sample. */
struct FmSample8 {
    const uint8_t *plane;
    ptrdiff_t stride;
};

__device__ __forceinline__ float fm_sample(const FmSample8 &s, int gx, int gy)
{
    return (float)s.plane[gy * s.stride + gx] - 128.0f;
}

/* picture_copy() of a 10-, 12- or 16-bit sample. */
struct FmSample16 {
    const uint8_t *plane;
    ptrdiff_t stride;
    float inv_scaler;
};

__device__ __forceinline__ float fm_sample(const FmSample16 &s, int gx, int gy)
{
    const uint16_t r = reinterpret_cast<const uint16_t *>(s.plane + gy * s.stride)[gx];
    return (float)r * s.inv_scaler - 128.0f;
}

/* Fill this block's (16 + 4) x (16 + 4) tile of converted samples, mirrored
 * at the frame edges. Every thread of the block loads its share. */
template <typename Sample>
__device__ __forceinline__ void fm_load_tile(float (*tile)[FM_TILE_W], const Sample &sample,
                                             unsigned width, unsigned height)
{
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    const int tile_ox = blockIdx.x * FM_BX - FM_RADIUS;
    const int tile_oy = blockIdx.y * FM_BY - FM_RADIUS;
    for (unsigned i = lid; i < FM_TILE_W * FM_TILE_H; i += FM_BX * FM_BY) {
        const unsigned tr = i / FM_TILE_W;
        const unsigned tc = i % FM_TILE_W;
        const int gx = fm_mirror(tile_ox + (int)tc, (int)width);
        const int gy = fm_mirror(tile_oy + (int)tr, (int)height);
        tile[tr][tc] = fm_sample(sample, gx, gy);
    }
}

/* This thread's blurred pixel: the vertical pass of each of the five columns,
 * then the horizontal pass over them, as convolution_f32_c_s() does. */
__device__ __forceinline__ float fm_blur_pixel(const float (*tile)[FM_TILE_W])
{
    const unsigned lx = threadIdx.x + FM_RADIUS;
    const unsigned ly = threadIdx.y + FM_RADIUS;
    float blurred = 0.0f;
#pragma unroll
    for (int xf = 0; xf < 5; xf++) {
        float v = 0.0f;
#pragma unroll
        for (int yf = 0; yf < 5; yf++) {
            v += FM_FILT[yf] * tile[ly - FM_RADIUS + yf][lx - FM_RADIUS + xf];
        }
        blurred += FM_FILT[xf] * v;
    }
    return blurred;
}

template <typename Sample>
__device__ __forceinline__ void fm_blur_block(float (*tile)[FM_TILE_W], const Sample &sample,
                                              float *__restrict__ cur_blur, unsigned width,
                                              unsigned height)
{
    fm_load_tile(tile, sample, width, height);
    __syncthreads();
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x < width && y < height) {
        cur_blur[(size_t)y * width + (size_t)x] = fm_blur_pixel(tile);
    }
}

} /* namespace */

extern "C" {

__global__ void float_motion_kernel_8bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                         float *__restrict__ cur_blur, unsigned width,
                                         unsigned height)
{
    __shared__ float s_tile[FM_TILE_H][FM_TILE_W];
    FmSample8 sample;
    sample.plane = ref;
    sample.stride = ref_stride;
    fm_blur_block(s_tile, sample, cur_blur, width, height);
}

__global__ void float_motion_kernel_16bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                          float *__restrict__ cur_blur, unsigned width,
                                          unsigned height, unsigned bpc)
{
    __shared__ float s_tile[FM_TILE_H][FM_TILE_W];
    float scaler = 1.0f;
    if (bpc == 10) {
        scaler = 4.0f;
    } else if (bpc == 12) {
        scaler = 16.0f;
    } else if (bpc == 16) {
        scaler = 256.0f;
    }
    FmSample16 sample;
    sample.plane = ref;
    sample.stride = ref_stride;
    sample.inv_scaler = 1.0f / scaler;
    fm_blur_block(s_tile, sample, cur_blur, width, height);
}

/* float_sad_line() of every row: one thread adds the absolute differences of
 * its row, left to right, into one fp32 accumulator. Launched with one thread
 * per row; `row_sad` receives `height` sums. */
__global__ void __launch_bounds__(FM_ROW_THREADS)
    float_motion_row_sad(const float *__restrict__ cur_blur, const float *__restrict__ prev_blur,
                         float *__restrict__ row_sad, unsigned width, unsigned height)
{
    const unsigned y = blockIdx.x * blockDim.x + threadIdx.x;
    if (y >= height) {
        return;
    }
    const float *cur = cur_blur + (size_t)y * width;
    const float *prev = prev_blur + (size_t)y * width;
    float accum = 0.0f;
    for (unsigned j = 0; j < width; j++) {
        const float diff = cur[j] - prev[j];
        accum += diff < 0.0f ? -diff : diff;
    }
    row_sad[y] = accum;
}

} /* extern "C" */
