# `smoke_multi_output_v0` — CI multi-output test fixture

> **Status — CI internal.** This is not a quality model and must not
> be used for video quality assessment. It exists solely as a multi-output
> graph probe for the libvmaf DNN integration CI gate.

`smoke_multi_output_v0` is a minimal ONNX graph that emits two separate named
output tensors (`mean_score` and `peak_score`). It is used to verify that the
C-side DNN loader (`dnn_attach_api.c`) and score collector correctly register
and record multiple named outputs from a single attached tiny model.

## Checkpoint facts

| Field | Value |
| --- | --- |
| Model id | `smoke_multi_output_v0` |
| Location | `model/tiny/smoke_multi_output_v0.onnx` |
| Architecture | Identity / multi-head split — intentional CI probe |
| Trainable parameters | 0 (Identity mapping) |
| Input | `luma` — float32 `[1, 1, H, W]` |
| Output | `mean_score`, `peak_score` — float32 `[1]` each |
| ONNX opset | 17 |
| License | BSD-2-Clause-Patent |
| Registry entry | `smoke_multi_output_v0` in `model/tiny/registry.json` (`"smoke": true`) |
| SHA-256 | `4b63ff5a9d82e21b7a2d3be16aee70c1840ea89bbf1b71d604b901fc82b8214d` |

## Purpose

The multi-output attach path in `vmaf_ctx_dnn_attach` allows tiny models to
record multiple per-frame metrics into the score dictionary under distinct
names. `smoke_multi_output_v0` verifies:

1. The companion sidecar (`model/tiny/smoke_multi_output_v0.json`) specifies
   `output_names: ["mean_score", "peak_score"]`.
2. The runtime attaches both output tensors and files per-frame values under
   their declared keys without memory leaks or name collisions.
3. Exercised in `core/test/dnn/test_vmaf_use_tiny_model.c` via
   `test_attached_multi_output_model_records_named_scores`.

## Output interpretation

Outputs are synthetic test signals. Values reflect test fixture identities, not
perceptual quality. PLCC / SROCC / RMSE are not applicable.

## Runnable usage example

```bash
# Verify via the C unit test suite:
meson test -C build --suite=dnn test_vmaf_use_tiny_model

# Or inspect the session outputs via Python:
python3 -c 'import onnxruntime as ort; sess = ort.InferenceSession("model/tiny/smoke_multi_output_v0.onnx"); print("Outputs:", [o.name for o in sess.get_outputs()])'
```

## Known limitations

- **CI internal only**: do not use for perceptual quality assessment.
- **Fixed batch**: batch dimension is 1; batched scheduling is not supported.
- **CPU only**: test fixture is intended for fast CI validation.

## See also

- [`smoke_v0.md`](smoke_v0.md) — single-output CI smoke probe.
- [`core/test/dnn/test_vmaf_use_tiny_model.c`](../../../core/test/dnn/test_vmaf_use_tiny_model.c)
  — regression tests.
- [ADR-0042](../../adr/0042-tinyai-docs-required-per-pr.md) —
  tiny-AI documentation standard.
