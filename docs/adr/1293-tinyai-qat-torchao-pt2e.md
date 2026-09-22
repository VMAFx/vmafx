<!-- markdownlint-disable MD013 MD060 -->
# ADR-1293: Move tiny-AI QAT onto torchao's pt2e API

- **Status**: Proposed
- **Date**: 2026-09-22
- **Deciders**: VMAFx maintainers
- **Tags**: `ai`, `python`, `dependencies`, `numerical-correctness`

## Context

`ai/train/qat.py` inserts fake-quant observers with
`torch.ao.quantization.quantize_fx.prepare_qat_fx` under
`get_default_qat_qconfig_mapping("x86")`, the recipe ADR-0207 §2 pinned.

PyTorch deprecated `torch.ao.quantization` wholesale. On torch 2.14 the call
raises, verbatim:

```text
DeprecationWarning: torch.ao.quantization is deprecated and will be removed in 2.10.
  2. FX graph mode quantization (prepare_fx, convert_fx), please migrate to use
     torchao pt2e quantization API instead (prepare_pt2e, convert_pt2e)
```

`ai/pyproject.toml` sets `filterwarnings = ["error"]`, so this is a test
failure, not a log line: `Tiny AI` reports `1 failed, 1302 passed` and the one
failure is `test_qat_smoke`. There is no non-deprecated path inside torch —
the warning names torchao as the successor, and `ai/AGENTS.md` has carried
`torchao.quantization.pt2e` as the migration target since the hook was written.

The blocking question was never which API to use but whether the migration is
mechanical. pt2e replaces FX symbolic tracing with `torch.export`, and
`_copy_qat_weights_into_fp32` transfers QAT-conditioned weights by matching
state-dict keys by name and shape — a rename of every parameter would trip its
own `RuntimeError("0 tensors copied")` guard. Measured on `LearnedFilter`
before writing any of this: the captured graph keeps `entry.weight`,
`body.0.block.0.weight` and the rest verbatim, and the matcher transfers
**20 of 20** tensors.

## Decision

Depend on `torchao>=0.18.0,<1.0` and prepare through
`torch.export.export(module, example_inputs, strict=True).module()` followed by
`torchao.quantization.pt2e.quantize_pt2e.prepare_qat_pt2e`, with
`X86InductorQuantizer` carrying
`get_default_x86_inductor_quantization_config(is_qat=True)`.

Two consequential details:

- **Train/eval dispatch.** An exported graph module refuses `.train()` and
  `.eval()` and requires torchao's `move_exported_model_to_train` / `_to_eval`.
  `_qat_fine_tune` runs against both a raw Lightning module (fp32 warm-start)
  and the prepared graph (QAT phase), so `_set_mode()` dispatches on
  `isinstance(module, torch.fx.GraphModule)` rather than each call site
  guessing.
- **ONNX exporter.** Phase 4 exported with `dynamo=False`, justified by
  quantization buffers the TorchDynamo exporter choked on. That justification
  no longer holds — the export target is a *fresh* fp32 module carrying only
  transferred weights — and the legacy path now warns on its own. Phase 4 uses
  the `torch.export`-based exporter, translating the caller's `dynamic_axes`
  into positional `dynamic_shapes` ordered by `input_names`.

The two-step pipeline ADR-0207 made load-bearing is untouched: QAT conditions
weights, `onnxruntime.quantization.quantize_static` emits the QDQ graph.
`convert_pt2e` is not called, for the same reason `convert_fx` was not.

## What changes numerically

Weights are unchanged: `torch.int8`, `per_channel_symmetric`, `ch_axis=0`,
`quant_min=-128`, `quant_max=127` on both sides, measured from the two configs
directly.

Activations change: the old mapping produced `quint8` per-tensor affine over
**[0, 127]**, the pt2e config `uint8` per-tensor affine over **[0, 255]**. The
7-bit ceiling is `reduce_range=True`, an FBGEMM workaround for accumulator
overflow on pre-VNNI AVX2. Nothing downstream of the QAT phase honours it:
the activation ranges that reach a shipped model are baked by ORT
`quantize_static`, which quantizes `QUInt8` over the full range. So the
recipe that failed to match the ORT side was the old one, and this narrows the
gap ADR-0207 §2 asked to close rather than opening one.

No shipped artefact moves as a result. CI never retrains; `model/tiny/` entries
and the `ai-quant-accuracy` PLCC budgets are computed from committed ONNX
files, which this change does not touch. The first model whose numbers this can
move is the next one somebody trains with `qat_train.py`, and that run reports
its own PLCC against the same budget gate.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Add `torch.ao.quantization` to `filterwarnings` as an ignore | One line; no dependency | Silences a removal notice for an API that is going away, leaving the hook to break outright on a later torch rather than warn | The repository's warning policy is to fix causes; an ignore would hide the deadline instead of meeting it |
| Pin `torch<2.14` | Defers everything | Contradicts `ai/pyproject.toml`'s `torch>=2.14.0` floor and the rest of this branch's torch-2.14 migrations; the API is removed regardless | Buys time the branch has already spent |
| `torchao.quantization.qat` module-swap quantizers | Stays module-based, so no graph capture | The shipped recipes there are LLM-oriented (int4 weight / int8 dynamic activation); none matches per-channel int8 conv against an x86 target | Wrong recipe for a conv filter feeding ORT static-PTQ |
| Drop QAT and ship static PTQ only | Deletes the problem | QAT is the documented third quant tier and ADR-0208 measured its delta on `learned_filter_v1` | Removes a capability to avoid maintaining it |
| `prepare_qat_pt2e` + `X86InductorQuantizer` | Named by the deprecation text and by `ai/AGENTS.md`; measured to preserve parameter names so the existing weight transfer is untouched; brings the activation range into line with ORT | New direct dependency; graph capture replaces symbolic tracing, so `.train()` / `.eval()` need dispatching | Chosen |

## Consequences

- **Positive**: `Tiny AI` goes from `1 failed, 1302 passed` to `1303 passed,
  1 skipped` (the skip needs a built `vmaf` binary); the QAT hook stops
  depending on an API with a published removal; the fake-quant activation
  range now matches what ORT bakes.
- **Negative**: `torchao` becomes a direct `ai/` runtime dependency, and the
  prepared model is a `torch.fx.GraphModule`, so anything that later reaches
  for `.train()` / `.eval()` on it must go through `_set_mode()`.
- **Neutral / follow-ups**: a retrain of `learned_filter_v1` under the new
  recipe has not been run; ADR-0208's measured QAT-vs-static delta is from the
  old one. The number to re-measure when that retrain happens is the PLCC drop
  against the per-model budget, not the graph.

## References

- req: "fix all, gogo, why stop" and "And get it green, what do you mean by not
  relevant? Are we fixing or destroying" — per user direction, the remaining
  `Tiny AI` failure is fixed at the cause rather than filtered out.
- [ADR-0207](0207-tinyai-qat-design.md) — the QAT design and the §2 recipe.
- [ADR-0208](0208-learned-filter-v1-qat-impl.md) — the model whose delta was
  measured under the old recipe.
- [ADR-0129](0129-tinyai-ptq-quantization.md) — the ORT static-PTQ path the
  recipe is meant to match.
- `docs/research/tinyai-qat-pt2e-migration-2026-09-22.md` — the measurements.
