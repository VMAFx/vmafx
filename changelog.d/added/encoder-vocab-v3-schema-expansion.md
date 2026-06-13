- **`ENCODER_VOCAB` v3 (16-slot) schema scaffold + retrain plan
  (ADR-0302 Proposed, Research-0075).** The codec-aware
  `fr_regressor_v2` regressor's encoder vocabulary expands from 13
  slots to 16 to cover three vmaf-tune codec adapters that landed
  since `fr_regressor_v2` shipped to production
  ([ADR-0291](../docs/adr/0291-fr-regressor-v2-prod-ship.md)):
  `libsvtav1` (slot 13), `h264_videotoolbox` (slot 14),
  `hevc_videotoolbox` (slot 15). This PR ships **scaffold only** —
  a parallel `ENCODER_VOCAB_V3` constant in
  [`ai/scripts/train_fr_regressor_v2.py`](../ai/scripts/train_fr_regressor_v2.py)
  that documents the target schema; the live `ENCODER_VOCAB` and
  `ENCODER_VOCAB_VERSION = 2` remain the source of truth. The
  in-tree v2 ONNX continues to serve every consumer; the
  load-fallback shim collapses unknown v3 strings into the
  `unknown` one-hot column. The follow-up retrain PR is gated on
  clearing the same mean LOSO PLCC ≥ 0.95 ship gate
  [ADR-0291](../docs/adr/0291-fr-regressor-v2-prod-ship.md)
  cleared on v2, plus the
  [ADR-0235](../docs/adr/0235-codec-aware-fr-regressor.md)
  multi-codec lift floor (≥ +0.005 PLCC over the v1 single-input
  regressor). Append-only ordering is preserved — the 13 v2 slot
  indices keep their column positions verbatim. Re-scopes PR #373
  (the VT-adapters-plus-vocab change deferred when the VT adapters
  landed via ADR-0283).
