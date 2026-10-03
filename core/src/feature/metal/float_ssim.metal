/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  Metal compute kernels for float_ssim (T8-1j / ADR-0421). The twin returns
 *  the CPU's scores bit for bit at scale 1 (ADR-1498; the design of the SYCL
 *  twin, ADR-1463, and of the CUDA twin, ADR-1464).
 *
 *  iqa_ssim() convolves the five SSIM statistics with the eleven-tap Gaussian
 *  in two passes ("valid" region, no padding), forms each window's luminance,
 *  contrast and structure terms in fp32 and fp64 and adds them into one
 *  double each, window after window in raster order. Metal has no fp64 type
 *  and a sum of doubles is its order, so:
 *
 *  Pass 0 (float_ssim_horiz): one thread per output column and row sums the
 *    fp32 products of the eleven taps as exact fp32 pairs and rounds each of
 *    the five sums to fp32 once, as iqa_convolve()'s fp64 sum rounds
 *    (metal_ssim_terms.h).
 *  Pass 1 (float_ssim_vert_terms / float_ssim_vert_lcs): one thread per window
 *    does the same vertically, forms the CPU's fp32 values and runs its fp64
 *    quotients on values held in 64-bit integers
 *    (vmaf_mtl_ssim_double_terms()). The kernel stores the bit pattern of the
 *    window's `lv * cv * sv` (or, for enable_lcs, of lv and cv and the fp32
 *    sv) at the window's raster position: there is no reduction on the
 *    device.
 *  Host (float_ssim_metal.mm): adds the stored terms in index order into one
 *  double each, divides by the window count and rounds to fp32, as iqa_ssim()
 *  does.
 *
 *  Inputs are float planes the host filled with picture_copy(): the CPU's
 *  normalisation to [0, 255] at every bit depth.
 *
 *  Buffer bindings for `float_ssim_horiz`:
 *   [[buffer(0)]] ref_f   — const float * (full W x H reference)
 *   [[buffer(1)]] dis_f   — const float * (full W x H distorted)
 *   [[buffer(2)]] hbuf    — float * (5 x w_h x H: mu_r | mu_d | sq_r | sq_d | rd)
 *   [[buffer(3)]] params  — uint4 (.x = W, .y = H, .z = w_h = W - 10)
 *
 *  Buffer bindings for `float_ssim_vert_terms`:
 *   [[buffer(0)]] hbuf    — const float * (pass 0)
 *   [[buffer(1)]] terms   — ulong * (w_h x h_v fp64 bit patterns of lv * cv * sv)
 *   [[buffer(2)]] params  — VmafMtlSsimWindowParams
 *
 *  Buffer bindings for `float_ssim_vert_lcs`:
 *   [[buffer(0)]] hbuf       — const float * (pass 0)
 *   [[buffer(1)]] luminance  — ulong * (fp64 bit patterns of lv)
 *   [[buffer(2)]] contrast   — ulong * (fp64 bit patterns of cv)
 *   [[buffer(3)]] structure  — float * (fp32 sv)
 *   [[buffer(4)]] params     — VmafMtlSsimWindowParams
 */

#include <metal_stdlib>
using namespace metal;

#include "metal_ssim_terms.h"

/* ------------------------------------------------------------------ */
/*  Pass 0: horizontal convolution                                      */
/* ------------------------------------------------------------------ */
kernel void float_ssim_horiz(
    const device float  *ref_f  [[buffer(0)]],
    const device float  *dis_f  [[buffer(1)]],
    device       float  *hbuf   [[buffer(2)]],
    constant     uint4  &params [[buffer(3)]],
    uint2  gid [[thread_position_in_grid]])
{
    const uint width = params.x;
    const uint height = params.y;
    const uint w_h = params.z;
    if (gid.x >= w_h || gid.y >= height) {
        return;
    }
    VmafMtlSsimPairs sums = vmaf_mtl_ssim_pairs_zero();
    for (int tap = 0; tap < VMAF_MTL_SSIM_TAPS; ++tap) {
        const uint index = gid.y * width + gid.x + (uint)tap;
        sums = vmaf_mtl_ssim_add_horizontal_tap(sums, ref_f[index], dis_f[index],
                                                vmaf_mtl_ssim_gauss[tap]);
    }
    const VmafMtlSsimMoments m = vmaf_mtl_ssim_round_moments(sums);
    const uint plane = w_h * height;
    const uint out = gid.y * w_h + gid.x;
    hbuf[0u * plane + out] = m.reference_mean;
    hbuf[1u * plane + out] = m.comparison_mean;
    hbuf[2u * plane + out] = m.reference_square;
    hbuf[3u * plane + out] = m.comparison_square;
    hbuf[4u * plane + out] = m.cross_product;
}

/* The CPU's lv, cv and sv of the window at (x, y). */
inline VmafMtlSsimDoubleTerms float_ssim_window_terms(const device float *hbuf,
                                                      constant VmafMtlSsimWindowParams &p,
                                                      uint x, uint y)
{
    const uint plane = p.horizontal_width * p.horizontal_height;
    VmafMtlSsimPairs sums = vmaf_mtl_ssim_pairs_zero();
    for (int tap = 0; tap < VMAF_MTL_SSIM_TAPS; ++tap) {
        const uint index = (y + (uint)tap) * p.horizontal_width + x;
        const VmafMtlSsimMoments row = vmaf_mtl_ssim_moments_make(
            hbuf[0u * plane + index], hbuf[1u * plane + index], hbuf[2u * plane + index],
            hbuf[3u * plane + index], hbuf[4u * plane + index]);
        sums = vmaf_mtl_ssim_add_vertical_tap(sums, row, vmaf_mtl_ssim_gauss[tap]);
    }
    return vmaf_mtl_ssim_double_terms(
        vmaf_mtl_ssim_float_parts(vmaf_mtl_ssim_round_moments(sums), p.c1, p.c2), p.c1, p.c2);
}

/* ------------------------------------------------------------------ */
/*  Pass 1: vertical convolution + the window's fp64 term               */
/* ------------------------------------------------------------------ */
kernel void float_ssim_vert_terms(
    const device float                    *hbuf   [[buffer(0)]],
    device       ulong                    *terms  [[buffer(1)]],
    constant     VmafMtlSsimWindowParams  &params [[buffer(2)]],
    uint2  gid [[thread_position_in_grid]])
{
    if (gid.x >= params.final_width || gid.y >= params.final_height) {
        return;
    }
    terms[params.offset + gid.y * params.final_width + gid.x] =
        vmaf_mtl_ssim_product_bits(float_ssim_window_terms(hbuf, params, gid.x, gid.y));
}

/* enable_lcs variant: lv and cv as fp64 bit patterns and the fp32 sv, each at
 * the window's raster position; the host forms the product and the sums. */
kernel void float_ssim_vert_lcs(
    const device float                    *hbuf      [[buffer(0)]],
    device       ulong                    *luminance [[buffer(1)]],
    device       ulong                    *contrast  [[buffer(2)]],
    device       float                    *structure [[buffer(3)]],
    constant     VmafMtlSsimWindowParams  &params    [[buffer(4)]],
    uint2  gid [[thread_position_in_grid]])
{
    if (gid.x >= params.final_width || gid.y >= params.final_height) {
        return;
    }
    const uint index = params.offset + gid.y * params.final_width + gid.x;
    const VmafMtlSsimDoubleTerms terms = float_ssim_window_terms(hbuf, params, gid.x, gid.y);
    luminance[index] = vmaf_mtl_signed_bits(terms.luminance);
    contrast[index] = vmaf_mtl_signed_bits(terms.contrast);
    structure[index] = terms.structure;
}
