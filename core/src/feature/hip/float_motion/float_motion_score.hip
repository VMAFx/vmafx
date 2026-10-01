/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP compute kernels for the float_motion feature extractor
 *  (T7-10b batch-2 / ADR-0373). Port of the CUDA twin at
 *  `core/src/feature/cuda/float_motion/float_motion_score.cu`, extended to
 *  the CPU float_motion options that need device work (ADR-1404).
 *
 *  Algorithm (must match CPU float_motion.c):
 *    1. Convert ref pixel: `val = (raw / scaler) - 128.0`.
 *    2. Separable blur, column then row, with the filter `motion_filter_size`
 *       selects (motion_blur_plane()): FILTER_5_s by default, FILTER_3_s for
 *       3, FILTER_5_NO_OP_s for 1.
 *    3. Frame > 0: |cur_blurred - prev_blurred| of every sample, stored by
 *       the blur kernel, and their sum, one fp32 sum per row
 *       (`float_motion_hip_row_sum`).
 *    4. `motion_add_scale1`: the same over both blurred frames scaled to half
 *       size with the bilinear scaler of motion.c
 *       (`float_motion_hip_scale1_diff`, then `float_motion_hip_row_sum`).
 *    `motion_add_uv` runs steps 1 to 4 once per plane; the host launches the
 *    same kernels with the chroma geometry.
 *
 *  Numerical contract (ADR-1407, ADR-1409). The blur is
 *  convolution_f32_c_s(): each tap one rounded fp32 multiply and one rounded
 *  fp32 add, taps in order; the HSACO builds without FMA contraction. The
 *  sums are float_sad_line() / vmaf_image_sad_c(): the absolute differences
 *  of a row added left to right into one fp32 accumulator, and the rows added
 *  top to bottom on the host (feature/float_motion_sad.h). A running fp32 sum
 *  rounds at every step, so the score depends on that order (up to 1.4e-4 at
 *  1920x1080): the row kernel runs one thread per row over the whole row,
 *  through float_motion_rows.h. A per-block partial sum, which this file used
 *  to produce, cannot give that value. The differences are stored transposed
 *  (float_motion_rows.h) so that the threads of a wave, which walk different
 *  rows, read consecutive memory at every step; read from the blurred planes
 *  the row sums took 145 ms per 3840x2160 frame on a gfx1036.
 *    Mirror padding: skip-boundary `2 * (sup - 1) - idx` (reflect-101), then
 *    clamped into the plane: the block loads a 20x20 tile for every thread,
 *    padding threads included, and one reflection of their halo leaves a
 *    plane smaller than the tile (index 33 of a 17-sample axis reflects to
 *    -1, and a 3-sample axis reaches -13), reading outside `ref`. The clamp is
 *    the identity for every sample an output consumes (hip_tile_index.h,
 *    ADR-1381), so no score changes.
 *
 *  Layout: the blur runs 16x16 work-groups with a 2-pixel halo (5-tap) and
 *  writes the blurred plane and the differences; the row kernel runs one
 *  thread per row, VMAF_HIP_FLOAT_MOTION_ROW_GROUP threads per block.
 *
 *  The 3-tap and the no-op filter run as 5 taps with zero outer weights: a
 *  zero weight times a finite sample adds 0, so the sums are the CPU's
 *  3-tap sums, and the tile and its halo stay the same for every filter.
 *
 *  HIP adaptation from CUDA:
 *  - Raw pointer parameters: the host passes `rb.device` as `float *row_sad`.
 *  - Entry-point names are `float_motion_hip_kernel_{8,16}bpc` (not
 *    `float_motion_kernel_{8,16}bpc`) so the Module API lookup in
 *    `float_motion_hip.c:fm_hip_module_load()` resolves correctly.
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include "../hip_tile_index.h"
#include "float_motion_rows.h"

#define FM_BX 16
#define FM_BY 16
#define FM_RADIUS 2
#define FM_TAPS 5
#define FM_TILE_W (FM_BX + 2 * FM_RADIUS)
#define FM_TILE_H (FM_BY + 2 * FM_RADIUS)

/* motion_tools.h FILTER_5_s, FILTER_3_s and FILTER_5_NO_OP_s; the 3-tap
 * filter sits in the middle of five taps. */
__device__ static const float FM_FILT[FM_TAPS] = {
    0.054488685f, 0.244201342f, 0.402619947f, 0.244201342f, 0.054488685f,
};
__device__ static const float FM_FILT_3[FM_TAPS] = {
    0.0f, 0.166378498f, 0.667243004f, 0.166378498f, 0.0f,
};
__device__ static const float FM_FILT_NO_OP[FM_TAPS] = {
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
};

namespace
{

/* The filter of CPU float_motion.c::motion_blur_plane() for
 * `motion_filter_size`: 1 is the no-op, 3 the 3-tap, anything else the
 * default 5-tap. */
__device__ __forceinline__ const float *fm_filter(unsigned filter_size)
{
    if (filter_size == 1u)
        return FM_FILT_NO_OP;
    return (filter_size == 3u) ? FM_FILT_3 : FM_FILT;
}

/* Tile-load index: the CPU's skip-boundary mirror (reflect-101), clamped into
 * the plane for the padding threads whose loads no output consumes. */
__device__ __forceinline__ int fm_tile_index(int idx, int sup)
{
    return vmaf_hip_tile_index(vmaf_hip_reflect_101(idx, sup), sup);
}

/* picture_copy() of one sample: the raw value over its bit-depth scaler,
 * minus 128. The scalers are powers of two, so the reciprocal is exact. */
template <typename Sample>
__device__ __forceinline__ float fm_sample(const uint8_t *row, int gx, float inv_scaler)
{
    return (float)reinterpret_cast<const Sample *>(row)[gx] * inv_scaler - 128.0f;
}

/* Collaborative tile load with mirror padding: every thread of the block
 * loads its share of the 20x20 tile. */
template <typename Sample>
__device__ __forceinline__ void fm_load_tile(float tile[FM_TILE_H][FM_TILE_W], const uint8_t *ref,
                                             ptrdiff_t ref_stride, unsigned width, unsigned height,
                                             float inv_scaler)
{
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    const int tile_ox = (int)(blockIdx.x * FM_BX) - FM_RADIUS;
    const int tile_oy = (int)(blockIdx.y * FM_BY) - FM_RADIUS;
    const unsigned tile_elems = FM_TILE_W * FM_TILE_H;
    const unsigned wg_size = FM_BX * FM_BY;

    for (unsigned i = lid; i < tile_elems; i += wg_size) {
        const unsigned tr = i / FM_TILE_W;
        const unsigned tc = i % FM_TILE_W;
        const int gx = fm_tile_index(tile_ox + (int)tc, (int)width);
        const int gy = fm_tile_index(tile_oy + (int)tr, (int)height);
        tile[tr][tc] = fm_sample<Sample>(ref + gy * ref_stride, gx, inv_scaler);
    }
}

/* Separable blur of this thread's pixel from the tile, column then row. */
__device__ __forceinline__ float fm_blur_pixel(const float tile[FM_TILE_H][FM_TILE_W],
                                               const float *filt)
{
    float blurred = 0.0f;
#pragma unroll
    for (int xf = 0; xf < FM_TAPS; xf++) {
        float v = 0.0f;
#pragma unroll
        for (int yf = 0; yf < FM_TAPS; yf++)
            v += filt[yf] * tile[threadIdx.y + yf][threadIdx.x + xf];
        blurred += filt[xf] * v;
    }
    return blurred;
}

/* Blur body, shared by the 8-bit and the 16-bit entry point: blurs the plane
 * into `cur_blur` and, when `compute_sad` is set, stores
 * |cur_blur - prev_blur| of every sample in the transposed plane `diff`. */
template <typename Sample>
__device__ __forceinline__ void fm_blur(const uint8_t *ref, ptrdiff_t ref_stride, float *cur_blur,
                                        const float *prev_blur, float *diff, unsigned width,
                                        unsigned height, float inv_scaler, unsigned filter_size,
                                        unsigned compute_sad)
{
    __shared__ float s_tile[FM_TILE_H][FM_TILE_W];
    fm_load_tile<Sample>(s_tile, ref, ref_stride, width, height, inv_scaler);
    __syncthreads();

    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;
    const float blurred = fm_blur_pixel(s_tile, fm_filter(filter_size));
    const size_t off = (size_t)y * width + (size_t)x;
    cur_blur[off] = blurred;
    if (compute_sad != 0u) {
        diff[vmaf_hip_float_motion_diff_index(x, y, width)] =
            vmaf_hip_float_motion_abs_diff(blurred, prev_blur[off]);
    }
}

} /* namespace */

extern "C" {

/**
 * 8-bpc kernel: ref is uint8_t row-major with ref_stride bytes per row.
 *
 * Blurs the plane into `cur_blur` with the filter of `filter_size` and, when
 * `compute_sad != 0`, stores `|cur_blur - prev_blur|` of every sample in the
 * transposed plane `diff` for float_motion_hip_row_sum.
 */
__global__ void float_motion_hip_kernel_8bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                             float *__restrict__ cur_blur,
                                             const float *__restrict__ prev_blur,
                                             float *__restrict__ diff, unsigned width,
                                             unsigned height, unsigned filter_size,
                                             unsigned compute_sad)
{
    fm_blur<uint8_t>(ref, ref_stride, cur_blur, prev_blur, diff, width, height, 1.0f, filter_size,
                     compute_sad);
}

/**
 * 10/12/16-bpc kernel: ref is byte-addressed; each pixel is uint16_t.
 * Scale factors: 10bpc=4, 12bpc=16, 16bpc=256.
 * All other algorithm details are identical to the 8-bpc kernel.
 */
__global__ void float_motion_hip_kernel_16bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                              float *__restrict__ cur_blur,
                                              const float *__restrict__ prev_blur,
                                              float *__restrict__ diff, unsigned width,
                                              unsigned height, unsigned bpc, unsigned filter_size,
                                              unsigned compute_sad)
{
    float scaler = 1.0f;
    if (bpc == 10u)
        scaler = 4.0f;
    else if (bpc == 12u)
        scaler = 16.0f;
    else if (bpc == 16u)
        scaler = 256.0f;
    fm_blur<uint16_t>(ref, ref_stride, cur_blur, prev_blur, diff, width, height, 1.0f / scaler,
                      filter_size, compute_sad);
}

/**
 * `motion_add_scale1`: the differences of the scale-1 term of
 * motion.c::vmaf_image_sad_c(). Both blurred frames (`width` x `height`,
 * packed) are scaled to `scaled_width` x `scaled_height` with
 * motion_scale_bilinear(), one thread per scaled sample, and |cur - prev| is
 * stored in the transposed plane `diff`. Launched over the scaled plane,
 * after the blur kernel of the same frame.
 */
__global__ void float_motion_hip_scale1_diff(const float *__restrict__ cur_blur,
                                             const float *__restrict__ prev_blur,
                                             float *__restrict__ diff, unsigned width,
                                             unsigned height, unsigned scaled_width,
                                             unsigned scaled_height)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= scaled_width || y >= scaled_height)
        return;
    const VmafHipFloatMotionScale1 scale1 = {cur_blur, prev_blur,    width,
                                             height,   scaled_width, scaled_height};
    diff[vmaf_hip_float_motion_diff_index(x, y, scaled_width)] =
        vmaf_hip_float_motion_scale1_abs_diff(&scale1, x, y);
}

/**
 * float_sad_line() of every row of a `width` x `height` plane: one thread per
 * row adds the row's absolute differences, left to right, into one fp32
 * accumulator. `diff` is the transposed plane the blur or the scale-1 kernel
 * wrote; `row_sad` receives `height` sums. Launched with
 * VMAF_HIP_FLOAT_MOTION_ROW_GROUP threads per block, one block per group of
 * rows.
 */
__global__ void float_motion_hip_row_sum(const float *__restrict__ diff,
                                         float *__restrict__ row_sad, unsigned width,
                                         unsigned height)
{
    const unsigned y = blockIdx.x * blockDim.x + threadIdx.x;
    if (y >= height)
        return;
    row_sad[y] = vmaf_hip_float_motion_row_sum(diff, width, y);
}

} /* extern "C" */
