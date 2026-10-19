---
paths:
  - ai/src/vmaf_train/predictor_train.py
  - ai/tests/test_predictor_train.py
  - ai/tests/test_predictor_card_markdown.py
  - ai/scripts/run_predictor_v2_training.sh
  - ai/scripts/train_predictor_v2_realcorpus.py
invariant: Predictor trainer is in vmaf_train (torch only in training envs, ADR-1886); imports vmaftune.predictor only.
---
<!-- markdownlint-disable MD013 MD060 -->
# vmaf-tune predictor trainer

- [ADR-1886](../../docs/adr/1886-torch-training-environments-only.md) —
  `vmaf_train.predictor_train` (was `vmaftune.predictor_train`) trains
  per-codec predictor MLPs that `tools/vmaf-tune` loads as ONNX. torch stays
  here; vmaf-tune declares no torch (`scripts/ci/check-torch-scope.py`).
- Direction of imports: trainer -> `vmaftune.predictor` (`_DEFAULT_COEFFS`,
  `Predictor`, `ShotFeatures`), so `CODECS` stays single-source. Installed
  `vmaftune` wins; checkout falls back to `tools/vmaf-tune/src` via
  `_ensure_vmaftune_importable()`. Never import trainer from vmaf-tune.
- Run: `python -m vmaf_train.predictor_train --output-dir model` in ai/
  environment. Tests run in suite `ai` (old `vmaf-tune-train` suite is
  gone); suite `ai` also lists `tools/vmaf-tune/src/vmaftune/predictor.py` as
  source, so predictor change reruns them.
