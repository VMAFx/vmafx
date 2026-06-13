- **KonViD MOS head v1 — first fork-trained subjective-MOS predictor
  ([ADR-0336](../docs/adr/0336-konvid-mos-head-v1.md), Phase 3 of
  [ADR-0325](../docs/adr/0325-konvid-150k-corpus-ingestion.md)).**
  New trainer
  [`ai/scripts/train_konvid_mos_head.py`](../ai/scripts/train_konvid_mos_head.py)
  produces `model/konvid_mos_head_v1.onnx` (~5K params, opset 17,
  ONNX-allowlist conformant), mapping the canonical-6 libvmaf features
  + saliency mean/var + 3 TransNet shot-metadata columns +
  ENCODER_VOCAB v4 single-slot one-hot to a scalar MOS in `[1.0,
  5.0]`. The vmaf-tune predictor surface gains
  `Predictor.predict_mos(features, codec)` with a documented linear
  approximation `mos = (predicted_vmaf - 30) / 14` as the fallback
  when the ONNX is missing — no hard dependency on the model file
  shipping. Production-flip gate (PLCC ≥ 0.85, SROCC ≥ 0.82, RMSE ≤
  0.45 MOS, spread ≤ 0.005) is unchanged from ADR-0325 and is **not**
  lowered when the real-corpus retrain misses (memory
  `feedback_no_test_weakening`); the synthetic-corpus surrogate gate
  ships at PLCC ≥ 0.75. The shipped ONNX is a deterministic-seeded
  synthetic-corpus checkpoint with `Status: Proposed`; production
  flip is gated on PR #447 (KonViD-150k ingestion) landing and the
  real-corpus retrain clearing the gate. Tests:
  `ai/tests/test_train_konvid_mos_head.py` (15 cases — schema pins,
  determinism, k-fold partition, gate evaluator, smoke run +
  allowlist conformance) and
  `tools/vmaf-tune/tests/test_predict_mos.py` (8 cases —
  fallback path, head path, clamps, missing-ONNX seam). User docs:
  [`model/konvid_mos_head_v1_card.md`](../model/konvid_mos_head_v1_card.md).
