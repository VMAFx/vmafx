- `vmaf --tiny-model <path>` now loads and runs the three shipped
  tiny FR-regressor checkpoints (`fr_regressor_v1`,
  `fr_regressor_v2`, `vmaf_tiny_v4`). Before this fix all three
  failed at attach time with errno `-95` (`ENOTSUP`) because the
  C-side loader rejected any ONNX whose input rank was not 4
  (NCHW image) — but every shipped FR-regressor is a rank-2
  feature-vector model. `vmaf_ctx_dnn_attach` now branches on
  rank, allocates a feature scratch buffer for rank-2 models,
  discovers the optional codec block's width via
  `vmaf_ort_input_shape_at`, and pre-seeds it to the "unknown"
  encoder one-hot. `vmaf_ctx_dnn_run_frame` materialises the
  canonical-6 features (`adm2`, `vif_scale0..3`, `motion2`)
  from libvmaf's classic feature collector at inference time
  and applies the sidecar's StandardScaler when present. The
  sidecar parser learns three new field names from the trainer
  contract (`feature_order` / `feature_mean` / `feature_std`
  for fr_regressor v1/v2; `features` / `input_mean` /
  `input_std` for `vmaf_tiny_v*`). External-data ONNX
  (`.onnx` + sibling `.onnx.data`) needs no special handling —
  ONNX Runtime resolves siblings automatically when
  `CreateSession` is given the absolute model path
  (ADR-0517).
