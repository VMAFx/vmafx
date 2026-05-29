# `fr_regressor_v1` — model card

> Full operator-facing doc: [`docs/ai/models/fr_regressor_v1.md`](../../docs/ai/models/fr_regressor_v1.md)

## Identity

| Field | Value |
| --- | --- |
| Model id | `fr_regressor_v1` |
| File | `model/tiny/fr_regressor_v1.onnx` |
| Sidecar | `model/tiny/fr_regressor_v1.json` |
| Architecture | `FRRegressor` — 2-layer GELU MLP, hidden=64, dropout=0.1 |
| ONNX opset | 17 |
| License | BSD-3-Clause-Plus-Patent |
| Status | Production baseline (C1); refreshed 2026-05-20 (ADR-0647) |

## Training data + provenance

Netflix Public Dataset refresh parquet
(`runs/full_features_netflix_refresh_20260520.parquet`, 11 190 rows). Teacher
label: `vmaf_v0.6.1` per-frame score. 9 reference sources, 70 distorted YUVs
(local-only; not redistributed).

## Hyperparameters

2-layer GELU MLP, hidden dim 64, dropout 0.1. 9-fold LOSO training. Feature
standardisation stats baked into the sidecar `feature_mean` / `feature_std`
fields (callers must normalise inputs externally — not baked into the graph).

## Eval metrics

| Holdout | PLCC | SROCC | RMSE |
| --- | ---: | ---: | ---: |
| 9-fold LOSO mean | 0.9982 | 0.9567 | 2.194 |
| 9-fold LOSO std | 0.0014 | — | — |
| In-sample | 0.9993 | — | — |

Detailed per-source breakdown in `docs/ai/models/fr_regressor_v1.md` §2026-05-20 refresh metrics.

## Operating point

- **Backend**: CPU / CUDA / SYCL (ONNX Runtime EP)
- **Resolution**: any (feature-based)
- **Bit depth**: 8 bpc and 10 bpc
- **Input**: `features` — float32 `[N, 6]` (caller must apply sidecar mean/std)
- **Output**: `score` — float32 `[N]`, VMAF scale `[0, 100]`

## Known limits

- Feature standardisation not baked into the graph; callers must normalise.
- Codec-blind (canonical-6 only). Use `fr_regressor_v3` for codec-conditioned predictions.

## License + lineage

BSD-3-Clause-Plus-Patent. Netflix Public Dataset (local extract, not
redistributed). See ADR-0249, ADR-0647, `registry.json` entry `fr_regressor_v1`.

## See also

- [`docs/ai/models/fr_regressor_v1.md`](../../docs/ai/models/fr_regressor_v1.md) — full doc
- [`registry.json`](registry.json)
