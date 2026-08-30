- **Per-shot VMAF predictor — training pipeline + 14 stub ONNX models
  ([ADR-0325](../docs/adr/0325-predictor-stub-models-policy.md)).**
  Companion to PR #430 (predictor runtime). Adds
  `tools/vmaf-tune/src/vmaftune/predictor_train.py` — a tiny-MLP
  trainer (14 inputs × 64 hidden × 1 output, opset 18, ~5K params)
  that consumes a vmaf-tune Phase A JSONL corpus and emits one
  `model/predictor_<codec>.onnx` per codec adapter, validated against
  the libvmaf op-allowlist. Ships **synthetic-stub** models for all
  14 codec adapters (`libx264`, `libx265`, `libsvtav1`, `libaom-av1`,
  `libvvenc`, plus the NVENC / AMF / QSV families across H.264, HEVC,
  AV1) trained from a deterministic 100-row synthetic corpus per
  codec so the runtime ONNX path is testable on a fresh checkout.
  Each shipped model carries a Markdown model card under
  `model/predictor_<codec>_card.md` flagging
  `corpus.kind: synthetic-stub-N=100` plus a do-not-use-in-production
  warning; cards switch to `corpus.kind: real-N=<rows>` when the
  operator runs the trainer against a real corpus. New tests under
  `tools/vmaf-tune/tests/test_predictor_train.py` pin (1) the trainer
  end-to-end on a 50-row synthetic corpus, (2) loadability +
  monotonicity of every shipped model under ONNX Runtime CPU, and
  (3) `Predictor(model_path=...)` routing through the ONNX session.
  User docs: [`docs/ai/predictor.md`](../docs/ai/predictor.md).
