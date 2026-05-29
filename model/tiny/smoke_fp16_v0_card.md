# `smoke_fp16_v0` — model card (fp16 I/O round-trip CI probe)

> **Status**: CI test fixture only — not a quality model.

## Identity

| Field | Value |
| --- | --- |
| Model id | `smoke_fp16_v0` |
| File | `model/tiny/smoke_fp16_v0.onnx` |
| Architecture | Identity op — fp16 I/O round-trip graph |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | CI probe only |

## Purpose

Exercises the fp16_io cast path in `vmaf_dnn_session_run`. Verifies that the
ONNX Runtime fp16 I/O cast round-trips without precision loss beyond
acceptable tolerance. Not a quality model.

## Known limits

Not applicable — CI fixture only.

## License + lineage

BSD-3-Clause-Plus-Patent. `registry.json` entry `smoke_fp16_v0`.
