# `smoke_v0_symbolic_batch` — CI symbolic batch dimension probe

> **Status — CI internal.** This is not a quality model and must not
> be used for video quality assessment. It exists solely as a shape-handling
> probe for the libvmaf DNN integration CI gate.

`smoke_v0_symbolic_batch` is a minimal ONNX graph (opset 17) whose input shape
declares a symbolic batch dimension (`dim_param="batch"`, surfaced by ORT as
`-1` or `0`). It was introduced under [ADR-0524](../../adr/0524-tiny-model-loader-symbolic-batch-dim.md)
to verify that `dnn_attach_api.c` and `dnn_api.c` accept symbolic batch inputs
and fold them to batch size 1 rather than rejecting them with `-ENOTSUP`.

## Checkpoint facts

| Field | Value |
| --- | --- |
| Model id | `smoke_v0_symbolic_batch` |
| Location | `model/tiny/smoke_v0_symbolic_batch.onnx` |
| Architecture | Identity mapping with dynamic batch axis |
| Trainable parameters | 0 |
| Input | `features` — float32 `["batch", 1, 4, 4]` |
| Output | `score` — float32 `["batch", 1]` |
| ONNX opset | 17 |
| License | BSD-2-Clause-Patent |
| Registry entry | `smoke_v0_symbolic_batch` in `model/tiny/registry.json` (`"smoke": true`) |
| SHA-256 | `ae850f00f074d28fe78df589e36506d396a6ff8bf9262f27b9c92257fa53a8a3` |

## Purpose

All shipped No-Reference tiny checkpoints (such as `nr_metric_v1.onnx`) declare
their input tensor with `dim_param="batch"`. Historically, the libvmaf shape
gate rejected `in_shape[0] != 1`. `smoke_v0_symbolic_batch` provides a
lightweight, fast CI probe verifying:

1. `in_shape[0] ∈ {1, -1, 0}` is accepted at attach time.
2. Symbolic H/W or fixed batch > 1 remains rejected with clear diagnostics.
3. Exercised in `core/test/dnn/test_vmaf_use_tiny_model.c` via
   `test_attach_accepts_symbolic_batch_rank4`.

## Output interpretation

Outputs are synthetic test signals. Values reflect fixture identities, not
perceptual quality. PLCC / SROCC / RMSE are not applicable.

## Runnable usage example

```bash
# Verify via the C unit test suite:
meson test -C build --suite=dnn test_vmaf_use_tiny_model

# Or inspect the symbolic dimension via Python:
python3 -c 'import onnxruntime as ort; sess = ort.InferenceSession("model/tiny/smoke_v0_symbolic_batch.onnx"); print("Input shape:", sess.get_inputs()[0].shape)'
```

## Known limitations

- **CI internal only**: do not use for perceptual quality assessment.
- **Batch folding**: symbolic batch is folded to 1; parallel multi-batch
  scheduling is not implemented.
- **CPU only**: designed for CI execution.

## See also

- [`smoke_v0.md`](smoke_v0.md) — baseline single-output smoke probe.
- [ADR-0524](../../adr/0524-tiny-model-loader-symbolic-batch-dim.md) —
  symbolic batch loader acceptance contract.
- [ADR-0042](../../adr/0042-tinyai-docs-required-per-pr.md) —
  tiny-AI documentation standard.
