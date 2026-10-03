/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2011, Tom Distler (http://tdistler.com)
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 *
 *  Metal compute kernels for float_ms_ssim (T8-2b / ADR-0490). The twin
 *  returns the CPU's scores bit for bit (ADR-1498; the design of the SYCL
 *  twin, ADR-1414 and ADR-1466, and of the CUDA twin, ADR-1403 and ADR-1465).
 *
 *  Same 5-scale pyramid and Wang weights as ms_ssim.c, and its arithmetic
 *  operation for operation:
 *
 *  1. `ms_ssim_decimate_h` / `ms_ssim_decimate_v` - ms_ssim_decimate.c's two
 *     separable passes of the 9-tap biorthogonal 9/7 low-pass with the
 *     period-2n mirror, every tap one fused multiply-add
 *     (vmaf_mtl_msdec_tap(), metal_ms_ssim_math.h). One call per
 *     inter-scale transition and side.
 *
 *  2. `ms_ssim_horiz` - the horizontal eleven-tap Gaussian over the five
 *     SSIM statistics, as exact fp32 pairs rounded once (metal_ssim_terms.h).
 *     Output width = W - 10, same height.
 *
 *  3. `ms_ssim_vert_lcs` - the vertical pass, the CPU's fp32 window values
 *     and its fp64 quotients on values held in 64-bit integers
 *     (vmaf_mtl_ssim_double_terms()). One thread per window stores lv and cv
 *     as fp64 bit patterns and sv as fp32 at the window's raster position
 *     of its (plane, scale) region: there is no reduction on the device.
 *
 *  The host adds the stored terms of each (plane, scale) in index order into
 *  one double each, divides by the window count, rounds each mean to fp32 and
 *  combines the scales as ms_ssim.c does (float_ms_ssim_metal.mm).
 *
 *  Buffer bindings:
 *   ms_ssim_decimate_h:
 *     [[buffer(0)]] src   - const float * (w_in x h_in)
 *     [[buffer(1)]] tmp   - float * (w_out x h_in)
 *     [[buffer(2)]] dims  - VmafMtlMsdecParams
 *   ms_ssim_decimate_v:
 *     [[buffer(0)]] tmp   - const float * (w_out x h_in)
 *     [[buffer(1)]] dst   - float * (w_out x h_out)
 *     [[buffer(2)]] dims  - VmafMtlMsdecParams
 *   ms_ssim_horiz:
 *     [[buffer(0)]] ref_in, [[buffer(1)]] cmp_in - const float * (W x H)
 *     [[buffer(2)]] hbuf  - float * (5 x w_h x H)
 *     [[buffer(3)]] params - uint4 (.x = W, .y = H, .z = w_h = W - 10)
 *   ms_ssim_vert_lcs:
 *     [[buffer(0)]] hbuf, [[buffer(1)]] luminance (ulong *),
 *     [[buffer(2)]] contrast (ulong *), [[buffer(3)]] structure (float *),
 *     [[buffer(4)]] params - VmafMtlSsimWindowParams
 *
 *  Min-dim guard (ADR-0153): 11 x 2^4 = 176, enforced in init() of the .mm.
 */

#include <metal_stdlib>
using namespace metal;

#include "metal_ms_ssim_math.h"
#include "metal_ssim_terms.h"

/* ------------------------------------------------------------------ */
/*  Kernels 1a and 1b: ms_ssim_decimate_{h,v}                          */
/* ------------------------------------------------------------------ */
kernel void ms_ssim_decimate_h(
    const device float                *src  [[buffer(0)]],
    device       float                *tmp  [[buffer(1)]],
    constant     VmafMtlMsdecParams   &dims [[buffer(2)]],
    uint2  gid [[thread_position_in_grid]])
{
    if (gid.x >= dims.output_width || gid.y >= dims.height) {
        return;
    }
    const int x_src = (int)gid.x * 2;
    float acc = 0.0f;
    for (int tap = 0; tap < VMAF_MTL_MSDEC_TAPS; ++tap) {
        const int xi =
            vmaf_mtl_msdec_mirror(x_src + tap - VMAF_MTL_MSDEC_HALF, (int)dims.width);
        acc = vmaf_mtl_msdec_tap(acc, src[gid.y * dims.width + (uint)xi],
                                 vmaf_mtl_msdec_lpf[tap]);
    }
    tmp[gid.y * dims.output_width + gid.x] = acc;
}

kernel void ms_ssim_decimate_v(
    const device float                *tmp  [[buffer(0)]],
    device       float                *dst  [[buffer(1)]],
    constant     VmafMtlMsdecParams   &dims [[buffer(2)]],
    uint2  gid [[thread_position_in_grid]])
{
    if (gid.x >= dims.output_width || gid.y >= dims.output_height) {
        return;
    }
    const int y_src = (int)gid.y * 2;
    float acc = 0.0f;
    for (int tap = 0; tap < VMAF_MTL_MSDEC_TAPS; ++tap) {
        const int yi =
            vmaf_mtl_msdec_mirror(y_src + tap - VMAF_MTL_MSDEC_HALF, (int)dims.height);
        acc = vmaf_mtl_msdec_tap(acc, tmp[(uint)yi * dims.output_width + gid.x],
                                 vmaf_mtl_msdec_lpf[tap]);
    }
    dst[gid.y * dims.output_width + gid.x] = acc;
}

/* ------------------------------------------------------------------ */
/*  Kernel 2: ms_ssim_horiz                                            */
/* ------------------------------------------------------------------ */
kernel void ms_ssim_horiz(
    const device float  *ref_in [[buffer(0)]],
    const device float  *cmp_in [[buffer(1)]],
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
        sums = vmaf_mtl_ssim_add_horizontal_tap(sums, ref_in[index], cmp_in[index],
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

/* ------------------------------------------------------------------ */
/*  Kernel 3: ms_ssim_vert_lcs                                         */
/* ------------------------------------------------------------------ */
kernel void ms_ssim_vert_lcs(
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
    const uint plane = params.horizontal_width * params.horizontal_height;
    VmafMtlSsimPairs sums = vmaf_mtl_ssim_pairs_zero();
    for (int tap = 0; tap < VMAF_MTL_SSIM_TAPS; ++tap) {
        const uint index = (gid.y + (uint)tap) * params.horizontal_width + gid.x;
        const VmafMtlSsimMoments row = vmaf_mtl_ssim_moments_make(
            hbuf[0u * plane + index], hbuf[1u * plane + index], hbuf[2u * plane + index],
            hbuf[3u * plane + index], hbuf[4u * plane + index]);
        sums = vmaf_mtl_ssim_add_vertical_tap(sums, row, vmaf_mtl_ssim_gauss[tap]);
    }
    const VmafMtlSsimDoubleTerms terms = vmaf_mtl_ssim_double_terms(
        vmaf_mtl_ssim_float_parts(vmaf_mtl_ssim_round_moments(sums), params.c1, params.c2),
        params.c1, params.c2);
    const uint index = params.offset + gid.y * params.final_width + gid.x;
    luminance[index] = vmaf_mtl_signed_bits(terms.luminance);
    contrast[index] = vmaf_mtl_signed_bits(terms.contrast);
    structure[index] = terms.structure;
}
