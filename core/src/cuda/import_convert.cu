/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Planarisation of imported semi-planar frames on a CUDA device (RC4 WP3,
 * ADR-1929 item 6, ADR-2023): NV12 / P010 / P016 chroma de-interleaved into
 * two planes, and the P010 luma shifted by 6 into a plane of its own. Nothing
 * else: every output sample is an input sample, right-shifted by `shift`, so
 * an imported frame scores bit for bit as the same frame uploaded from the
 * host. The host reference is the row reader of core/src/metal/
 * iosurface_layout.h (vmaf_metal_read_plane()); a change to either changes
 * both.
 *
 * Inputs are read a byte at a time and assembled little-endian, so a plane
 * may start at any byte; outputs are planes of the library's own
 * (pitch-aligned). One thread per output sample, a 2D grid over the plane.
 */

#include <cstddef>
#include <cstdint>

namespace
{

__device__ __forceinline__ unsigned load16(const uint8_t *row, unsigned i)
{
    const size_t at = (size_t)2u * i;
    return (unsigned)row[at] | ((unsigned)row[at + 1u] << 8u);
}

__device__ __forceinline__ void store16(uint8_t *row, unsigned i, unsigned v)
{
    reinterpret_cast<uint16_t *>(row)[i] = (uint16_t)v;
}

} // namespace

extern "C" {

/* NV12: src row y holds w pairs (Cb, Cr) of 8-bit samples. */
__global__ void vmafx_import_deint_8(const uint8_t *src, size_t src_pitch, uint8_t *cb, uint8_t *cr,
                                     size_t dst_pitch, unsigned w, unsigned h)
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
__global__ void vmafx_import_deint_16(const uint8_t *src, size_t src_pitch, uint8_t *cb,
                                      uint8_t *cr, size_t dst_pitch, unsigned w, unsigned h,
                                      unsigned shift)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    const uint8_t *const row = src + (size_t)y * src_pitch;
    store16(cb + (size_t)y * dst_pitch, x, load16(row, 2u * x) >> shift);
    store16(cr + (size_t)y * dst_pitch, x, load16(row, 2u * x + 1u) >> shift);
}

/* P010 luma: each little-endian 16-bit sample shifted right by `shift`. */
__global__ void vmafx_import_shift_16(const uint8_t *src, size_t src_pitch, uint8_t *dst,
                                      size_t dst_pitch, unsigned w, unsigned h, unsigned shift)
{
    const unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) {
        return;
    }
    store16(dst + (size_t)y * dst_pitch, x, load16(src + (size_t)y * src_pitch, x) >> shift);
}

} /* extern "C" */
