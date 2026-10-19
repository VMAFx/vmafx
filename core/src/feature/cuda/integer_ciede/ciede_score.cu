/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright (c) 2019 Joshua Holmer
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent AND MIT
 *
 *  CUDA compute kernel for the ciede2000 feature extractor
 *  (T7-23 / batch 1c part 2; the CPU's arithmetic since ADR-1426).
 *
 *  Each thread converts one pixel pair to L*a*b* and computes its CIEDE2000
 *  difference with ciede_device.h, which is core/src/feature/ciede.c
 *  statement for statement: fp64 where the reference computes in double,
 *  float where it stores in float. The value is written to its raster
 *  position; nothing is reduced on the device, because the reference adds
 *  every pixel into one double in raster order and a sum in another order
 *  rounds differently. The host adds the plane it reads back and applies
 *  `45 - 20 * log10(mean)`.
 *
 *  Subsampling: the kernel grid is at luma resolution. Each thread reads the
 *  chroma at the subsampled position derived from ss_hor / ss_ver, matching
 *  the reference's scale_chroma_planes() nearest-neighbour upscale.
 */

#include "cuda_helper.cuh"
#include "cuda/integer_ciede_cuda.h"
#include "common.h"
#include "cuda_device_ptr.cuh"

#include "ciede_device.h"

extern "C" {

/* Channel reads follow ADR-0762: typed `__restrict__` row pointers taken from
 * the picture before the per-pixel body, every sample through __ldg(). */

__global__ void calculate_ciede_kernel_8bpc(const VmafPicture ref, const VmafPicture dis,
                                            VmafCudaBuffer terms, unsigned width, unsigned height,
                                            unsigned bpc, unsigned ss_hor, unsigned ss_ver)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;

    const unsigned cx = ss_hor ? (x >> 1) : x;
    const unsigned cy = ss_ver ? (y >> 1) : y;
    const uint8_t *__restrict__ r_y = (const uint8_t *)ref.data[0] + (size_t)y * ref.stride[0];
    const uint8_t *__restrict__ r_u = (const uint8_t *)ref.data[1] + (size_t)cy * ref.stride[1];
    const uint8_t *__restrict__ r_v = (const uint8_t *)ref.data[2] + (size_t)cy * ref.stride[2];
    const uint8_t *__restrict__ d_y = (const uint8_t *)dis.data[0] + (size_t)y * dis.stride[0];
    const uint8_t *__restrict__ d_u = (const uint8_t *)dis.data[1] + (size_t)cy * dis.stride[1];
    const uint8_t *__restrict__ d_v = (const uint8_t *)dis.data[2] + (size_t)cy * dis.stride[2];

    float *const term_out = VMAF_CUDA_DPTR(float, terms.data);
    term_out[(size_t)y * width + x] =
        ciede_pixel((float)__ldg(&r_y[x]), (float)__ldg(&r_u[cx]), (float)__ldg(&r_v[cx]),
                    (float)__ldg(&d_y[x]), (float)__ldg(&d_u[cx]), (float)__ldg(&d_v[cx]), bpc);
}

__global__ void calculate_ciede_kernel_16bpc(const VmafPicture ref, const VmafPicture dis,
                                             VmafCudaBuffer terms, unsigned width, unsigned height,
                                             unsigned bpc, unsigned ss_hor, unsigned ss_ver)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;

    const unsigned cx = ss_hor ? (x >> 1) : x;
    const unsigned cy = ss_ver ? (y >> 1) : y;
    const uint16_t *__restrict__ r_y =
        (const uint16_t *)((const uint8_t *)ref.data[0] + (size_t)y * ref.stride[0]);
    const uint16_t *__restrict__ r_u =
        (const uint16_t *)((const uint8_t *)ref.data[1] + (size_t)cy * ref.stride[1]);
    const uint16_t *__restrict__ r_v =
        (const uint16_t *)((const uint8_t *)ref.data[2] + (size_t)cy * ref.stride[2]);
    const uint16_t *__restrict__ d_y =
        (const uint16_t *)((const uint8_t *)dis.data[0] + (size_t)y * dis.stride[0]);
    const uint16_t *__restrict__ d_u =
        (const uint16_t *)((const uint8_t *)dis.data[1] + (size_t)cy * dis.stride[1]);
    const uint16_t *__restrict__ d_v =
        (const uint16_t *)((const uint8_t *)dis.data[2] + (size_t)cy * dis.stride[2]);

    float *const term_out = VMAF_CUDA_DPTR(float, terms.data);
    term_out[(size_t)y * width + x] =
        ciede_pixel((float)__ldg(&r_y[x]), (float)__ldg(&r_u[cx]), (float)__ldg(&r_v[cx]),
                    (float)__ldg(&d_y[x]), (float)__ldg(&d_u[cx]), (float)__ldg(&d_v[cx]), bpc);
}

} /* extern "C" */
