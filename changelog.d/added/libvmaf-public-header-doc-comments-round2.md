## Added

- **Doxygen comments on under-documented public libvmaf C-API entry points**
  (`core/include/libvmaf/feature.h`, `model.h`, `dnn.h`): round-2 follow-on
  to PR #302's targeted gap-close. Adds `@brief`, `@param`, `@return`, and
  ownership/lifetime contracts to 15 public surfaces that the ffmpeg patch
  stack, the Go/Rust bindings, and downstream MCP consumers depend on:

  - `feature.h`: `VmafFeatureDictionary` (struct), `vmaf_feature_dictionary_set`,
    `vmaf_feature_dictionary_free` (full file was undocumented).
  - `model.h`: `VmafModelFlags`, `VmafModelConfig`, `vmaf_model_load`,
    `vmaf_model_load_from_path`, `vmaf_model_feature_overload`,
    `vmaf_model_destroy`, `VmafModelCollection`,
    `VmafModelCollectionScoreType`, `VmafModelCollectionScore`,
    `vmaf_model_collection_load`, `vmaf_model_collection_load_from_path`,
    `vmaf_model_collection_feature_overload`,
    `vmaf_model_collection_destroy`.
  - `dnn.h`: `vmaf_dnn_session_close`.

  Each block documents return semantics (negative errno convention),
  ownership-transfer rules for dictionaries that pass into the library, and
  the `vmaf_model_destroy` / `vmaf_model_collection_destroy` pairing
  required to avoid double-free of collection-owned sub-models.

- **NOLINT citations on upstream-mirror include guards**: the three touched
  Netflix-copyright headers retain their `__VMAF_*_H__` include guards
  verbatim for rebase parity with Netflix/vmaf master. Each `#ifndef` /
  `#define` carries an inline NOLINT for `bugprone-reserved-identifier`
  citing CLAUDE.md §10 (Upstream sync) and §12 r12 (touched-file
  lint-clean rule per ADR-0278). No identifier changes; no ABI impact.

No semantic or ABI change. Doc-only comment additions plus the NOLINT
annotations required to leave the touched files lint-clean.
