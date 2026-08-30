- Restored two missing rows in `model/tiny/registry.json` that
  drifted away from the production checkpoints shipped on disk:
  - `fr_regressor_v3` (ADR-0323 / ADR-0302; LOSO mean PLCC 0.9975,
    `smoke: false`, sha256
    `eaa16d23461eda74940b2ed590edfcaf13428aade294e47792a5a15f4d3b999c`).
  - `saliency_student_v2` (ADR-0332 / Research-0089; val IoU 0.7105,
    `smoke: false`, sha256
    `8cdaa6c10ab44d9b03a6a207603bd0b61f5c75a6beafdc82da57215f0a2ab2ed`).
  Both ONNX blobs were tracked on `master` and documented as
  production-shipped, but their `registry.json` rows were absent —
  any `--tiny-model=<id>` lookup would have failed despite the ONNX
  files being present and signed. `core/test/dnn/test_registry.sh`
  now reports `OK: 23 registry entries verified`.
- Also restored `vmaf_tiny_v1` + `vmaf_tiny_v1_medium` rows. The ONNX
  blobs are tracked on `master` and actively referenced by
  `docs/ai/loso-eval.md`, `docs/ai/quant-eps.md`,
  `docs/ai/quantization.md`, and ADR-0203 as the legacy LOSO baseline
  and the quant-eps regression fixture, but had no registry rows.
  Marked `smoke: true` since they are superseded by `vmaf_tiny_v2` /
  `vmaf_tiny_v3` as production defaults (ADR-0244).
- Backfilled `license` + `license_url` + `sigstore_bundle` metadata
  on 15 pre-existing registry entries that were missing one or more
  of those fields (ADR-0211 / T6-9 contract). Fork-local models
  (`fr_regressor_v1`, `fr_regressor_v2`,
  `fr_regressor_v2_ensemble_v1_seed{0..4}`, `fr_regressor_v3`,
  `mobilesal_placeholder_v0`) get `BSD-3-Clause-Plus-Patent` +
  the fork `LICENSE` URL; upstream-derived models (`fastdvdnet_pre`,
  `transnet_v2`) keep their original upstream license/URL and only
  gain the `sigstore_bundle` field. `saliency_student_v1`,
  `saliency_student_v2`, `vmaf_tiny_v3`, `vmaf_tiny_v4` already
  carried their fork license fields and only needed the bundle path.
  Closes the `test_every_entry_has_license_metadata` gap in
  `python/test/model_registry_schema_test.py` — the suite is now
  green for the first time (10/10 passing).
