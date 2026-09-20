/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP compute kernels for the float_vif feature extractor.
 *  Direct port of the CUDA twin at
 *  `libvmaf/src/feature/cuda/float_vif/float_vif_score.cu`.
 *
 *  Two operating modes:
 *    float_vif_compute:  read ref+dis at this scale, apply separable
 *      V→H filter inline (5 outputs: mu1, mu2, ref², dis², ref·dis),
 *      run vif_stat per pixel, reduce (num, den) per block.
 *    float_vif_decimate: apply this scale's filter at the previous
 *      scale's full dimensions, sample at (2*gx, 2*gy) for output.
 *      CPU's VIF_OPT_HANDLE_BORDERS branch — mirror padding for taps
 *      near the edge.
 *
 *  Mirror padding: reflect-102 (both axes):
 *    idx < 0  →  -idx
 *    idx >= sup  →  2*sup - idx - 2
 *
 *  Per scale: 4 compute + 3 decimate launches. Filter coefficients
 *  constant-folded per scale.
 *
 *  HIP adaptation from CUDA:
 *  - GCN/RDNA1 warp size is 64; RDNA2+ (gfx1030/gfx1100/gfx1101) is 32.
 *    The per-block warp partial array is sized for the minimum warp size (32)
 *    so it is large enough on wave32 hardware (256/32 = 8 warps needed).
 *    On wave64 hardware only 4 of the 8 slots are populated — no overflow.
 *    `warpSize` is used at runtime for the reduction loop and lane detection.
 *  - `__shfl_down` used without mask (HIP convention).
 *  - Entry-point names match the CUDA twin so the host Module API lookup
 *    in `float_vif_hip.c` resolves correctly without a name change.
 *
 *  Precision contract: places=3 (same as CUDA twin per ADR-0192).
 */

#include <hip/hip_runtime.h>
#include <stdint.h>

#define FVIF_BX 16
#define FVIF_BY 16
#define FVIF_MAX_FW 17
#define FVIF_MAX_HFW 8

/* Minimum HIP warp size across supported AMD targets (wave32 = RDNA2+).
 * The warp partial array is sized for the minimum (32) so it is large enough
 * on wave32 hardware (BX*BY/32 = 8 slots).  At runtime `warpSize` is used
 * for the reduction loop and lane/warp_id calculation. */
#define FVIF_MIN_WARP_SIZE 32
#define FVIF_WARPS_PER_BLOCK (FVIF_BX * FVIF_BY / FVIF_MIN_WARP_SIZE)

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

__device__ __forceinline__ const float *fvif_coeffs(int scale)
{
    if (scale == 0)
        return FVIF_COEFF_S0;
    if (scale == 1)
        return FVIF_COEFF_S1;
    if (scale == 2)
        return FVIF_COEFF_S2;
    return FVIF_COEFF_S3;
}

__device__ __forceinline__ int fvif_fw(int scale)
{
    const int fw[4] = {17, 9, 5, 3};
    return fw[scale];
}

__device__ __forceinline__ int fvif_mirror_v(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * sup - idx - 2;
    return idx;
}

__device__ __forceinline__ int fvif_mirror_h(int idx, int sup)
{
    if (idx < 0)
        return -idx;
    if (idx >= sup)
        return 2 * sup - idx - 2;
    return idx;
}

/* HIP warp-level float reduction — uses warpSize at runtime to handle
 * both wave64 (GCN/RDNA1, warpSize=64) and wave32 (RDNA2+, warpSize=32). */
__device__ __forceinline__ float fvif_warp_reduce(float v)
{
    for (int off = (int)warpSize / 2; off > 0; off >>= 1)
        v += __shfl_down(v, off);
    return v;
}

/* Read raw pixel (uint8/16) and convert to float. */
__device__ __forceinline__ float fvif_read_raw(const uint8_t *plane, int stride_bytes, int y, int x,
                                               unsigned bpc)
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

typedef struct FvifMoments {
    float mu1;
    float mu2;
    float xx;
    float yy;
    float xy;
} FvifMoments;

typedef struct FvifScore {
    float num;
    float den;
} FvifScore;

__device__ __forceinline__ void fvif_load_tile(const uint8_t *ref_raw, const uint8_t *dis_raw,
                                               ptrdiff_t raw_stride, const float *ref_f,
                                               const float *dis_f, ptrdiff_t f_stride, float *s_ref,
                                               float *s_dis, int scale, int tile_w, int tile_h,
                                               int hfw, unsigned width, unsigned height,
                                               unsigned bpc, unsigned lid)
{
    const int tile_oy = blockIdx.y * FVIF_BY - hfw;
    const int tile_ox = blockIdx.x * FVIF_BX - hfw;
    const bool is_raw = scale == 0;
    for (int i = (int)lid; i < tile_h * tile_w; i += FVIF_BX * FVIF_BY) {
        const int tr = i / tile_w;
        const int tc = i - tr * tile_w;
        const int py = fvif_mirror_v(tile_oy + tr, (int)height);
        const int px = fvif_mirror_h(tile_ox + tc, (int)width);
        if (is_raw) {
            s_ref[tr * 32 + tc] = fvif_read_raw(ref_raw, (int)raw_stride, py, px, bpc);
            s_dis[tr * 32 + tc] = fvif_read_raw(dis_raw, (int)raw_stride, py, px, bpc);
        } else {
            s_ref[tr * 32 + tc] = ref_f[(size_t)py * f_stride + px];
            s_dis[tr * 32 + tc] = dis_f[(size_t)py * f_stride + px];
        }
    }
}

__device__ __forceinline__ void fvif_vertical_filter(const float *s_ref, const float *s_dis,
                                                     float *s_v_mu1, float *s_v_mu2, float *s_v_xx,
                                                     float *s_v_yy, float *s_v_xy, int fw,
                                                     int tile_w, const float *coeff, unsigned lid)
{
    for (int i = (int)lid; i < FVIF_BY * tile_w; i += FVIF_BX * FVIF_BY) {
        const int r = i / tile_w;
        const int c = i - r * tile_w;
        FvifMoments value = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        for (int k = 0; k < fw; k++) {
            const float weight = coeff[k];
            const float ref_v = s_ref[(r + k) * 32 + c];
            const float dis_v = s_dis[(r + k) * 32 + c];
            value.mu1 += weight * ref_v;
            value.mu2 += weight * dis_v;
            value.xx += weight * (ref_v * ref_v);
            value.yy += weight * (dis_v * dis_v);
            value.xy += weight * (ref_v * dis_v);
        }
        s_v_mu1[r * 32 + c] = value.mu1;
        s_v_mu2[r * 32 + c] = value.mu2;
        s_v_xx[r * 32 + c] = value.xx;
        s_v_yy[r * 32 + c] = value.yy;
        s_v_xy[r * 32 + c] = value.xy;
    }
}

__device__ __forceinline__ FvifMoments fvif_horizontal_filter(
    const float *s_v_mu1, const float *s_v_mu2, const float *s_v_xx, const float *s_v_yy,
    const float *s_v_xy, int fw, const float *coeff, int lx, int ly)
{
    FvifMoments value = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int k = 0; k < fw; k++) {
        const float weight = coeff[k];
        const int offset = ly * 32 + lx + k;
        value.mu1 += weight * s_v_mu1[offset];
        value.mu2 += weight * s_v_mu2[offset];
        value.xx += weight * s_v_xx[offset];
        value.yy += weight * s_v_yy[offset];
        value.xy += weight * s_v_xy[offset];
    }
    return value;
}

__device__ __forceinline__ FvifScore fvif_score(FvifMoments value, float vif_sigma_nsq,
                                                float vif_egl, float sigma_max_inv)
{
    const float eps = 1.0e-10f;
    float sigma1_sq = value.xx - value.mu1 * value.mu1;
    float sigma2_sq = value.yy - value.mu2 * value.mu2;
    const float sigma12 = value.xy - value.mu1 * value.mu2;
    sigma1_sq = sigma1_sq < 0.0f ? 0.0f : sigma1_sq;
    sigma2_sq = sigma2_sq < 0.0f ? 0.0f : sigma2_sq;
    float gain = sigma12 / (sigma1_sq + eps);
    float sv_sq = sigma2_sq - gain * sigma12;
    if (sigma1_sq < eps) {
        gain = 0.0f;
        sv_sq = sigma2_sq;
        sigma1_sq = 0.0f;
    }
    if (sigma2_sq < eps) {
        gain = 0.0f;
        sv_sq = 0.0f;
    }
    if (gain < 0.0f) {
        sv_sq = sigma2_sq;
        gain = 0.0f;
    }
    sv_sq = sv_sq < eps ? eps : sv_sq;
    gain = gain > vif_egl ? vif_egl : gain;
    FvifScore out = {
        log2f(1.0f + (gain * gain * sigma1_sq) / (sv_sq + vif_sigma_nsq)),
        log2f(1.0f + sigma1_sq / vif_sigma_nsq),
    };
    if (sigma12 < 0.0f)
        out.num = 0.0f;
    if (sigma1_sq < vif_sigma_nsq) {
        out.num = 1.0f - sigma2_sq * sigma_max_inv;
        out.den = 1.0f;
    }
    return out;
}

__device__ __forceinline__ void fvif_reduce_score(FvifScore score, unsigned lid,
                                                  unsigned grid_x_count, float *s_num_warps,
                                                  float *s_den_warps, float *num_partials,
                                                  float *den_partials)
{
    score.num = fvif_warp_reduce(score.num);
    score.den = fvif_warp_reduce(score.den);
    const unsigned ws = (unsigned)warpSize;
    const unsigned lane = lid % ws;
    const unsigned warp_id = lid / ws;
    if (lane == 0u) {
        s_num_warps[warp_id] = score.num;
        s_den_warps[warp_id] = score.den;
    }
    __syncthreads();
    if (lid != 0u)
        return;

    FvifScore total = {0.0f, 0.0f};
    for (unsigned i = 0; i < (FVIF_BX * FVIF_BY) / ws; i++) {
        total.num += s_num_warps[i];
        total.den += s_den_warps[i];
    }
    const unsigned wg_idx = blockIdx.y * grid_x_count + blockIdx.x;
    num_partials[wg_idx] = total.num;
    den_partials[wg_idx] = total.den;
}

/**
 * Compute kernel — reads ref+dis at `scale`, applies separable V→H filter,
 * computes vif_stat per pixel, reduces (num, den) per block.
 *
 * At scale=0, ref_raw/dis_raw are the source pixel planes (uint8/16).
 * At scale>0, ref_f/dis_f are float buffers at this scale's dimensions.
 */
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

    const int tile_w = FVIF_BX + 2 * hfw;
    const int tile_h = FVIF_BY + 2 * hfw;

    __shared__ float s_ref[32 * 32];
    __shared__ float s_dis[32 * 32];
    __shared__ float s_v_mu1[FVIF_BY * 32];
    __shared__ float s_v_mu2[FVIF_BY * 32];
    __shared__ float s_v_xx[FVIF_BY * 32];
    __shared__ float s_v_yy[FVIF_BY * 32];
    __shared__ float s_v_xy[FVIF_BY * 32];
    __shared__ float s_num_warps[FVIF_WARPS_PER_BLOCK];
    __shared__ float s_den_warps[FVIF_WARPS_PER_BLOCK];

    const int gx = blockIdx.x * blockDim.x + threadIdx.x;
    const int gy = blockIdx.y * blockDim.y + threadIdx.y;
    const int lx = threadIdx.x;
    const int ly = threadIdx.y;
    const unsigned lid = (unsigned)(ly * FVIF_BX + lx);

    fvif_load_tile(ref_raw, dis_raw, raw_stride, ref_f, dis_f, f_stride_floats, s_ref, s_dis, scale,
                   tile_w, tile_h, hfw, width, height, bpc, lid);
    __syncthreads();

    fvif_vertical_filter(s_ref, s_dis, s_v_mu1, s_v_mu2, s_v_xx, s_v_yy, s_v_xy, fw, tile_w, coeff,
                         lid);
    __syncthreads();

    FvifScore score = {0.0f, 0.0f};
    if (gx < (int)width && gy < (int)height) {
        const FvifMoments value =
            fvif_horizontal_filter(s_v_mu1, s_v_mu2, s_v_xx, s_v_yy, s_v_xy, fw, coeff, lx, ly);
        score = fvif_score(value, vif_sigma_nsq, vif_egl, sigma_max_inv);
    }
    fvif_reduce_score(score, lid, grid_x_count, s_num_warps, s_den_warps, num_partials,
                      den_partials);
}

/**
 * Decimate kernel — applies `scale`'s filter at the previous scale's
 * dimensions, samples at (2*gx, 2*gy) for output.
 * Output dimensions: in_w / 2, in_h / 2.
 */
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
                r = fvif_read_raw(ref_raw, (int)raw_stride, py, px, bpc);
                d = fvif_read_raw(dis_raw, (int)raw_stride, py, px, bpc);
            } else {
                r = ref_f[(size_t)py * f_stride_floats + px];
                d = dis_f[(size_t)py * f_stride_floats + px];
            }
            v_ref += c_i * r;
            v_dis += c_i * d;
        }
        acc_ref += c_j * v_ref;
        acc_dis += c_j * v_dis;
    }

    ref_out[(size_t)gy * out_stride_floats + gx] = acc_ref;
    dis_out[(size_t)gy * out_stride_floats + gx] = acc_dis;
}
