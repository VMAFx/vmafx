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
- `ai/scripts/train_fr_regressor_v3.py` discharged its ADR-0141 touched-file
  debt in the same change: `_load_corpus` (149 LOC), `main` (122) and
  `run_loso` (79) split into named helpers so every function is inside the
  HISS-04 / NASA Rule 4 60-LOC limit. Pure extraction — no behavioural change —
  and the repository's HISS infraction total drops from 286 to 283.
