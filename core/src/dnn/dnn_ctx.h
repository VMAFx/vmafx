/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Internal bridge between the public `dnn.h` surface (dnn_api.c) and the
 *  VmafContext-owning translation unit (libvmaf.c). Keeps VmafContext opaque
 *  to the DNN module while letting dnn_api.c hand off the opened ORT session
 *  for per-frame inference.
 */

#ifndef LIBVMAF_DNN_DNN_CTX_H_
#define LIBVMAF_DNN_DNN_CTX_H_

#include <stddef.h>
#include <stdint.h>

#include "libvmaf/libvmaf.h"

#include "model_loader.h"
#include "ort_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Attach an opened ORT session to the VmafContext. Ownership of @p sess,
 * @p meta, and @p feature_name (strdup'd) is transferred to the context; the
 * caller must not close/free them on success. On failure the caller retains
 * ownership.
 *
 * @param in_shape  static input shape reported by ORT (rank dims).
 * @param in_rank   rank of in_shape. Accepts:
 *                    - 4: NCHW [1, 1, H, W] single-channel luma image
 *                         (legacy path, runs the picture through
 *                         vmaf_tensor_from_luma each frame).
 *                    - 2: [batch, feature_count] feature-vector model
 *                         (ADR-0517). The host materialises the
 *                         feature vector from libvmaf's classic
 *                         feature collector at inference time.
 *
 * Anything else is rejected with -ENOTSUP so the failure is visible,
 * not silent. The accepted ranks cover every tiny-AI model shipped
 * under model/tiny/ to date.
 */
int vmaf_ctx_dnn_attach(VmafContext *ctx, VmafOrtSession *sess, const VmafModelSidecar *meta,
                        const int64_t *in_shape, size_t in_rank, const char *feature_name);

/** Returns 1 if a tiny model is attached to @p ctx, else 0. */
int vmaf_ctx_dnn_has_session(const VmafContext *ctx);

/**
 * Populate the codec one-hot block of an attached codec-aware tiny model
 * (ADR-0519). Bridge for the public `vmaf_dnn_set_codec_context` API
 * declared in `libvmaf/dnn.h`; see that header for the parameter and
 * return-value contract.
 */
int vmaf_ctx_dnn_set_codec_context(VmafContext *ctx, const char *codec_name, const char *preset,
                                   int crf);

/**
 * Bridge for the public `vmaf_dnn_set_resize_mode` setter (ADR-0550).
 * The @p mode int is `VmafDnnResizeMode` cast to int; values are
 * 0=DISABLED (default), 1=BILINEAR, 2=NEAREST, 3=BICUBIC. Stored on
 * the context and consumed by the NCHW dispatch in
 * `vmaf_ctx_dnn_run_frame_nchw`.
 */
int vmaf_ctx_dnn_set_resize_mode(VmafContext *ctx, int mode);

/**
 * Bridge for the public `vmaf_dnn_is_codec_aware` query (dnn.h).
 * Returns 1 if the attached tiny model has a sidecar with codec_aware=true
 * and a non-zero codec block (extra_in_buf / extra_in_width allocated).
 * Returns 0 if no model is attached, no sidecar, or no codec block.
 */
int vmaf_ctx_dnn_is_codec_aware(const VmafContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_DNN_DNN_CTX_H_ */
