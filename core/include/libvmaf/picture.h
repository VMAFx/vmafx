/* SPDX-License-Identifier: BSD-2-Clause-Patent */
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
/* NOLINTBEGIN(performance-enum-size): C header included by C and C++ translation units; C has no fixed enum underlying type across the required toolchains (ADR-1470). ADR-1138. */
enum VmafPixelFormat {
    VMAF_PIX_FMT_UNKNOWN, /**< Unset / sentinel value. */
    VMAF_PIX_FMT_YUV420P, /**< 4:2:0 chroma subsampling, planar. */
    VMAF_PIX_FMT_YUV422P, /**< 4:2:2 chroma subsampling, planar. */
    VMAF_PIX_FMT_YUV444P, /**< 4:4:4 (no subsampling), planar. */
    VMAF_PIX_FMT_YUV400P, /**< Luma only (no chroma planes). */
};
/* NOLINTEND(performance-enum-size) */

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
/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units; C has no `using`. ADR-1138. */
typedef struct VmafRef VmafRef;
/* NOLINTEND(modernize-use-using) */

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
/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units; C has no `using`. ADR-1138. */
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
/* NOLINTEND(modernize-use-using) */

/**
 * @brief Allocate a planar picture buffer sized for the given format + dimensions.
 *
 * Sets every field of @p pic according to @p pix_fmt, @p bpc, @p w, and @p h:
 * per-plane widths/heights are derived from the chroma subsampling encoded in
 * @p pix_fmt, each plane's row stride is the plane width rounded up to a
 * multiple of 64 samples (64 bytes for 8-bit, 128 bytes for >8-bit; so AVX2 /
 * AVX-512 SIMD paths can load full vector registers without tail handling),
 * and the sample buffer is contiguously allocated for all three planes on a
 * 64-byte boundary. The buffer is zero-filled; the caller is expected to
 * overwrite it with the sample data before passing the picture to
 * @ref vmaf_read_pictures.
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
 * @note Thread safety: Not thread-safe. Use one VmafContext (and its pictures) per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_DEPRECATED("use vmafx_frame_create_host")
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
 * @note Thread safety: Not thread-safe. Use one VmafContext (and its pictures) per thread.
 *
 * @since libvmaf 3.0.0 (upstream).
 */
VMAF_DEPRECATED("use vmafx_frame_unref")
VMAF_EXPORT int vmaf_picture_unref(VmafPicture *pic);

/**
 * @enum  VmafColorRange
 * @brief Sample range of a picture's code values (Netflix/vmaf 0497a0f29).
 *
 * `VMAF_COLOR_RANGE_UNKNOWN` is the unset value; the converter rejects it.
 * Enumerator values are append-only.
 */
enum VmafColorRange {
    VMAF_COLOR_RANGE_UNKNOWN, /**< Unset. */
    VMAF_COLOR_RANGE_LIMITED, /**< Studio / limited range. */
    VMAF_COLOR_RANGE_FULL,    /**< Full range. */
};

/** @enum VmafColorPrimaries @brief Colour primaries. Append-only. */
enum VmafColorPrimaries {
    VMAF_COLOR_PRIMARIES_UNKNOWN = 0, /**< Unset. */
    VMAF_COLOR_PRIMARIES_BT709,       /**< ITU-R BT.709. */
    VMAF_COLOR_PRIMARIES_BT2020,      /**< ITU-R BT.2020. */
    VMAF_COLOR_PRIMARIES_SMPTE432,    /**< SMPTE ST 432-1 (DCI-P3 D65). */
};

/** @enum VmafColorTransferCharacteristic @brief Transfer function. Append-only. */
enum VmafColorTransferCharacteristic {
    VMAF_COLOR_TRC_UNKNOWN = 0, /**< Unset. */
    VMAF_COLOR_TRC_BT709,       /**< ITU-R BT.709. */
    VMAF_COLOR_TRC_SMPTE2084,   /**< SMPTE ST 2084 (PQ). */
    VMAF_COLOR_TRC_SRGB, /**< IEC 61966-2-1 (sRGB; RGB input statement; the converter refuses it). */
    VMAF_COLOR_TRC_HLG,  /**< ARIB STD-B67 (HLG; refused by the converter). */
    VMAF_COLOR_TRC_LINEAR, /**< Linear light (refused). */
};

/** @enum VmafColorMatrixCoefficients @brief YCbCr matrix. Append-only. */
enum VmafColorMatrixCoefficients {
    VMAF_COLOR_MATRIX_UNKNOWN = 0, /**< Unset. */
    VMAF_COLOR_MATRIX_BT709,       /**< ITU-R BT.709. */
    VMAF_COLOR_MATRIX_BT2020_NCL,  /**< ITU-R BT.2020 non-constant luminance. */
    VMAF_COLOR_MATRIX_ICTCP,       /**< ICtCp. */
    VMAF_COLOR_MATRIX_BT601, /**< ITU-R BT.601 (RGB input conversion; the converter refuses it). */
    VMAF_COLOR_MATRIX_BT2020_CL, /**< ITU-R BT.2020 constant luminance (declared; refused). */
};

/**
 * @struct VmafColor
 * @brief Colour description of a picture: range, primaries, transfer function, matrix.
 *
 * Same type as upstream Netflix/vmaf 0497a0f29. Upstream also embeds one in
 * `VmafPicture`; the fork does not (binary compatibility, ADR-1822), so the
 * colour of a source picture travels as an argument of
 * `vmaf_picture_convert_context_init_with_color` instead.
 */
typedef struct VmafColor {
    enum VmafColorRange range;                /**< Code-value range. */
    enum VmafColorPrimaries primaries;        /**< Colour primaries. */
    enum VmafColorTransferCharacteristic trc; /**< Transfer function. */
    enum VmafColorMatrixCoefficients matrix;  /**< YCbCr matrix. */
} VmafColor;

/**
 * @enum  VmafResampleFilter
 * @brief Scaling filter used when the target size differs from the source size.
 */
enum VmafResampleFilter {
    VMAF_RESAMPLE_DEFAULT,  /**< The converter's default (bicubic). */
    VMAF_RESAMPLE_BILINEAR, /**< Bilinear. */
    VMAF_RESAMPLE_BICUBIC,  /**< Bicubic. */
    VMAF_RESAMPLE_LANCZOS,  /**< Lanczos. */
};

/**
 * @struct VmafPictureConvertTarget
 * @brief Format a converted picture is produced in.
 *
 * `w` / `h` of 0 keep the source size. Every field of `color` must be set.
 */
typedef struct VmafPictureConvertTarget {
    enum VmafPixelFormat pix_fmt;            /**< Target pixel format (not UNKNOWN). */
    unsigned bpc;                            /**< Target bits per component, 8 to 16. */
    unsigned w;                              /**< Target luma width, 0 = source width. */
    unsigned h;                              /**< Target luma height, 0 = source height. */
    VmafColor color;                         /**< Target colour description. */
    enum VmafResampleFilter resample_filter; /**< Scaling filter. */
} VmafPictureConvertTarget;

/**
 * @typedef VmafPictureConvertContext
 * @brief Opaque conversion graph.
 */
/* NOLINTBEGIN(modernize-use-using): C header included by C and C++ translation units; C has no `using`. ADR-1138. */
typedef struct VmafPictureConvertContext VmafPictureConvertContext;
/* NOLINTEND(modernize-use-using) */

/**
 * @brief Create a conversion context from the format of @p src to @p target.
 *
 * The context fixes the source pixel format, bit depth, size and colour; it
 * converts any number of pictures of that same format.
 *
 * Requires a libvmaf built with `-Denable_zimg=true`. Without it nothing is
 * created, @p *ctx is left untouched, an error is logged and `-ENOTSUP` is
 * returned.
 *
 * Differs from upstream Netflix/vmaf `vmaf_picture_convert_context_init()`
 * (ADR-1822): upstream reads the source colour from `src->color`, a field
 * `VmafPicture` does not have here, so the caller passes it.
 *
 * @param ctx       Out: the new context. Not written on failure.
 * @param src       Example source picture (format, bit depth and size only).
 * @param src_color Source colour description, every field set to a supported value.
 * @param target    Target format, every colour field set to a supported value.
 *
 * @return 0 on success, `-EINVAL` for a NULL argument, an unset or unsupported
 *         colour value, a target bit depth outside 8 to 16 or a conversion
 *         zimg cannot build, `-ENOMEM` when out of memory, `-ENOTSUP` without zimg.
 *
 * @since libvmaf 3.0.0 (fork addition).
 */
VMAF_DEPRECATED("use vmafx_frame_converter_create")
VMAF_EXPORT int
vmaf_picture_convert_context_init_with_color(VmafPictureConvertContext **ctx,
                                             const VmafPicture *src, const VmafColor *src_color,
                                             const VmafPictureConvertTarget *target);

/**
 * @brief Convert @p src into a newly allocated picture @p dst.
 *
 * @p src must have the pixel format, bit depth and size the context was
 * created with. On success @p dst is allocated with @ref vmaf_picture_alloc and
 * owned by the caller (release it with @ref vmaf_picture_unref); on failure it
 * is left untouched. The colour description of @p dst is the `color` of the
 * context's target; the picture does not carry it.
 *
 * @return 0 on success, `-EINVAL` for a NULL argument, a source that does not
 *         match the context or a conversion failure, `-ENOMEM`, `-ENOTSUP`
 *         without zimg.
 *
 * @since libvmaf 3.0.0 (fork addition; same signature as upstream).
 */
VMAF_DEPRECATED("use vmafx_frame_convert")
VMAF_EXPORT int vmaf_picture_convert(VmafPictureConvertContext *ctx, VmafPicture *dst,
                                     const VmafPicture *src);

/**
 * @brief Free a conversion context.
 *
 * @return 0 on success, `-EINVAL` for a NULL context, `-ENOTSUP` without zimg.
 *
 * @since libvmaf 3.0.0 (fork addition; same signature as upstream).
 */
VMAF_DEPRECATED("use vmafx_frame_converter_destroy")
VMAF_EXPORT int vmaf_picture_convert_context_close(VmafPictureConvertContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_PICTURE_H */
