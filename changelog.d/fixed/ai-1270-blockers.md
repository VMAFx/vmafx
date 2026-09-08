- Triage and point-of-use guards for issue #1270 blockers:
  - `docs/metrics/dists.md`: documented that `model/tiny/dists_sq.onnx` is a 3-op
    synthetic MSE smoke placeholder lacking the learned Ding et al. multi-scale
    backbone; added runtime warning at point of use in `core/src/feature/feature_dists.c`
    when loading placeholder checkpoints. Tracked as
    `T-DISTS-PLACEHOLDER-CHECKPOINT-2026-09-08`.
  - `docs/ai/models/mobilesal.md`: corrected stale documentation claiming saliency
    is a placeholder; production saliency uses `saliency_student_v2.onnx` (ADR-0444)
    and `saliency_student_v1.onnx` (ADR-0286). `mobilesal.onnx` is the legacy smoke
    placeholder, and `core/src/feature/feature_mobilesal.c` warns at point of use
    recommending `saliency_student_v2.onnx`.
  - `docs/ai/predictor.md`: documented that software (`libx264`, `libx265`,
    `libsvtav1`, `libaom-av1`, `libvvenc`) and AMF (`h264_amf`, `hevc_amf`,
    `av1_amf`) models are synthetic stubs (`synthetic-stub-N=100`, ADR-0325); added
    point-of-use warning in Python `Predictor` / `vmaf-tune` and Go `NewWithModel`.
    Tracked as `T-PREDICTOR-SOFTWARE-AMF-STUB-MODELS-2026-09-08`.
