/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  HIP kernels of the float_vif feature extractor (ADR-0379; the CPU's
 *  arithmetic since ADR-1444). They are the kernels of the CUDA twin
 *  (cuda/float_vif/float_vif_score.cu, ADR-1412) for a HIP device.
 *
 *  Per scale, in this order on one stream:
 *    float_vif_decimate  (scales 1..3) filters the previous scale with this
 *                        scale's Gaussian and keeps every second sample of
 *                        every second row: vif.c::vif_downsample().
 *    float_vif_compute   filters the five moments (mu1, mu2, ref^2, dis^2,
 *                        ref*dis) and stores the numerator and denominator
 *                        term of every pixel: vif_filter1d_*_s() and
 *                        vif_pixel_statistic_s().
 *    float_vif_row_sums  adds the terms of each row, left to right, in one
 *                        thread per row: the inner loop of vif_statistic_s().
 *  The host adds the rows, top to bottom (fvif_sum_rows()).
 *
 *  Numerical contract (ADR-1407, ADR-1444): every output is the CPU
 *  extractor's, bit for bit. The arithmetic is feature/float_vif_gpu_common.h,
 *  which the CUDA twin and a host test compile too; what this file adds is
 *  the tiling. The header's operators are the plain ones here: this file is
 *  built with the strict FP list of core/src/meson.build, so no product is
 *  fused into a sum and fp32 `/` is correctly rounded, and the gfx1036's fp64
 *  `+` and `/` return the host's bits (33.5 million operand pairs, ADR-1444).
 *  The CPU's frame sums are sequential fp32 accumulations whose result
 *  depends on the order, so no kernel here reduces per block or per wave: a
 *  partial sum in any other order is a different number.
 *
 *  Mirror padding is reflect-101 on both axes, as convolution_edge_s() and
 *  vif_mirror_index() do it. A tile also loads samples that no output of the
 *  plane consumes (the padding threads of the last row and column of blocks);
 *  those indices are clamped so the load stays inside the plane on frames
 *  smaller than a tile (hip_tile_index.h).
 */

#include <hip/hip_runtime.h>
#include <cstddef>
#include <cstdint>

#include "../hip_tile_index.h"

#define FVIF_HD __device__ __forceinline__
#define FVIF_FLOAT_AS_UINT(value) __float_as_uint(value)
#define FVIF_UINT_AS_FLOAT(bits) __uint_as_float(bits)
#define FVIF_DEVICE_ONLY
#include "feature/float_vif_gpu_common.h"

#define FVIF_MAX_TILE_W (FVIF_BX + 2 * FVIF_MAX_HFW)

namespace {

/* The shared argument structs carry device pointers as uint64_t; this is the
 * one place they become typed pointers (same size, same address). */
template <typename T>
__device__ __forceinline__ T *fvif_device_ptr(uint64_t address)
{
    return __builtin_bit_cast(T *, address);
}

/* picture_copy() with offset -128: an 8-bit sample as is, a 10-, 12- or
 * 16-bit sample divided by 4, 16 or 256 (exact in fp32). */
__device__ __forceinline__ float fvif_read_raw(const uint8_t *plane, ptrdiff_t stride_bytes,
                                                      int y, int x, unsigned bpc)
{
    if (bpc <= 8u) {
        return FVIF_FSUB((float)plane[y * stride_bytes + x], 128.0f);
    }
    const uint16_t v = reinterpret_cast<const uint16_t *>(plane + y * stride_bytes)[x];
    float scaler = 1.0f;
    if (bpc == 10u) {
        scaler = 4.0f;
    } else if (bpc == 12u) {
        scaler = 16.0f;
    } else if (bpc == 16u) {
        scaler = 256.0f;
    }
    return FVIF_FSUB(FVIF_FDIV((float)v, scaler), 128.0f);
}

/* One sample of each input plane at (x, y), already inside the plane. */
__device__ __forceinline__ void fvif_read_pair(const FloatVifGpuInput &in, int y, int x,
                                                      float *ref, float *dis)
{
    if (in.is_raw != 0u) {
        *ref = fvif_read_raw(fvif_device_ptr<const uint8_t>(in.ref), (ptrdiff_t)in.stride, y, x,
                             in.bpc);
        *dis = fvif_read_raw(fvif_device_ptr<const uint8_t>(in.dis), (ptrdiff_t)in.stride, y, x,
                             in.bpc);
        return;
    }
    const size_t at = (size_t)y * (size_t)in.stride + (size_t)x;
    *ref = fvif_device_ptr<const float>(in.ref)[at];
    *dis = fvif_device_ptr<const float>(in.dis)[at];
}

/* Reflect-101 for a consumed sample, clamped for one that is not. */
__device__ __forceinline__ int fvif_plane_index(int idx, int extent)
{
    return vmaf_hip_tile_index(vmaf_hip_reflect_101(idx, extent), extent);
}

/* The five vertically filtered moments of a block: FVIF_BY rows over every
 * tile column. */
struct FvifMoments {
    float mu1[FVIF_BY * FVIF_MAX_TILE_W];
    float mu2[FVIF_BY * FVIF_MAX_TILE_W];
    float xx[FVIF_BY * FVIF_MAX_TILE_W];
    float yy[FVIF_BY * FVIF_MAX_TILE_W];
    float xy[FVIF_BY * FVIF_MAX_TILE_W];
};

/* Phase 1: this thread's share of the block's tile, mirrored at the plane
 * edges. The tile is the block plus `hfw` samples on every side. */
__device__ __forceinline__ void fvif_load_tile(const FloatVifGpuInput &in, int hfw,
                                                      float *s_ref, float *s_dis)
{
    const int tile_w = FVIF_BX + 2 * hfw;
    const int tile_h = FVIF_BY + 2 * hfw;
    const int tile_oy = (int)blockIdx.y * FVIF_BY - hfw;
    const int tile_ox = (int)blockIdx.x * FVIF_BX - hfw;
    const int lid = (int)threadIdx.y * FVIF_BX + (int)threadIdx.x;
    for (int i = lid; i < tile_h * tile_w; i += FVIF_BX * FVIF_BY) {
        const int tr = i / tile_w;
        const int tc = i - tr * tile_w;
        const int py = fvif_plane_index(tile_oy + tr, (int)in.height);
        const int px = fvif_plane_index(tile_ox + tc, (int)in.width);
        fvif_read_pair(in, py, px, &s_ref[tr * FVIF_MAX_TILE_W + tc],
                       &s_dis[tr * FVIF_MAX_TILE_W + tc]);
    }
}

/* Phase 2: this thread's share of the vertical pass, taps in order. */
__device__ __forceinline__ void fvif_vertical_pass(const FloatVifGpuTaps &taps,
                                                          const float *s_ref, const float *s_dis,
                                                          FvifMoments *v)
{
    const int tile_w = FVIF_BX + 2 * (taps.width / 2);
    const int lid = (int)threadIdx.y * FVIF_BX + (int)threadIdx.x;
    for (int i = lid; i < FVIF_BY * tile_w; i += FVIF_BX * FVIF_BY) {
        const int r = i / tile_w;
        const int c = i - r * tile_w;
        float a_mu1 = 0.0f;
        float a_mu2 = 0.0f;
        float a_xx = 0.0f;
        float a_yy = 0.0f;
        float a_xy = 0.0f;
        for (int k = 0; k < taps.width; k++) {
            const float c_k = taps.coeff[k];
            const float ref_v = s_ref[(r + k) * FVIF_MAX_TILE_W + c];
            const float dis_v = s_dis[(r + k) * FVIF_MAX_TILE_W + c];
            a_mu1 = fvif_tap(a_mu1, c_k, ref_v);
            a_mu2 = fvif_tap(a_mu2, c_k, dis_v);
            a_xx = fvif_tap(a_xx, c_k, FVIF_FMUL(ref_v, ref_v));
            a_yy = fvif_tap(a_yy, c_k, FVIF_FMUL(dis_v, dis_v));
            a_xy = fvif_tap(a_xy, c_k, FVIF_FMUL(ref_v, dis_v));
        }
        v->mu1[r * FVIF_MAX_TILE_W + c] = a_mu1;
        v->mu2[r * FVIF_MAX_TILE_W + c] = a_mu2;
        v->xx[r * FVIF_MAX_TILE_W + c] = a_xx;
        v->yy[r * FVIF_MAX_TILE_W + c] = a_yy;
        v->xy[r * FVIF_MAX_TILE_W + c] = a_xy;
    }
}

/* Phase 3: this thread's pixel. The horizontal pass over the vertically
 * filtered moments, then the statistic. */
__device__ __forceinline__ void
fvif_pixel_terms(const FloatVifGpuComputeArgs &args, const FvifMoments *v, float *num, float *den)
{
    const int at = (int)threadIdx.y * FVIF_MAX_TILE_W + (int)threadIdx.x;
    float mu1 = 0.0f;
    float mu2 = 0.0f;
    float xx = 0.0f;
    float yy = 0.0f;
    float xy = 0.0f;
    for (int k = 0; k < args.taps.width; k++) {
        const float c_k = args.taps.coeff[k];
        mu1 = fvif_tap(mu1, c_k, v->mu1[at + k]);
        mu2 = fvif_tap(mu2, c_k, v->mu2[at + k]);
        xx = fvif_tap(xx, c_k, v->xx[at + k]);
        yy = fvif_tap(yy, c_k, v->yy[at + k]);
        xy = fvif_tap(xy, c_k, v->xy[at + k]);
    }
    fvif_pixel_statistic(mu1, mu2, xx, yy, xy, args.sigma_max_inv, args.vif_enhn_gain_limit,
                         args.vif_sigma_nsq, num, den);
}

} /* namespace */

extern "C" {

/* One block of 16x16 pixels: load the tile with its filter halo, filter
 * vertically for the block's 16 rows over every tile column, then each thread
 * filters horizontally and stores its pixel's two terms. */
__global__ void float_vif_compute(FloatVifGpuComputeArgs args)
{
    __shared__ float s_ref[FVIF_MAX_TILE_W * FVIF_MAX_TILE_W];
    __shared__ float s_dis[FVIF_MAX_TILE_W * FVIF_MAX_TILE_W];
    __shared__ FvifMoments s_v;

    fvif_load_tile(args.in, args.taps.width / 2, s_ref, s_dis);
    __syncthreads();
    fvif_vertical_pass(args.taps, s_ref, s_dis, &s_v);
    __syncthreads();

    const uint32_t gx = blockIdx.x * FVIF_BX + threadIdx.x;
    const uint32_t gy = blockIdx.y * FVIF_BY + threadIdx.y;
    if (gx >= args.in.width || gy >= args.in.height)
        return;

    float num = 0.0f;
    float den = 0.0f;
    fvif_pixel_terms(args, &s_v, &num, &den);

    float *terms = fvif_device_ptr<float>(args.terms);
    const size_t at = fvif_term_index(gx, gy, args.in.height);
    terms[at] = num;
    terms[at + 1u] = den;
}

/* The sequential sum of one row per thread. */
__global__ void float_vif_row_sums(FloatVifGpuRowArgs args)
{
    const uint32_t y = blockIdx.x * FVIF_ROW_THREADS + threadIdx.x;
    if (y >= args.height)
        return;
    float num = 0.0f;
    float den = 0.0f;
    fvif_row_sum(fvif_device_ptr<const float>(args.terms), args.width, args.height, y, &num,
                 &den);
    float *rows = fvif_device_ptr<float>(args.rows);
    rows[(size_t)y * FVIF_TERM_FLOATS] = num;
    rows[(size_t)y * FVIF_TERM_FLOATS + 1u] = den;
}

/* vif_downsample(): this scale's filter over the previous scale's plane,
 * sampled at (2 * gx, 2 * gy). The vertical pass of each of the `fw` columns
 * the horizontal pass reads, then the horizontal pass over them, which is
 * what vif_filter1d_s() computes for that sample. */
__global__ void float_vif_decimate(FloatVifGpuDecimateArgs args)
{
    const int gx = blockIdx.x * FVIF_BX + threadIdx.x;
    const int gy = blockIdx.y * FVIF_BY + threadIdx.y;
    if (gx >= (int)args.out_width || gy >= (int)args.out_height)
        return;

    const int fw = args.taps.width;
    const int hfw = fw / 2;
    const float *coeff = args.taps.coeff;
    const int in_w = (int)args.in.width;
    const int in_h = (int)args.in.height;
    const int in_x = 2 * gx;
    const int in_y = 2 * gy;

    float acc_ref = 0.0f;
    float acc_dis = 0.0f;
    for (int kj = 0; kj < fw; kj++) {
        const int px = vmaf_hip_reflect_101(in_x - hfw + kj, in_w);
        float v_ref = 0.0f;
        float v_dis = 0.0f;
        for (int ki = 0; ki < fw; ki++) {
            const int py = vmaf_hip_reflect_101(in_y - hfw + ki, in_h);
            float r = 0.0f;
            float d = 0.0f;
            fvif_read_pair(args.in, py, px, &r, &d);
            v_ref = fvif_tap(v_ref, coeff[ki], r);
            v_dis = fvif_tap(v_dis, coeff[ki], d);
        }
        acc_ref = fvif_tap(acc_ref, coeff[kj], v_ref);
        acc_dis = fvif_tap(acc_dis, coeff[kj], v_dis);
    }

    const size_t at = (size_t)gy * (size_t)args.out_width + (size_t)gx;
    fvif_device_ptr<float>(args.ref_out)[at] = acc_ref;
    fvif_device_ptr<float>(args.dis_out)[at] = acc_dis;
}

} /* extern "C" */
