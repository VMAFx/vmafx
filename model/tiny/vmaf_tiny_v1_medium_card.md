# `vmaf_tiny_v1_medium` — model card (legacy baseline)

> **Status**: Legacy — superseded by `vmaf_tiny_v3` (`mlp_medium`) as the
> medium-capacity shipped model. Retained for LOSO-eval baselines.

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_tiny_v1_medium` |
| File | `model/tiny/vmaf_tiny_v1_medium.onnx` |
| Architecture | `mlp_medium` sibling of `vmaf_tiny_v1` |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Legacy / retained for LOSO-eval baselines |

## Training data

Netflix Public Dataset (9 sources, fork-local extract); teacher label is
`vmaf_v0.6.1` per-frame score. Same corpus and recipe as `vmaf_tiny_v1` but
with a wider hidden layer.

## Eval metrics

Single-split validation on Netflix Public Dataset. Per-fold LOSO numbers are
documented in `docs/ai/loso-eval.md`.

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Resolution**: any (feature-based)
- **Input**: canonical-6 libvmaf features — `adm2`, `vif_scale0..3`, `motion2`
- **Output**: `vmaf` scalar in `[0, 100]`

## Known limits

- Superseded by `vmaf_tiny_v3` in both accuracy and the StandardScaler-baked
  graph contract.
- Retained only for LOSO-eval baseline comparison; do not use in production
  scoring pipelines.

## License + lineage

BSD-3-Clause-Plus-Patent. Trained on the Netflix Public Dataset (local
extract; not redistributed). See `registry.json` entry `vmaf_tiny_v1_medium`.

## See also

- [`vmaf_tiny_v3.md`](../../docs/ai/models/vmaf_tiny_v3.md) — current medium-capacity default
- [`registry.json`](registry.json) — registry entry `vmaf_tiny_v1_medium`
