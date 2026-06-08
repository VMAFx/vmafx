/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Licensed under the BSD+Patent License (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      https://opensource.org/licenses/BSDplusPatent
 */

/**
 * @file dnn.h
 * @brief Public DNN surface — load/execute tiny ONNX models alongside SVM models.
 *
 * All functions return 0 on success and a negative errno on failure. When
 * libvmaf was built with `-Denable_dnn=false`, `vmaf_dnn_available()`
 * returns 0 and every other entry point returns -ENOSYS.
 */

#ifndef LIBVMAF_DNN_H
#define LIBVMAF_DNN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libvmaf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Execution-provider hint for @ref VmafDnnConfig. The AUTO value asks
 * ORT to pick the best available provider; the explicit values pin a
 * single provider (see per-value docs for the exact semantics).
 */
typedef enum VmafDnnDevice {
    VMAF_DNN_DEVICE_AUTO = 0,     /**< Let ORT pick the best provider. */
    VMAF_DNN_DEVICE_CPU = 1,      /**< CPU execution provider. */
    VMAF_DNN_DEVICE_CUDA = 2,     /**< NVIDIA CUDA EP. */
    VMAF_DNN_DEVICE_OPENVINO = 3, /**< OpenVINO EP, GPU device type with CPU fallback */
    VMAF_DNN_DEVICE_ROCM = 4,     /**< AMD ROCm EP. */
    /**
     * Apple CoreML execution provider. The base value lets CoreML pick
     * any compute unit; the explicit ANE/GPU/CPU variants pin a single
     * compute unit via the `MLComputeUnits` flags exposed by the CoreML
     * EP factory. Values 5..8 are append-only.
     *
     * - `VMAF_DNN_DEVICE_COREML`     — `ALL` (CoreML auto-routes)
     * - `VMAF_DNN_DEVICE_COREML_ANE` — `CPU_AND_NEURAL_ENGINE` (Apple
     *                                  Neural Engine highest-perf path
     *                                  on M-series silicon)
     * - `VMAF_DNN_DEVICE_COREML_GPU` — `CPU_AND_GPU` (Metal-backed GPU)
     * - `VMAF_DNN_DEVICE_COREML_CPU` — `CPU_ONLY` (universal fallback)
     *
     * On non-Apple hosts the EP is not present in ORT and the session
     * silently degrades to the CPU EP — see ADR-0365.
     */
    VMAF_DNN_DEVICE_COREML = 5,     /**< CoreML EP, MLComputeUnits = ALL. */
    VMAF_DNN_DEVICE_COREML_ANE = 6, /**< CoreML EP, pinned to the Apple Neural Engine. */
    VMAF_DNN_DEVICE_COREML_GPU = 7, /**< CoreML EP, pinned to the Metal GPU. */
    VMAF_DNN_DEVICE_COREML_CPU = 8, /**< CoreML EP, pinned to the CPU compute unit. */
    /**
     * OpenVINO EP pinned to a single device type (no fallback). NPU targets
     * the Intel AI-PC neural processing unit (Meteor / Lunar / Arrow Lake);
     * CPU and GPU disambiguate the OpenVINO CPU and iGPU/dGPU plugins from
     * each other. See Research-0031 (docs/research/0031-intel-ai-pc-applicability.md).
     * Values 9..11 are append-only.
     */
    VMAF_DNN_DEVICE_OPENVINO_NPU = 9,  /**< OpenVINO EP pinned to the NPU. */
    VMAF_DNN_DEVICE_OPENVINO_CPU = 10, /**< OpenVINO EP pinned to the CPU plugin. */
    VMAF_DNN_DEVICE_OPENVINO_GPU = 11, /**< OpenVINO EP pinned to the iGPU/dGPU. */
} VmafDnnDevice;

/**
 * DNN session configuration. Passed to @ref vmaf_use_tiny_model and
 * @ref vmaf_dnn_session_open; safe to zero-initialise.
 */
typedef struct VmafDnnConfig {
    VmafDnnDevice device; /**< execution-provider hint; AUTO lets ORT choose */
    int device_index;     /**< multi-GPU index; 0 for single-GPU/CPU */
    int threads;          /**< CPU EP intra-op threads; 0 = ORT default */
    bool fp16_io;         /**< request fp16 tensors when supported */
} VmafDnnConfig;

/**
 * Returns 1 if libvmaf was built with DNN support (-Denable_dnn=true) and
 * ONNX Runtime is linked, 0 otherwise.
 *
 * @return 1 if DNN support was compiled in, 0 otherwise.
 *
 * @thread-safety Safe to call from any thread.
 */
VMAF_EXPORT int vmaf_dnn_available(void);

/**
 * Attach a tiny ONNX model (C1 / C2) to @p ctx. The model is registered
 * alongside any SVM models and participates in the same per-frame pipeline.
 *
 * @param ctx        live VmafContext (from vmaf_init())
 * @param onnx_path  filesystem path to a .onnx file; must be a regular file
 * @param cfg        optional device config; NULL uses VMAF_DNN_DEVICE_AUTO
 *
 * @return 0 on success, -ENOSYS if built without DNN support, -EINVAL on bad
 *         args, -ENOENT if the path does not exist, -E2BIG if the file is
 *         larger than the compile-time 50 MB cap (VMAF_DNN_DEFAULT_MAX_BYTES).
 *
 * @thread-safety Not thread-safe. Call before vmaf_read_pictures() on the
 *               same context.
 */
VMAF_EXPORT int vmaf_use_tiny_model(VmafContext *ctx, const char *onnx_path,
                                    const VmafDnnConfig *cfg);

/**
 * Populate the codec one-hot block of an attached codec-aware tiny model
 * (ADR-0519). Must be called **after** vmaf_use_tiny_model() and
 * **before** the first vmaf_read_pictures() call.
 *
 * For codec-aware models such as `fr_regressor_v2`, the loader pre-seeds
 * the codec block to the "unknown" encoder baseline at attach time
 * (ADR-0518). This function overrides that seed with the actual encoding
 * parameters so the model receives the correct conditioning vector.
 *
 * The codec block layout is
 * `[encoder_onehot(N_VOCAB), preset_norm, crf_norm]`. Encoder names are
 * validated against the sidecar's `encoder_vocab`; unknown names return
 * `-ENOENT` (the "unknown" bucket is still written so callers can choose
 * to continue). Preset strings are looked up in a per-encoder ordinal
 * table that mirrors `ai/scripts/train_fr_regressor_v2.py::PRESET_ORDINAL`;
 * unknown presets fall back to ordinal 5 ("medium"-equivalent). The CRF
 * is clamped to [0, 63] before normalisation.
 *
 * Common ffprobe aliases (`h264`, `hevc`, `av1`, `vp9`, `vvc`) are
 * accepted and remapped to their canonical encoder names so callers can
 * pipe `ffprobe -show_entries stream=codec_name` output directly.
 *
 * @param ctx         live VmafContext with a tiny model attached via
 *                    vmaf_use_tiny_model().
 * @param codec_name  encoder name (e.g. "libx264", "libx265", "libsvtav1",
 *                    "h264_nvenc"). NULL or "" maps to the "unknown"
 *                    bucket and returns 0.
 * @param preset      encoder preset string (e.g. "medium", "slow",
 *                    "p4", "5"). NULL defaults to ordinal 5.
 * @param crf         CRF / QP integer; clamped to [0, 63].
 *
 * @return  0          success (codec found and block written, or the
 *                     caller asked for the "unknown" bucket).
 * @return -ENOENT     @p codec_name is non-NULL but not in the model's
 *                     `encoder_vocab`; the "unknown" bucket was used.
 * @return -ENOSYS     libvmaf was built without DNN support.
 * @return -EINVAL     @p ctx is NULL or no tiny model is attached.
 * @return -ENOTSUP    the attached model has no codec block (rank-4
 *                     image model or rank-2 single-input model).
 *
 * @thread-safety Not thread-safe. Call before the first vmaf_read_pictures()
 *               on the same context.
 */
VMAF_EXPORT int vmaf_dnn_set_codec_context(VmafContext *ctx, const char *codec_name,
                                           const char *preset, int crf);

/**
 * Returns 1 if the tiny model attached to @p ctx requires a codec context
 * (i.e. the sidecar declares `"codec_aware": true` and the model was loaded
 * with a `codec_block` second input). Returns 0 if no model is attached, if
 * the model has no codec block, or if libvmaf was built without DNN support.
 *
 * Intended for CLI / wrapper validation: callers can use this function after
 * vmaf_use_tiny_model() to detect that --tiny-codec / codec-block conditioning
 * is required and emit a clear error before inference starts.
 *
 * @param ctx  live VmafContext (may be NULL — returns 0 safely).
 * @return 1 if a codec-aware model with a codec block is attached, 0 otherwise.
 *
 * @thread-safety Safe to call before vmaf_read_pictures() on the same context.
 */
VMAF_EXPORT int vmaf_dnn_is_codec_aware(const VmafContext *ctx);

/**
 * Resize filter selector for the NCHW tiny-model dispatch (ADR-0550).
 * When a tiny model's expected input dims (e.g. 224x224 for the
 * `nr_metric_v1` NR scorer) do not match the user-supplied frame
 * (e.g. 576x324 from `--width / --height`), the per-frame dispatch
 * resamples the luma plane to the model dims using the selected
 * filter before invoking ONNX Runtime. Bit-exact when src dims
 * already equal model dims (the routine forwards to
 * `vmaf_tensor_from_luma` unchanged).
 *
 * - `DISABLED` (default) — no resampling; a size mismatch returns
 *   `-ERANGE` (the pre-ADR-0550 behaviour). Operator must explicitly
 *   opt in to a resize filter via `--tiny-resize`. This preserves the
 *   strict mode for parity harnesses and avoids a silent free parameter.
 * - `BILINEAR` — matches torchvision `Resize(..., antialias=False)`
 *   and OpenCV `INTER_LINEAR`, the convention every shipped NR /
 *   image-input tiny-AI model was trained against. Enables auto-resize.
 * - `NEAREST`  — deterministic, no filtering; floor of the source
 *   coord (OpenCV `INTER_NEAREST`). Cheaper and useful for debugging.
 * - `BICUBIC`  — separable Catmull-Rom (a = -0.5); parity with
 *   exporters that use `transforms.Resize(interpolation=BICUBIC)`.
 *
 * Note: `BILINEAR`, `NEAREST`, and `BICUBIC` produce scores that differ
 * by approximately 2% on the same input — treat filter choice as a model
 * hyperparameter and document it alongside the model checkpoint.
 */
typedef enum VmafDnnResizeMode {
    VMAF_DNN_RESIZE_DISABLED = 0, /**< default; no resampling — mismatch -> -ERANGE */
    VMAF_DNN_RESIZE_BILINEAR = 1, /**< OpenCV INTER_LINEAR / torchvision BILINEAR */
    VMAF_DNN_RESIZE_NEAREST = 2,  /**< nearest-neighbour, floor coord */
    VMAF_DNN_RESIZE_BICUBIC = 3,  /**< Catmull-Rom (a = -0.5), separable */
} VmafDnnResizeMode;

/**
 * Configure the auto-resize filter used by the NCHW tiny-model dispatch
 * when the user-supplied frame dims don't match the model's expected
 * input shape. Default is `VMAF_DNN_RESIZE_DISABLED` (mismatch -> -ERANGE).
 * Pass `VMAF_DNN_RESIZE_BILINEAR` (or nearest/bicubic) to enable
 * auto-resize. May be called before or after `vmaf_use_tiny_model()`;
 * takes effect on the next `vmaf_read_pictures()` call. See @ref
 * VmafDnnResizeMode for filter semantics. ADR-0550.
 *
 * @param ctx   live VmafContext (from vmaf_init()).
 * @param mode  resize filter selector; see @ref VmafDnnResizeMode.
 *
 * @return  0          success.
 * @return -EINVAL     @p ctx is NULL or @p mode is outside the enum.
 * @return -ENOSYS     libvmaf was built without DNN support.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per driver thread.
 */
VMAF_EXPORT int vmaf_dnn_set_resize_mode(VmafContext *ctx, VmafDnnResizeMode mode);

/**
 * Standalone DNN session for filter-style inference (learned pre-processing,
 * C3). Unlike vmaf_use_tiny_model() this path does NOT need a VmafContext —
 * intended for consumers that want luma-in / luma-out without scoring.
 */
typedef struct VmafDnnSession VmafDnnSession;

/**
 * Open a session against @p onnx_path. Applies the same size-cap + allowlist
 * validation as vmaf_use_tiny_model().
 *
 * @param out        receives the new session handle on success. Pair with
 *                   @ref vmaf_dnn_session_close.
 * @param onnx_path  filesystem path to a .onnx file; must be a regular file.
 * @param cfg        optional device config; NULL uses VMAF_DNN_DEVICE_AUTO.
 *
 * @return 0 on success; -ENOSYS if built without DNN support; -EINVAL on
 *         NULL @p out / @p onnx_path; -ENOENT if the file does not exist;
 *         -E2BIG if the file exceeds VMAF_DNN_DEFAULT_MAX_BYTES; -EIO on
 *         ORT failure.
 *
 * @thread-safety Not thread-safe. Each session handle must be owned by one
 *               thread at a time; create one session per thread for concurrency.
 */
VMAF_EXPORT int vmaf_dnn_session_open(VmafDnnSession **out, const char *onnx_path,
                                      const VmafDnnConfig *cfg);

/**
 * Run one luma-in / luma-out pass. The model's input must be NCHW
 * [1, 1, H, W] float32. Input luma is normalised to [0,1] (and mean/std
 * from the sidecar if available); output is denormalised, rounded, and
 * clamped to [0, 255].
 *
 * @param sess        open session from @ref vmaf_dnn_session_open.
 * @param in          input luma plane (uint8).
 * @param in_stride   source row stride in bytes (>= w).
 * @param w           plane width; must match the model's static input shape.
 * @param h           plane height; must match the model's static input shape.
 * @param out         output luma plane (uint8); caller-owned.
 * @param out_stride  destination row stride in bytes.
 *
 * @return 0 on success, -ENOTSUP if the model shape is not luma-only,
 *         -ERANGE if @p w/@p h don't match the model's static input shape.
 *
 * @thread-safety Not thread-safe. Each session must be owned by one thread
 *               at a time.
 */
VMAF_EXPORT int vmaf_dnn_session_run_luma8(VmafDnnSession *sess, const uint8_t *in,
                                           size_t in_stride, int w, int h, uint8_t *out,
                                           size_t out_stride);

/**
 * 10-/12-/16-bit variant of @ref vmaf_dnn_session_run_luma8 — accepts a
 * packed uint16 little-endian plane and writes one back. The same model
 * trained on normalized float [0,1] works for any bit depth because the
 * loader simply divides by `(1 << bpc) - 1` on the way in and multiplies
 * on the way out. Used by the ffmpeg `vmaf_pre` filter for
 * yuv420p10le / yuv422p10le / yuv444p10le (and 12le) formats, and — on
 * any bit depth — to filter chroma planes with their own dimensions.
 * ADR-0170 / T6-4.
 *
 * @param sess        open session from @ref vmaf_dnn_session_open.
 * @param in          packed uint16 LE input plane.
 * @param in_stride   source row stride in **bytes** (>= w * 2).
 * @param w, h        plane dimensions. Must match the model's static
 *                    input shape or the chroma-plane dimensions if the
 *                    model was re-opened at chroma resolution.
 * @param bpc         bits per component in [9, 16].
 * @param out         packed uint16 LE output plane.
 * @param out_stride  destination row stride in **bytes**.
 *
 * @return 0 on success; -ENOTSUP if the model shape is not plane-only
 *         single-channel; -ERANGE if @p w/@p h don't match; -EINVAL on
 *         a bad @p bpc.
 *
 * @thread-safety Not thread-safe. Each session must be owned by one thread
 *               at a time.
 */
VMAF_EXPORT int vmaf_dnn_session_run_plane16(VmafDnnSession *sess, const uint16_t *in,
                                             size_t in_stride, int w, int h, int bpc, uint16_t *out,
                                             size_t out_stride);

/**
 * One input tensor passed to vmaf_dnn_session_run(). @p name binds by
 * ONNX graph input name when non-NULL; when NULL, the tensor is bound
 * positionally at the descriptor's array index. Tensors are float32,
 * row-major, with @p rank dimensions.
 */
typedef struct VmafDnnInput {
    const char *name;     /**< ONNX graph input name, or NULL for positional binding */
    const float *data;    /**< float32 element buffer, row-major */
    const int64_t *shape; /**< element extents, length @p rank */
    size_t rank;          /**< number of dimensions in @p shape */
} VmafDnnInput;

/**
 * One output tensor for vmaf_dnn_session_run(). @p data / @p capacity
 * are caller-owned; @p written is populated with the element count
 * actually produced. @p name binds by ONNX graph output name when
 * non-NULL, else positionally.
 */
typedef struct VmafDnnOutput {
    const char *name; /**< ONNX graph output name, or NULL for positional binding */
    float *data;      /**< caller-owned float32 output buffer */
    size_t capacity;  /**< @p data element capacity */
    size_t written;   /**< populated by the call with the element count produced */
} VmafDnnOutput;

/**
 * Run one inference pass with arbitrary named inputs and outputs. All
 * tensors are float32. The session's ONNX graph must declare exactly
 * @p n_inputs inputs and @p n_outputs outputs; mismatched arity returns
 * -EINVAL. Output buffers that are smaller than the produced tensor
 * return -ENOSPC; on -ENOSPC the @p written field is still populated
 * with the required element count so the caller can resize and retry.
 *
 * @param sess       open session from @ref vmaf_dnn_session_open.
 * @param inputs     array of @p n_inputs input descriptors.
 * @param n_inputs   element count of @p inputs.
 * @param outputs    array of @p n_outputs output descriptors.
 * @param n_outputs  element count of @p outputs.
 *
 * @return 0 on success; -ENOSYS if built without DNN support;
 *         -EINVAL on bad arity / null pointers; -ENOSPC if any output
 *         buffer is too small; -EIO on ORT failure.
 *
 * @thread-safety Not thread-safe. Each session must be owned by one thread
 *               at a time.
 */
VMAF_EXPORT int vmaf_dnn_session_run(VmafDnnSession *sess, const VmafDnnInput *inputs,
                                     size_t n_inputs, VmafDnnOutput *outputs, size_t n_outputs);

/**
 * Close a standalone DNN session and release all owned resources. Tears down
 * the ONNX Runtime session, releases input/output binding caches, and frees
 * the handle. Safe to call with a NULL @p sess. After this call any pointer
 * cached from @ref vmaf_dnn_session_attached_ep is invalidated. Pair with
 * every successful @ref vmaf_dnn_session_open.
 *
 * @param sess Session handle from @ref vmaf_dnn_session_open. NULL is a no-op.
 *
 * @thread-safety Not thread-safe. Use one VmafContext per thread.
 */
VMAF_EXPORT void vmaf_dnn_session_close(VmafDnnSession *sess);

/**
 * Name of the ONNX Runtime execution provider that actually bound to the
 * session. Useful for diagnostics and for asserting AUTO-chain behaviour
 * in tests. Stable strings: "CPU", "CUDA", "ROCm", "CoreML", "CoreML:ANE",
 * "CoreML:GPU", "CoreML:CPU", "OpenVINO:CPU", "OpenVINO:GPU",
 * "OpenVINO:NPU". Returns NULL if @p sess is NULL or libvmaf was built
 * without DNN support. Lifetime: owned by @p sess.
 *
 * @param sess  open session from @ref vmaf_dnn_session_open.
 *
 * @return NUL-terminated provider tag, or NULL if @p sess is NULL or
 *         libvmaf was built without DNN support.
 *
 * @thread-safety Safe to call from any thread once the session is open and
 *               no concurrent inference is in flight on that session.
 */
VMAF_EXPORT const char *vmaf_dnn_session_attached_ep(VmafDnnSession *sess);

/**
 * Verify the Sigstore bundle for a tiny model against the model registry
 * (T6-9 / ADR-0211). Looks up @p onnx_path's basename in
 * `model/tiny/registry.json` (alongside @p onnx_path unless
 * @p registry_path is non-NULL), reads the entry's `sigstore_bundle`
 * field, and shells out to `cosign verify-blob` via `posix_spawnp(3p)`.
 *
 * Designed to fail closed: any error short-circuits model load. Wired
 * through the CLI by `--tiny-model-verify`.
 *
 * @param onnx_path      filesystem path to the model file.
 * @param registry_path  optional explicit registry path; NULL → look up
 *                       `registry.json` next to @p onnx_path.
 *
 * @return 0 on successful verification, -ENOENT on missing registry /
 *         missing bundle / no matching entry, -EACCES when `cosign` is
 *         not on PATH, -EPROTO when cosign exits non-zero, -ENOSYS on
 *         Windows (the supply-chain workflow runs on Linux/macOS only),
 *         -EINVAL on a NULL @p onnx_path.
 *
 * @thread-safety Safe to call from any thread; uses no shared mutable state.
 */
VMAF_EXPORT int vmaf_dnn_verify_signature(const char *onnx_path, const char *registry_path);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_DNN_H */
