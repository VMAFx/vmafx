# `vmaf_tiny_v1` — model card (legacy baseline)

> **Status**: Legacy — superseded by `vmaf_tiny_v2` as the default tiny FR
> fusion model. Retained for LOSO-eval baselines and quantisation regression
> tests. Full operator-facing doc: none (legacy; v2+ are the reference
> documentation).

## Identity

| Field | Value |
| --- | --- |
| Model id | `vmaf_tiny_v1` |
| File | `model/tiny/vmaf_tiny_v1.onnx` |
| Architecture | `mlp_small` — Linear(6, 16) → ReLU → Linear(16, 8) → ReLU → Linear(8, 1), ~144 params |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Legacy / retained for regression baselines |

## Training data

Netflix Public Dataset (9 sources, fork-local extract); teacher label is
`vmaf_v0.6.1` per-frame score.

## Hyperparameters

Single-split training. Architecture: `mlp_small` (6 → 16 → 8 → 1). See
`ai/scripts/train_vmaf_tiny.py` for the full recipe.

## Eval metrics

Single-split validation on Netflix Public Dataset. Per-fold LOSO numbers are
documented in `docs/ai/models/` alongside v2–v4 (see `vmaf_tiny_v2.md`
§Training data for the shared LOSO methodology). Ship gate: PLCC ≥ 0.95.

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Resolution**: any (feature-based, not pixel-based)
- **Bit depth**: 8 bpc and 10 bpc (features are bit-depth agnostic)
- **Input**: canonical-6 libvmaf features — `adm2`, `vif_scale0..3`, `motion2`
- **Output**: `vmaf` scalar in `[0, 100]`

## Known limits

- Superseded by `vmaf_tiny_v2` in accuracy (+0.005–0.018 PLCC).
- No StandardScaler baked into the graph — callers must normalise inputs
  externally (or use v2 / v3 / v4 which bake the scaler).
- Retained only for regression baseline and quantisation test purposes; do not
  use in production scoring pipelines.

## License + lineage

BSD-3-Clause-Plus-Patent. Trained on the Netflix Public Dataset (local
extract; not redistributed). Derived checkpoint only. See `registry.json` for
SHA-256 and Sigstore bundle reference.

## See also

- [`vmaf_tiny_v2.md`](../../docs/ai/models/vmaf_tiny_v2.md) — current default
- [`registry.json`](registry.json) — registry entry `vmaf_tiny_v1`
