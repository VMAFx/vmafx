# `dists_sq_placeholder_v0` — model card (smoke placeholder)

> **Status**: Smoke placeholder — NOT a production DISTS checkpoint.
> Full operator-facing doc: [`docs/ai/models/dists_sq.md`](../../docs/ai/models/dists_sq.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `dists_sq_placeholder_v0` |
| File | `model/tiny/dists_sq.onnx` |
| Sidecar | `model/tiny/dists_sq.json` |
| Architecture | Synthetic mean-squared tensor distance over two ImageNet-normalised RGB NCHW inputs |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Smoke placeholder only — not trained on any corpus |

## Training data + provenance

No training. Synthetic implementation: computes mean-squared distance between
two input tensors. Stands in for a real DISTS (Ding et al. 2020) checkpoint
until upstream-derived weights can be integrated (tracked as T7-DISTS-followup,
ADR-0236). Real DISTS weights are not redistributed in this fork.

## Eval metrics

Not applicable — smoke placeholder only.

## Operating point

- **Inputs**: two float32 NCHW `[1, 3, H, W]` ImageNet-normalised RGB tensors
- **Output**: scalar perceptual distance

## Known limits

- Synthetic: output is mean-squared distance, not DISTS perceptual distance.
- Do not use for any production quality evaluation.

## License + lineage

BSD-3-Clause-Plus-Patent. See ADR-0236, `registry.json` entry `dists_sq_placeholder_v0`.

## See also

- [`docs/ai/models/dists_sq.md`](../../docs/ai/models/dists_sq.md)
- [`registry.json`](registry.json)
