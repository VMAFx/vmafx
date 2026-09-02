- **`fr_regressor_v3` — train + register on `ENCODER_VOCAB` v3
  (16-slot) — gate PASSED (ADR-0323).** Closes the v3 retrain
  deferral landed by [ADR-0302](../docs/adr/0302-encoder-vocab-v3-schema-expansion.md)
  (PR #401). New trainer
  [`ai/scripts/train_fr_regressor_v3.py`](../ai/scripts/train_fr_regressor_v3.py)
  reuses the LOSO recipe from
  [ADR-0319](../docs/adr/0319-ensemble-loso-trainer-real-impl.md) —
  9-fold leave-one-source-out over the Phase A canonical-6 corpus,
  fold-local StandardScaler, `FRRegressor(in_features=6,
  num_codecs=18)`, Adam(lr=5e-4, wd=1e-5), MSE, 200 epochs, bs=32 —
  on the NVENC-only Phase A corpus (5,640 rows). **Gate PASS:** mean
  LOSO PLCC = **0.9975 ± 0.0018**, every source above 0.99 — clears
  the ADR-0302 / [ADR-0291](../docs/adr/0291-fr-regressor-v2-prod-ship.md)
  0.95 floor with ~5pp margin. New artefacts:
  `model/tiny/fr_regressor_v3.onnx` (opset 17, two-input
  `features:[N,6]` + `codec_block:[N,18]` → `vmaf:[N]`),
  `model/tiny/fr_regressor_v3.json` (sidecar mirrors
  `fr_regressor_v2.json` with `encoder_vocab_version: 3`, full
  per-fold trace, `corpus` + `corpus_sha256`), registry row
  `fr_regressor_v3` (`smoke: false`), tests
  `ai/tests/test_train_fr_regressor_v3.py`, model card
  [`docs/ai/models/fr_regressor_v3.md`](../docs/ai/models/fr_regressor_v3.md).
  Live `ENCODER_VOCAB_VERSION = 2` in `train_fr_regressor_v2.py`
  **stays authoritative for `fr_regressor_v2.onnx`** — v3 ships as a
  parallel checkpoint; v2 → v3 in-place promotion is a separate PR
  per ADR-0302's production-flip checklist. NVENC-only corpus caveat
  documented honestly in the model card: 15 of 16 vocab slots
  receive zero training rows;
  [ADR-0235](../docs/adr/0235-codec-aware-fr-regressor.md)
  multi-codec lift floor (≥+0.005 PLCC) is not yet measurable on
  this corpus drop and is deferred to a multi-codec retrain.
