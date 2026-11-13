/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

#include "picture_sample_range.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "log.h"
#include "picture.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. ADR-1138. */

/* Column of the first sample of `row` above `limit`, or `w` when none is. */
static unsigned first_above(const uint16_t *row, unsigned w, unsigned limit)
{
    for (unsigned x = 0u; x < w; x++) {
        if (row[x] > limit) {
            return x;
        }
    }
    return w;
}

/* True when the host can read the samples of `pic`. */
int vmaf_picture_host_readable(const VmafPicture *pic)
{
    const VmafPicturePrivate *priv = pic->priv;
    if (!priv) {
        return 1;
    }
    switch (priv->buf_type) {
    case VMAF_PICTURE_BUFFER_TYPE_HOST:
    case VMAF_PICTURE_BUFFER_TYPE_CUDA_HOST_PINNED:
    case VMAF_PICTURE_BUFFER_TYPE_SYCL_HOST_PINNED:
        return 1;
    default:
        return 0;
    }
}

int vmaf_picture_check_sample_range(const VmafPicture *pic, const char *which)
{
    if (!pic || !which) {
        return -EINVAL;
    }
    if (pic->bpc <= 8u || pic->bpc >= 16u) {
        return 0;
    }
    if (!vmaf_picture_host_readable(pic)) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR,
                 "vmaf_read_pictures: the sample range check reads the %s picture on the "
                 "host, and it is in device memory\n",
                 which);
        return -ENOTSUP;
    }
    const unsigned limit = (1u << pic->bpc) - 1u;
    const unsigned planes = (pic->pix_fmt == VMAF_PIX_FMT_YUV400P) ? 1u : 3u;
    /* Plane-pointer invariant: the row loop below reads pic->data[p] for every
     * plane the format advertises.  YUV400P owns only plane 0; the three-plane
     * formats must also carry non-NULL chroma buffers, or the loop would
     * dereference NULL.  vmaf_picture_host_readable() above only checks the
     * buffer type, not the plane pointers. */
    assert(pic->data[0] != NULL);
    assert(planes == 1u || (pic->data[1] != NULL && pic->data[2] != NULL));
    for (unsigned p = 0u; p < planes; p++) {
        /* Stride-bound invariant: by the early return above bpc is 9..15, so
         * every sample is a uint16_t (2 bytes).  Casting base + y*stride[p] to
         * uint16_t* and reading row[x] for x < w[p] stays in-bounds only if
         * stride[p] covers w[p] samples.  stride is signed ptrdiff_t; compare
         * the byte width of one row on the same signed type to keep
         * -Wsign-compare quiet. */
        assert(pic->stride[p] >= (ptrdiff_t)pic->w[p] * (ptrdiff_t)sizeof(uint16_t));
        const uint8_t *base = (const uint8_t *)pic->data[p];
        for (unsigned y = 0u; y < pic->h[p]; y++) {
            const uint16_t *row = (const uint16_t *)(base + ((size_t)y * (size_t)pic->stride[p]));
            const unsigned x = first_above(row, pic->w[p], limit);
            if (x < pic->w[p]) {
                vmaf_log(VMAF_LOG_LEVEL_ERROR,
                         "vmaf_read_pictures: %s picture, plane %u, row %u, column %u: sample "
                         "%u is above %u, the largest %u-bit value\n",
                         which, p, y, x, (unsigned)row[x], limit, pic->bpc);
                return -EINVAL;
            }
        }
    }
    return 0;
}

/* NOLINTEND(modernize-use-nullptr) */
