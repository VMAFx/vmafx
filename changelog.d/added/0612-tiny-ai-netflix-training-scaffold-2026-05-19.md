- **Tiny-AI Netflix corpus training scaffold (2026-05-19 iteration, ADR-0612)**: adds
  refreshed research digest 0607 surveying 2024–2026 distillation literature
  (EfficientVMAF, temperature-scaled IQA distillation, ONNX Runtime 1.19/1.20
  MatMul optimisations, learned feature-reweighting); formalises the architecture
  alternatives table (MLP 2×64 nano / MLP 3×128 tiny / attention-pooled / feature-
  reweighting) and the `--data-root` CLI contract for the training harness in `ai/`.
  No training run; no Netflix golden assertions modified.  Architecture selection
  and the actual training run are deferred to a follow-up PR.
