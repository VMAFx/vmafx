**vmaf_init double-init guard + vmaf_close pointer-contract + DNN fp32 fallback** (ADR-1032)

- `vmaf_init` now returns `-EINVAL` immediately when `*vmaf` is non-NULL,
  preventing silent context leaks caused by accidental double-initialisation.
- `vmaf_close` pointer-invalidity contract documented in
  `core/include/libvmaf/libvmaf.h` with a `@code` example showing the
  recommended null-after-close pattern.
- DNN `vmaf_dnn_session_open`: when the `.int8.onnx` sidecar is absent or
  fails validation, the session now falls through to the fp32 baseline model
  instead of returning an error, matching the stated "better degraded than
  dead" design intent.  A `VMAF_LOG_LEVEL_DEBUG` message makes the degradation
  observable.
- Unit test `test_vmaf_init_double_init_guard` added to the `fast` suite in
  `core/test/test_context.c`.
