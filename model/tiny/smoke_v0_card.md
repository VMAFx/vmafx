# `smoke_v0` — model card (CI load-path probe)

> **Status**: CI test fixture only — not a quality model.

## Identity

| Field | Value |
| --- | --- |
| Model id | `smoke_v0` |
| File | `model/tiny/smoke_v0.onnx` |
| Architecture | Conv + Identity — minimal graph for ONNX loader smoke test |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | CI probe only |

## Purpose

Exercises the `vmaf_dnn_session_create` / `vmaf_dnn_session_run` load path
end-to-end without requiring trained weights on disk. Not a quality model and
not referenced in any production scoring pipeline.

## Known limits

Not applicable — CI fixture only.

## License + lineage

BSD-3-Clause-Plus-Patent. `registry.json` entry `smoke_v0`.
