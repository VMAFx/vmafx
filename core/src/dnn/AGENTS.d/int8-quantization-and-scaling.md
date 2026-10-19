---
paths:
  - core/src/dnn/dnn_api.c
  - core/src/dnn/dnn_attach_api.c
  - core/src/dnn/ort_backend.c
invariant: Quantized int8 models redirect through fallback session opening and declare onnx_has_scaler to prevent drift.
---
<!-- markdownlint-disable MD013 -->
# Int8 Quantization Redirect and Scaler Declarations

## Invariant — int8 loader redirect and scaler declaration contract

- **Sidecar `quant_mode` drives redirect**:
  - entry points: `vmaf_use_tiny_model()` (`dnn_attach_api.c`), `vmaf_dnn_session_open()` (`dnn_api.c`).
  - explicit `.int8.onnx` paths: if the caller passes a path ending in `.int8.onnx`, path resolution preserves it directly without appending a redundant `.int8` suffix (shared `kInt8Suffix` early return in both entry points).
  - sidecar `quant_mode != VMAF_QUANT_FP32` -> load sibling `<basename>.int8.onnx` when present and valid; else fp32 baseline, logged at `VMAF_LOG_LEVEL_DEBUG` (ADR-1032).
  - trigger 1: int8 file fails size cap or op allowlist -> each entry point's own path resolver.
  - trigger 2: `vmaf_ort_open()` fails on int8 graph that passed those gates (ONNX Runtime build without kernel for quantised op; seen: `ConvInteger`) -> `vmaf_ort_open_with_fallback()` in `ort_backend.c`, only home. First attempt logs its `CreateSession` failure at DEBUG.
  - both entry points open sessions through `vmaf_ort_open_with_fallback()`; never `vmaf_ort_open()` on int8 path directly. Two private copies drifted once: `T-DNN-ATTACH-INT8-REDIRECT-MISSING-2026-09-04`.
  - never turn invocation that works on fp32 baseline into hard failure. Covered by `core/test/dnn/test_cli.sh` (`--tiny-model model/tiny/nr_metric_v1.onnx`).
- **`onnx_has_scaler` must match graph**: If int8 model's ONNX graph
  bakes in input normalisation / scaling ops (`Sub`/`Div` or scalar constants),
  its companion sidecar `.json` must declare `"onnx_has_scaler": true` so
  runtime normalisation is bypassed, double-scaling prevented.
  Enforced over every `model/tiny/*.int8.onnx` by
  `core/test/dnn/test_registry.sh`, `python/test/model_registry_schema_test.py`,
  and `ai/scripts/validate_model_registry.py`. Measured cost of getting this
  wrong: pooled `vmaf_tiny_model` 16.02 instead of 71.95 on Netflix src01
  pair (`T-TINY-V3-INT8-SIDECAR-MISSING-ONNX-HAS-SCALER-2026-09-04`).
- **Redirect does not check `int8_sha256`**: only load-time gates are
  size cap and op allowlist, in both `dnn_api.c` and
  `dnn_attach_api.c`. Never add digest check to one twin without other.
  Not at all without ADR — digest mismatch is third outcome that
  ADR-1032's fp32-fallback semantics do not currently define.
