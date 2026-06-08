/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#ifndef LIBVMAF_DNN_MODEL_LOADER_H_
#define LIBVMAF_DNN_MODEL_LOADER_H_

#include <stdbool.h>
#include <stddef.h>

#include "libvmaf/model.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time cap for ONNX file size (50 MB).
 *
 *  The historical `VMAF_MAX_MODEL_BYTES` env override was removed in
 *  T7-12 once two release cycles passed without a shipped model
 *  approaching the cap. The constant is now the single source of
 *  truth — bump it here (and re-run the size-cap tests) if a future
 *  use case genuinely needs a larger envelope. */
#define VMAF_DNN_DEFAULT_MAX_BYTES ((size_t)50u * 1024u * 1024u)

/** Compile-time cap for the companion JSON sidecar file (1 MiB = 2^20 bytes).
 *  The sidecar carries only metadata (kind, opset, feature_order, etc.);
 *  a 1 MiB limit is already very generous.  All three guard points in
 *  model_loader.c use this single constant so a future bump only needs one
 *  edit here. */
#define DNN_SIDECAR_JSON_MAX ((size_t)(1u << 20u))

/** Post-training quantisation mode (ADR-0129 / ADR-0173). Tracks the
 *  per-model registry field of the same name; FP32 means the loader
 *  uses the .onnx file as shipped, the other three modes mean the
 *  loader prefers a sibling `<basename>.int8.onnx` produced by
 *  `ai/scripts/ptq_*.py` / `qat_train.py`. */
typedef enum VmafModelQuantMode {
    VMAF_QUANT_FP32 = 0,
    VMAF_QUANT_DYNAMIC = 1,
    VMAF_QUANT_STATIC = 2,
    VMAF_QUANT_QAT = 3,
} VmafModelQuantMode;

/** Maximum number of features in a feature-vector tiny model. The shipped
 *  models top out at 6 (canonical-6); 32 leaves comfortable headroom for any
 *  future extension (e.g. canonical-6 + extended motion stats) without
 *  bloating the sidecar struct or forcing a heap allocation per feature
 *  name. */
#define VMAF_DNN_MAX_FEATURE_NAMES 32u

/** Maximum entries in the encoder vocabulary (ADR-0519). The v2 vocabulary
 *  currently has 12 slots; 32 leaves room for hardware-encoder additions
 *  and the planned v3 16-slot expansion (ADR-0302) without a struct
 *  layout change. */
#define VMAF_DNN_MAX_ENCODER_VOCAB 32u

/** Maximum number of attached tiny-model outputs routed into the feature
 *  collector. Mirrors VMAF_ORT_MAX_IO without making the model-sidecar parser
 *  depend on the ORT wrapper header. */
#define VMAF_DNN_MAX_OUTPUT_NAMES 8u

typedef struct VmafModelSidecar {
    VmafModelKind kind; /**< mirrors sidecar "kind" field */
    int opset;
    char *name;        /**< owned */
    char *input_name;  /**< owned */
    char *output_name; /**< owned; legacy single-output spelling */
    size_t n_output_names;
    char *output_names[VMAF_DNN_MAX_OUTPUT_NAMES]; /**< owned */
    /* ADR-0976 — removed dead `norm_mean`, `norm_std`, `has_norm`,
     * `expected_min`, `expected_max`, `has_range` fields. None of the
     * shipped trainers (ai/scripts/) write the corresponding JSON keys
     * and `vmaf_dnn_sidecar_load` never populated them, so the consumer
     * branches in dnn_api.c / libvmaf.c were dead code. The deletion is
     * documented in ADR-0114 (Alternatives §2) as a follow-up cleanup. */
    VmafModelQuantMode quant_mode; /**< default FP32; mirrors sidecar/registry */

    /** Feature-vector tiny models (ADR-0518). The trainer ships the
     *  per-feature order, mean, and std in the sidecar; the loader
     *  parses them so the C-side `vmaf_ctx_dnn_run_frame` path knows
     *  which canonical libvmaf features to materialise into the input
     *  tensor, and which scaler to apply before inference.
     *
     *  Field-name compatibility: sidecars from both
     *  `train_fr_regressor_v2.py` (which writes `feature_order` /
     *  `feature_mean` / `feature_std`) and `vmaf_tiny_v4`-style
     *  trainers (which write `features` / `input_mean` / `input_std`)
     *  are accepted; the parser tries both keys.
     *
     *  `n_features == 0` means the sidecar carried no feature schema
     *  — the loader treats the model as image-rank (rank-4 NCHW). */
    size_t n_features;
    char *feature_names[VMAF_DNN_MAX_FEATURE_NAMES]; /**< owned */
    float feature_mean[VMAF_DNN_MAX_FEATURE_NAMES];
    float feature_std[VMAF_DNN_MAX_FEATURE_NAMES];
    bool has_feature_scaler; /**< true iff feature_mean / feature_std parsed */

    /** Codec-aware models (ADR-0519). When the sidecar carries an
     *  `encoder_vocab` JSON array, the loader populates these fields so
     *  the CLI can validate user-supplied `--tiny-codec` names before
     *  encoding the one-hot and reject unknown codecs with a clear error.
     *
     *  `n_encoder_vocab == 0` means the model is not codec-aware;
     *  `--tiny-codec` / `--tiny-preset` / `--tiny-crf` are silently
     *  ignored for such models. */
    size_t n_encoder_vocab;
    char *encoder_vocab[VMAF_DNN_MAX_ENCODER_VOCAB]; /**< owned */
    bool codec_aware; /**< true iff encoder_vocab was present in the sidecar */
} VmafModelSidecar;

/**
 * Fill the codec one-hot block @p buf (length @p buf_len) from the user-
 * supplied codec name, preset, and CRF — matching the encoding the
 * fr_regressor_v2 trainer uses (ADR-0522 / train_fr_regressor_v2.py).
 *
 * Layout (see sidecar field ``codec_block_layout``):
 *   slots 0 .. n_vocab-1 : encoder one-hot (exactly one slot set to 1.0)
 *   slot  n_vocab        : preset_norm     = ordinal / 9.0
 *   slot  n_vocab + 1    : crf_norm        = crf / 63.0
 *
 * @p vocab and @p n_vocab must match the sidecar's ``encoder_vocab`` order.
 * When @p codec_name is NULL or empty the "unknown" bucket (last entry in
 * the vocab) is used and the function returns 0. When @p codec_name is
 * non-NULL but does not appear in @p vocab the "unknown" bucket is still
 * written and the function returns -ENOENT so callers can hard-fail on
 * typos. @p preset may be NULL (defaults to ordinal 5 = "medium").
 *
 * @p crf is clamped to [0, 63] before normalisation.
 *
 * Returns 0 on success, -ENOENT on unknown @p codec_name, -EINVAL on null
 * pointers or @p buf_len != n_vocab + 2.
 */
int vmaf_dnn_codec_block_fill(float *buf, size_t buf_len, const char *const *vocab, size_t n_vocab,
                              const char *codec_name, const char *preset, int crf);

/** Byte-identical magic check. Returns VMAF_MODEL_KIND_SVM for libsvm json/pkl,
 *  DNN_FR/DNN_NR for ONNX (kind refined from sidecar), or -1 on unknown. */
int vmaf_dnn_sniff_kind(const char *path);

/** Loads `<path>.json` next to the ONNX file. Returns 0 on success. */
int vmaf_dnn_sidecar_load(const char *onnx_path, VmafModelSidecar *out);
void vmaf_dnn_sidecar_free(VmafModelSidecar *s);

/** Validate an ONNX file on disk: size cap + operator allowlist. */
int vmaf_dnn_validate_onnx(const char *path, size_t max_bytes);

/** Verify an ONNX file's Sigstore bundle by shelling out to `cosign
 *  verify-blob` (T6-9 / ADR-0211). Wired through the CLI by
 *  ``--tiny-model-verify``. The function:
 *
 *    1. Locates the registry entry whose ``onnx`` field has the same
 *       basename as ``onnx_path`` (registry path defaults to
 *       ``model/tiny/registry.json`` next to ``onnx_path`` unless the
 *       caller passes a different ``registry_path``).
 *    2. Reads the entry's ``sigstore_bundle`` path (relative to
 *       ``model/tiny/``).
 *    3. Spawns ``cosign verify-blob --bundle=<bundle> --certificate-identity-regexp …
 *       <onnx>`` via ``posix_spawnp`` (NOT ``system(3)`` — banned by
 *       CLAUDE.md §6) and waits for completion.
 *
 *  Returns 0 on success, ``-ENOENT`` when the registry / bundle is
 *  missing, ``-EACCES`` when ``cosign`` is unavailable on PATH, and
 *  ``-EPROTO`` when the verifier exits non-zero. Designed to fail
 *  closed: any error short-circuits model load.
 *
 *  ``registry_path`` may be NULL — the loader then computes
 *  ``<dirname(onnx_path)>/registry.json``. */
/* Public API — must be visible in the shared library (ADR-0379). */
VMAF_EXPORT int vmaf_dnn_verify_signature(const char *onnx_path, const char *registry_path);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_DNN_MODEL_LOADER_H_ */
