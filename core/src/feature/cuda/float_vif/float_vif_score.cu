/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA compute kernel for the float_vif feature extractor
 *  (T7-23 / batch 3 part 5b — ADR-0192 / ADR-0197). CUDA twin of
 *  float_vif_vulkan.
 *
 *  Two operating modes via MODE template parameter at launch:
 *    MODE = 0 (compute):   read ref+dis at this scale, apply
 *      separable V→H filter inline (5 outputs: mu1, mu2, ref²,
 *      dis², ref·dis), run vif_stat_one_pixel per pixel, reduce
 *      (num, den) per block.
 *    MODE = 1 (decimate):  apply this scale's filter at the
 *      *previous* scale's full dimensions, sample at (2*gx, 2*gy)
 *      for output. CPU's `VIF_OPT_HANDLE_BORDERS` branch — the
 *      filter relies on mirror padding for taps near the edge.
 *
 *  Mirror padding diverges per axis:
 *    vertical:   2 * extent - idx - 2  for idx >= extent
 *    horizontal: 2 * extent - idx - 1  for idx >= extent
 *
 *  Per scale: 4 compute pipelines + 3 decimate pipelines (no scale-0
 *  decimate). Filter coefficients constant-folded per scale.
 *
 *  Precision contract per ADR-0192: places=3 (matches float_motion
 *  drift profile vs CPU AVX2). Scalar-CPU comparison is far tighter.
 */

#include "cuda_helper.cuh"
#include "common.h"

#define FVIF_BX 16
#define FVIF_BY 16
#define FVIF_MAX_FW 17
#define FVIF_MAX_HFW 8

__device__ static const float FVIF_COEFF_S0[FVIF_MAX_FW] = {
    0.00745626912f, 0.0142655009f, 0.0250313189f, 0.0402820669f, 0.0594526194f, 0.0804751068f,
    0.0999041125f,  0.113746084f,  0.118773937f,  0.113746084f,  0.0999041125f, 0.0804751068f,
    0.0594526194f,  0.0402820669f, 0.0250313189f, 0.0142655009f, 0.00745626912f};
__device__ static const float FVIF_COEFF_S1[FVIF_MAX_FW] = {
    0.0189780835f, 0.0558981746f, 0.120920904f,  0.192116052f, 0.224173605f, 0.192116052f,
    0.120920904f,  0.0558981746f, 0.0189780835f, 0.0f,         0.0f,         0.0f,
    0.0f,          0.0f,          0.0f,          0.0f,         0.0f};
__device__ static const float FVIF_COEFF_S2[FVIF_MAX_FW] = {
    0.054488685f, 0.244201347f, 0.402619958f, 0.244201347f, 0.054488685f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f,         0.0f,         0.0f,         0.0f,         0.0f,         0.0f, 0.0f, 0.0f};
__device__ static const float FVIF_COEFF_S3[FVIF_MAX_FW] = {
    0.166378498f, 0.667243004f, 0.166378498f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f,         0.0f,         0.0f,         0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

__device__ static __forceinline__ const float *fvif_coeffs(int scale)
{
    if (scale == 0)
        return FVIF_COEFF_S0;
    if (scale == 1)
        return FVIF_COEFF_S1;
    if (scale == 2)
        return FVIF_COEFF_S2;
    return FVIF_COEFF_S3;
}
__device__ static __forceinline__ int fvif_fw(int scale)
{
    static const int fw[4] = {17, 9, 5, 3};
    return fw[scale];
}

__device__ static __forceinline__ int fvif_mirror_v(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * sup - idx - 2;
    return idx;
}
/* H mirror matches CPU `convolution_edge_s` (the AVX2 border path that
 * runs in production at default vif_sigma_nsq=2.0). Differs by one from
 * `vif_mirror_tap_h` in vif_tools.c (scalar-only path). */
__device__ static __forceinline__ int fvif_mirror_h(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * sup - idx - 2;
    return idx;
}

__device__ static __forceinline__ float fvif_warp_reduce(float v)
{
    for (int off = 16; off > 0; off >>= 1)
        v += __shfl_down_sync(0xffffffff, v, off);
    return v;
}

/* Read raw pixel (uint8/16) and convert to float in [-128, peak-128].
 * stride_w is the row stride in pixels (== width for tightly-packed
 * upload buffers). bpc selects scaler. */
__device__ static __forceinline__ float fvif_read_raw(const uint8_t *plane, int stride_bytes, int y,
                                                      int x, unsigned bpc)
{
    if (bpc <= 8u) {
        return (float)plane[y * stride_bytes + x] - 128.0f;
    }
    const uint16_t v = reinterpret_cast<const uint16_t *>(plane + y * stride_bytes)[x];
    float scaler = 1.0f;
    if (bpc == 10u)
        scaler = 4.0f;
    else if (bpc == 12u)
        scaler = 16.0f;
    else if (bpc == 16u)
        scaler = 256.0f;
    return (float)v / scaler - 128.0f;
}

struct FvifVerticalBuffers {
    float *mu1;
    float *mu2;
    float *xx;
    float *yy;
    float *xy;
};

struct FvifMoments {
    float mu1;
    float mu2;
    float xx;
    float yy;
    float xy;
};

__device__ static void fvif_load_tile(float *tile_ref, float *tile_dis, int scale,
                                      const uint8_t *ref_raw, const uint8_t *dis_raw,
                                      ptrdiff_t raw_stride, const float *ref_f, const float *dis_f,
                                      ptrdiff_t float_stride, unsigned width, unsigned height,
                                      unsigned bpc, int half_width, int tile_width, int tile_height,
                                      unsigned lid)
{
    constexpr int max_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    const int origin_y = blockIdx.y * FVIF_BY - half_width;
    const int origin_x = blockIdx.x * FVIF_BX - half_width;
    for (int i = lid; i < tile_height * tile_width; i += FVIF_BX * FVIF_BY) {
        const int row = i / tile_width;
        const int column = i - row * tile_width;
        const int source_y = fvif_mirror_v(origin_y + row, (int)height);
        const int source_x = fvif_mirror_h(origin_x + column, (int)width);
        if (scale == 0) {
            tile_ref[row * max_tile_width + column] =
                fvif_read_raw(ref_raw, raw_stride, source_y, source_x, bpc);
            tile_dis[row * max_tile_width + column] =
                fvif_read_raw(dis_raw, raw_stride, source_y, source_x, bpc);
        } else {
            tile_ref[row * max_tile_width + column] = ref_f[source_y * float_stride + source_x];
            tile_dis[row * max_tile_width + column] = dis_f[source_y * float_stride + source_x];
        }
    }
}

__device__ static void fvif_vertical_filter(const float *tile_ref, const float *tile_dis,
                                            FvifVerticalBuffers output, const float *coeff,
                                            int filter_width, int tile_width, unsigned lid)
{
    constexpr int max_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    for (int i = lid; i < FVIF_BY * tile_width; i += FVIF_BX * FVIF_BY) {
        const int row = i / tile_width;
        const int column = i - row * tile_width;
        FvifMoments sum = {};
        for (int k = 0; k < filter_width; k++) {
            const float weight = coeff[k];
            const float ref = tile_ref[(row + k) * max_tile_width + column];
            const float dis = tile_dis[(row + k) * max_tile_width + column];
            sum.mu1 += weight * ref;
            sum.mu2 += weight * dis;
            sum.xx += weight * (ref * ref);
            sum.yy += weight * (dis * dis);
            sum.xy += weight * (ref * dis);
        }
        output.mu1[row * max_tile_width + column] = sum.mu1;
        output.mu2[row * max_tile_width + column] = sum.mu2;
        output.xx[row * max_tile_width + column] = sum.xx;
        output.yy[row * max_tile_width + column] = sum.yy;
        output.xy[row * max_tile_width + column] = sum.xy;
    }
}

__device__ static FvifMoments fvif_horizontal_filter(const FvifVerticalBuffers input,
                                                     const float *coeff, int filter_width, int x,
                                                     int y)
{
    constexpr int max_tile_width = FVIF_BX + 2 * FVIF_MAX_HFW;
    FvifMoments sum = {};
    for (int k = 0; k < filter_width; k++) {
        const float weight = coeff[k];
        const int index = y * max_tile_width + x + k;
        sum.mu1 += weight * input.mu1[index];
        sum.mu2 += weight * input.mu2[index];
        sum.xx += weight * input.xx[index];
        sum.yy += weight * input.yy[index];
        sum.xy += weight * input.xy[index];
    }
    return sum;
}

__device__ static void fvif_statistic(FvifMoments moments, float vif_sigma_nsq, float vif_egl,
                                      float sigma_max_inv, float *num, float *den)
{
    const float eps = 1.0e-10f;
    float sigma1_sq = fmaxf(moments.xx - moments.mu1 * moments.mu1, 0.0f);
    const float sigma2_sq = fmaxf(moments.yy - moments.mu2 * moments.mu2, 0.0f);
    const float sigma12 = moments.xy - moments.mu1 * moments.mu2;
    float gain = sigma12 / (sigma1_sq + eps);
    float residual = sigma2_sq - gain * sigma12;
    if (sigma1_sq < eps) {
        gain = 0.0f;
        residual = sigma2_sq;
        sigma1_sq = 0.0f;
    }
    if (sigma2_sq < eps) {
        gain = 0.0f;
        residual = 0.0f;
    }
    if (gain < 0.0f) {
        residual = sigma2_sq;
        gain = 0.0f;
    }
    residual = fmaxf(residual, eps);
    gain = fminf(gain, vif_egl);
    *num = log2f(1.0f + (gain * gain * sigma1_sq) / (residual + vif_sigma_nsq));
    *den = log2f(1.0f + sigma1_sq / vif_sigma_nsq);
    if (sigma12 < 0.0f)
        *num = 0.0f;
    if (sigma1_sq < vif_sigma_nsq) {
        *num = 1.0f - sigma2_sq * sigma_max_inv;
        *den = 1.0f;
    }
}

__device__ static void fvif_reduce_partials(float num, float den, float *num_warps,
                                            float *den_warps, float *num_partials,
                                            float *den_partials, unsigned grid_x_count,
                                            unsigned lid)
{
    const float warp_num = fvif_warp_reduce(num);
    const float warp_den = fvif_warp_reduce(den);
    const int lane = lid % 32;
    const int warp_id = lid / 32;
    if (lane == 0) {
        num_warps[warp_id] = warp_num;
        den_warps[warp_id] = warp_den;
    }
    __syncthreads();
    if (lid != 0)
        return;
    float total_num = 0.0f, total_den = 0.0f;
    for (int i = 0; i < FVIF_BX * FVIF_BY / 32; i++) {
        total_num += num_warps[i];
        total_den += den_warps[i];
    }
    const unsigned workgroup = blockIdx.y * grid_x_count + blockIdx.x;
    num_partials[workgroup] = total_num;
    den_partials[workgroup] = total_den;
}

/* Compute kernel — SCALE selects the filter and tile halo. The shader
 * expects `ref_in` / `dis_in` to be float buffers at this scale's
 * dimensions, EXCEPT at SCALE=0 where they're raw (uint plane) and
 * the kernel converts inline. We pass a `bpc` arg + `is_raw` bool
 * to handle that. The caller binds the appropriate buffer. */
extern "C" __global__ void
float_vif_compute(int scale, const uint8_t *ref_raw, const uint8_t *dis_raw, ptrdiff_t raw_stride,
                  const float *ref_f, const float *dis_f, ptrdiff_t f_stride_floats,
                  float *num_partials, float *den_partials, unsigned width, unsigned height,
                  unsigned bpc, unsigned grid_x_count, float vif_sigma_nsq, float vif_egl,
                  float sigma_max_inv)
{
    const int fw = fvif_fw(scale);
    const int hfw = fw / 2;
    const float *coeff = fvif_coeffs(scale);

    /* Tile size = WG + 2*hfw on each axis. We allocate worst-case
     * (WG=16, max_hfw=8) → 32×32 = 1024 entries. Used cells = (16 +
     * 2*hfw)² per scale. */
    constexpr int MAX_TILE_W = FVIF_BX + 2 * FVIF_MAX_HFW;
    const int tile_w = FVIF_BX + 2 * hfw;
    const int tile_h = FVIF_BY + 2 * hfw;

    __shared__ float s_ref[MAX_TILE_W * MAX_TILE_W];
    __shared__ float s_dis[MAX_TILE_W * MAX_TILE_W];
    __shared__ float s_v_mu1[FVIF_BY * MAX_TILE_W];
    __shared__ float s_v_mu2[FVIF_BY * MAX_TILE_W];
    __shared__ float s_v_xx[FVIF_BY * MAX_TILE_W];
    __shared__ float s_v_yy[FVIF_BY * MAX_TILE_W];
    __shared__ float s_v_xy[FVIF_BY * MAX_TILE_W];
    __shared__ float s_num_warps[FVIF_BX * FVIF_BY / 32];
    __shared__ float s_den_warps[FVIF_BX * FVIF_BY / 32];

    const int gx = blockIdx.x * blockDim.x + threadIdx.x;
    const int gy = blockIdx.y * blockDim.y + threadIdx.y;
    const int lx = threadIdx.x;
    const int ly = threadIdx.y;
    const unsigned lid = ly * FVIF_BX + lx;
    const bool valid = (gx < (int)width && gy < (int)height);
    FvifVerticalBuffers vertical = {s_v_mu1, s_v_mu2, s_v_xx, s_v_yy, s_v_xy};
    fvif_load_tile(s_ref, s_dis, scale, ref_raw, dis_raw, raw_stride, ref_f, dis_f, f_stride_floats,
                   width, height, bpc, hfw, tile_w, tile_h, lid);
    __syncthreads();
    fvif_vertical_filter(s_ref, s_dis, vertical, coeff, fw, tile_w, lid);
    __syncthreads();
    float my_num = 0.0f, my_den = 0.0f;
    if (valid) {
        const FvifMoments moments = fvif_horizontal_filter(vertical, coeff, fw, lx, ly);
        fvif_statistic(moments, vif_sigma_nsq, vif_egl, sigma_max_inv, &my_num, &my_den);
    }
    fvif_reduce_partials(my_num, my_den, s_num_warps, s_den_warps, num_partials, den_partials,
                         grid_x_count, lid);
}

/* Decimate kernel — applies SCALE's filter at PREVIOUS scale's
 * dimensions, samples at (2*gx, 2*gy) for output. CPU's
 * VIF_OPT_HANDLE_BORDERS branch — mirror padding handles taps near
 * the edge. Output dimensions: in_w / 2, in_h / 2. */
extern "C" __global__ void
float_vif_decimate(int scale, const uint8_t *ref_raw, const uint8_t *dis_raw, ptrdiff_t raw_stride,
                   const float *ref_f, const float *dis_f, ptrdiff_t f_stride_floats,
                   float *ref_out, float *dis_out, ptrdiff_t out_stride_floats, unsigned out_w,
                   unsigned out_h, unsigned in_w, unsigned in_h, unsigned bpc)
{
    const int gx = blockIdx.x * blockDim.x + threadIdx.x;
    const int gy = blockIdx.y * blockDim.y + threadIdx.y;
    if (gx >= (int)out_w || gy >= (int)out_h)
        return;

    const int fw = fvif_fw(scale);
    const int hfw = fw / 2;
    const float *coeff = fvif_coeffs(scale);
    const int in_x = 2 * gx;
    const int in_y = 2 * gy;
    const bool is_raw = (scale == 1);

    /* V-inner / H-outer ordering matches CPU vif_filter1d_s. */
    float acc_ref = 0.0f, acc_dis = 0.0f;
    for (int kj = 0; kj < fw; kj++) {
        const float c_j = coeff[kj];
        const int px = fvif_mirror_h(in_x - hfw + kj, (int)in_w);
        float v_ref = 0.0f, v_dis = 0.0f;
        for (int ki = 0; ki < fw; ki++) {
            const float c_i = coeff[ki];
            const int py = fvif_mirror_v(in_y - hfw + ki, (int)in_h);
            float r, d;
            if (is_raw) {
                r = fvif_read_raw(ref_raw, raw_stride, py, px, bpc);
                d = fvif_read_raw(dis_raw, raw_stride, py, px, bpc);
            } else {
                r = ref_f[py * f_stride_floats + px];
                d = dis_f[py * f_stride_floats + px];
            }
            v_ref += c_i * r;
            v_dis += c_i * d;
        }
        acc_ref += c_j * v_ref;
        acc_dis += c_j * v_dis;
    }

    ref_out[gy * out_stride_floats + gx] = acc_ref;
    dis_out[gy * out_stride_floats + gx] = acc_dis;
}
