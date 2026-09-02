- **fr_regressor_v2 flips smoke → production (ADR-0291).** Retrained on
  the Phase A real-corpus aggregate (216 cells from 33,840 per-frame
  canonical-6 rows × ENCODER_VOCAB v2 hardware encoders). LOSO PLCC =
  **0.9681 ± 0.0207** clears the [ADR-0235](docs/adr/0235-codec-aware-fr-regressor.md)
  0.95 ship gate. Registry sha256 updated; sidecar JSON refreshed; ONNX
  shipped at 13,674 bytes. Companion research digest
  [Research-0067](docs/research/0067-fr-regressor-v2-prod-loso.md).
