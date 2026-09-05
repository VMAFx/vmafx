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
#include "log.h"
#include "model_loader.h"
#include "ort_backend.h"

#if defined(VMAF_HAVE_DNN) && VMAF_HAVE_DNN
/**
 * Resolve which ONNX file a quantised sidecar should actually load.
 *
 * ADR-0174 / ADR-1032: when the sidecar declares `quant_mode != FP32`, the
 * caller-supplied path is the fp32 baseline and the runtime should open the
 * sibling `<basename>.int8.onnx` instead. This mirrors the resolution in
 * `vmaf_dnn_session_open` (dnn_api.c) — the two must stay identical; see
 * `core/src/dnn/AGENTS.md`.
 *
 * @p buf (of @p buf_size bytes) receives the derived int8 path when one is
 * built. On success writes the path to open into @p out_path — either @p buf,
 * or @p onnx_path when the caller already named an int8 graph or when the int8
 * sibling is missing / oversized / carries a non-allowlisted op (ADR-1032 fp32
 * fallback, logged at debug level) — and returns 0. The only hard error today
 * is `-ENAMETOOLONG` for a path that would not fit @p buf.
 *
 * The int8 file is *not* checked against the sidecar's `int8_sha256`; the
 * only load-time gates are the size cap and the op allowlist. See
 * docs/ai/quantization.md ("The redirect does not verify int8_sha256").
 */
static int resolve_quantised_load_path(const char *onnx_path, size_t max_bytes, char *buf,
                                       size_t buf_size, const char **out_path)
{
    assert(onnx_path != NULL);
    assert(buf != NULL);
    assert(out_path != NULL);

    static const char kInt8Suffix[] = ".int8.onnx";
    static const char kOnnxSuffix[] = ".onnx";
    const size_t plen = strlen(onnx_path);
    const size_t int8_len = sizeof(kInt8Suffix) - 1u;
    const size_t onnx_len = sizeof(kOnnxSuffix) - 1u;

    /* The caller already named the int8 graph — nothing to derive. */
    if (plen >= int8_len && strcmp(onnx_path + plen - int8_len, kInt8Suffix) == 0) {
        *out_path = onnx_path;
        return 0;
    }

    const size_t base_len =
        (plen >= onnx_len && strcmp(onnx_path + plen - onnx_len, kOnnxSuffix) == 0) ?
            plen - onnx_len :
            plen;
    if (base_len + sizeof(kInt8Suffix) > buf_size)
        return -ENAMETOOLONG;
    memcpy(buf, onnx_path, base_len);
    memcpy(buf + base_len, kInt8Suffix, sizeof(kInt8Suffix));

    const int rc = vmaf_dnn_validate_onnx(buf, max_bytes);
    if (rc < 0) {
        /* int8 file missing or fails the allowlist — fall back to fp32;
         * better degraded than dead (ADR-1032 Fix 3). The caller keeps the
         * sidecar, so the session still reports the declared quant_mode;
         * only the weights are fp32. */
        vmaf_log(VMAF_LOG_LEVEL_DEBUG,
                 "dnn: int8 sidecar unavailable (%s, rc=%d); falling back to fp32 path\n", buf, rc);
        *out_path = onnx_path;
        return 0;
    }
    *out_path = buf;
    return 0;
}

/**
 * Query the opened session's input shape and hand ownership to `libvmaf.c`.
 *
 * Split out of vmaf_use_tiny_model() so the entry point stays inside the
 * `readability-function-size` budget. On any negative return the caller still
 * owns @p sess and @p meta and must close / free them.
 */
static int attach_opened_session(VmafContext *ctx, VmafOrtSession *sess, VmafModelSidecar *meta,
                                 bool have_meta)
{
    assert(ctx != NULL);
    assert(sess != NULL);

    int64_t in_shape[4] = {0};
    size_t in_rank = 0;
    const int rc = vmaf_ort_input_shape(sess, in_shape, 4u, &in_rank);
    if (rc < 0)
        return rc;

    const char *feature_name =
        (have_meta && meta->name && *meta->name) ? meta->name : "vmaf_tiny_model";
    return vmaf_ctx_dnn_attach(ctx, sess, have_meta ? meta : NULL, in_shape, in_rank, feature_name);
}

/**
 * Load the companion sidecar, treating "absent" as success.
 *
 * A missing sidecar is not fatal — it only carries NR/FR disambiguation,
 * quant_mode, and pretty-printing metadata; its absence defaults to FR.
 * Sets @p have_meta and returns 0 in that case; returns the loader's error
 * for any other failure (a malformed or oversized sidecar).
 */
static int load_optional_sidecar(const char *onnx_path, VmafModelSidecar *meta, bool *have_meta)
{
    memset(meta, 0, sizeof(*meta));
    *have_meta = false;
    const int rc = vmaf_dnn_sidecar_load(onnx_path, meta);
    if (rc < 0 && rc != -ENOENT)
        return rc;
    *have_meta = (rc == 0);
    return 0;
}
#endif

int vmaf_use_tiny_model(VmafContext *ctx, const char *onnx_path, const VmafDnnConfig *cfg)
{
#if defined(VMAF_HAVE_DNN) && VMAF_HAVE_DNN
    if (!ctx || !onnx_path)
        return -EINVAL;
    assert(ctx != NULL);
    assert(onnx_path != NULL);

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

    /* ADR-0174 / ADR-1032 — a non-fp32 sidecar redirects to the int8 sibling.
     * resolve_quantised_load_path() falls back to the fp32 baseline on its own
     * when the sibling is unusable, so a 0 return always yields an openable path. */
    const char *load_path = onnx_path;
    char int8_path[4096];
    if (have_meta && meta.quant_mode != VMAF_QUANT_FP32) {
        rc = resolve_quantised_load_path(onnx_path, max_bytes, int8_path, sizeof(int8_path),
                                         &load_path);
        if (rc < 0) {
            vmaf_dnn_sidecar_free(&meta);
            return rc;
        }
    }

    VmafOrtSession *sess = NULL;
    rc = vmaf_ort_open(&sess, load_path, cfg);
    if (rc < 0) {
        if (have_meta)
            vmaf_dnn_sidecar_free(&meta);
        return rc;
    }

    rc = attach_opened_session(ctx, sess, &meta, have_meta);
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
    /* ctx is non-NULL and mode is a valid enum value at this point. */
    assert(ctx != NULL);
    return vmaf_ctx_dnn_set_resize_mode(ctx, (int)mode);
#else
    (void)ctx;
    (void)mode;
    return -ENOSYS;
#endif
}
