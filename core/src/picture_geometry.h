/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Plane geometry shared by the picture allocators and the extractors that
 *  size their own buffers for a plane they copy.
 */

#ifndef VMAF_SRC_PICTURE_GEOMETRY_H_
#define VMAF_SRC_PICTURE_GEOMETRY_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extent of a chroma plane along one axis: ceil(luma / 2) when the format
 * subsamples that axis, the luma extent otherwise. An odd luma width or
 * height gets the extra chroma column or row that covers its last luma
 * sample (Research-0094). Written as floor + remainder so that it cannot
 * wrap for any unsigned luma extent.
 *
 * vmaf_picture_alloc() (picture.c) and the CUDA picture pool
 * (cuda/picture_cuda.c) set VmafPicture::w[1..2] / h[1..2] with this, and
 * picture_copy() copies exactly those extents. An extractor that allocates a
 * buffer for a chroma plane sizes it from the same function: a floor division
 * under-allocates by one row or column for odd sizes. */
static inline unsigned vmaf_chroma_extent(unsigned luma, bool subsampled)
{
    return subsampled ? (luma >> 1u) + (luma & 1u) : luma;
}

#ifdef __cplusplus
}
#endif

#endif /* VMAF_SRC_PICTURE_GEOMETRY_H_ */
