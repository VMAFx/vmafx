**Wire int8 redirect and fp32 fallback into vmaf_use_tiny_model** (ADR-0174, ADR-1032)

- `vmaf_use_tiny_model()` in `core/src/dnn/dnn_attach_api.c` now redirects to load `<basename>.int8.onnx`
  when the companion sidecar declares `quant_mode != VMAF_QUANT_FP32`, matching the behaviour of
  `vmaf_dnn_session_open()`.
- If the `.int8.onnx` model is missing or fails validation, the loader emits a debug-level log message
  and gracefully falls back to loading the fp32 baseline model per ADR-1032.
- The same fallback now also covers an int8 graph that passes the size cap and the op allowlist but
  that ONNX Runtime cannot create a session for (a build without a kernel for one of its quantised
  ops, e.g. `ConvInteger`). Both loader twins — `vmaf_use_tiny_model()` and
  `vmaf_dnn_session_open()` — retry the fp32 baseline once instead of failing, so the redirect never
  turns a working `--tiny-model` invocation into an error.
- Added regression tests in `core/test/dnn/test_vmaf_use_tiny_model.c` verifying the redirect, the fallback,
  and that missing external data files return an error rather than aborting.
