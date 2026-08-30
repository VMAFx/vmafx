- **Tiny-model loader accepts ONNX inputs with a symbolic batch
  dimension (ADR-0524).** `vmaf_ctx_dnn_attach` rejected
  `model/tiny/nr_metric_v1.onnx` and every other shipped NR
  checkpoint because their input shape is declared as
  `['batch', 1, 224, 224]`, where the first dim is the ONNX
  `dim_param='batch'` token that ORT surfaces through the C API as
  `-1`. The legacy gate required `in_shape[0] == 1` and failed
  with `-ENOTSUP` (errno 95). Surfaced by the `--no-reference`
  wiring agent (PR #1280 / ADR-0520) as the next blocker: every
  shipped NR tiny model uses the PyTorch / ONNX
  `torch.onnx.export(..., dynamic_axes=…)` default. Fix: fold
  `in_shape[0] ∈ {1, -1}` to batch=1 in both `dnn_attach_nchw` and
  `dnn_attach_feature_vector` (and on the optional rank-2 second
  input, e.g. `fr_regressor_v2`'s `codec` block). A fixed batch
  greater than 1 stays rejected (no batched-inference scheduler
  exists; the per-frame loop always emits batch=1 on the ORT Run
  call). Symbolic spatial dims (H/W) remain rejected because the
  scratch buffer is sized once at attach time — the diagnostic is
  sharpened to distinguish symbolic H/W from "C != 1" so the
  failure mode is observable. Regression test
  `test_attach_accepts_symbolic_batch_rank4` in
  `core/test/dnn/test_vmaf_use_tiny_model.c` exercises the
  fixture `model/tiny/smoke_v0_symbolic_batch.onnx`; the
  `test_cli.sh` `--no-reference` smoke now uses
  `nr_metric_v1.onnx` end-to-end (replacing the prior
  load-fails-for-unrelated-reasons `dists_sq.onnx` placeholder).
