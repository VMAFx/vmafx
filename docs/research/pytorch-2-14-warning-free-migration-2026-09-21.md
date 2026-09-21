<!-- markdownlint-disable MD013 MD060 -->
# PyTorch 2.14 warning-free migration research — 2026-09-21

## Question

How can the complete AI and vmaf-tune package suites run with warnings as errors
on the release dependency set without filtering diagnostics or removing QAT?

## Reproduction

The exact source image was built from commit `981ef5df1` and reported:

```text
torch 2.14.0+cu130
torchvision 0.29.0+cu130
vmaf-train 3.2.1
No broken requirements found.
```

The complete AI suite reached 1,285 passes and one environmental skip, then
failed on 18 remaining cases. The warning failures grouped into four causes:

1. deprecated `torch.ao.quantization.quantize_fx.prepare_qat_fx`;
2. legacy or mixed-contract ONNX export (`dynamo=False`, or `dynamic_axes` with
   the dynamo exporter);
3. Lightning `self.log()` calls on models intentionally exercised without an
   attached Trainer;
4. NumPy `nanmean` / `nanstd` on all-NaN columns.

The same run also found stale test seams: package-qualified imports were patched
through their old top-level names, per-shot subprocess doubles treated the
literal output format `json` as a path, and tests that intentionally load a
synthetic predictor warning did not isolate the ONNX session.

## QAT migration probe

The already-recorded migration target was replayed against the exact dependency
set:

- torchao 0.17.0 fails to import on Python 3.14 while assigning metadata to a
  `typing.Union`.
- torchao 0.18.0 imports on Python 3.14 and PyTorch 2.14.
- `torch.export.export(model, example_inputs).module()` followed by
  `prepare_qat_pt2e(..., X86InductorQuantizer)` prepares the shipped
  `LearnedFilter` successfully.
- The prepared graph preserves all twelve original parameter keys, including
  `entry.weight`, `body.0.block.0.weight`, and `exit.weight`. The existing
  key-plus-shape copy and zero-copy guard therefore remain valid.
- The high-level torchao `QATConfig` currently targets linear and embedding
  modules, so it does not cover the convolutional learned-filter contract.

## ONNX migration probe

PyTorch 2.14's installed `torch.onnx.export` signature defaults `dynamo=True`
and documents `dynamic_shapes` as the matching shape contract. For positional
tensor inputs, one shared `torch.export.Dim("batch")` object preserves equal
batch dimensions across inputs. Output dynamism is inferred from the exported
graph. Spatially dynamic models use independent height and width `Dim` objects.

The installed exporter has native implementations at opset 18. Requesting opset
17 directly does not fail, but emits a warning while performing its own automatic
conversion. The warning-free path therefore exports a private, self-contained
opset-18 graph, loads that graph with ONNX, explicitly runs
`onnx.version_converter.convert_version(..., 17)`, and atomically publishes the
converted graph. This retains the deployed opset-17 registry contract without
falling back to the deprecated TorchScript exporter or hiding PyTorch's warning.

The migration must validate the serialized model with ONNX Runtime because an
export call returning successfully is not evidence that input names, dynamic
dimensions, or the libvmaf operator allowlist stayed compatible.

## Decision evidence

| Requirement | Evidence |
|---|---|
| No warning filter | Package configs retain `filterwarnings = ["error"]`; fixes remove emitters or explicitly assert a public warning contract |
| Keep QAT | PT2E prepares the existing convolutional model and retains the ORT static-QDQ phase |
| Keep state transfer | Original parameter keys survive export and PT2E preparation |
| Keep opset 17 | Native opset-18 export followed by explicit ONNX conversion produces the final self-contained opset-17 artifact |
| Current dependency set | Exact container carries PyTorch 2.14.0, torchvision 0.29.0, and Python 3.14 |
| Fail closed | Container build ends in `pip check`; package suites and strict whole-scope mypy are required before integration |

## Primary sources

- [torchao PT2E QAT tutorial](https://docs.pytorch.org/ao/stable/pt2e_quantization/pt2e_quant_qat.html)
- [torchao QAT API](https://docs.pytorch.org/ao/stable/api_reference/api_ref_qat.html)
- [PyTorch `torch.export` documentation](https://docs.pytorch.org/docs/stable/export.html)
- [torchao upstream repository](https://github.com/pytorch/ao)
