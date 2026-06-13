/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef LIBVMAF_DNN_TENSOR_IO_H_
#define LIBVMAF_DNN_TENSOR_IO_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Minimum bits-per-component accepted by the 16-bit tensor conversion
 *  functions (vmaf_tensor_from_plane16 / vmaf_tensor_to_plane16 /
 *  vmaf_dnn_session_run_plane16).  Values below this floor cannot be
 *  meaningfully represented in a uint16_t plane and indicate a caller
 *  bug (8-bit paths use the uint8_t API instead).  The constant is
 *  shared between tensor_io.c and dnn_api.c so both guard points stay
 *  in sync automatically. */
#define DNN_MIN_BIT_DEPTH 9

typedef enum VmafTensorLayout {
    VMAF_TENSOR_LAYOUT_NCHW = 0,
    VMAF_TENSOR_LAYOUT_NHWC = 1,
} VmafTensorLayout;

typedef enum VmafTensorDType {
    VMAF_TENSOR_DTYPE_F32 = 0,
    VMAF_TENSOR_DTYPE_F16 = 1,
} VmafTensorDType;

/**
 * Resize filter used by @ref vmaf_tensor_from_luma_resize. The default
 * is `DISABLED` (ADR-0550): the libvmaf NCHW dispatch returns -ERANGE on
 * any dim mismatch so the operator must explicitly opt in to a filter.
 * `BILINEAR` matches the export-time torchvision pipeline most tiny-AI
 * models are trained against (`PIL.Image.BILINEAR`); `NEAREST` is a
 * deterministic fast fallback; `BICUBIC` is provided for parity with
 * exporters that use `transforms.Resize(interpolation=BICUBIC)`.
 * Note: BILINEAR/NEAREST/BICUBIC produce scores differing by ~2%.
 */
typedef enum VmafTinyResize {
    VMAF_TINY_RESIZE_DISABLED =
        0, /**< default; caller demands exact-dims match (mismatch -> -ERANGE) */
    VMAF_TINY_RESIZE_BILINEAR = 1, /**< OpenCV / torchvision BILINEAR */
    VMAF_TINY_RESIZE_NEAREST = 2,  /**< nearest-neighbour; floor of unnormalised src coord */
    VMAF_TINY_RESIZE_BICUBIC = 3,  /**< Catmull-Rom (a=-0.5), separable */
} VmafTinyResize;

/**
 * Convert a single-channel 8-bit luma frame to a normalized float32/float16
 * tensor. Values are scaled to [0, 1] (divide by 255) and, if @p mean and
 * @p std are non-NULL, standardized channel-wise.
 *
 * @param src       uint8 luma plane (H rows of W pixels, stride = stride_src)
 * @param stride_src  bytes per source row (>= width)
 * @param width, height  frame dimensions
 * @param layout    NCHW (default for ORT) or NHWC
 * @param dtype     F32 or F16
 * @param mean, std per-channel normalization (length 1 for luma-only); NULL = skip
 * @param dst       destination buffer; required size = width*height*sizeof(dtype)
 *
 * @return 0 on success, -EINVAL on bad args.
 */
int vmaf_tensor_from_luma(const uint8_t *src, size_t stride_src, int width, int height,
                          VmafTensorLayout layout, VmafTensorDType dtype, const float *mean,
                          const float *std, void *dst);

/**
 * Variant of @ref vmaf_tensor_from_luma that resamples the input plane from
 * (@p src_w, @p src_h) to (@p dst_w, @p dst_h) on the fly using the filter
 * selected by @p mode (nearest / bilinear / bicubic). ADR-0550.
 *
 * Coordinate convention is `half-pixel-centers`:
 *
 * ```
 *   sx = (dx + 0.5) * (src_w / dst_w) - 0.5
 *   sy = (dy + 0.5) * (src_h / dst_h) - 0.5
 * ```
 *
 * which matches OpenCV `INTER_LINEAR` / `INTER_CUBIC` and the PyTorch /
 * torchvision `Resize(antialias=False)` default. Out-of-bounds source
 * samples are clamped (replicate-edge) so the filter is well-defined at
 * frame borders. The bicubic kernel is Catmull-Rom (a = -0.5).
 *
 * When (@p src_w, @p src_h) == (@p dst_w, @p dst_h) the routine forwards
 * to @ref vmaf_tensor_from_luma so the no-resize fast path is
 * bit-identical to the legacy code (no rounding error introduced).
 *
 * @return 0 on success, -EINVAL on bad args (including
 *         `VMAF_TINY_RESIZE_DISABLED` — the caller is responsible for
 *         routing the disabled mode to its own error path).
 */
int vmaf_tensor_from_luma_resize(const uint8_t *src, size_t stride_src, int src_w, int src_h,
                                 int dst_w, int dst_h, VmafTensorLayout layout,
                                 VmafTensorDType dtype, const float *mean, const float *std,
                                 VmafTinyResize mode, void *dst);

/**
 * Convert a normalized float32/float16 NCHW or NHWC tensor back to an 8-bit
 * luma plane. De-normalizes (if mean/std provided), multiplies by 255 with
 * round-half-to-even, clamps to [0, 255]. Used by learned-filter output.
 */
int vmaf_tensor_to_luma(const void *src, VmafTensorLayout layout, VmafTensorDType dtype, int width,
                        int height, const float *mean, const float *std, uint8_t *dst,
                        size_t stride_dst);

/**
 * Convert three planar 8-bit R/G/B channels to an NCHW [1,3,H,W] float32
 * tensor with per-channel ImageNet normalization (mean =
 * [0.485, 0.456, 0.406], std = [0.229, 0.224, 0.225]). Each source is a
 * planar uint8 buffer at its own stride; input values are scaled to
 * [0, 1] by dividing by 255 and then standardized channel-wise. The
 * destination buffer must be at least 3 * width * height * sizeof(float)
 * bytes and is written contiguously in NCHW order (R plane, then G, then
 * B). Matches torchvision's `transforms.Normalize(mean, std)` after a
 * `ToTensor()` call — the de-facto convention for RGB ImageNet models.
 *
 * @return 0 on success, -EINVAL on bad args.
 */
int vmaf_tensor_from_rgb_imagenet(const uint8_t *src_r, size_t stride_r, const uint8_t *src_g,
                                  size_t stride_g, const uint8_t *src_b, size_t stride_b, int width,
                                  int height, float *dst);

/**
 * 10-/12-/16-bit variant of @ref vmaf_tensor_from_luma for single-plane
 * input (packed uint16_t little-endian, stride in bytes). Values are
 * scaled to [0, 1] by dividing by `(1 << bpc) - 1` and, if @p mean and
 * @p std are non-NULL, standardized. Used by `vmaf_dnn_session_run_plane16`
 * (ADR-0170 / T6-4) for 10/12/16-bit YUV + per-plane chroma dispatch.
 *
 * @param src         uint16 single-plane buffer (little-endian per pixel)
 * @param stride_src  bytes per source row (>= width * sizeof(uint16_t))
 * @param width, height  plane dimensions
 * @param bpc         bits per component in [9, 16]; value range [0, (1<<bpc)-1]
 * @param layout      NCHW (default) or NHWC
 * @param dtype       F32 or F16
 * @param mean, std   per-channel normalization (length 1); NULL = skip
 * @param dst         destination buffer; size = width*height*sizeof(dtype)
 *
 * @return 0 on success, -EINVAL on bad args.
 */
int vmaf_tensor_from_plane16(const uint16_t *src, size_t stride_src, int width, int height, int bpc,
                             VmafTensorLayout layout, VmafTensorDType dtype, const float *mean,
                             const float *std, void *dst);

/**
 * Inverse of @ref vmaf_tensor_from_plane16. De-normalizes, multiplies by
 * `(1 << bpc) - 1` with round-half-to-even, and clamps to
 * `[0, (1 << bpc) - 1]`. Writes a packed uint16 plane back to @p dst.
 */
int vmaf_tensor_to_plane16(const void *src, VmafTensorLayout layout, VmafTensorDType dtype,
                           int width, int height, int bpc, const float *mean, const float *std,
                           uint16_t *dst, size_t stride_dst);

/** Convert float32 ↔ float16 in-place element counts. */
void vmaf_f32_to_f16(const float *src, uint16_t *dst, size_t n);
void vmaf_f16_to_f32(const uint16_t *src, float *dst, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_DNN_TENSOR_IO_H_ */
