---
name: add-model
description: Register a new VMAF model (.json / .pkl / .onnx) with the build, validate its schema, and add a loader smoke test.
---
<!-- markdownlint-disable MD013 -->

# /add-model

## Invocation

```text
/add-model <path> [--type=classical-json|bootstrap-pkl|tiny-onnx] [--install]
```

## Steps

1. Validate file exists + has allowed extension.
2. `.json`: parse; require top-level keys `model_type`, `feature_names`,
   `score_clip`, `model` (LibsvmNuSvr / BootstrapLibsvmNuSvr / Onnx).
   Reject on unknown keys.
3. `.pkl`: use `python/vmaf/tools/check_pkl_model.py` (if present)
   to validate pickle -> refuse arbitrary-code-execution pickles
   (see SECURITY.md).
4. `.onnx`: require opset ≥ 17, run `onnx.checker.check_model(...)`, verify
   input/output shapes match declared `model_type`.
5. Copy to `model/` (or `model/tiny/` for `.onnx`) -> never overwrite.
6. Tiny-AI `.onnx` additions: verify exporter/trainer wrote sidecar with
   `run_provenance.schema == "ai-run-provenance-v1"`. Script lacks evidence
   -> run `/ai-run-manifest` first, fix script before registering model.
7. `--install` passed -> patch `core/src/meson.build` or `model/meson.build`
   to add file to install set.
8. Add loader test in `core/test/test_model.c`: load model, assert basic
   metadata read, unload cleanly.
9. Emit summary: model name, type, install target, test added.

## Guardrails

- `.pkl` models load in sandboxed Python subprocess with `restrictedpython`
  or equivalent; never trusted to execute arbitrary code at load time.
- `.onnx` models run through `onnxruntime.InferenceSession` with operator
  allowlist specified in `core/src/dnn/allowed_ops.txt`.
- Tiny-AI model artifacts must have replay evidence. New exporter/trainer
  sidecars use `aiutils.run_manifest.write_run_manifest()` unless embedding
  provenance into already-stable report schema.
