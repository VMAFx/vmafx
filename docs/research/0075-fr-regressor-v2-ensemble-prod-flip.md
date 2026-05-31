<!-- markdownlint-disable MD049 -->
# Research-0075: `fr_regressor_v2` ensemble — production flip LOSO protocol

- **Date**: 2026-05-05
- **Authors**: Lusoris, Claude (Anthropic)
- **Status**: Final (scaffold-time digest — closes the literature loop
  before the trainer + CI gate ship)
- **Tags**: ai, fr-regressor, ensemble, probabilistic, loso, conformal
- **Companion ADR**: [ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md)
- **Predecessors**: [Research-0067 (probabilistic)](0067-fr-regressor-v2-probabilistic.md),
  [Research-0067 (prod-loso)](0067-fr-regressor-v2-prod-loso.md)

## Question

What ship gate does the deep ensemble for `fr_regressor_v2` need to
clear before the five smoke-seed registry rows (PR #372) flip from
`smoke: true` to `smoke: false`, and what is the minimum-cost LOSO
protocol that exercises that gate on the existing Phase A corpus?

## Method

The literature loop closes around three reference points:

1. **Lakshminarayanan, Pritzel, Blundell (2017) — _Simple and Scalable
   Predictive Uncertainty Estimation using Deep Ensembles._ NIPS 2017.**
   Core finding: independently-seeded networks with proper scoring rule
   training (NLL or MSE for regression) dominate single-network
   uncertainty estimators on calibration quality. The empirical
   recommendation is **N=5 ensemble members** — N<5 is unreliable, N>5
   is diminishing returns on calibration quality at linear inference
   cost. The Lakshminarayanan calibration plots show that on regression
   benchmarks (UCI, year-prediction, video MOS), 5-member ensembles
   reach within 1–2 % of the asymptotic calibration of 100-member
   ensembles.

2. **Romano, Patterson, Candès (2019) — _Conformalized Quantile
   Regression._ NeurIPS 2019.** The conformal correction we apply on
   top of the ensemble's empirical CDF gives a marginal coverage
   guarantee `P(y ∈ [q_lo, q_hi]) ≥ 1 - α` independent of the base
   model's calibration. This is the "free" piece — it adds no
   training-time cost, only a calibration-fold pass. **Out of scope for
   this PR** (the conformal layer is in the probabilistic-head backlog
   under ADR-0279); we record the protocol here so the eventual
   prod-flip can plug it in.

3. **Foong et al. (2019) — _On the Expressiveness of Approximate
   Inference in Bayesian Neural Networks._** Demonstrates empirically
   that MC-dropout under-estimates predictive variance on regression
   tasks where the residual error is content-dependent (which is
   exactly our setting — VMAF residuals correlate with content
   complexity). This is the negative result that pushes us to deep
   ensembles instead of MC-dropout.

## Conformal calibration sketch (deferred)

The eventual probabilistic-head flip will layer split-conformal on top
of the ensemble's predictive CDF:

1. Hold out a calibration split (~10 % of training rows, stratified by
   source × encoder).
2. Train the 5-seed ensemble on the remaining 90 %.
3. For each calibration row, compute the conformity score
   `r_i = |y_i - μ_ensemble_i| / σ_ensemble_i` (normalised residual).
4. The 95 % conformal interval at inference is
   `[μ - q̂_{0.95}(r) · σ, μ + q̂_{0.95}(r) · σ]` where `q̂_{0.95}` is
   the empirical 95-percentile of `r` over the calibration split.

This sketch is recorded for the follow-up; the trainer in this PR
emits enough metadata (per-seed predictions on each fold) for the
conformal layer to consume without re-running training.

## 9-fold LOSO protocol — Netflix Public Dataset

The LOSO eval runs nine folds, one per Netflix source:

```text
sources = [
    "BigBuckBunny_25fps", "BirdsInCage_30fps",  "CrowdRun_25fps",
    "ElFuente1_30fps",    "ElFuente2_30fps",    "FoxBird_25fps",
    "OldTownCross_25fps", "Seeking_25fps",      "Tennis_24fps",
]
```

Per fold:

- **Held-out**: all rows where `source == held_out`.
- **Train**: all other rows in `runs/phase_a/full_grid/per_frame_canonical6.jsonl`
  (the corpus from PR #392 — 33,840 per-frame canonical-6 rows × 9
  Netflix sources × NVENC + QSV codec families).
- **Per-seed**: train `FRRegressor(in_features=6, num_codecs=12)` for
  200 epochs (Adam, lr=5e-4, batch=32, weight_decay=1e-5) under five
  seeds {0, 1, 2, 3, 4}. StandardScaler fit on the training fold,
  applied to the held-out fold.
- **Eval**: PLCC, SROCC, RMSE per fold per seed.
- **Aggregate**: mean PLCC across folds → `PLCC_i` for seed `i`.

The trainer emits one JSON per seed (`loso_seed{N}.json`) with the
schema:

```json
{
  "seed": 0,
  "corpus": "runs/phase_a/full_grid/per_frame_canonical6.jsonl",
  "n_folds": 9,
  "folds": [
    {"held_out": "BigBuckBunny_25fps", "plcc": 0.9712, "srocc": 0.99, "rmse": 3.7, "n_train": 30000, "n_val": 3840},
    ...
  ],
  "mean_plcc": 0.9683,
  "std_plcc": 0.0211,
  "wall_time_s": 28.4
}
```

The CI gate (`scripts/ci/ensemble_prod_gate.py`) reads the five JSONs
and decides:

```python
seeds = [load(f"loso_seed{i}.json") for i in range(5)]
mean_plccs = [s["mean_plcc"] for s in seeds]
gate_pass = (
    sum(mean_plccs) / 5 >= 0.95             # mean ship gate
    and max(mean_plccs) - min(mean_plccs) <= 0.005  # variance bound
)
```

## Expected PLCC baseline

ADR-0291 / Research-0067 (prod-loso) reported deterministic v2 LOSO
PLCC = **0.9681 ± 0.0207** on the same corpus. The expected ensemble
behaviour:

- **Mean per-seed PLCC ≥ 0.99** *baseline aspiration* — averaging
  five independent trainings on the same data should slightly improve
  on the single-network 0.9681 (typical ensemble lift on UCI-style
  regression: +0.005–0.02 PLCC). Calling 0.99 a baseline is
  optimistic for the OldTownCross outlier fold but realistic for the
  other eight; the **gate stays at the conservative 0.95** so we
  don't accidentally bake in a baseline the corpus can't sustain.
- **Per-seed std ≤ 0.005** — reseed-to-reseed variation on the same
  training corpus + protocol typically lands at ~0.002–0.004 PLCC for
  small MLPs. The 0.005 variance bound is calibrated against this
  expected envelope plus a margin.

## Reproducer (smoke-only — no real corpus on this branch)

```bash
# Smoke-only: argparse + scaffold smoke (no real training).
python ai/scripts/train_fr_regressor_v2_ensemble_loso.py --help

# When the real corpus is present (follow-up PR):
python ai/scripts/train_fr_regressor_v2_ensemble_loso.py \
    --seeds 0,1,2,3,4 \
    --corpus runs/phase_a/full_grid/per_frame_canonical6.jsonl \
    --out-dir runs/ensemble_loso/

# Then run the gate:
python scripts/ci/ensemble_prod_gate.py runs/ensemble_loso/
```

## Caveats / known limitations

- **OldTownCross fold remains the calibration weak point** — single-seed
  v2 reported PLCC=0.9183 on this fold (Research-0067 prod-loso). The
  ensemble may not lift this fold's PLCC above 0.95 because the
  underlying VMAF range on OldTownCross encodes is too compressed
  (most cells score 96–99). The mean-across-folds gate is the
  primary protection; we accept that one fold may stay sub-gate.
- **No SW encoders in corpus** — vmaf-tune Phase A used NVENC + QSV
  hardware encoders (12-encoder vocab v2). The eventual ensemble
  flip inherits this corpus restriction; a separate retrain on a
  software-encoder sweep (T-FR-V2-SW-CORPUS) is tracked but blocks
  neither this scaffold nor the prod flip.
- **Conformal layer not exercised here** — Research-0075's main
  contribution is the ensemble training + gate; the conformal CDF
  pass is sketched for the probabilistic-head follow-up only.

## References

- Lakshminarayanan, B., Pritzel, A., Blundell, C. (2017). _Simple
  and Scalable Predictive Uncertainty Estimation using Deep
  Ensembles._ NIPS 2017.
- Romano, Y., Patterson, E., Candès, E. (2019). _Conformalized
  Quantile Regression._ NeurIPS 2019.
- Foong, A., Burt, D., Li, Y., Turner, R. (2019). _On the
  Expressiveness of Approximate Inference in Bayesian Neural
  Networks._ NeurIPS 2019.
- [ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md) —
  ensemble prod-flip trainer + CI gate decision (this digest).
- [ADR-0291](../adr/0291-fr-regressor-v2-prod-ship.md) —
  deterministic v2 prod flip + 0.95 ship gate.
- [ADR-0279](../adr/0279-fr-regressor-v2-probabilistic.md) —
  probabilistic head + ensemble scaffold (PR #372).
- [Research-0067 (prod-loso)](0067-fr-regressor-v2-prod-loso.md) —
  deterministic v2 LOSO baseline (mean PLCC=0.9681 ± 0.0207).
- [Research-0067 (probabilistic)](0067-fr-regressor-v2-probabilistic.md)
  — ensemble vs MC-dropout vs SWAG calibration shootout.
