# Research 0127: Tiny-AI quantisation doc status

- **Date**: 2026-05-15
- **Workstream**: tiny-AI documentation hygiene
- **Tags**: tiny-ai, quantisation, docs, fork-local

## Question

Does `docs/ai/quantization.md` still describe the shipped tiny-AI
quantisation state accurately after the v3/v4 PTQ follow-ups landed?

## Findings

The central quantisation page already listed `learned_filter_v1` and
`nr_metric_v1` as dynamic-PTQ models, but its caveat section still said
that no model was currently quantised. That sentence is stale: the
registry contains non-fp32 rows for `learned_filter_v1`, `nr_metric_v1`,
`vmaf_tiny_v3`, and `vmaf_tiny_v4`, each with `quant_mode: "dynamic"`,
an `int8_sha256`, and a 0.01 PLCC budget.

The per-model cards for `vmaf_tiny_v3` and `vmaf_tiny_v4` already carry
the measured dynamic-PTQ drops and file sizes:

| Model | fp32 | int8 | Drop |
| --- | --- | --- | --- |
| `vmaf_tiny_v3` | 4 496 B | 4 267 B | 0.000120 |
| `vmaf_tiny_v4` | 14 046 B | 7 769 B | 0.000145 |

No registry row currently uses static PTQ or QAT, so the accurate
operator-facing caveat is "all shipped int8 sidecars are dynamic PTQ",
not "no model is currently quantised".

## Decision Matrix

| Option | Pros | Cons | Decision |
| --- | --- | --- | --- |
| Leave the page as-is | No churn | Direct contradiction with registry and model cards | Rejected |
| Update only the caveat | Removes the contradiction | Still omits the shipped v3/v4 sidecars from the summary table | Rejected |
| Update the table and caveat | Aligns the central page with the registry and model cards | Documentation-only PR | Accepted |

## Reproducer

```bash
rg -n '"quant_mode": "dynamic"|int8_sha256' model/tiny/registry.json
rg -n 'No model is currently quantised|vmaf_tiny_v3|vmaf_tiny_v4' docs/ai/quantization.md
```
