/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Plane geometry shared by the picture allocators and the extractors that
 *  size their own buffers for a plane they copy.
 */

#ifndef VMAF_SRC_PICTURE_GEOMETRY_H_
#define VMAF_SRC_PICTURE_GEOMETRY_H_

/* Extent of a chroma plane along one axis: ceil(luma / 2) when the format
 * subsamples that axis (subsampled = 1), the luma extent otherwise
 * (subsampled = 0). An odd luma width or height gets the extra chroma column
 * or row that covers its last luma sample (Research-0094).
 *
 * vmaf_picture_alloc() (picture.c) and the CUDA picture pool
 * (cuda/picture_cuda.c) set VmafPicture::w[1..2] / h[1..2] with this, and
 * picture_copy() copies exactly those extents. Any extractor that allocates a
 * buffer for a chroma plane must size it from the same function: a floor
 * division here under-allocates by one row or column for odd sizes. */
static inline unsigned vmaf_chroma_extent(unsigned luma, unsigned subsampled)
{
    return (luma + subsampled) >> subsampled;
}

#endif /* VMAF_SRC_PICTURE_GEOMETRY_H_ */
