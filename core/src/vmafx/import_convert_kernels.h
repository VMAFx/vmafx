/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Planarisation of imported semi-planar and packed frames on a device (RC4
 * WP3, ADR-1929 item 6, ADR-2133): the chroma of NV12 / NV16 / NV24 and the
 * P0xx / P2xx / P4xx family de-interleaved into two planes, the P010-style
 * luma shifted by 6 into a plane of its own, and the packed layouts (Y210,
 * Y212, YUYV422, Y410 / XV30, XV36, VUYX) and planar words with the sample
 * in the top bits (YUV444P_MSB) gathered into planes. Nothing else: every
 * output sample is an input sample, right-shifted by `shift` and (XV30)
 * masked to 10 bits, so an imported frame scores bit for bit as the same
 * frame uploaded from the host. The host reference is
 * vmafx_import_read_plane() (core/src/vmafx/import_convert.h); a change to
 * either changes both.
 *
 * One definition for the CUDA and HIP lanes (HISS-19): nvcc compiles it
 * through core/src/cuda/import_convert.cu (ADR-2023), hipcc through
 * core/src/hip/import_convert.hip (ADR-2092); the kernel syntax is the one
 * both accept. Device code only.
 *
 * Inputs are read a byte at a time and assembled little-endian, so a plane
 * may start at any byte; outputs are planes of the library's own
 * (pitch-aligned). One thread per output sample, a 2D grid over the plane.
 *
 * The header holds each kernel's body; import_convert.cu and
 * import_convert.hip define the extern "C" __global__ entry points the
 * module lookup finds (vmafx_import_deint_8, vmafx_import_deint_16,
 * vmafx_import_shift_16, vmafx_import_gather), each calling its `_body` here, as
 * feature/float_moment_sum_gpu.h does for the moment twins.
 */

#ifndef VMAF_SRC_VMAFX_IMPORT_CONVERT_KERNELS_H_
#define VMAF_SRC_VMAFX_IMPORT_CONVERT_KERNELS_H_

#include <cstddef>
#include <cstdint>

static __device__ __forceinline__ unsigned vmafx_import_load16(const uint8_t *row, unsigned i)
{
    const size_t at = (size_t)2u * i;
    return (unsigned)row[at] | ((unsigned)row[at + 1u] << 8u);
}

static __device__ __forceinline__ void vmafx_import_store16(uint8_t *row, unsigned i, unsigned v)
{
    reinterpret_cast<uint16_t *>(row)[i] = (uint16_t)v;
}

/* NV12: src row y holds w pairs (Cb, Cr) of 8-bit samples. */
static __device__ __forceinline__ void vmafx_import_deint_8_body(const uint8_t *src,
                                                                 size_t src_pitch, uint8_t *cb,
                                                                 uint8_t *cr, size_t dst_pitch,
                                                                 unsigned w, unsigned h)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    const uint8_t *const row = src + (size_t)y * src_pitch;
    const size_t at = (size_t)2u * x;
    cb[(size_t)y * dst_pitch + x] = row[at];
    cr[(size_t)y * dst_pitch + x] = row[at + 1u];
}

/* P010 / P016: src row y holds w pairs (Cb, Cr) of little-endian 16-bit
 * samples, each shifted right by `shift` (6 for P010, 0 for P016). */
static __device__ __forceinline__ void
vmafx_import_deint_16_body(const uint8_t *src, size_t src_pitch, uint8_t *cb, uint8_t *cr,
                           size_t dst_pitch, unsigned w, unsigned h, unsigned shift)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    const uint8_t *const row = src + (size_t)y * src_pitch;
    vmafx_import_store16(cb + (size_t)y * dst_pitch, x, vmafx_import_load16(row, 2u * x) >> shift);
    vmafx_import_store16(cr + (size_t)y * dst_pitch, x,
                         vmafx_import_load16(row, 2u * x + 1u) >> shift);
}

/* P010 luma: each little-endian 16-bit sample shifted right by `shift`. */
static __device__ __forceinline__ void vmafx_import_shift_16_body(const uint8_t *src,
                                                                  size_t src_pitch, uint8_t *dst,
                                                                  size_t dst_pitch, unsigned w,
                                                                  unsigned h, unsigned shift)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    vmafx_import_store16(dst + (size_t)y * dst_pitch, x,
                         vmafx_import_load16(src + (size_t)y * src_pitch, x) >> shift);
}

/* Packed layouts and planar words with the sample in the top bits: one output
 * plane from the producer's plane by the plan of vmafx_import_plane_read()
 * (core/src/vmafx/import_convert.h, VmafxImportRead): sample x of row y is the
 * element x * step + offset of the producer's row (1, 2 or 4 bytes,
 * little-endian), shifted right by `shift` and masked with `mask` (0: none),
 * stored as 1 or 2 bytes. One thread per output sample; the host launches it
 * once per output plane. */
static __device__ __forceinline__ void
vmafx_import_gather_body(const uint8_t *src, size_t src_pitch, uint8_t *dst, size_t dst_pitch,
                         unsigned w, unsigned h, unsigned step, unsigned offset, unsigned in_bytes,
                         unsigned shift, unsigned mask, unsigned out_bytes)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    const uint8_t *const row = src + (size_t)y * src_pitch;
    const size_t at = ((size_t)x * step + offset) * in_bytes;
    unsigned v = row[at];
    if (in_bytes >= 2u) {
        v |= (unsigned)row[at + 1u] << 8u;
    }
    if (in_bytes == 4u) {
        v |= ((unsigned)row[at + 2u] << 16u) | ((unsigned)row[at + 3u] << 24u);
    }
    v >>= shift;
    if (mask != 0u) {
        v &= mask;
    }
    if (out_bytes == 1u) {
        dst[(size_t)y * dst_pitch + x] = (uint8_t)v;
    } else {
        vmafx_import_store16(dst + (size_t)y * dst_pitch, x, v);
    }
}

#endif /* VMAF_SRC_VMAFX_IMPORT_CONVERT_KERNELS_H_ */
