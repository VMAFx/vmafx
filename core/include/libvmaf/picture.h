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

#include <libvmaf/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum  VmafPixelFormat
 * @brief Planar YUV pixel format discriminator for @ref VmafPicture.
 *
 * Drives chroma plane sizing in @ref vmaf_picture_alloc and downstream
 * feature-extractor dispatch. libvmaf only supports planar layouts — interleaved
 * formats (NV12, YUYV, ...) must be repacked by the caller before allocation.
 *
 *   - `VMAF_PIX_FMT_UNKNOWN` — sentinel / uninitialised; rejected by allocators.
 *   - `VMAF_PIX_FMT_YUV420P` — 4:2:0, chroma subsampled 2x horiz + 2x vert
 *                              (most common SDR / HDR delivery format).
 *   - `VMAF_PIX_FMT_YUV422P` — 4:2:2, chroma subsampled 2x horiz only.
 *   - `VMAF_PIX_FMT_YUV444P` — 4:4:4, no chroma subsampling.
 *   - `VMAF_PIX_FMT_YUV400P` — luma only (a.k.a. Y-only / monochrome);
 *                              `w[1] = h[1] = w[2] = h[2] = 0`.
 *
 * Stable enumerator values — append-only across libvmaf releases for ABI
 * compatibility with downstream consumers (the ffmpeg `libvmaf` filter,
 * Go/Rust bindings).
 */
enum VmafPixelFormat {
    VMAF_PIX_FMT_UNKNOWN, /**< Unset / sentinel value. */
    VMAF_PIX_FMT_YUV420P, /**< 4:2:0 chroma subsampling, planar. */
    VMAF_PIX_FMT_YUV422P, /**< 4:2:2 chroma subsampling, planar. */
    VMAF_PIX_FMT_YUV444P, /**< 4:4:4 (no subsampling), planar. */
    VMAF_PIX_FMT_YUV400P, /**< Luma only (no chroma planes). */
};

/**
 * @typedef VmafRef
 * @brief Opaque atomic refcount handle managed by libvmaf — do not dereference.
 *
 * Embedded in @ref VmafPicture::ref to track per-picture borrows across feature
 * extractors. Lifetime is bound to the owning picture: created by
 * @ref vmaf_picture_alloc, retained internally on each `vmaf_picture_ref`,
 * released on the matching @ref vmaf_picture_unref, and freed by libvmaf when
 * the count reaches zero. Callers must treat the field as opaque; the layout
 * is libvmaf-internal and may change between releases without notice.
 */
typedef struct VmafRef VmafRef;

/**
 * @struct VmafPicture
 * @brief Reference-counted planar picture passed to feature extractors.
 *
 * Allocated by `vmaf_picture_alloc()` (or obtained from the preallocated
 * pool via `vmaf_fetch_preallocated_picture()`) and released by
 * `vmaf_picture_unref()`. Ownership transfers to the `VmafContext` on
 * `vmaf_read_pictures()`; do not free / unref a picture after handing it
 * to the context.
 */
typedef struct VmafPicture {
    enum VmafPixelFormat pix_fmt; /**< planar pixel format (see VmafPixelFormat). */
    unsigned bpc;                 /**< bits per component, typically 8, 10, 12, or 16. */
    unsigned w[3];                /**< per-plane width in samples; index 0 = Y, 1 = U, 2 = V. */
    unsigned h[3];                /**< per-plane height in samples; same indexing as @ref w. */
    ptrdiff_t stride[3];          /**< per-plane row stride in **bytes** (>= w * sample_size). */
    void *data[3]; /**< per-plane sample buffer; uint8_t* when bpc <= 8, uint16_t* otherwise. */
    /* INTERNAL — do not access; layout and semantics may change without notice. */
    VmafRef *ref; /**< INTERNAL: opaque refcount handle managed by libvmaf — do not touch. */
    void *priv; /**< INTERNAL: opaque per-picture private slot managed by libvmaf — do not touch. */
} VmafPicture;

/**
 * @brief Allocate a planar picture buffer sized for the given format + dimensions.
 *
 * Sets every field of @p pic according to @p pix_fmt, @p bpc, @p w, and @p h:
 * per-plane widths/heights are derived from the chroma subsampling encoded in
 * @p pix_fmt, row strides are rounded up to a 32-byte boundary (so AVX2 /
 * AVX-512 SIMD paths can load full vector registers without tail handling),
 * and the sample buffer is contiguously allocated for all three planes. The
 * data buffers are *uninitialised*; the caller is expected to fill them
 * before passing the picture to @ref vmaf_read_pictures.
 *
 * The picture's refcount is initialised to 1. Pair every successful
 * allocation with exactly one @ref vmaf_picture_unref unless ownership is
 * transferred via @ref vmaf_read_pictures (which calls
 * @ref vmaf_picture_unref internally).
 *
 * @param pic     Out: receives the populated picture descriptor (struct passed
 *                by pointer is filled in place). Must not be NULL.
 * @param pix_fmt Planar pixel format. `VMAF_PIX_FMT_UNKNOWN` is rejected.
 * @param bpc     Bits per component, typically 8, 10, 12, or 16. Values <= 8
 *                produce a `uint8_t`-typed sample buffer; > 8 produces
 *                `uint16_t` (packed LE).
 * @param w       Luma width in samples. Must be > 0.
 * @param h       Luma height in samples. Must be > 0.
 *
 * @return 0 on success, or a negative errno code on error: `-EINVAL` on bad
 *         arguments (NULL pointer, unknown format, zero dimensions),
 *         `-ENOMEM` on allocation failure.
 *
 * @thread-safety Not thread-safe. Use one VmafContext (and its pictures) per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT int vmaf_picture_alloc(VmafPicture *pic, enum VmafPixelFormat pix_fmt, unsigned bpc,
                                   unsigned w, unsigned h);

/**
 * @brief Drop one reference to a picture, freeing its buffers when the count
 *        reaches zero.
 *
 * Symmetric to @ref vmaf_picture_alloc: every allocation contributes one
 * outstanding reference, every successful @ref vmaf_read_pictures contributes
 * one more (which libvmaf releases internally once feature extraction
 * completes). When the final reference goes, the underlying sample buffers
 * are freed and the descriptor fields are zeroed so a stale handle cannot be
 * accidentally reused.
 *
 * Safe to call with a `NULL` @p pic (no-op). Safe to call on a picture that
 * was preallocated via @ref vmaf_fetch_preallocated_picture — the picture
 * returns to its pool instead of being freed.
 *
 * @param pic Picture descriptor to unref. NULL is a no-op.
 *
 * @return 0 on success, or a negative errno code on error.
 *
 * @thread-safety Not thread-safe. Use one VmafContext (and its pictures) per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_EXPORT int vmaf_picture_unref(VmafPicture *pic);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_PICTURE_H */
