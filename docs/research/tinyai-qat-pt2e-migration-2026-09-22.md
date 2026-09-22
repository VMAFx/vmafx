<!-- markdownlint-disable MD013 -->
# Research digest: migrating tiny-AI QAT to torchao pt2e

- **Date**: 2026-09-22
- **Decision recorded in**: [ADR-1293](../adr/1293-tinyai-qat-torchao-pt2e.md)
- **Code**: `ai/train/qat.py`, `ai/pyproject.toml`

Every measurement below was taken in a venv matching the `Tiny AI` job: torch
2.14.0+cpu, torchvision 0.29.0+cpu, pytorch-lightning 2.6.6, torchao 0.18.0,
numpy 2.5.3, Python 3.14.7.

## The failure

```text
DeprecationWarning: torch.ao.quantization is deprecated and will be removed in 2.10.
  1. Eager mode quantization ... migrate to torchao eager mode quantize_ API
  2. FX graph mode quantization (prepare_fx, convert_fx) ... migrate to
     torchao pt2e quantization API instead (prepare_pt2e, convert_pt2e)
```

Raised by the decorator around `prepare_qat_fx`, not at import: importing
`torch.ao.quantization` and `torch.ao.quantization.quantize_fx` under
`warnings.simplefilter("error")` succeeds. `ai/pyproject.toml` sets
`filterwarnings = ["error"]`, so the call fails `test_qat_smoke`. Before this
change the file suite is `1 failed, 1302 passed`.

## Recipe comparison

Read off the two configuration objects directly rather than inferred:

| Observer setting | `get_default_qat_qconfig_mapping("x86")` | `get_default_x86_inductor_quantization_config(is_qat=True)` |
| --- | --- | --- |
| activation dtype | `torch.quint8` | `torch.uint8` |
| activation qscheme | `per_tensor_affine` | `per_tensor_affine` |
| activation range | **0 … 127** | **0 … 255** |
| weight dtype | `torch.qint8` | `torch.int8` |
| weight qscheme | `per_channel_symmetric` | `per_channel_symmetric` |
| weight `ch_axis` | 0 | 0 |
| weight range | -128 … 127 | -128 … 127 |

Weights are identical. The activation range differs because the old x86 QAT
default sets `reduce_range=True` — an FBGEMM workaround for accumulator
overflow on AVX2 without VNNI. Reading that config also emits its own notice:

```text
UserWarning: Please use quant_min and quant_max to specify the range for observers.
             reduce_range will be deprecated in a future release of PyTorch.
```

Nothing downstream honours the 7-bit ceiling. The QAT phase only conditions
weights; `onnxruntime.quantization.quantize_static` bakes the activation
ranges afterwards and quantizes `QUInt8` over the full 0 … 255. So the widened
range narrows the recipe mismatch ADR-0207 §2 exists to avoid.

## Does the weight transfer survive graph capture?

The risk was that `torch.export` renames parameters and
`_copy_qat_weights_into_fp32` — which matches state-dict keys by name and
shape — transfers nothing, tripping its own `RuntimeError("0 tensors copied")`.
Probed against a real `LearnedFilter` before any edit:

```text
fp32 state_dict keys : entry.weight, entry.bias, body.0.block.0.weight, ...
pt2e state_dict keys : entry.weight, entry.bias, activation_post_process_1.scale, ...
name+shape matches   : 20 / 20
forward OK (1, 1, 32, 32)
```

The captured graph keeps the original parameter names and adds observer
buffers alongside. The matcher is unaffected.

## Two things the API change forced

1. `torch.export.export_for_training` **does not exist in torch 2.14** — it
   folded back into `torch.export.export`. Guides written against 2.5–2.9 still
   name it; the first probe failed with `AttributeError: module 'torch.export'
   has no attribute 'export_for_training'`.
2. An exported graph module refuses mode switches:

   ```text
   NotImplementedError: Calling train() or eval() is not supported for exported models.
   Please call `torchao.quantization.pt2e.move_exported_model_to_train(model)` (or eval) instead.
   ```

   `_qat_fine_tune` is called with a raw Lightning module for the fp32
   warm-start and with the prepared graph for the QAT phase, so `_set_mode()`
   dispatches on `isinstance(module, torch.fx.GraphModule)`: `True` for the
   capture, `False` for `LearnedFilter`.

## The exporter, unmasked

With the pt2e failure gone, `test_qat_run_smoke` failed on the next warning in
the same call chain:

```text
DeprecationWarning: You are using the legacy TorchScript-based ONNX export.
Starting in PyTorch 2.9, the new torch.export-based ONNX exporter has become the default.
```

`_export_fp32_onnx` pinned `dynamo=False` against "quantization-related
intermediate buffers". That justification does not apply: phase 4 exports a
*fresh* `model_factory()` module carrying transferred weights, with no
observers. Dropping the pin and translating `dynamic_axes` into positional
`dynamic_shapes` ordered by `input_names` clears it — the same translation
`ef16e5760` applied to the other export sites.

## Result

```text
ai/tests/test_qat_smoke.py            2 passed in 5.04s
ai/tests                              1303 passed, 1 skipped in 28.69s
```

The skip is `test_e2e_frame_to_score.py`, which needs a built `vmaf` binary
that this workstation's tree does not have at `core/build-cpu/tools/vmaf`; the
`Tiny AI` job builds one.

## Not measured here

No model was retrained. ADR-0208's QAT-vs-static delta for `learned_filter_v1`
was measured under the old recipe and is not re-measured by this change,
because no CI job retrains and the registry's `.int8.onnx` files are untouched.
The next `qat_train.py` run is what will show the new recipe's effect, against
the same `ai-quant-accuracy` PLCC budget.
