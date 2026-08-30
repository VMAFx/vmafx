/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  HIP compute kernel for the ciede2000 feature extractor (T7-10b
 *  batch-4 / ADR-0377). Direct port of the CUDA twin at
 *  `libvmaf/src/feature/cuda/integer_ciede/ciede_score.cu`.
 *
 *  Per-pixel: convert YUV → L*a*b* (BT.709 primaries), compute CIEDE2000
 *  ΔE between ref and dis, reduce per warp, lane 0 writes warp sum into a
 *  per-block float in the `partials` array; host accumulates in double and
 *  applies `45 - 20 * log10(mean_dE)`.
 *
 *  HIP adaptation from CUDA:
 *  - Warp size varies: 64 on GCN/RDNA1 (wave64), 32 on RDNA2+ (wave32).
 *    `warpSize` is read at runtime; `__shfl_down` (no mask argument).
 *  - `s_warp_sums` shared array sized BLOCK_X * BLOCK_Y / CIEDE_MIN_WARP_SIZE
 *    = 256/32 = 8 (worst case for wave32, safely over-allocated for wave64).
 *  - `partials` is a plain `float *` (not VmafCudaBuffer).
 *  - No `cuda_helper.cuh`; include `<hip/hip_runtime.h>` directly.
 *
 *  Float per-pixel math (matches CUDA twin; empirical floor places=4 on
 *  real hardware — ADR-0187).
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#define BLOCK_X 16
#define BLOCK_Y 16
/* Minimum HIP warp size across supported targets (32 on RDNA2+, 64 on GCN/RDNA1).
 * Shared memory is sized for the worst case (wave32 = most warps per block).
 * The runtime `warpSize` device variable is used in reduction loops and lane
 * detection so the same binary runs correctly on both wave32 and wave64. */
#define CIEDE_MIN_WARP_SIZE 32
/* Warps per block = 256 / 32 = 8 (wave32 worst case). */
#define CIEDE_WARPS_PER_BLOCK (BLOCK_X * BLOCK_Y / CIEDE_MIN_WARP_SIZE)

__device__ __forceinline__ float pow_pos_2_4(float x)
{
    return powf(x, 2.4f);
}

__device__ __forceinline__ float srgb_to_linear(float c)
{
    if (c > 10.0f / 255.0f) {
        const float A = 0.055f;
        const float D = 1.0f / 1.055f;
        return pow_pos_2_4((c + A) * D);
    }
    return c / 12.92f;
}

__device__ __forceinline__ float xyz_to_lab_map(float t)
{
    if (t > 0.008856f)
        return cbrtf(t);
    return 7.787f * t + (16.0f / 116.0f);
}

__device__ __forceinline__ void yuv_to_lab(float y_lim, float u_lim, float v_lim, unsigned bpc,
                                           float *L, float *A, float *B)
{
    float scale = 1.0f;
    if (bpc == 10u)
        scale = 4.0f;
    else if (bpc == 12u)
        scale = 16.0f;
    else if (bpc == 16u)
        scale = 256.0f;
    float y = (y_lim - 16.0f * scale) * (1.0f / (219.0f * scale));
    float u = (u_lim - 128.0f * scale) * (1.0f / (224.0f * scale));
    float v = (v_lim - 128.0f * scale) * (1.0f / (224.0f * scale));
    float r = y + 1.28033f * v;
    float g = y - 0.21482f * u - 0.38059f * v;
    float b = y + 2.12798f * u;
    r = srgb_to_linear(r);
    g = srgb_to_linear(g);
    b = srgb_to_linear(b);
    float x = r * 0.4124564390896921f + g * 0.357576077643909f + b * 0.18043748326639894f;
    float yy = r * 0.21267285140562248f + g * 0.715152155287818f + b * 0.07217499330655958f;
    float z = r * 0.019333895582329317f + g * 0.119192025881303f + b * 0.9503040785363677f;
    x *= 1.0f / 0.95047f;
    z *= 1.0f / 1.08883f;
    float lx = xyz_to_lab_map(x);
    float ly = xyz_to_lab_map(yy);
    float lz = xyz_to_lab_map(z);
    *L = 116.0f * ly - 16.0f;
    *A = 500.0f * (lx - ly);
    *B = 200.0f * (ly - lz);
}

__device__ __forceinline__ float get_h_prime_dev(float b, float a)
{
    if (b == 0.0f && a == 0.0f)
        return 0.0f;
    float h = atan2f(b, a);
    if (h < 0.0f)
        h += 6.283185307179586f;
    return h * 180.0f / 3.141592653589793f;
}

__device__ __forceinline__ float get_delta_h_prime_dev(float c1, float c2, float h1, float h2)
{
    if (c1 * c2 == 0.0f)
        return 0.0f;
    float diff = h2 - h1;
    if (fabsf(diff) <= 180.0f)
        return diff * 3.141592653589793f / 180.0f;
    if (diff > 180.0f)
        return (diff - 360.0f) * 3.141592653589793f / 180.0f;
    return (diff + 360.0f) * 3.141592653589793f / 180.0f;
}

__device__ __forceinline__ float get_upcase_h_bar_prime_dev(float h1, float h2)
{
    float diff = fabsf(h1 - h2);
    if (diff > 180.0f)
        return ((h1 + h2 + 360.0f) / 2.0f) * 3.141592653589793f / 180.0f;
    return ((h1 + h2) / 2.0f) * 3.141592653589793f / 180.0f;
}

__device__ __forceinline__ float get_upcase_t_dev(float h_bar)
{
    return 1.0f - 0.17f * cosf(h_bar - 3.141592653589793f / 6.0f) +
           0.24f * cosf(2.0f * h_bar) +
           0.32f * cosf(3.0f * h_bar + 3.141592653589793f / 30.0f) -
           0.20f * cosf(4.0f * h_bar - 63.0f * 3.141592653589793f / 180.0f);
}

__device__ __forceinline__ float get_r_sub_t_dev(float c_bar, float h_bar)
{
    float exponent = -powf((h_bar * 180.0f / 3.141592653589793f - 275.0f) / 25.0f, 2.0f);
    float c7 = powf(c_bar, 7.0f);
    float r_c = 2.0f * sqrtf(c7 / (c7 + powf(25.0f, 7.0f)));
    return -sinf(60.0f * 3.141592653589793f / 180.0f * expf(exponent)) * r_c;
}

__device__ __forceinline__ float ciede2000_dev(float l1, float a1, float b1, float l2, float a2,
                                               float b2)
{
    const float k_l = 0.65f;
    const float k_c = 1.0f;
    const float k_h = 4.0f;
    float dl_p = l2 - l1;
    float l_bar = 0.5f * (l1 + l2);
    float c1 = sqrtf(a1 * a1 + b1 * b1);
    float c2 = sqrtf(a2 * a2 + b2 * b2);
    float c_bar = 0.5f * (c1 + c2);
    float c_bar_7 = powf(c_bar, 7.0f);
    float g_factor = 1.0f - sqrtf(c_bar_7 / (c_bar_7 + powf(25.0f, 7.0f)));
    float a1_p = a1 + 0.5f * a1 * g_factor;
    float a2_p = a2 + 0.5f * a2 * g_factor;
    float c1_p = sqrtf(a1_p * a1_p + b1 * b1);
    float c2_p = sqrtf(a2_p * a2_p + b2 * b2);
    float c_bar_p = 0.5f * (c1_p + c2_p);
    float dc_p = c2_p - c1_p;
    float dl2 = (l_bar - 50.0f) * (l_bar - 50.0f);
    float s_l = 1.0f + (0.015f * dl2) / sqrtf(20.0f + dl2);
    float s_c = 1.0f + 0.045f * c_bar_p;
    float h1_p = get_h_prime_dev(b1, a1_p);
    float h2_p = get_h_prime_dev(b2, a2_p);
    float dh_p = get_delta_h_prime_dev(c1, c2, h1_p, h2_p);
    float dH_p = 2.0f * sqrtf(c1_p * c2_p) * sinf(dh_p / 2.0f);
    float H_bar_p = get_upcase_h_bar_prime_dev(h1_p, h2_p);
    float t_term = get_upcase_t_dev(H_bar_p);
    float s_h = 1.0f + 0.015f * c_bar_p * t_term;
    float r_t = get_r_sub_t_dev(c_bar_p, H_bar_p);
    float lightness = dl_p / (k_l * s_l);
    float chroma = dc_p / (k_c * s_c);
    float hue = dH_p / (k_h * s_h);
    return sqrtf(lightness * lightness + chroma * chroma + hue * hue + r_t * chroma * hue);
}

__device__ __forceinline__ float ciede_warp_reduce(float v)
{
    /* HIP __shfl_down — mask not needed. Uses runtime warpSize to handle
     * wave64 (GCN/RDNA1, warpSize=64) and wave32 (RDNA2+, warpSize=32). */
    for (int off = (int)warpSize / 2; off > 0; off >>= 1)
        v += __shfl_down(v, off);
    return v;
}

extern "C" {

/**
 * 8-bpc kernel: ref/dis Y/U/V planes are uint8_t * with separate strides.
 * Chroma subsampling controlled by ss_hor/ss_ver (0 = 4:4:4, 1 = subsampled).
 * Writes one float partial per block into partials[block_idx].
 */
__global__ void calculate_ciede_kernel_8bpc(
    const uint8_t *__restrict__ ref_y, ptrdiff_t ref_y_stride,
    const uint8_t *__restrict__ ref_u, ptrdiff_t ref_u_stride,
    const uint8_t *__restrict__ ref_v, ptrdiff_t ref_v_stride,
    const uint8_t *__restrict__ dis_y, ptrdiff_t dis_y_stride,
    const uint8_t *__restrict__ dis_u, ptrdiff_t dis_u_stride,
    const uint8_t *__restrict__ dis_v, ptrdiff_t dis_v_stride,
    float *__restrict__ partials, unsigned width, unsigned height,
    unsigned bpc, unsigned ss_hor, unsigned ss_ver)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    float my_de = 0.0f;
    if (x < (int)width && y < (int)height) {
        const unsigned cx = ss_hor ? ((unsigned)x >> 1) : (unsigned)x;
        const unsigned cy = ss_ver ? ((unsigned)y >> 1) : (unsigned)y;
        float l1, a1, b1, l2, a2, b2;
        yuv_to_lab((float)ref_y[y * ref_y_stride + x],
                   (float)ref_u[cy * ref_u_stride + cx],
                   (float)ref_v[cy * ref_v_stride + cx],
                   bpc, &l1, &a1, &b1);
        yuv_to_lab((float)dis_y[y * dis_y_stride + x],
                   (float)dis_u[cy * dis_u_stride + cx],
                   (float)dis_v[cy * dis_v_stride + cx],
                   bpc, &l2, &a2, &b2);
        my_de = ciede2000_dev(l1, a1, b1, l2, a2, b2);
    }

    /* Per-block partial sum — host accumulates in double for places=4
     * (ADR-0187). Warp-64 reduction then block aggregation. */
    __shared__ float s_warp_sums[CIEDE_WARPS_PER_BLOCK];
    float warp_sum = ciede_warp_reduce(my_de);
    const unsigned lane = lid % (unsigned)warpSize;
    const unsigned warp_id = lid / (unsigned)warpSize;
    if (lane == 0)
        s_warp_sums[warp_id] = warp_sum;
    __syncthreads();
    if (lid == 0) {
        float block_sum = 0.0f;
        const int nwarps = (int)(blockDim.x * blockDim.y) / (int)warpSize;
        for (int i = 0; i < nwarps; i++)
            block_sum += s_warp_sums[i];
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        partials[block_idx] = block_sum;
    }
}

/**
 * 10/12/16-bpc kernel: ref/dis Y/U/V planes are uint16_t (via uint8_t *
 * byte-addressed row pointers). Same reduction shape as 8-bpc kernel.
 */
__global__ void calculate_ciede_kernel_16bpc(
    const uint8_t *__restrict__ ref_y, ptrdiff_t ref_y_stride,
    const uint8_t *__restrict__ ref_u, ptrdiff_t ref_u_stride,
    const uint8_t *__restrict__ ref_v, ptrdiff_t ref_v_stride,
    const uint8_t *__restrict__ dis_y, ptrdiff_t dis_y_stride,
    const uint8_t *__restrict__ dis_u, ptrdiff_t dis_u_stride,
    const uint8_t *__restrict__ dis_v, ptrdiff_t dis_v_stride,
    float *__restrict__ partials, unsigned width, unsigned height,
    unsigned bpc, unsigned ss_hor, unsigned ss_ver)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const unsigned lid = threadIdx.y * blockDim.x + threadIdx.x;

    float my_de = 0.0f;
    if (x < (int)width && y < (int)height) {
        const unsigned cx = ss_hor ? ((unsigned)x >> 1) : (unsigned)x;
        const unsigned cy = ss_ver ? ((unsigned)y >> 1) : (unsigned)y;
        const uint16_t ry = reinterpret_cast<const uint16_t *>(ref_y + y * ref_y_stride)[x];
        const uint16_t ru = reinterpret_cast<const uint16_t *>(ref_u + cy * ref_u_stride)[cx];
        const uint16_t rv = reinterpret_cast<const uint16_t *>(ref_v + cy * ref_v_stride)[cx];
        const uint16_t dy = reinterpret_cast<const uint16_t *>(dis_y + y * dis_y_stride)[x];
        const uint16_t du = reinterpret_cast<const uint16_t *>(dis_u + cy * dis_u_stride)[cx];
        const uint16_t dv = reinterpret_cast<const uint16_t *>(dis_v + cy * dis_v_stride)[cx];
        float l1, a1, b1, l2, a2, b2;
        yuv_to_lab((float)ry, (float)ru, (float)rv, bpc, &l1, &a1, &b1);
        yuv_to_lab((float)dy, (float)du, (float)dv, bpc, &l2, &a2, &b2);
        my_de = ciede2000_dev(l1, a1, b1, l2, a2, b2);
    }

    __shared__ float s_warp_sums[CIEDE_WARPS_PER_BLOCK];
    float warp_sum = ciede_warp_reduce(my_de);
    const unsigned lane = lid % (unsigned)warpSize;
    const unsigned warp_id = lid / (unsigned)warpSize;
    if (lane == 0)
        s_warp_sums[warp_id] = warp_sum;
    __syncthreads();
    if (lid == 0) {
        float block_sum = 0.0f;
        const int nwarps = (int)(blockDim.x * blockDim.y) / (int)warpSize;
        for (int i = 0; i < nwarps; i++)
            block_sum += s_warp_sums[i];
        const unsigned block_idx = blockIdx.y * gridDim.x + blockIdx.x;
        partials[block_idx] = block_sum;
    }
}

} /* extern "C" */
