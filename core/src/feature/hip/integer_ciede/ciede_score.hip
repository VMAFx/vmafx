/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2019 Joshua Holmer
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND MIT
 *
 *  HIP compute kernel for the ciede2000 feature extractor (ADR-0377; the
 *  CPU's arithmetic since ADR-1448).
 *
 *  Each thread converts one pixel pair to L*a*b* and computes its CIEDE2000
 *  difference with feature/ciede_ff_math.h, which is core/src/feature/ciede.c
 *  statement for statement with every fp64 value as an fp32 pair and every
 *  fp64 math-library call as a pair function (feature/ff_math.h). It is the
 *  arithmetic of the SYCL twin (ADR-1436), from the same header, on the
 *  primitives integer_ciede/ciede_hip_math.h names. The device has an fp64
 *  type, but its fp64 pow(), atan2(), sin(), cos() and exp() take 17 times
 *  the frame time of the fp32 kernel this replaces (ADR-1448).
 *
 *  The value is written to its raster position; nothing is reduced on the
 *  device, because the reference adds every pixel into one double in raster
 *  order and a sum in another order rounds differently. The host adds the
 *  plane it reads back and applies `45 - 20 * log10(mean)`.
 *
 *  What the twin does not share with the CPU: a pair carries about 48 bits
 *  where the reference has 53, which decides the rounding of a float
 *  differently on a few pixels in ten million, and ciede.c's float power
 *  powf(x, 7) is the correctly rounded value here where glibc's powf() is not
 *  always (ADR-1448 has the count; the float square is a product on both
 *  sides since ADR-1467). The arithmetic around it is exact: this file is
 *  built with the strict FP list of core/src/meson.build.
 *
 *  ciede.c's constants are evaluated by the compiler (make_constants() is
 *  constexpr), one set per bit depth, and the two tables of ff_math.h are
 *  constants of the module; the host passes neither.
 *
 *  Subsampling: the kernel grid is at luma resolution. Each thread reads the
 *  chroma at the subsampled position derived from ss_hor / ss_ver, matching
 *  the reference's scale_chroma_planes() nearest-neighbour upscale.
 */

#include <hip/hip_runtime.h>
#include <stddef.h>
#include <stdint.h>

/* feature/ciede_ff_math.h on a HIP device's primitives. */
#include "feature/hip/integer_ciede/ciede_hip_math.h"

/* ciede.c's constants for the bit depths a picture can have: entry `bpc - 8`
 * (ciede_hip_depth_index() in ciede_hip.c). */
static constexpr vmaf_hip_ciede::Constants kCiedeConstants[9] = {
    vmaf_hip_ciede::make_constants(8u),  vmaf_hip_ciede::make_constants(9u),
    vmaf_hip_ciede::make_constants(10u), vmaf_hip_ciede::make_constants(11u),
    vmaf_hip_ciede::make_constants(12u), vmaf_hip_ciede::make_constants(13u),
    vmaf_hip_ciede::make_constants(14u), vmaf_hip_ciede::make_constants(15u),
    vmaf_hip_ciede::make_constants(16u),
};

/* The planes of one picture: packed rows, the chroma planes at their own
 * resolution. */
struct CiedeHipPlanes {
    const uint8_t *y;
    const uint8_t *u;
    const uint8_t *v;
    ptrdiff_t y_stride; /* bytes */
    ptrdiff_t c_stride;
};

/* The three samples of pixel (x, y) as the floats ciede.c converts them to. */
template <typename Sample>
__device__ __forceinline__ vmaf_hip_ciede::Samples
ciede_samples(const CiedeHipPlanes &p, unsigned x, unsigned y, unsigned cx, unsigned cy)
{
    return {.y = (float)reinterpret_cast<const Sample *>(p.y + (size_t)y * p.y_stride)[x],
            .u = (float)reinterpret_cast<const Sample *>(p.u + (size_t)cy * p.c_stride)[cx],
            .v = (float)reinterpret_cast<const Sample *>(p.v + (size_t)cy * p.c_stride)[cx]};
}

/* One pixel's CIEDE2000 difference into its raster position. */
template <typename Sample>
__device__ __forceinline__ void
ciede_store_pixel(const CiedeHipPlanes &ref, const CiedeHipPlanes &dis, float *terms,
                  unsigned width, unsigned height, unsigned depth, unsigned ss_hor, unsigned ss_ver)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;
    const unsigned cx = ss_hor ? (x >> 1) : x;
    const unsigned cy = ss_ver ? (y >> 1) : y;
    const vmaf_hip_ffm::Tables tables = {.atan = vmaf_hip_ffm::kAtanTable,
                                         .sin_cos = vmaf_hip_ffm::kSinCosTable};
    terms[(size_t)y * width + x] = vmaf_hip_ciede::pixel(
        ciede_samples<Sample>(ref, x, y, cx, cy), ciede_samples<Sample>(dis, x, y, cx, cy),
        kCiedeConstants[depth > 8u ? 8u : depth], tables);
}

extern "C" {

/* 8-bpc kernel: the six planes hold uint8_t samples. `depth` indexes
 * kCiedeConstants. */
__global__ void calculate_ciede_kernel_8bpc(CiedeHipPlanes ref, CiedeHipPlanes dis, float *terms,
                                            unsigned width, unsigned height, unsigned depth,
                                            unsigned ss_hor, unsigned ss_ver)
{
    ciede_store_pixel<uint8_t>(ref, dis, terms, width, height, depth, ss_hor, ss_ver);
}

/* 10/12/16-bpc kernel: the six planes hold uint16_t samples. */
__global__ void calculate_ciede_kernel_16bpc(CiedeHipPlanes ref, CiedeHipPlanes dis, float *terms,
                                             unsigned width, unsigned height, unsigned depth,
                                             unsigned ss_hor, unsigned ss_ver)
{
    ciede_store_pixel<uint16_t>(ref, dis, terms, width, height, depth, ss_hor, ss_ver);
}

} /* extern "C" */
