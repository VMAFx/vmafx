# `smoke_v0` — CI load-path probe

> **Status — CI internal.** This is not a quality model and must not
> be used for video quality assessment. It exists solely as a load-path
> probe for the libvmaf DNN integration CI gate.

`smoke_v0` is a minimal ONNX graph (Conv + Identity, one initializer
tensor, opset 17) used to verify that the C-side DNN loader, the ONNX
wire-format scanner, the op-allowlist gate, and the ORT execution path
all function correctly without requiring any training corpus or real
model weights. It is exercised in `core/test/` as part of the
`meson test --suite=fast` gate.

## Checkpoint facts

| Field | Value |
| --- | --- |
| Model id | `smoke_v0` |
| Location | `model/tiny/smoke_v0.onnx` |
| Architecture | Conv (1-weight kernel) + Identity — intentional CI probe |
| Trainable parameters | **1** (1-element Conv kernel; no bias) |
| Input | `features` — float32 `[N, …]` |
| Output | `score` — float32 `[N]` |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Registry entry | `smoke_v0` in `model/tiny/registry.json` (`"smoke": true`) |
| SHA-256 | `c83a5f217fa3736bf575c52f7f4c187a6201951e8ddccb51bdcdcc136108fbe0` |

## Purpose

The DNN integration CI gate needs a model file that:

1. Loads without error through the ONNX wire-format scanner.
2. Passes the op-allowlist check (Conv + Identity are both on the
   allowlist in `core/src/dnn/op_allowlist.c`).
3. Runs a forward pass through ORT without segfault or numerical error.
4. Does not require an actual training corpus, GPU, or network access.

`smoke_v0` satisfies all four criteria with a trivial 1-weight Conv.
The model is registered with `"smoke": true` in `registry.json` so
validation scripts know to skip quality-metric assertions for it.

## Evaluation metrics

Intentional smoke probe — not evaluated on any quality corpus. PLCC /
SROCC / RMSE figures are not applicable. The only gate the model must
pass is ORT load-and-run without error.

## Known limitations / when NOT to use

- Do not use for video quality assessment of any kind.
- Do not reference `smoke_v0` as a production model in any downstream
  pipeline. Its Conv kernel weight is a synthetic constant; its output
  has no relation to perceptual quality.
- The `"smoke": true` registry flag means model-registry validation
  scripts skip quality assertions. Any code path that branches on
  `smoke == false` for production use will correctly skip this model.

## See also

- [`smoke_fp16_v0.md`](smoke_fp16_v0.md) — companion fp16 I/O round-trip
  probe.
- `core/src/dnn/op_allowlist.c` — the allowlist this probe exercises.
- [ADR-0042](../../adr/0042-tinyai-docs-required-per-pr.md) — tiny-AI
  doc-substance rule this card satisfies.
