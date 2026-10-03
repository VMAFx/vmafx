/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2019 Joshua Holmer
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND MIT
 *
 *  Metal compute kernel for the ciede2000 feature extractor (ADR-0421; the
 *  CPU's arithmetic since ADR-1498).
 *
 *  Each thread converts one pixel pair to L*a*b* and computes its CIEDE2000
 *  difference with feature/ciede_ff_math.h, which is core/src/feature/ciede.c
 *  statement for statement with every fp64 value as an fp32 pair and every
 *  fp64 math-library call as a pair function (feature/ff_math.h): the
 *  arithmetic of the SYCL and HIP twins (ADR-1436, ADR-1448), from the same
 *  header, on the Metal primitives metal_ciede_math.h names. Metal has no fp64
 *  type; the pair arithmetic needs fp32 with fma, correctly rounded division
 *  and square root, and no contraction, which this file gets from the strict
 *  FP list of core/src/metal/meson.build (-fno-fast-math -ffp-contract=off).
 *
 *  The value is written to its raster position; nothing is reduced on the
 *  device, because the reference adds every pixel into one double in raster
 *  order and a sum in another order rounds differently. The host
 *  (integer_ciede_metal.mm) adds the plane with ciede_frame_sum() and applies
 *  `45 - 20 * log10(mean)`.
 *
 *  ciede.c's constants for the frame's bit depth come from the host, which
 *  evaluates make_constants() in fp64 (buffer 8); the two tables of
 *  ff_math.h are constants of the program.
 *
 *  Subsampling: the host upscales U and V to luma resolution with the
 *  reference's nearest-neighbour rule (ciede.c's scale_chroma_planes()), so
 *  the six planes are packed at luma resolution, `width` samples per row.
 *
 *  Buffer bindings (both kernels; host must match integer_ciede_metal.mm):
 *   [[buffer(0..2)]] ref_y, ref_u, ref_v -- const uchar / ushort *
 *   [[buffer(3..5)]] dis_y, dis_u, dis_v -- const uchar / ushort *
 *   [[buffer(6)]]    terms               -- float *, width x height
 *   [[buffer(7)]]    dim                 -- uint2 (width, height)
 *   [[buffer(8)]]    constants           -- vmaf_metal_ciede::Constants
 */

#include "metal_ciede_math.h"

/* The three samples of a pixel as the floats ciede.c converts them to. */
template <typename Sample>
inline vmaf_metal_ciede::Samples ciede_samples(const device Sample *y, const device Sample *u,
                                               const device Sample *v, uint i)
{
    const vmaf_metal_ciede::Samples s = {(float)y[i], (float)u[i], (float)v[i]};
    return s;
}

/* One pixel's CIEDE2000 difference, ciede.c's statements. */
inline float ciede_pixel(vmaf_metal_ciede::Samples ref, vmaf_metal_ciede::Samples dis,
                         constant vmaf_metal_ciede::Constants &constants)
{
    const vmaf_metal_ciede::Constants k = constants;
    const vmaf_metal_ffm::Tables tables = {vmaf_metal_ffm::kAtanTable,
                                           vmaf_metal_ffm::kSinCosTable};
    return vmaf_metal_ciede::pixel(ref, dis, k, tables);
}

/* Every pixel's value into its raster position. */
template <typename Sample>
inline void ciede_store_pixel(const device Sample *ref_y, const device Sample *ref_u,
                              const device Sample *ref_v, const device Sample *dis_y,
                              const device Sample *dis_u, const device Sample *dis_v,
                              device float *terms, uint2 dim,
                              constant vmaf_metal_ciede::Constants &constants, uint2 gid)
{
    if (gid.x >= dim.x || gid.y >= dim.y) {
        return;
    }
    const uint i = gid.y * dim.x + gid.x;
    terms[i] = ciede_pixel(ciede_samples(ref_y, ref_u, ref_v, i),
                           ciede_samples(dis_y, dis_u, dis_v, i), constants);
}

/* 8 bpc: the six planes hold uchar samples. */
kernel void integer_ciede_kernel_8bpc(const device uchar *ref_y [[buffer(0)]],
                                      const device uchar *ref_u [[buffer(1)]],
                                      const device uchar *ref_v [[buffer(2)]],
                                      const device uchar *dis_y [[buffer(3)]],
                                      const device uchar *dis_u [[buffer(4)]],
                                      const device uchar *dis_v [[buffer(5)]],
                                      device float *terms [[buffer(6)]],
                                      constant uint2 &dim [[buffer(7)]],
                                      constant vmaf_metal_ciede::Constants &constants
                                      [[buffer(8)]],
                                      uint2 gid [[thread_position_in_grid]])
{
    ciede_store_pixel(ref_y, ref_u, ref_v, dis_y, dis_u, dis_v, terms, dim, constants, gid);
}

/* 10, 12 and 16 bpc: the six planes hold ushort samples. */
kernel void integer_ciede_kernel_16bpc(const device ushort *ref_y [[buffer(0)]],
                                       const device ushort *ref_u [[buffer(1)]],
                                       const device ushort *ref_v [[buffer(2)]],
                                       const device ushort *dis_y [[buffer(3)]],
                                       const device ushort *dis_u [[buffer(4)]],
                                       const device ushort *dis_v [[buffer(5)]],
                                       device float *terms [[buffer(6)]],
                                       constant uint2 &dim [[buffer(7)]],
                                       constant vmaf_metal_ciede::Constants &constants
                                       [[buffer(8)]],
                                       uint2 gid [[thread_position_in_grid]])
{
    ciede_store_pixel(ref_y, ref_u, ref_v, dis_y, dis_u, dis_v, terms, dim, constants, gid);
}
