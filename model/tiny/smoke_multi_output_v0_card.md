# `smoke_multi_output_v0` — model card (multi-output CI probe)

> **Status**: CI test fixture only — not a quality model.

## Identity

| Field | Value |
| --- | --- |
| Model id | `smoke_multi_output_v0` |
| File | `model/tiny/smoke_multi_output_v0.onnx` |
| Architecture | Minimal graph with multiple outputs |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | CI probe only |

## Purpose

Exercises the multi-output ONNX session path in `vmaf_dnn_session_run`
(ADR-0040 multi-output extension). Verifies that the loader correctly handles
graphs with more than one named output tensor. Not a quality model.

## Known limits

Not applicable — CI fixture only.

## License + lineage

BSD-3-Clause-Plus-Patent. `registry.json` entry `smoke_multi_output_v0`
(if present) or referenced from `smoke_multi_output_v0.json`.
