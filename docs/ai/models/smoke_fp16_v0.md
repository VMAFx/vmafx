# `smoke_fp16_v0` — CI fp16 I/O round-trip probe

> **Status — CI internal.** This is not a quality model and must not
> be used for video quality assessment. It exists solely as a fp16
> cast-path probe for the libvmaf DNN integration CI gate.

`smoke_fp16_v0` is a minimal ONNX graph (Identity op, no initializer
tensors, opset 17) with fp16 input and output tensors. It exercises the
fp16 I/O cast path in the C-side DNN loader — specifically the path that
casts float32 activations to fp16 before inference and back to float32
after. No trained weights are involved; the Identity op passes values
through unchanged.

## Checkpoint facts

| Field | Value |
| --- | --- |
| Model id | `smoke_fp16_v0` |
| Location | `model/tiny/smoke_fp16_v0.onnx` |
| Architecture | Identity (fp16 I/O) — intentional CI fp16 cast probe |
| Trainable parameters | **0** (no initializer tensors) |
| Input | fp16 tensor (cast from float32 by the DNN loader) |
| Output | fp16 tensor (cast back to float32 by the DNN loader) |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Registry entry | `smoke_fp16_v0` in `model/tiny/registry.json` (`"smoke": true`) |
| SHA-256 | `6cbf16be5d2cfb858f1eb60bfdcc9e674c15f17b3ff365afa475bbe9be76258b` |

## Purpose

The libvmaf DNN integration supports models that declare fp16 input/output
tensors, with the loader performing cast from float32 → fp16 on input and
fp16 → float32 on output. `smoke_fp16_v0` verifies that the full round-trip
cast path functions correctly:

1. The loader detects fp16 I/O via the ONNX graph element type.
2. The cast-to-fp16 step runs without error.
3. ORT executes the Identity op in fp16.
4. The cast-from-fp16 step runs and the output is numerically close to
   the input (within fp16 precision, ~3 ULP for values in the VMAF
   feature range).

The model is registered with `"smoke": true` in `registry.json` so
validation scripts know to skip quality-metric assertions.

## Evaluation metrics

Intentional smoke probe — not evaluated on any quality corpus. PLCC /
SROCC / RMSE figures are not applicable. The only gate the model must
pass is ORT load-and-run without error, plus round-trip numerical
proximity within fp16 precision.

## Known limitations / when NOT to use

- Do not use for video quality assessment of any kind.
- fp16 I/O incurs precision loss relative to fp32. For production VMAF
  fusion models the fork ships fp32-only checkpoints; the fp16 path is
  reserved for future optimized inference variants.
- The `"smoke": true` registry flag means model-registry validation
  scripts skip quality assertions.

## See also

- [`smoke_v0.md`](smoke_v0.md) — companion fp32 load-path probe.
- `core/src/dnn/` — the DNN loader that exercises the fp16 cast path.
- [ADR-0042](../../adr/0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule this card satisfies.
