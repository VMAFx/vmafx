/**
 *  Copyright 2001-2012 Xiph.Org and contributors.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-2-Clause
 *
 *  Metal compute kernels for integer_ssim (fixed-point SSIM, feature `ssim`).
 *  The twin returns the CPU's score bit for bit (ADR-1498; the design of the
 *  SYCL twin, ADR-1443, and of the CUDA and HIP twins, ADR-1424, ADR-1438).
 *
 *  integer_ssim.c accumulates int64 window moments with the nine-tap integer
 *  Gaussian {2, 9, 28, 55, 68, 55, 28, 9, 2} in both directions, truncated at
 *  the frame's edges, forms each pixel's term in fp64 and calc_ssim() adds
 *  every term into one double, left to right and top to bottom. Metal has no
 *  fp64 type and a sum of doubles is its order, so:
 *
 *  Pass 0 (integer_ssim_horiz_{8,16}bpc): one thread per pixel accumulates
 *    the five horizontal int64 moments (mux, muy, x2, xy, y2) over the taps
 *    inside the row and writes them to five W x H planes of `hbuf`.
 *  Pass 1 (integer_ssim_vert_terms): one thread per pixel accumulates the
 *    vertical moments over the rows inside the frame, takes the window weight
 *    as the product of the two tap sums, and runs the reference's fp64
 *    operations on values held in 64-bit integers
 *    (vmaf_mtl_issim_term_bits(), metal_integer_ssim_math.h). It stores the
 *    term's fp64 bit pattern at the pixel's raster position: there is no
 *    reduction on the device.
 *  Host (integer_ssim_metal.mm): adds the plane in index order, which is
 *    calc_ssim()'s order, and divides by the weight sum.
 *
 *  Buffer bindings for integer_ssim_horiz_{8,16}bpc:
 *   [[buffer(0)]] ref    — const uchar * (packed luma rows)
 *   [[buffer(1)]] dis    — const uchar *
 *   [[buffer(2)]] hbuf   — device long * (5 x W x H: mux, muy, x2, xy, y2)
 *   [[buffer(3)]] params — uint4 (.x = W, .y = H, .z = ref row bytes,
 *                                 .w = dis row bytes)
 *
 *  Buffer bindings for integer_ssim_vert_terms:
 *   [[buffer(0)]] hbuf   — const device long * (pass 0)
 *   [[buffer(1)]] terms  — device ulong * (W x H fp64 bit patterns)
 *   [[buffer(2)]] params — VmafMtlIssimParams (frame, stabiliser bits)
 */

#include <metal_stdlib>
using namespace metal;

#include "metal_integer_ssim_math.h"

/* The horizontal moments of the pixel at (x, y) over the taps of its row. */
inline VmafMtlIssimSums issim_row_sums_8(const device uchar *s_row, const device uchar *d_row,
                                         uint x, uint width)
{
    const VmafMtlIssimTaps taps = vmaf_mtl_issim_tap_range(x, width);
    VmafMtlIssimSums sums = vmaf_mtl_issim_sums_make(0, 0, 0, 0, 0);
    for (int tap = taps.first; tap < taps.last; ++tap) {
        const uint source = (uint)((int)x - VMAF_MTL_ISSIM_HALF + tap);
        sums = vmaf_mtl_issim_horizontal_tap(sums, (long)vmaf_mtl_issim_kernel[tap],
                                             (long)s_row[source], (long)d_row[source]);
    }
    return sums;
}

inline VmafMtlIssimSums issim_row_sums_16(const device ushort *s_row, const device ushort *d_row,
                                          uint x, uint width)
{
    const VmafMtlIssimTaps taps = vmaf_mtl_issim_tap_range(x, width);
    VmafMtlIssimSums sums = vmaf_mtl_issim_sums_make(0, 0, 0, 0, 0);
    for (int tap = taps.first; tap < taps.last; ++tap) {
        const uint source = (uint)((int)x - VMAF_MTL_ISSIM_HALF + tap);
        sums = vmaf_mtl_issim_horizontal_tap(sums, (long)vmaf_mtl_issim_kernel[tap],
                                             (long)s_row[source], (long)d_row[source]);
    }
    return sums;
}

inline void issim_store_row_sums(device long *hbuf, uint plane, uint index, VmafMtlIssimSums sums)
{
    hbuf[0u * plane + index] = sums.mux;
    hbuf[1u * plane + index] = sums.muy;
    hbuf[2u * plane + index] = sums.x2;
    hbuf[3u * plane + index] = sums.xy;
    hbuf[4u * plane + index] = sums.y2;
}

/* ------------------------------------------------------------------ */
/*  Pass 0: horizontal moment accumulation (8 bpc)                      */
/* ------------------------------------------------------------------ */
kernel void integer_ssim_horiz_8bpc(
    const device uchar  *ref    [[buffer(0)]],
    const device uchar  *dis    [[buffer(1)]],
    device       long   *hbuf   [[buffer(2)]],
    constant     uint4  &params [[buffer(3)]],
    uint2  gid [[thread_position_in_grid]])
{
    if (gid.x >= params.x || gid.y >= params.y) {
        return;
    }
    const VmafMtlIssimSums sums =
        issim_row_sums_8(ref + gid.y * params.z, dis + gid.y * params.w, gid.x, params.x);
    issim_store_row_sums(hbuf, params.x * params.y, gid.y * params.x + gid.x, sums);
}

/* ------------------------------------------------------------------ */
/*  Pass 0: horizontal moment accumulation (16 bpc)                     */
/* ------------------------------------------------------------------ */
kernel void integer_ssim_horiz_16bpc(
    const device uchar  *ref    [[buffer(0)]],
    const device uchar  *dis    [[buffer(1)]],
    device       long   *hbuf   [[buffer(2)]],
    constant     uint4  &params [[buffer(3)]],
    uint2  gid [[thread_position_in_grid]])
{
    if (gid.x >= params.x || gid.y >= params.y) {
        return;
    }
    const device ushort *s_row = (const device ushort *)(ref + gid.y * params.z);
    const device ushort *d_row = (const device ushort *)(dis + gid.y * params.w);
    const VmafMtlIssimSums sums = issim_row_sums_16(s_row, d_row, gid.x, params.x);
    issim_store_row_sums(hbuf, params.x * params.y, gid.y * params.x + gid.x, sums);
}

/* ------------------------------------------------------------------ */
/*  Pass 1: vertical accumulation and the pixel's fp64 term             */
/* ------------------------------------------------------------------ */
kernel void integer_ssim_vert_terms(
    const device long                *hbuf   [[buffer(0)]],
    device       ulong               *terms  [[buffer(1)]],
    constant     VmafMtlIssimParams  &params [[buffer(2)]],
    uint2  gid [[thread_position_in_grid]])
{
    const uint width = params.width;
    const uint height = params.height;
    if (gid.x >= width || gid.y >= height) {
        return;
    }
    const uint plane = width * height;
    const VmafMtlIssimTaps rows = vmaf_mtl_issim_tap_range(gid.y, height);
    VmafMtlIssimSums sums = vmaf_mtl_issim_sums_make(0, 0, 0, 0, 0);
    for (int tap = rows.first; tap < rows.last; ++tap) {
        const uint source = (uint)((int)gid.y - VMAF_MTL_ISSIM_HALF + tap) * width + gid.x;
        const VmafMtlIssimSums row =
            vmaf_mtl_issim_sums_make(hbuf[0u * plane + source], hbuf[1u * plane + source],
                                     hbuf[2u * plane + source], hbuf[3u * plane + source],
                                     hbuf[4u * plane + source]);
        sums = vmaf_mtl_issim_vertical_tap(sums, (long)vmaf_mtl_issim_kernel[tap], row);
    }
    const VmafMtlIssimMoments m =
        vmaf_mtl_issim_moments(sums, vmaf_mtl_issim_tap_weight(rows),
                               vmaf_mtl_issim_tap_weight(vmaf_mtl_issim_tap_range(gid.x, width)));
    terms[gid.y * width + gid.x] =
        vmaf_mtl_issim_term_bits(m, vmaf_mtl_issim_stabilisers(params.k1_bits, params.k2_bits));
}
