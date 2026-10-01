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
 *    3. Frame > 0: SAD += |cur_blurred - prev_blurred|.
 *    4. `motion_add_scale1`: a second SAD over both blurred frames scaled to
 *       half size with the bilinear scaler of motion.c
 *       (`float_motion_hip_scale1_sad`).
 *    `motion_add_uv` runs steps 1 to 4 once per plane; the host launches the
 *    same kernels with the chroma geometry.
 *    Mirror padding: skip-boundary `2 * (sup - 1) - idx` (reflect-101), then
 *    clamped into the plane: the block loads a 20x20 tile for every thread,
 *    padding threads included, and one reflection of their halo leaves a
 *    plane smaller than the tile (index 33 of a 17-sample axis reflects to
 *    -1, and a 3-sample axis reaches -13), reading outside `ref`. The clamp is
 *    the identity for every sample an output consumes (hip_tile_index.h,
 *    ADR-1381), so no score changes.
 *
 *  Layout: 16x16 work-group with 2-pixel halo (5-tap), produces a float
 *  per-pixel blurred output and one float per-block SAD partial.
 *  Host accumulates partials in `double` and divides by (w*h).
 *
 *  The 3-tap and the no-op filter run as 5 taps with zero outer weights: a
 *  zero weight times a finite sample adds 0, so the sums are the CPU's
 *  3-tap sums, and the tile and its halo stay the same for every filter.
 *
 *  HIP adaptation from CUDA:
 *  - GCN/RDNA1 warp size is 64; RDNA2+ (gfx1030/gfx1100/gfx1101) is 32.
 *    The shared-memory array for warp partial sums is sized for the minimum
 *    warp size (32) so it is large enough on wave32: FM_BX*FM_BY/32 = 8 slots.
 *    At runtime `warpSize` is used for lane/warp_id and loop bounds.
 *  - `__shfl_down` used without mask (HIP convention; CUDA uses
 *    `__shfl_down_sync(0xffffffff, ...)`).
 *  - `VmafCudaBuffer sad` replaced by plain `float *partials` since the
 *    HIP host passes `rb.device` as a raw pointer.
 *  - Entry-point names are `float_motion_hip_kernel_{8,16}bpc` (not
 *    `float_motion_kernel_{8,16}bpc`) so the Module API lookup in
 *    `float_motion_hip.c:fm_hip_module_load()` resolves correctly.
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#include "../hip_tile_index.h"

#define FM_BX 16
#define FM_BY 16
#define FM_RADIUS 2
#define FM_TAPS 5
#define FM_TILE_W (FM_BX + 2 * FM_RADIUS)
#define FM_TILE_H (FM_BY + 2 * FM_RADIUS)

/* Minimum HIP warp size across supported AMD targets (wave32 = RDNA2+).
 * Array sized for the minimum so it is large enough on wave32 hardware.
 * Runtime `warpSize` is used for the reduction loop and lane/warp_id. */
#define FM_MIN_WARP_SIZE 32
#define FM_WARPS_PER_BLOCK (FM_BX * FM_BY / FM_MIN_WARP_SIZE)

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

/* HIP warp-level float reduction — uses warpSize at runtime to handle
 * both wave64 (GCN/RDNA1, warpSize=64) and wave32 (RDNA2+, warpSize=32). */
__device__ __forceinline__ float fm_warp_reduce(float v)
{
    for (int off = (int)warpSize / 2; off > 0; off >>= 1)
        v += __shfl_down(v, off);
    return v;
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

/* Reduce one float per thread to partials[block]: warp reduce, then the warp
 * sums in shared memory. Every thread of the block must call it. */
__device__ __forceinline__ void fm_block_reduce(float value, float *partials)
{
    __shared__ float s_warp[FM_WARPS_PER_BLOCK];
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;
    const unsigned ws = (unsigned)warpSize;
    const float w = fm_warp_reduce(value);
    if (lid % ws == 0u)
        s_warp[lid / ws] = w;
    __syncthreads();
    if (lid != 0u)
        return;

    float total = 0.0f;
    const unsigned warps_active = (FM_BX * FM_BY) / ws;
    for (unsigned i = 0; i < warps_active; i++)
        total += s_warp[i];
    partials[blockIdx.y * gridDim.x + blockIdx.x] = total;
}

/* Blur + SAD body, shared by the 8-bit and the 16-bit entry point: blurs the
 * plane into `cur_blur` and, when `compute_sad` is set, reduces
 * |cur_blur - prev_blur| to one float per block; otherwise the block's
 * partial is 0. */
template <typename Sample>
__device__ __forceinline__ void
fm_blur_sad(const uint8_t *ref, ptrdiff_t ref_stride, float *cur_blur, const float *prev_blur,
            float *partials, unsigned width, unsigned height, float inv_scaler,
            unsigned filter_size, unsigned compute_sad)
{
    __shared__ float s_tile[FM_TILE_H][FM_TILE_W];
    fm_load_tile<Sample>(s_tile, ref, ref_stride, width, height, inv_scaler);
    __syncthreads();

    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    float abs_diff = 0.0f;
    if (x < width && y < height) {
        const float blurred = fm_blur_pixel(s_tile, fm_filter(filter_size));
        const size_t off = (size_t)y * width + (size_t)x;
        cur_blur[off] = blurred;
        if (compute_sad != 0u) {
            const float diff = blurred - prev_blur[off];
            abs_diff = diff < 0.0f ? -diff : diff;
        }
    }
    fm_block_reduce(abs_diff, partials);
}

/* motion.c::motion_mirror_f() for a non-negative right edge. */
__device__ __forceinline__ float fm_reflect_f(float i, float right)
{
    if (i < 0.0f)
        return -i;
    return (i > right) ? 2.0f * right - i : i;
}

/* motion.c::motion_bilinear_interp(): the sample of a `width` x `height`
 * plane at (x, y), every operand in the CPU's order. Contraction is off so
 * that the four products and their sum round as on the CPU. */
__device__ __forceinline__ float fm_bilinear(const float *src, unsigned width, unsigned height,
                                             float x, float y)
{
#pragma clang fp contract(off)
    const float right = (float)(width - 1u);
    const float bottom = (float)(height - 1u);
    const int x1 = (int)fm_reflect_f(floorf(x), right);
    const int x2 = (int)fm_reflect_f(ceilf(x), right);
    const int y1 = (int)fm_reflect_f(floorf(y), bottom);
    const int y2 = (int)fm_reflect_f(ceilf(y), bottom);
    const float dx = x - (float)x1;
    const float dy = y - (float)y1;
    const size_t row1 = (size_t)y1 * width;
    const size_t row2 = (size_t)y2 * width;
    return ((1.0f - dy) * (1.0f - dx) * src[row1 + (size_t)x1] +
            (1.0f - dy) * dx * src[row1 + (size_t)x2] + dy * (1.0f - dx) * src[row2 + (size_t)x1] +
            dy * dx * src[row2 + (size_t)x2]);
}

} /* namespace */

extern "C" {

/**
 * 8-bpc kernel: ref is uint8_t row-major with ref_stride bytes per row.
 *
 * Blurs the plane into `cur_blur` with the filter of `filter_size` and, when
 * `compute_sad != 0`, accumulates per-pixel `|cur_blur - prev_blur|` into
 * one float per block in `partials[block_idx]`.
 */
__global__ void float_motion_hip_kernel_8bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                             float *__restrict__ cur_blur,
                                             const float *__restrict__ prev_blur,
                                             float *__restrict__ partials, unsigned width,
                                             unsigned height, unsigned filter_size,
                                             unsigned compute_sad)
{
    fm_blur_sad<uint8_t>(ref, ref_stride, cur_blur, prev_blur, partials, width, height, 1.0f,
                         filter_size, compute_sad);
}

/**
 * 10/12/16-bpc kernel: ref is byte-addressed; each pixel is uint16_t.
 * Scale factors: 10bpc=4, 12bpc=16, 16bpc=256.
 * All other algorithm details are identical to the 8-bpc kernel.
 */
__global__ void float_motion_hip_kernel_16bpc(const uint8_t *__restrict__ ref, ptrdiff_t ref_stride,
                                              float *__restrict__ cur_blur,
                                              const float *__restrict__ prev_blur,
                                              float *__restrict__ partials, unsigned width,
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
    fm_blur_sad<uint16_t>(ref, ref_stride, cur_blur, prev_blur, partials, width, height,
                          1.0f / scaler, filter_size, compute_sad);
}

/**
 * `motion_add_scale1`: the scale-1 term of motion.c::vmaf_image_sad_c(). Both
 * blurred frames (`width` x `height`, packed) are scaled to `scaled_width` x
 * `scaled_height` with motion_scale_bilinear() and |cur - prev| is reduced to
 * one float per block. Launched over the scaled plane, after the blur kernel
 * of the same frame.
 */
__global__ void float_motion_hip_scale1_sad(const float *__restrict__ cur_blur,
                                            const float *__restrict__ prev_blur,
                                            float *__restrict__ partials, unsigned width,
                                            unsigned height, unsigned scaled_width,
                                            unsigned scaled_height)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    float abs_diff = 0.0f;
    if (x < scaled_width && y < scaled_height) {
#pragma clang fp contract(off)
        const float ratio_x = (float)width / (float)scaled_width;
        const float ratio_y = (float)height / (float)scaled_height;
        const float xx = ((float)x + 0.5f) * ratio_x - 0.5f;
        const float yy = ((float)y + 0.5f) * ratio_y - 0.5f;
        const float diff = fm_bilinear(cur_blur, width, height, xx, yy) -
                           fm_bilinear(prev_blur, width, height, xx, yy);
        abs_diff = diff < 0.0f ? -diff : diff;
    }
    fm_block_reduce(abs_diff, partials);
}

} /* extern "C" */
