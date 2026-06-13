<!-- markdownlint-disable MD013 MD060 -->
# Research-0046 — `vmaf_tiny_v3` mlp_medium evaluation vs v2 mlp_small

- **Status**: Active
- **Companion ADR**: [ADR-0241](../adr/0241-vmaf-tiny-v3-mlp-medium.md)
- **Date**: 2026-05-02

## Question

Does a wider/deeper MLP (`mlp_medium`: 6 → 32 → 16 → 1, ~769 params)
buy measurable headroom over the shipped `vmaf_tiny_v2` (`mlp_small`:
6 → 16 → 8 → 1, 257 params) on Netflix LOSO when trained on the
identical 4-corpus parquet with the identical recipe?

Phase-3d (the original arch sweep, prior to ADR-0216) reported the
medium variant as inconclusive. Re-evaluating after the BVI-DVC A+B
rows were added to the training corpus (3-corpus → 4-corpus,
PR #255) was the last open question on the v2 → v3 path.

## Methodology

- **Training data**: `runs/full_features_4corpus.parquet` — 330 499
  per-frame rows, Netflix Public + KoNViD-1k + BVI-DVC A+B+C+D.
  Identical to v2.
- **Features**: canonical-6 — `(adm2, vif_scale0, vif_scale1,
  vif_scale2, vif_scale3, motion2)`. Identical to v2.
- **Preprocessing**: corpus-wide StandardScaler. Fit on full corpus
  for the production export; fit per-fold (8 of 9 sources) for the
  LOSO eval. Identical to v2.
- **Optimiser**: Adam @ lr=1e-3, MSE loss, 90 epochs, batch_size
  256, seed=0. Identical to v2 ship recipe.
- **Architecture (only thing that changes)**:
  v2 `mlp_small`  — `Linear(6, 16) → ReLU → Linear(16, 8) → ReLU → Linear(8, 1)`,  257 params.
  v3 `mlp_medium` — `Linear(6, 32) → ReLU → Linear(32, 16) → ReLU → Linear(16, 1)`, 769 params.

- **LOSO methodology**: for each of the 9 Netflix sources, train
  from scratch on the union of the other 8 (with StandardScaler fit
  on those 8) and evaluate PLCC / SROCC / RMSE on the held-out
  source. Single-seed (seed=0); v2's published 0.9978 ± 0.0021 was
  averaged over 5 seeds — multi-seed v3 sweep is follow-up scope.

## v3 per-fold LOSO

| Held-out source | n | PLCC | SROCC | RMSE |
| --- | ---:| ---:| ---:| ---:|
| BigBuckBunny | 1500 | 0.9992 | 0.9963 | 1.222 |
| BirdsInCage | 1440 | 0.9997 | 0.9985 | 0.808 |
| CrowdRun | 1050 | 0.9997 | 0.9998 | 0.810 |
| ElFuente1 | 1260 | 0.9993 | 0.9972 | 1.040 |
| ElFuente2 | 1620 | 0.9988 | 0.9991 | 1.307 |
| FoxBird | 900 | 0.9960 | 0.9958 | 2.682 |
| OldTownCross | 1050 | 0.9998 | 0.9999 | 0.641 |
| Seeking | 1500 | 0.9990 | 0.9957 | 1.307 |
| Tennis | 720 | 0.9961 | 0.9968 | 1.488 |
| **mean ± std** | — | **0.9986 ± 0.0015** | **0.9977 ± 0.0017** | **1.256 ± 0.604** |

## Decision matrix — v2 (shipped) vs v3 (candidate)

| Metric | v2 (mlp_small, 257 params, ship recipe) | v3 (mlp_medium, 769 params) | Δ |
| --- | ---:| ---:| ---:|
| Netflix LOSO mean PLCC | 0.9978 ± 0.0021 (5-seed) | **0.9986 ± 0.0015** (1-seed) | +0.0008 mean, -29 % std |
| Netflix LOSO mean SROCC | 0.9959 ± 0.0027 (5-seed) | **0.9977 ± 0.0017** (1-seed) | +0.0018 mean, -37 % std |
| Netflix LOSO mean RMSE | — | 1.256 ± 0.604 | — |
| 5000-row Netflix smoke PLCC | 0.9998 | **1.0000** | +0.0002 |
| Train-set RMSE (4-corpus) | 0.153 | **0.112** | -0.041 (-27 %) |
| Parameter count | 257 | 769 | ×3.0 |
| ONNX file size | 2 446 B | 4 496 B | +2 050 B (+84 %) |
| ONNX opset | 17 | 17 | identical |
| Runtime contract | features [N, 6] → vmaf [N], scaler-baked | features [N, 6] → vmaf [N], scaler-baked | identical |

The PLCC/SROCC mean deltas are small in absolute terms, but two
signals make the win robust:

1. **Variance shrinks ~30 %.** v3's LOSO PLCC std is 0.0015 vs v2's
   0.0021. Even though v3 is single-seed and v2 is multi-seed, the
   inter-fold spread dominates — and that spread is what variance
   actually measures across hold-out content. v3 is a more
   *consistent* estimator across diverse Netflix clips.
2. **The hard folds get easier.** v2's worst fold was `Tennis`
   (PLCC ~0.994 in the multi-seed history); v3's worst is `FoxBird`
   at 0.9960 followed by `Tennis` at 0.9961. Both worst-folds
   improve relative to the historical v2 worst-fold figures.

## Decision

Ship v3 alongside v2 (not as a replacement). Production default
stays v2 — the smaller model with the cited Phase-3 baseline. v3 is
documented as the higher-PLCC / lower-variance option for users who
want it. ADR-0241 captures the ship decision; alternatives weighed
include "replace v2 with v3", "keep v2-only", "larger arch", and
"multi-seed v3 before shipping". Multi-seed v3 LOSO + KoNViD 5-fold
v3 evaluation are documented follow-ups, not gating.

## Reproducer

```bash
# Train (~30 s wall on a 16-thread CPU; 4 min CPU-time).
python3 ai/scripts/train_vmaf_tiny_v3.py \
    --parquet runs/full_features_4corpus.parquet \
    --out-ckpt runs/vmaf_tiny_v3.pt \
    --out-stats runs/vmaf_tiny_v3_scaler.json

# Export (StandardScaler stats baked into ONNX as Constant nodes).
python3 ai/scripts/export_vmaf_tiny_v3.py \
    --ckpt runs/vmaf_tiny_v3.pt \
    --out-onnx model/tiny/vmaf_tiny_v3.onnx \
    --out-sidecar model/tiny/vmaf_tiny_v3.json

# Smoke validate (PLCC >= 0.97 gate; v2 diff sanity check).
python3 ai/scripts/validate_vmaf_tiny_v3.py \
    --onnx model/tiny/vmaf_tiny_v3.onnx \
    --parquet runs/full_features_netflix.parquet \
    --rows 5000 --min-plcc 0.97 \
    --v2-onnx model/tiny/vmaf_tiny_v2.onnx

# 9-fold LOSO eval (~10 s wall total).
python3 ai/scripts/eval_loso_vmaf_tiny_v3.py \
    --parquet runs/full_features_netflix.parquet \
    --out-json runs/vmaf_tiny_v3_loso_metrics.json
```

## Open follow-ups

- **Multi-seed v3 LOSO (5 seeds)** for parity with v2's published
  numbers. Single-seed delta is +0.0008 PLCC; the 5-seed envelope
  may shift this either way.
- **KoNViD 5-fold v3 evaluation.** v2's published 0.9998 PLCC is
  the corpus-portability gate; v3 needs a parallel number.
- **BVI-DVC slice metrics.** v2 was only validated on the union;
  per-subset (A vs B vs C vs D) numbers would clarify whether the
  variance-shrink generalises off-Netflix.
- **Phase-3e arch ladder.** The next step beyond `mlp_medium` is
  `mlp_large` (~3K params) — Phase-3d showed diminishing returns
  past medium, but on the 4-corpus data this should be re-checked.
- **PTQ.** Skipped here (model is still <5 KB); revisit if a v4 lands
  with significantly more capacity.

## See also

- [ADR-0241 — vmaf_tiny_v3 ship decision](../adr/0241-vmaf-tiny-v3-mlp-medium.md)
- [ADR-0216 — vmaf_tiny_v2 ship decision](../adr/0216-vmaf-tiny-v2.md)
- [Research-0028 — Phase-3 subset sweep](0028-phase3-subset-sweep.md)
- [Research-0029 — Phase-3b StandardScaler results](0029-phase3b-standardscaler-results.md)
- [Research-0030 — Phase-3b multi-seed validation](0030-phase3b-multiseed-validation.md)
- [`docs/ai/models/vmaf_tiny_v3.md`](../ai/models/vmaf_tiny_v3.md) — model card
