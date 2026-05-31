/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#ifndef LIBVMAF_PICTURE_H
#define LIBVMAF_PICTURE_H

#include <stddef.h>

#include "libvmaf/macros.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pixel format of a @ref VmafPicture. Mirrors the FFmpeg YUV planar
 * formats libvmaf consumes.
 */
enum VmafPixelFormat {
    VMAF_PIX_FMT_UNKNOWN, /**< Unset / sentinel value. */
    VMAF_PIX_FMT_YUV420P, /**< 4:2:0 chroma subsampling, planar. */
    VMAF_PIX_FMT_YUV422P, /**< 4:2:2 chroma subsampling, planar. */
    VMAF_PIX_FMT_YUV444P, /**< 4:4:4 (no subsampling), planar. */
    VMAF_PIX_FMT_YUV400P, /**< Luma only (no chroma planes). */
};

/** Opaque reference-counted handle backing a `VmafPicture`. */
typedef struct VmafRef VmafRef;

/**
 * Frame the library scores. Populated by the caller (or by
 * `vmaf_picture_alloc`); released with `vmaf_picture_unref`.
 */
typedef struct VmafPicture {
    enum VmafPixelFormat pix_fmt; /**< Pixel format. */
    unsigned bpc;                 /**< Bits per component (8 / 10 / 12 / 16). */
    unsigned w[3];                /**< Per-plane width. */
    unsigned h[3];                /**< Per-plane height. */
    ptrdiff_t stride[3];          /**< Per-plane row stride, in bytes. */
    void *data[3];                /**< Per-plane sample buffer. */
    VmafRef *ref;                 /**< Internal refcount cookie; do not touch. */
    void *priv;                   /**< Opaque slot for caller-owned metadata. */
} VmafPicture;

/**
 * Allocate the per-plane buffers + refcount cookie of @p pic.
 *
 * @param pic      Caller-owned picture struct to populate.
 * @param pix_fmt  Pixel format.
 * @param bpc      Bits per component (8 / 10 / 12 / 16).
 * @param w        Luma (frame) width.
 * @param h        Luma (frame) height.
 *
 * @return 0 on success, a negative errno on failure.
 */
VMAF_EXPORT int vmaf_picture_alloc(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned bpc,
                                   unsigned w, unsigned h);

/**
 * Drop the caller's reference on @p pic. When the last reference is
 * released, the per-plane buffers are freed and @p pic is zeroed.
 *
 * @param pic  Picture to release.
 *
 * @return 0 on success, a negative errno on failure.
 */
VMAF_EXPORT int vmaf_picture_unref(VmafPicture *pic);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_PICTURE_H */
