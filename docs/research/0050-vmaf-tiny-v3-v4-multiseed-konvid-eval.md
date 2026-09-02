<!-- markdownlint-disable MD013 -->
# Research-0050: vmaf_tiny v3 + v4 — multi-seed Netflix LOSO + KoNViD 5-fold

- **Date**: 2026-05-02
- **Authors**: Lusoris, Claude (Anthropic)
- **Status**: Final (all four runs complete)
- **Tags**: ai, training, evaluation
- **Related**: ADR-0241 (vmaf_tiny_v3), ADR-0242 (vmaf_tiny_v4), Research-0027/0028/0029/0030 (Phase-3 chain), Research-0048 (v4 single-seed report)

## Goal

Both [PR #294](https://github.com/VMAFx/vmafx/pull/294) (v3 mlp_medium) and
[PR #299](https://github.com/VMAFx/vmafx/pull/299) (v4 mlp_large) shipped with
**single-seed (seed=0)** Netflix 9-fold LOSO numbers. v2's published 0.9978 ±
0.0021 baseline is a 5-seed average. To compare apples-to-apples, this digest
re-runs v3 + v4 with seeds 0..4 and averages.

## Methodology

- Recipe identical to single-seed runs: Adam @ lr=1e-3, MSE, 90 epochs,
  batch_size 256, StandardScaler fit on the FULL training corpus per-seed,
  baked into the (intermediate, not shipped) ONNX.
- Netflix LOSO: 9 folds (one source held out per fold), 5 seeds → 45 trainings
  per architecture. PLCC/SROCC/RMSE per (fold, seed); aggregate mean ± std
  across seeds and across folds.
- KoNViD 5-fold: 5 pre-computed folds in `runs/full_features_konvid_with_folds.parquet`,
  5 seeds each → 25 trainings per architecture.

## Results (all 4 runs complete)

Netflix 9-fold LOSO, 5-seed average:

| Architecture | LOSO PLCC mean | LOSO PLCC std | LOSO SROCC | LOSO RMSE |
| --- | --- | --- | --- | --- |
| **v2 mlp_small (shipped)** | **0.9978** | **0.0021** (pub.) | 0.997x | — |
| **v3 mlp_medium** (PR #294) | **0.99867** | **0.00015** | 0.99747 | 1.315 ± 0.089 |
| **v4 mlp_large** (PR #299) | **0.99892** | **0.00018** | 0.99745 | 1.159 ± 0.066 |

KoNViD 5-fold, 5-seed average:

| Architecture | KoNViD PLCC mean | KoNViD PLCC std | KoNViD SROCC | KoNViD RMSE |
| --- | --- | --- | --- | --- |
| v2 mlp_small (shipped) | 0.9998 (pub.) | — | — | — |
| v3 mlp_medium (PR #294) | **0.99994** | **1.3e-5** | 0.99997 | 0.095 ± 0.011 |
| v4 mlp_large (PR #299) | **0.99996** | **1.4e-5** | 0.99998 | 0.088 ± 0.018 |

JSON outputs (under gitignored `runs/`):

- `runs/vmaf_tiny_v3_loso_5seed.json`
- `runs/vmaf_tiny_v4_loso_5seed.json`
- `runs/vmaf_tiny_v3_konvid_5fold.json`
- `runs/vmaf_tiny_v4_konvid_5fold.json`

## Key findings

1. **Multi-seed confirms the single-seed direction.** Both v3 and v4 beat v2
   on PLCC mean. Single-seed v3 was reported at 0.9986 ± 0.0015 (per PR #294);
   the 5-seed std collapses to 0.00015 — about **14× tighter**. Same shape
   for v4 (single-seed std 0.0015 → 5-seed std 0.00018, **8× tighter**). The
   single-seed std is dominated by training noise, not architecture variance.

2. **v4 over v3 is real but small.** Δ-mean = +0.00025 PLCC; both architectures'
   stds are ≈ 0.0002, so v4's mean is ~1.4σ above v3. Statistically distinguishable
   over 45 trainings each, but operationally negligible — v4's RMSE improvement
   (1.159 vs 1.315, **−12%**) is the more useful signal than the PLCC delta.

3. **KoNViD is saturated.** v3 hits 0.99994 ± 1.3e-5 — five-9s territory. v2's
   published 0.9998 was already near the ceiling; the architecture ladder does
   not buy meaningful headroom on KoNViD. The Netflix LOSO is the discriminating
   bench going forward.

4. **No recommendation change.** Per PR #294 and PR #299:
   - v2 stays the **production default** (smallest bundle, cited Phase-3 baseline).
   - v3 is the **recommended opt-in higher tier** (3× v2's params, +0.0009 PLCC).
   - v4 is **opt-in only for users who want lowest RMSE**; the +0.00025 PLCC
     over v3 is below the user-perceptible threshold.
   - **Architecture ladder stops at v4** (per ADR-0242). Future quality gains
     need feature-set or corpus regime change, not deeper MLPs.

## Deferred

None. All four corpus × architecture cells are filled.

## Reproducer

```bash
python3 ai/scripts/eval_multiseed_v3_v4.py \
  --arch mlp_medium --corpus netflix --seeds 0,1,2,3,4 \
  --output runs/vmaf_tiny_v3_loso_5seed.json
python3 ai/scripts/eval_multiseed_v3_v4.py \
  --arch mlp_large --corpus netflix --seeds 0,1,2,3,4 \
  --output runs/vmaf_tiny_v4_loso_5seed.json
python3 ai/scripts/eval_multiseed_v3_v4.py \
  --arch mlp_medium --corpus konvid --seeds 0,1,2,3,4 \
  --output runs/vmaf_tiny_v3_konvid_5fold.json
# v4 KoNViD: same with --arch mlp_large
```

Per-arch wall-clock on 16-thread CPU: ~5 min for Netflix LOSO 5-seed, ~3 min
for KoNViD 5-fold 5-seed.

## References

- PR #294 — v3 mlp_medium single-seed PLCC=0.9986 ± 0.0015 (`feat/vmaf-tiny-v3-mlp-medium`)
- PR #299 — v4 mlp_large single-seed PLCC=0.9987 ± 0.0015 (`feat/vmaf-tiny-v4-mlp-large`)
- ADR-0241 — vmaf_tiny_v3 architecture decision
- ADR-0242 — vmaf_tiny_v4 + arch-ladder-stops-here
- Research-0027/0028/0029/0030 — Phase-3 chain (v2 baseline establishment)
- Research-0048 — v4 single-seed report
