/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Tiny-AI extractor template — shared scaffolding for ONNX-Runtime-backed
 *  feature extractors (`feature_lpips.c`, `fastdvdnet_pre.c`,
 *  `feature_mobilesal.c`, `feature_transnet_v2.c`, ...).
 *
 *  Why this header exists
 *  ----------------------
 *  Every tiny-AI extractor previously open-coded ~150 LOC of identical
 *  boilerplate: `model_path` option + env-var fallback resolution, the
 *  `vmaf_dnn_session_open()` call, BT.709 limited-range YUV→RGB
 *  conversion, a NULL-safe `aligned_free` cleanup loop, and a `VmafOption`
 *  table entry that differs only in the env-var help string. New
 *  extractors should be ~30 LOC of extractor-specific tensor wiring, not
 *  ~150 LOC where 70 % is copy-pasted plumbing.
 *
 *  What's in here
 *  --------------
 *  Four small inline helpers + one option-table macro. Nothing magical:
 *
 *    - vmaf_tiny_ai_require_runtime()  — standard disabled-build guard:
 *      return -ENOSYS before model-path probing when libvmaf was built
 *      without the DNN runtime.
 *    - vmaf_tiny_ai_resolve_model_path() — feature-option-then-env-var
 *      lookup with a single user-facing log line on failure.
 *    - vmaf_tiny_ai_open_session()       — `resolve` + `vmaf_dnn_session_open`
 *      with the standard `<name>: vmaf_dnn_session_open(...) failed`
 *      log line on error.
 *    - vmaf_tiny_ai_yuv8_to_rgb8_planes() / vmaf_tiny_ai_yuv_to_rgb8_planes()
 *      — BT.709 limited-range YUV→RGB with nearest-neighbour chroma
 *      upsampling, shared with `ciede.c`'s colour-conversion convention
 *      so saliency / LPIPS / future colour-sensitive extractors stay
 *      numerically comparable on identical inputs. Hoisted verbatim
 *      from `feature_lpips.c`; `feature_mobilesal.c` ships an identical
 *      copy that this header retires.
 *    - VMAF_TINY_AI_MODEL_PATH_OPTION() — emits the standard
 *      `model_path` row of a per-extractor `VmafOption[]` table; the
 *      help string is the only varying field.
 *
 *  What's deliberately NOT in here
 *  -------------------------------
 *  The extractor's `init` / `extract` / `close` lifecycle stays
 *  per-extractor. The state struct, tensor shapes, output names, ring-
 *  buffer / window logic, and the score names emitted via
 *  `vmaf_feature_collector_append()` differ enough between extractors
 *  that a one-size-fits-all macro would cost more than it saves and risk
 *  Power-of-10 violations (rule 1: control-flow simplicity, rule 6:
 *  data scoping). The shared lifecycle skeleton is documented as a
 *  pattern in `docs/ai/extractor-template.md` instead — code stays
 *  hand-written, the recipe is the dedup.
 *
 *  Power-of-10 / SEI CERT compliance
 *  ---------------------------------
 *  - All loops are statically bounded: 3-channel chroma upsample, 3-row
 *    inner loop, no recursion, no setjmp/longjmp.
 *  - Every non-void return is checked or `(void)`-discarded by callers.
 *  - No allocation in inline helpers; the extractor owns its buffers.
 *  - `getenv()` is permitted (one of the option-resolution paths) and
 *    is well-defined per CERT ENV03-C; we treat the result as untrusted
 *    and only pass it to `vmaf_dnn_validate_onnx()` via
 *    `vmaf_dnn_session_open()` which performs `realpath` hardening.
 *  - Disabled-build `-ENOSYS` is checked before model-path probing so
 *    callers get the optional-runtime contract from ADR-0374 instead of
 *    a misleading "no model path" error on builds that could not run the
 *    model even with a valid path.
 */

#ifndef LIBVMAF_DNN_TINY_EXTRACTOR_TEMPLATE_H_
#define LIBVMAF_DNN_TINY_EXTRACTOR_TEMPLATE_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libvmaf/dnn.h"
#include "libvmaf/picture.h"

#include "log.h"
#include "opt.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Enforce the build-time optional DNN runtime contract shared by every
 * tiny-AI extractor. Call this from `init()` after validating the pixel
 * format / bit depth but before resolving the model path.
 *
 * @param feature_name short name used in the error log (e.g. "lpips").
 * @return 0 when the DNN runtime is available, -ENOSYS when libvmaf was
 *         built without ONNX Runtime support.
 */
static inline int vmaf_tiny_ai_require_runtime(const char *feature_name)
{
    if (vmaf_dnn_available()) {
        return 0;
    }
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "%s: tiny-AI DNN runtime unavailable (rebuild libvmaf with "
             "-Denable_dnn=enabled)\n",
             feature_name ? feature_name : "tiny_ai");
    return -ENOSYS;
}

/**
 * Resolve a tiny-AI extractor's ONNX model path: feature option first,
 * dedicated env var as fallback, NULL if neither is set. Logs one
 * user-facing error on the NULL case so each call site doesn't have to.
 *
 * @param feature_name  short name used in the error log (e.g. "lpips").
 * @param option_value  the `model_path` field from the per-extractor
 *                      state struct (owned by `opt.c`); may be NULL.
 * @param env_var       environment variable consulted when the option
 *                      is unset (e.g. "VMAF_LPIPS_MODEL_PATH").
 * @return non-NULL path on success, NULL when neither source provided
 *         a usable path. The returned pointer aliases either
 *         @p option_value or `getenv(env_var)`; do not free it.
 */
static inline const char *vmaf_tiny_ai_resolve_model_path(const char *feature_name,
                                                          const char *option_value,
                                                          const char *env_var)
{
    if (option_value && *option_value) {
        return option_value;
    }
    if (env_var && *env_var) {
        const char *env = getenv(env_var);
        if (env && *env) {
            return env;
        }
    }
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "%s: no model path (set feature option model_path or env %s)\n",
             feature_name ? feature_name : "tiny_ai", env_var ? env_var : "<unset>");
    return NULL;
}

/**
 * Open a `VmafDnnSession` from a resolved path with the standard
 * `<name>: vmaf_dnn_session_open(<path>) failed: <rc>` error log on
 * failure. Returns 0 and writes the new session into @p out on success.
 *
 * Caller responsibilities:
 *   - Provide a non-NULL @p out and ensure @p out points at a writable
 *     `VmafDnnSession *` slot inside the per-extractor state.
 *   - Resolve @p path via `vmaf_tiny_ai_resolve_model_path()` first;
 *     this helper does not consult env vars itself.
 *   - On success, eventually call `vmaf_dnn_session_close()` (or rely on
 *     the extractor's `close` hook) — this helper transfers ownership
 *     of the session to the caller.
 */
static inline int vmaf_tiny_ai_open_session(const char *feature_name, const char *path,
                                            VmafDnnSession **out)
{
    if (!out || !path) {
        return -EINVAL;
    }
    int rc = vmaf_dnn_session_open(out, path, NULL);
    if (rc < 0) {
        vmaf_log(VMAF_LOG_LEVEL_ERROR, "%s: vmaf_dnn_session_open(%s) failed: %d\n",
                 feature_name ? feature_name : "tiny_ai", path, rc);
        return rc;
    }
    return 0;
}

/* BT.709 limited-range YUV → RGB pixel kernel. Studio-swing
 * normalisation: Y∈[16,235], UV∈[16,240]. Saturating clamp to [0, 1]
 * then scale to [0, 255] with round-to-nearest.
 *
 * Inlined so call sites get the same code generation they had when this
 * lived per-file. Bit-exact with the historical `feature_lpips.c`
 * `yuv_bt709_to_rgb8_pixel` body. */
static inline void vmaf_tiny_ai_yuv_bt709_to_rgb8_pixel(int y, int u, int v, uint8_t *r, uint8_t *g,
                                                        uint8_t *b)
{
    const float yn = ((float)y - 16.0f) * (1.0f / 219.0f);
    const float un = ((float)u - 128.0f) * (1.0f / 224.0f);
    const float vn = ((float)v - 128.0f) * (1.0f / 224.0f);

    float rf = yn + 1.28033f * vn;
    float gf = yn - 0.21482f * un - 0.38059f * vn;
    float bf = yn + 2.12798f * un;

    if (rf < 0.0f) {
        rf = 0.0f;
    } else if (rf > 1.0f) {
        rf = 1.0f;
    }
    if (gf < 0.0f) {
        gf = 0.0f;
    } else if (gf > 1.0f) {
        gf = 1.0f;
    }
    if (bf < 0.0f) {
        bf = 0.0f;
    } else if (bf > 1.0f) {
        bf = 1.0f;
    }

    *r = (uint8_t)(rf * 255.0f + 0.5f);
    *g = (uint8_t)(gf * 255.0f + 0.5f);
    *b = (uint8_t)(bf * 255.0f + 0.5f);
}

/**
 * Expand planar YUV (4:2:0 / 4:2:2 / 4:4:4, 8-bit) to three packed RGB
 * planes (W bytes per row, no inter-plane padding). Chroma is upsampled
 * by nearest-neighbour replication — the same convention used by
 * `ciede.c`, so colour-sensitive tiny-AI extractors stay numerically
 * comparable with the rest of the CPU pipeline under identical inputs.
 *
 * Bit-exact with the original `yuv8_to_rgb8_planes` bodies in
 * `feature_lpips.c` and `feature_mobilesal.c`; this is a literal
 * deduplication, not a behaviour change.
 *
 * @return 0 on success, -ENOTSUP if @p pic is not 8-bit, -EINVAL on
 *         NULL inputs.
 */
static inline int vmaf_tiny_ai_yuv8_to_rgb8_planes(const VmafPicture *pic, uint8_t *dst_r,
                                                   uint8_t *dst_g, uint8_t *dst_b)
{
    if (!pic || !dst_r || !dst_g || !dst_b) {
        return -EINVAL;
    }
    if (pic->bpc != 8) {
        return -ENOTSUP;
    }
    const int ss_hor = (pic->pix_fmt != VMAF_PIX_FMT_YUV444P) ? 1 : 0;
    const int ss_ver = (pic->pix_fmt == VMAF_PIX_FMT_YUV420P) ? 1 : 0;
    const unsigned w = pic->w[0];
    const unsigned h = pic->h[0];
    const uint8_t *Y = pic->data[0];
    const uint8_t *U = pic->data[1];
    const uint8_t *V = pic->data[2];
    const size_t sy = pic->stride[0];
    const size_t su = pic->stride[1];
    const size_t sv = pic->stride[2];

    for (unsigned i = 0; i < h; ++i) {
        const uint8_t *yrow = Y + (size_t)i * sy;
        const unsigned ci = ss_ver ? (i >> 1) : i;
        const uint8_t *urow = U + (size_t)ci * su;
        const uint8_t *vrow = V + (size_t)ci * sv;
        uint8_t *rrow = dst_r + (size_t)i * w;
        uint8_t *grow = dst_g + (size_t)i * w;
        uint8_t *brow = dst_b + (size_t)i * w;
        for (unsigned j = 0; j < w; ++j) {
            const unsigned cj = ss_hor ? (j >> 1) : j;
            vmaf_tiny_ai_yuv_bt709_to_rgb8_pixel(yrow[j], urow[cj], vrow[cj], rrow + j, grow + j,
                                                 brow + j);
        }
    }
    return 0;
}

static inline int vmaf_tiny_ai_sample_to_8bit(uint16_t v, unsigned bpc)
{
    if (bpc == 8u) {
        return (int)v;
    }
    if (bpc < 8u || bpc > 16u) {
        return -EINVAL;
    }
    const unsigned max_sample = (bpc == 16u) ? 65535u : ((1u << bpc) - 1u);
    unsigned sample = (v > max_sample) ? max_sample : (unsigned)v;
    const unsigned shift = bpc - 8u;
    const unsigned round = (shift == 0u) ? 0u : (1u << (shift - 1u));
    return (int)((sample + round) >> shift);
}

/**
 * Expand planar YUV (4:2:0 / 4:2:2 / 4:4:4, 8/10/12/16-bit) to RGB8.
 * High-bit-depth input is interpreted as little-endian uint16 sample
 * containers and rounded down to the 8-bit domain before applying the
 * existing BT.709 limited-range conversion. That preserves the tiny-AI
 * ImageNet tensor ABI while removing the raw-YUV 8-bit-only limitation.
 */
static inline int vmaf_tiny_ai_yuv_to_rgb8_planes(const VmafPicture *pic, uint8_t *dst_r,
                                                  uint8_t *dst_g, uint8_t *dst_b)
{
    if (!pic || !dst_r || !dst_g || !dst_b) {
        return -EINVAL;
    }
    if (pic->pix_fmt == VMAF_PIX_FMT_YUV400P) {
        return -EINVAL;
    }
    if (pic->bpc == 8u) {
        return vmaf_tiny_ai_yuv8_to_rgb8_planes(pic, dst_r, dst_g, dst_b);
    }
    if (pic->bpc != 10u && pic->bpc != 12u && pic->bpc != 16u) {
        return -ENOTSUP;
    }
    const int ss_hor = (pic->pix_fmt != VMAF_PIX_FMT_YUV444P) ? 1 : 0;
    const int ss_ver = (pic->pix_fmt == VMAF_PIX_FMT_YUV420P) ? 1 : 0;
    const unsigned w = pic->w[0];
    const unsigned h = pic->h[0];
    const uint8_t *Y = pic->data[0];
    const uint8_t *U = pic->data[1];
    const uint8_t *V = pic->data[2];
    const size_t sy = pic->stride[0];
    const size_t su = pic->stride[1];
    const size_t sv = pic->stride[2];

    for (unsigned i = 0; i < h; ++i) {
        const uint8_t *yrow = Y + (size_t)i * sy;
        const unsigned ci = ss_ver ? (i >> 1) : i;
        const uint8_t *urow = U + (size_t)ci * su;
        const uint8_t *vrow = V + (size_t)ci * sv;
        uint8_t *rrow = dst_r + (size_t)i * w;
        uint8_t *grow = dst_g + (size_t)i * w;
        uint8_t *brow = dst_b + (size_t)i * w;
        for (unsigned j = 0; j < w; ++j) {
            const unsigned cj = ss_hor ? (j >> 1) : j;
            const uint16_t y = (uint16_t)yrow[j * 2u] | ((uint16_t)yrow[j * 2u + 1u] << 8u);
            const uint16_t u = (uint16_t)urow[cj * 2u] | ((uint16_t)urow[cj * 2u + 1u] << 8u);
            const uint16_t v = (uint16_t)vrow[cj * 2u] | ((uint16_t)vrow[cj * 2u + 1u] << 8u);
            const int y8 = vmaf_tiny_ai_sample_to_8bit(y, pic->bpc);
            const int u8 = vmaf_tiny_ai_sample_to_8bit(u, pic->bpc);
            const int v8 = vmaf_tiny_ai_sample_to_8bit(v, pic->bpc);
            if (y8 < 0 || u8 < 0 || v8 < 0) {
                return -EINVAL;
            }
            vmaf_tiny_ai_yuv_bt709_to_rgb8_pixel(y8, u8, v8, rrow + j, grow + j, brow + j);
        }
    }
    return 0;
}

/**
 * Emit the standard `model_path` row of a per-extractor `VmafOption[]`
 * table. The help string is the only field that varies — every
 * extractor takes a string-typed feature option named `model_path`,
 * defaults to NULL (env-var fallback), and carries the
 * `VMAF_OPT_FLAG_FEATURE_PARAM` flag.
 *
 * Usage:
 *
 *   static const VmafOption lpips_options[] = {
 *       VMAF_TINY_AI_MODEL_PATH_OPTION(LpipsState,
 *           "Filesystem path to the LPIPS ONNX model. "
 *           "Overrides the VMAF_LPIPS_MODEL_PATH env var."),
 *       {NULL},
 *   };
 *
 * The macro is plain text substitution — it expands to a single
 * struct-literal element with no control flow, no recursion, and no
 * variadic shenanigans (Power-of-10 rule 1 / rule 9 compliant).
 */
#define VMAF_TINY_AI_MODEL_PATH_OPTION(state_t, help_text)                                         \
    {                                                                                              \
        .name = "model_path",                                                                      \
        .help = (help_text),                                                                       \
        .offset = offsetof(state_t, model_path),                                                   \
        .type = VMAF_OPT_TYPE_STRING,                                                              \
        .default_val.s = NULL,                                                                     \
        .flags = VMAF_OPT_FLAG_FEATURE_PARAM,                                                      \
    }

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_DNN_TINY_EXTRACTOR_TEMPLATE_H_ */
