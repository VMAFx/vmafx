/**
 *  Copyright 2016-2020 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Metal compute kernels for the float_vif feature extractor (ADR-1498).
 *  MSL port of the SYCL twin `core/src/feature/sycl/float_vif_sycl.cpp`
 *  (ADR-1422; the CUDA and HIP twins compute the same values, ADR-1412 and
 *  ADR-1444): the twin returns the CPU extractor's values bit for bit.
 *
 *   - taps: the filter of each scale is an argument; the host takes it from
 *     vif_get_filter(), as float_vif.c does. No kernel file holds a tap.
 *   - filters: each tap is one rounded fp32 multiply and one rounded fp32
 *     add, taps in order, the vertical pass then the horizontal pass, over
 *     the reflect-101 mirror of vif_tools.c.
 *   - statistic: vif_pixel_statistic_s() and log2f_approx() operation for
 *     operation, in metal_float_vif_math.h (no fp64 type: exact fp32 pairs,
 *     and the reference's fp64 operations replayed in 64-bit integers next
 *     to a rounding boundary).
 *   - sums: vif_statistic_s() adds the terms of a row into one fp32
 *     accumulator and the rows into another. float_vif_row_sums adds a row
 *     left to right in one thread and the host adds the rows top to bottom.
 *     No threadgroup or atomic reduction of the terms exists in this file.
 *
 *  Kernels, dispatched in this order by `float_vif_metal.mm` (the first
 *  three once per scale, float_vif_decimate before them from scale 1):
 *
 *   1. float_vif_vertical  - the five vertical moments of every pixel
 *   2. float_vif_compute   - the horizontal pass, then the pixel statistic;
 *                            stores the numerator and denominator term
 *   3. float_vif_row_sums  - one thread per row
 *   4. float_vif_decimate  - this scale's filter over the previous scale's
 *                            plane, sampled at even positions
 *
 *  A float plane has the stride of its width. Scale 0 reads the raw frame
 *  (8-bit: uint8 - 128; 10, 12 and 16 bits: uint16 / scaler - 128, as
 *  picture_copy() reads it) unless the host prescaled it, in which case
 *  scale 0 reads the prescaled float planes.
 */

#include <metal_stdlib>
using namespace metal;

#include "metal_float_vif_math.h"

/* Threads per threadgroup in each axis. The kernels use no threadgroup
 * memory, so this is a scheduling choice only. */
#define FVIF_TG 16

/* Read a raw uint8/uint16 pixel and convert it to float in [-128, peak-128]
 * as picture_copy() does (vmaf_mtl_fvif_raw_to_float()); `stride_bytes` is
 * the row stride in bytes. float_vif_sycl.cpp read_vif_raw_plane() exactly. */
inline float fvif_read_raw(const device uchar *plane, uint stride_bytes, uint bpc, int y, int x)
{
    if (bpc <= 8u) {
        return vmaf_mtl_fvif_raw_to_float((uint)plane[(uint)y * stride_bytes + (uint)x], bpc);
    }
    const device ushort *row = (const device ushort *)(plane + (uint)y * stride_bytes);
    return vmaf_mtl_fvif_raw_to_float((uint)row[(uint)x], bpc);
}

/* One sample of the plane a kernel reads. */
inline float fvif_sample(const device uchar *raw_plane, const device float *float_plane,
                         constant VmafMtlFvifInputArgs &input, uint float_stride, int y, int x)
{
    if (input.raw != 0u) {
        return fvif_read_raw(raw_plane, input.raw_stride, input.bpc, y, x);
    }
    return float_plane[(uint)y * float_stride + (uint)x];
}

/* ------------------------------------------------------------------ */
/*  Kernel 1: float_vif_vertical                                       */
/*                                                                      */
/*   [[buffer(0)]] ref_raw, [[buffer(1)]] dis_raw  raw planes           */
/*   [[buffer(2)]] ref_f,   [[buffer(3)]] dis_f    float planes         */
/*   [[buffer(4)]] moments  5 planes: mu1 mu2 ref^2 dis^2 ref*dis       */
/*   [[buffer(5)]] taps     this scale's filter                         */
/*   [[buffer(6)]] filter   VmafMtlFvifFilterArgs                       */
/*   [[buffer(7)]] input    VmafMtlFvifInputArgs                        */
/*  Grid: width x height threads, one per pixel.                        */
/* ------------------------------------------------------------------ */
kernel void float_vif_vertical(const device uchar *ref_raw [[buffer(0)]],
                               const device uchar *dis_raw [[buffer(1)]],
                               const device float *ref_f [[buffer(2)]],
                               const device float *dis_f [[buffer(3)]],
                               device float *moments [[buffer(4)]],
                               constant float *taps [[buffer(5)]],
                               constant VmafMtlFvifFilterArgs &geometry [[buffer(6)]],
                               constant VmafMtlFvifInputArgs &input [[buffer(7)]],
                               uint2 gid [[thread_position_in_grid]])
{
    const int width = (int)geometry.width;
    const int height = (int)geometry.height;
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x >= width || y >= height) {
        return;
    }
    const int half_width = (int)geometry.taps / 2;
    float mu1 = 0.0f;
    float mu2 = 0.0f;
    float xx = 0.0f;
    float yy = 0.0f;
    float xy = 0.0f;
    for (uint k = 0u; k < geometry.taps; ++k) {
        const int row = vmaf_mtl_fvif_mirror(y - half_width + (int)k, height);
        const float c = taps[k];
        const float r = fvif_sample(ref_raw, ref_f, input, geometry.width, row, x);
        const float d = fvif_sample(dis_raw, dis_f, input, geometry.width, row, x);
        const float rr = r * r;
        const float dd = d * d;
        const float rd = r * d;
        mu1 = vmaf_mtl_fvif_tap(mu1, c, r);
        mu2 = vmaf_mtl_fvif_tap(mu2, c, d);
        xx = vmaf_mtl_fvif_tap(xx, c, rr);
        yy = vmaf_mtl_fvif_tap(yy, c, dd);
        xy = vmaf_mtl_fvif_tap(xy, c, rd);
    }
    const uint at = (uint)y * geometry.width + (uint)x;
    moments[at] = mu1;
    moments[geometry.plane + at] = mu2;
    moments[2u * geometry.plane + at] = xx;
    moments[3u * geometry.plane + at] = yy;
    moments[4u * geometry.plane + at] = xy;
}

/* ------------------------------------------------------------------ */
/*  Kernel 2: float_vif_compute                                        */
/*                                                                      */
/*   [[buffer(0)]] moments  the five planes of kernel 1                 */
/*   [[buffer(1)]] terms    (num, den) of pixel (x, y) at               */
/*                          vmaf_mtl_fvif_term_index(x, y, height)      */
/*   [[buffer(2)]] taps     this scale's filter                         */
/*   [[buffer(3)]] filter   VmafMtlFvifFilterArgs                       */
/*   [[buffer(4)]] stat     VmafMtlFvifStatisticArgs                    */
/*  Grid: width x height threads, one per pixel.                        */
/* ------------------------------------------------------------------ */
kernel void float_vif_compute(const device float *moments [[buffer(0)]],
                              device float *terms [[buffer(1)]],
                              constant float *taps [[buffer(2)]],
                              constant VmafMtlFvifFilterArgs &geometry [[buffer(3)]],
                              constant VmafMtlFvifStatisticArgs &stat [[buffer(4)]],
                              uint2 gid [[thread_position_in_grid]])
{
    const int width = (int)geometry.width;
    const int height = (int)geometry.height;
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x >= width || y >= height) {
        return;
    }
    const int half_width = (int)geometry.taps / 2;
    const uint row_at = (uint)y * geometry.width;
    float mu1 = 0.0f;
    float mu2 = 0.0f;
    float xx = 0.0f;
    float yy = 0.0f;
    float xy = 0.0f;
    for (uint k = 0u; k < geometry.taps; ++k) {
        const uint at = row_at + (uint)vmaf_mtl_fvif_mirror(x - half_width + (int)k, width);
        const float c = taps[k];
        mu1 = vmaf_mtl_fvif_tap(mu1, c, moments[at]);
        mu2 = vmaf_mtl_fvif_tap(mu2, c, moments[geometry.plane + at]);
        xx = vmaf_mtl_fvif_tap(xx, c, moments[2u * geometry.plane + at]);
        yy = vmaf_mtl_fvif_tap(yy, c, moments[3u * geometry.plane + at]);
        xy = vmaf_mtl_fvif_tap(xy, c, moments[4u * geometry.plane + at]);
    }
    const VmafMtlFvifStatParams params = vmaf_mtl_fvif_stat_params_make(
        stat.noise_mant_hi, stat.noise_mant_lo, stat.noise_exp, stat.noise_hi, stat.noise_lo,
        stat.noise_above, stat.gain_limit, stat.sigma_max_inv);
    const VmafMtlFvifTerm term = vmaf_mtl_fvif_pixel_term(mu1, mu2, xx, yy, xy, params);
    const uint out = vmaf_mtl_fvif_term_index((uint)x, (uint)y, geometry.height);
    terms[out] = term.num;
    terms[out + 1u] = term.den;
}

/* ------------------------------------------------------------------ */
/*  Kernel 3: float_vif_row_sums                                       */
/*                                                                      */
/*  vif_statistic_s()'s inner loop for row y: the terms added left to   */
/*  right into one fp32 accumulator per output. The order is the        */
/*  result; do not split, stride or reduce this loop.                   */
/*                                                                      */
/*   [[buffer(0)]] terms    the plane of kernel 2                       */
/*   [[buffer(1)]] rows     numerator row sums, then denominator row    */
/*                          sums (height floats each)                   */
/*   [[buffer(2)]] row      VmafMtlFvifRowArgs                          */
/*  Grid: height threads, one per row.                                  */
/* ------------------------------------------------------------------ */
kernel void float_vif_row_sums(const device float *terms [[buffer(0)]],
                               device float *rows [[buffer(1)]],
                               constant VmafMtlFvifRowArgs &row [[buffer(2)]],
                               uint y [[thread_position_in_grid]])
{
    if (y >= row.height) {
        return;
    }
    float numerator = 0.0f;
    float denominator = 0.0f;
    for (uint x = 0u; x < row.width; ++x) {
        const uint at = vmaf_mtl_fvif_term_index(x, y, row.height);
        numerator += terms[at];
        denominator += terms[at + 1u];
    }
    rows[y] = numerator;
    rows[row.height + y] = denominator;
}

/* ------------------------------------------------------------------ */
/*  Kernel 4: float_vif_decimate                                       */
/*                                                                      */
/*  This scale's filter at the PREVIOUS scale's dimensions, sampled at  */
/*  (2*gx, 2*gy): vertical taps inside, horizontal outside, as          */
/*  vif_filter1d_s() and float_vif_sycl.cpp decimate_vif_pixel().       */
/*                                                                      */
/*   [[buffer(0)]] ref_raw, [[buffer(1)]] dis_raw  raw planes           */
/*   [[buffer(2)]] ref_f,   [[buffer(3)]] dis_f    previous scale       */
/*   [[buffer(4)]] ref_out, [[buffer(5)]] dis_out  out_width stride     */
/*   [[buffer(6)]] taps     this scale's filter                         */
/*   [[buffer(7)]] dims     VmafMtlFvifDecimateArgs                     */
/*   [[buffer(8)]] input    VmafMtlFvifInputArgs                        */
/*  Grid: out_width x out_height threads, one per output pixel.         */
/* ------------------------------------------------------------------ */
kernel void float_vif_decimate(const device uchar *ref_raw [[buffer(0)]],
                               const device uchar *dis_raw [[buffer(1)]],
                               const device float *ref_f [[buffer(2)]],
                               const device float *dis_f [[buffer(3)]],
                               device float *ref_out [[buffer(4)]],
                               device float *dis_out [[buffer(5)]],
                               constant float *taps [[buffer(6)]],
                               constant VmafMtlFvifDecimateArgs &dims [[buffer(7)]],
                               constant VmafMtlFvifInputArgs &input [[buffer(8)]],
                               uint2 gid [[thread_position_in_grid]])
{
    const int x = (int)gid.x;
    const int y = (int)gid.y;
    if (x >= (int)dims.out_width || y >= (int)dims.out_height) {
        return;
    }
    const int half_width = (int)dims.taps / 2;
    float acc_ref = 0.0f;
    float acc_dis = 0.0f;
    for (uint kj = 0u; kj < dims.taps; ++kj) {
        const float c_j = taps[kj];
        const int px = vmaf_mtl_fvif_mirror(2 * x - half_width + (int)kj, (int)dims.in_width);
        float v_ref = 0.0f;
        float v_dis = 0.0f;
        for (uint ki = 0u; ki < dims.taps; ++ki) {
            const float c_i = taps[ki];
            const int py = vmaf_mtl_fvif_mirror(2 * y - half_width + (int)ki, (int)dims.in_height);
            const float r = fvif_sample(ref_raw, ref_f, input, dims.in_width, py, px);
            const float d = fvif_sample(dis_raw, dis_f, input, dims.in_width, py, px);
            v_ref = vmaf_mtl_fvif_tap(v_ref, c_i, r);
            v_dis = vmaf_mtl_fvif_tap(v_dis, c_i, d);
        }
        acc_ref = vmaf_mtl_fvif_tap(acc_ref, c_j, v_ref);
        acc_dis = vmaf_mtl_fvif_tap(acc_dis, c_j, v_dis);
    }
    const uint out = (uint)y * dims.out_width + (uint)x;
    ref_out[out] = acc_ref;
    dis_out[out] = acc_dis;
}
