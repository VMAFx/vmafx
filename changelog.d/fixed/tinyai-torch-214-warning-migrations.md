- Tiny-AI ONNX exports migrated off `dynamic_axes` to `dynamic_shapes`
  (`vmaf_train.models.exports`, `ai/train/train.py`,
  `ai/scripts/train_fr_regressor_v3.py`) and `export_u2netp_mirror.py` off the
  legacy TorchScript exporter. `torch.onnx.export` has defaulted to the dynamo
  exporter since torch 2.9 and warns on both spellings. Every migration was
  checked on torch 2.14 to produce a byte-identical ONNX graph; the U2NETP
  mirror was additionally checked against the real upstream architecture
  (identical onnxruntime output, H/W still dynamic, graph still allowlist-clean).
- `FRRegressor._loss` split out of `FRRegressor._step`, so the loss arithmetic
  can be exercised without the Trainer-scoped `self.log()` calls that Lightning
  warns about when no `Trainer` is attached. Logging behaviour inside a real
  fit loop is unchanged — same metric names, same flags.
