/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  `vmaf_use_tiny_model` — public entry point that opens an ORT session
 *  via ort_backend.c and hands ownership to libvmaf.c via the `dnn_ctx`
 *  bridge so per-frame inference runs alongside SVM models.
 *
 *  Carved out of `dnn_api.c` so the rest of the dnn TU set (used by
 *  `feature_lpips.c` from the test binaries) does not pull in
 *  `vmaf_ctx_dnn_attach` — that symbol lives in `libvmaf.c`, which is
 *  not linked into the unit-test executables.
 *
 *  Disabled-build stub contract (see ADR-0374):
 *  When built with -Denable_dnn=false, the `#else` branch at the bottom
 *  of this file provides a stub that returns -ENOSYS so callers degrade
 *  gracefully.  The public symbol is always present; callers must check
 *  vmaf_dnn_available() at runtime and treat -ENOSYS as "DNN not built
 *  in", not as a programming error.
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/dnn.h"
#include "libvmaf/vmaf_assert.h"

#include "dnn_ctx.h"
#include "model_loader.h"
#include "ort_backend.h"

#if defined(VMAF_HAVE_DNN) && VMAF_HAVE_DNN
static int load_optional_sidecar(const char *onnx_path, VmafModelSidecar *meta, bool *have_meta)
{
    memset(meta, 0, sizeof(*meta));
    *have_meta = false;
    int rc = vmaf_dnn_sidecar_load(onnx_path, meta);
    if (rc == 0) {
        *have_meta = true;
        return 0;
    }
    if (rc == -ENOENT)
        return 0;
    return rc;
}

static int open_session_and_probe_input(const char *onnx_path, const VmafDnnConfig *cfg,
                                        VmafOrtSession **sess_out, int64_t in_shape[4],
                                        size_t *in_rank)
{
    VmafOrtSession *sess = nullptr;
    int rc = vmaf_ort_open(&sess, onnx_path, cfg);
    if (rc < 0)
        return rc;

    rc = vmaf_ort_input_shape(sess, in_shape, 4u, in_rank);
    if (rc < 0) {
        vmaf_ort_close(sess);
        return rc;
    }
    *sess_out = sess;
    return 0;
}
#endif

int vmaf_use_tiny_model(VmafContext *ctx, const char *onnx_path, const VmafDnnConfig *cfg)
{
#if defined(VMAF_HAVE_DNN) && VMAF_HAVE_DNN
    if (!ctx || !onnx_path)
        return -EINVAL;
    assert(ctx != nullptr);
    assert(onnx_path != nullptr);

    /* T7-12: the historical VMAF_MAX_MODEL_BYTES env override has been
     * removed; see dnn_api.c for the rationale. */
    const size_t max_bytes = VMAF_DNN_DEFAULT_MAX_BYTES;
    int rc = vmaf_dnn_validate_onnx(onnx_path, max_bytes);
    if (rc < 0)
        return rc;

    VmafModelSidecar meta;
    bool have_meta = false;
    rc = load_optional_sidecar(onnx_path, &meta, &have_meta);
    if (rc < 0)
        return rc;

    VmafOrtSession *sess = nullptr;
    int64_t in_shape[4] = {0};
    size_t in_rank = 0;
    rc = open_session_and_probe_input(onnx_path, cfg, &sess, in_shape, &in_rank);
    if (rc < 0) {
        if (have_meta)
            vmaf_dnn_sidecar_free(&meta);
        return rc;
    }

    const char *feature_name =
        (have_meta && meta.name && *meta.name) ? meta.name : "vmaf_tiny_model";

    rc = vmaf_ctx_dnn_attach(ctx, sess, have_meta ? &meta : nullptr, in_shape, in_rank,
                             feature_name);
    if (rc < 0) {
        vmaf_ort_close(sess);
        if (have_meta)
            vmaf_dnn_sidecar_free(&meta);
        return rc;
    }
    /* Ownership transferred — do NOT close sess / free meta here. */
    return 0;
#else
    (void)ctx;
    (void)onnx_path;
    (void)cfg;
    return -ENOSYS;
#endif
}

int vmaf_dnn_set_codec_context(VmafContext *ctx, const char *codec_name, const char *preset,
                               int crf)
{
#if defined(VMAF_HAVE_DNN) && VMAF_HAVE_DNN
    if (!ctx)
        return -EINVAL;
    return vmaf_ctx_dnn_set_codec_context(ctx, codec_name, preset, crf);
#else
    (void)ctx;
    (void)codec_name;
    (void)preset;
    (void)crf;
    return -ENOSYS;
#endif
}

/* Public query for codec-awareness of the attached tiny model. The context
 * state lives in libvmaf.c; delegate to the bridge in dnn_ctx.h. When built
 * without DNN support the function always returns 0 (no session, no codec
 * block) so callers do not need an #ifdef guard. */
int vmaf_dnn_is_codec_aware(const VmafContext *ctx)
{
#if defined(VMAF_HAVE_DNN) && VMAF_HAVE_DNN
    return vmaf_ctx_dnn_is_codec_aware(ctx);
#else
    (void)ctx;
    return 0;
#endif
}

/* ADR-0550 — public setter for the NCHW dispatch auto-resize filter.
 * The on-context state lives in libvmaf.c (the only TU that sees the
 * VmafContext layout); this stub validates the enum and delegates. */
int vmaf_dnn_set_resize_mode(VmafContext *ctx, VmafDnnResizeMode mode)
{
#if defined(VMAF_HAVE_DNN) && VMAF_HAVE_DNN
    if (!ctx)
        return -EINVAL;
    if (mode != VMAF_DNN_RESIZE_BILINEAR && mode != VMAF_DNN_RESIZE_NEAREST &&
        mode != VMAF_DNN_RESIZE_BICUBIC && mode != VMAF_DNN_RESIZE_DISABLED) {
        return -EINVAL;
    }
    /* ctx is non-nullptr and mode is a valid enum value at this point. */
    assert(ctx != nullptr);
    return vmaf_ctx_dnn_set_resize_mode(ctx, (int)mode);
#else
    (void)ctx;
    (void)mode;
    return -ENOSYS;
#endif
}
