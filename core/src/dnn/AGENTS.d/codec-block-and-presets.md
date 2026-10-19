---
paths:
  - core/src/dnn/model_loader.c
  - core/src/dnn/model_loader.h
invariant: Codec block layout and preset ordinal mapping exactly mirror Python trainer constants and sidecar vocabularies.
---
<!-- markdownlint-disable MD013 -->
# Codec Block Layout and Preset Ordinals

- **No pre-seeded codec block (ADR-1520).** codec block of
  codec-aware model starts zero and model refuses to score until
  caller names codec; `vmaf_dnn_codec_block_fill()` finds `"unknown"`
  entry by name and returns `-ENOENT` for vocabulary without one. Never
  bring back positional default (third-from-last or last slot): in
  `fr_regressor_v3`'s vocabulary those slots are real encoders. See
  [feature-vector-inputs](feature-vector-inputs.md).

## Invariant — `PRESET_ORDINAL` mirrors Python trainer (ADR-0519)

`model_loader.c::codec_block_preset_ordinal()` is verbatim port of
`ai/scripts/train_fr_regressor_v2.py::PRESET_ORDINAL` (lines
169..234). When trainer adds encoder (e.g. AMD AMF in
ADR-0302 v3 retrain) or changes preset ordinal, C-side table
must update in same PR. Otherwise codec block populated by
`--tiny-codec` diverges from what model was trained against.

`PRESET_MAX_ORDINAL = 9.0` and `CRF_MAX = 63.0` constants are
shared invariants between two files; they appear inline in C
helper rather than as named constants so `grep '/ 9.0f'` /
`'/ 63.0f'` finds them.

Encoder vocabulary itself comes from sidecar's
`encoder_vocab` array (loaded into `VmafModelSidecar.encoder_vocab[]`),
not from duplicated C-side constant, so vocab bumps only require
new sidecar JSON — no C recompile.

## Invariant — codec block layout (ADR-0522)

Second input of `fr_regressor_v2` is exactly
`[encoder_onehot(N_VOCAB), preset_norm, crf_norm]`. Runtime
guards check `extra_in_width == n_encoder_vocab + 2u` at attach time
*and* in `vmaf_ctx_dnn_set_codec_context` bridge; both checks
must agree. If future codec-aware model uses different layout
(e.g. multi-scale codec mixing), bump sidecar
`codec_block_layout` array, add dispatch branch — never silently
extend `vmaf_dnn_codec_block_fill` to different layout.

## Invariant — the sidecar declares the scalar slots' normalisation (ADR-1558)

`VmafModelSidecar.codec_encoding` comes from `codec_preset_norm` /
`codec_preset_value` / `codec_crf_norm` / `codec_crf_min` / `codec_crf_max`;
absent keys mean `fr_regressor_v2` encoding (ordinal / 9, CRF / 63), and
unknown value or missing bound makes `vmaf_dnn_sidecar_load()` return
`-EINVAL`. `vmaf_dnn_codec_block_fill_encoded()` is one fill routine
(`vmaf_dnn_codec_block_fill()` passes NULL): min-max is v3 trainer's
formula, unclamped, 0.5 for empty range. Never infer model's encoding
in C from its id or vocabulary, and never clamp min-max value.
`test_codec_block_fill_encoded_*`, `test_sidecar_codec_encoding_*` and
`test_fr_v3_preset_slot_is_the_trained_constant` guard it.
