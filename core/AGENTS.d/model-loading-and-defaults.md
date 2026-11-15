---
paths:
  - core/include/libvmaf/model.h
  - core/src/model.c
  - core/src/predict.c
invariant: Default model version defined solely by VMAF_DEFAULT_MODEL_VERSION; model loading scales without ceiling.
area: engine-and-runtime
---
<!-- markdownlint-disable MD013 MD060 -->
# Model loading, default model definition, and prediction scores

- **JSON model loader has no fixed feature/knot schema ceiling.**
  [`src/read_json_model.c`](../src/read_json_model.c) grows
  `VmafModel.feature` and `score_transform.knots.list` from JSON
  payload. Never restore old `MAX_FEATURE_COUNT` / `MAX_KNOT_COUNT`
  rejection pattern during upstream sync; external model JSONs with
  65+ features or 11+ piecewise-linear knots must parse if payload
  is otherwise valid. Regression coverage lives in
  [`test/test_model.c`](../test/test_model.c).

- **`predict_load_feature_score` EAGAIN vs EINVAL**: when feature vector
  is absent from collector (i.e., `fv == NULL` and
  `vmaf_feature_collector_get_score` returns `-EINVAL`), `predict_load_feature_score`
  must return `-EAGAIN`, not `-EINVAL`. This preserves Netflix#755 / ADR-0154:
  "score not yet written" is transient; only genuine programmer error
  (bad range, NULL pointer) returns `-EINVAL` from `vmaf_score_pooled`.

- **Authoritative twin sides for model and unit tests (ADR-1153)**:
  `core/src/model.c` is sole authoritative implementation of model-loading
  and collection APIs; `model.cpp` was deleted as dead and stale. In `core/test/`,
  `test_dict.cpp` and `test_feature.cpp` are sole authoritative tests;
  uncompiled legacy C twins `test_dict.c` and `test_feature.c` were deleted.

## The default model has exactly one definition

`VMAF_DEFAULT_MODEL_VERSION` in `core/include/libvmaf/model.h` is only
place fork decides which model to score with when caller names none
(ADR-1168). Never write `"vmaf_v0.6.1"` as fallback anywhere else:

- C / C++ compiled against headers use macro.
- Anything linking libvmaf at runtime calls `vmaf_default_model_version()`.
- Go and Python tools use their gate-checked mirrors
  (`pkg/model.DefaultVersion`, `vmaftune.defaultmodel.DEFAULT_MODEL`,
  `vmafroiscore.defaultmodel.DEFAULT_MODEL`).

`scripts/ci/check-default-model-single-source.sh` fails build on drifted
mirror or new hardcoded fallback, so this is enforced rather than advisory.

**Rebase-sensitive:** macro and accessor do not exist upstream. AOM CTC
preset in `core/tools/cli_parse.cpp` deliberately keeps literal
`"vmaf_v0.6.1"` with `vmaf-model-pin:` comment because CTC specification
mandates that exact model. Upstream sync reverting either of those breaks
gate. See `docs/rebase-notes.md`.

**Changing value is more than text edit.** One Netflix golden assertion,
`vmafexec_test.py::test_run_vmafexec_runner_use_default_built_in_model`, pins
default model's scores, so any change of default breaks it and ADR-0024
forbids editing it. Read `docs/development/default-model.md` before touching
value.

## The default model is `vmaf_v1.0.16_3d0h`, and NEG is not

Since [ADR-1169](../../docs/adr/1169-default-model-v1-0-16.md) fork scores with
`vmaf_v1.0.16_3d0h` when no model is named. **Upstream Netflix still defaults to
`vmaf_v0.6.1`**, so upstream sync will look like it wants to revert this. It
does not. See `docs/rebase-notes.md`.

Two things easy to get wrong:

- **NEG is not derived from default.** There is no NEG counterpart to any
  `vmaf_v1.0.16_*` model — Netflix published NEG for v0.6.1 family only.
  `DefaultNEGVersion` / `DEFAULT_MODEL_NEG` are independent constants naming
  `vmaf_v0.6.1neg`. Writing `DefaultVersion + "neg"` synthesises
  `vmaf_v1.0.16_3d0hneg`, which does not exist and which libvmaf rejects at
  load. Python mirror *did* derive it that way, had to be fixed.
- **default change breaks golden test by KeyError, not by value drift.**
  `vmafexec_test.py::test_run_vmafexec_runner_use_default_built_in_model`
  asserts v0.6.1 feature-family values (`vif_scale0..3`, `motion2`); v1
  family emits `integer_aim` / `cambi` / `speed_chroma` and none of those. Changing
  default again surfaces
  `KeyError('VMAFEXEC_vif_scale0_score')`. Resolve by naming model in
  that test, exactly as ADR-1169 did — **never** by editing
  `assertAlmostEqual` value (ADR-0024).
