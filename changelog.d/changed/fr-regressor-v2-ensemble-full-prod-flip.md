- **`fr_regressor_v2_ensemble_v1` flipped to full production
  (real ONNX + per-seed sidecars + `smoke: false`).** Closes the
  ADR-0303 / ADR-0309 / ADR-0319 production-flip workflow. The
  five `fr_regressor_v2_ensemble_v1_seed{0..4}` rows in
  `model/tiny/registry.json` now point at LOSO-gated, full-corpus-
  trained ONNX weights (5,640-row Phase A canonical-6 corpus, 9
  sources, h264_nvenc, mean LOSO PLCC 0.997 ± 0.001 spread per
  `runs/ensemble_v2_real/PROMOTE.json`) instead of the 3025-byte
  synthetic-corpus scaffold weights. Each row gains a per-seed
  sidecar `model/tiny/fr_regressor_v2_ensemble_v1_seed{N}.json`
  mirroring the canonical `fr_regressor_v2.json` shape (encoder
  vocab v2, codec_block_layout, scaler params, training_recipe)
  plus seed-specific gate evidence — required by
  `core/test/dnn/test_registry.sh` for every non-smoke ONNX.
  New driver `ai/scripts/export_ensemble_v2_seeds.py` reuses the
  LOSO trainer's `_load_corpus` for codec-block fidelity and fits
  one full-corpus FRRegressor per seed. Going forward, any re-flip
  (corpus refresh, recipe change) must regenerate ONNX bytes +
  sidecars together via the same driver — see
  [`ai/AGENTS.md`](../../ai/AGENTS.md) and
  [ADR-0321](../../docs/adr/0321-fr-regressor-v2-ensemble-full-prod-flip.md).
