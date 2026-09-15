- Confine Go predictor model-card reads to the model directory, retaining
  registry-resolved model names and known-stub fallback when an optional card is absent or escapes via a symlink.
- Require the Go security and test job before merging; route its native and
  ONNX checks from Go/core/model changes while documentation-only changes
  report an explicit skip of the heavy work (ADR-1238).
