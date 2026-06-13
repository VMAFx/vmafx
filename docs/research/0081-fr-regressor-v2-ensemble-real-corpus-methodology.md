<!-- markdownlint-disable MD049 MD060 -->
# Research-0081: `fr_regressor_v2` ensemble — real-corpus retrain methodology

- **Status**: Active
- **Date**: 2026-05-05
- **ADR**: [ADR-0309](../adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
- **Related**: [Research-0075](0075-fr-regressor-v2-ensemble-prod-flip.md)
  (parent — gate theory + conformal calibration sketch),
  [Research-0067](0067-fr-regressor-v2-prod-loso.md)
  (deterministic LOSO baseline),
  [Research-0058](0058-fr-regressor-v2-feasibility.md)
  (codec-aware feasibility).

This digest answers three operational questions for the real-corpus
LOSO retrain ADR-0309 specifies:

1. **Is the locally available Netflix corpus large enough?**
2. **Does the 9-fold LOSO sizing carry over from the deterministic
   ADR-0291 run?**
3. **Which hyperparameters drive seed diversity (the load-bearing
   property for the `max - min ≤ 0.005` gate)?**

## 1. Corpus-size sufficiency

The corpus at `.workingdir2/netflix/` carries 9 reference clips and
70 distorted clips, sized at ~37 GB total. Per Research-0067 §Corpus
sizing:

- Per source, the deterministic ADR-0291 run derived 33,840 per-frame
  canonical-6 rows over the Phase A grid (9 sources × NVENC + QSV ×
  preset/CRF sweep). That corpus has saturated PLCC at 0.964 ±
  0.002 across re-runs (Research-0067 Table 2).
- The minimum source count for stable 9-fold LOSO is **8** — at 7,
  Research-0067 saw a 0.004 PLCC variance pop because one held-out
  source carried a content type underrepresented in the training
  fold. At 9 sources the variance dropped back to 0.001.

**Verdict**: the local corpus matches the corpus that produced the
0.964 deterministic baseline. No corpus-size shortfall risk for the
ensemble retrain.

## 2. LOSO fold sizing

The 9 Netflix sources baked into
`ai/scripts/train_fr_regressor_v2_ensemble_loso.py::NETFLIX_SOURCES`
match the order from `eval_loso_vmaf_tiny_v3.py` /
`eval_loso_vmaf_tiny_v5.py` so the fold-by-fold PLCC traces are
**directly comparable** between the deterministic ADR-0291 run and
the ensemble seeds.

Per-fold expected PLCC (from Research-0067 §LOSO baselines on the
deterministic v2):

| Held-out fold       | Mean PLCC | Notes                                                |
|---------------------|-----------|------------------------------------------------------|
| BigBuckBunny_25fps  | 0.962     | clean-source baseline                                |
| BirdsInCage_30fps   | 0.967     | high-motion natural                                  |
| CrowdRun_25fps      | 0.971     | dense crowd, hard for `motion2`                      |
| ElFuente1_30fps     | 0.954     | low-light + grain                                    |
| ElFuente2_30fps     | 0.961     | dim, complementary to fuente1                        |
| FoxBird_25fps       | 0.965     | static + small-motion mix                            |
| OldTownCross_25fps  | 0.969     | textured                                             |
| Seeking_25fps       | 0.952     | weakest fold; rapid camera motion                    |
| Tennis_24fps        | 0.974     | strongest fold; predictable motion                   |
| **Mean across folds** | **0.964** | — |

The **`Seeking_25fps` fold is the load-bearing weak point** — if
seed diversity causes one ensemble member to overfit a Seeking-like
content distribution, that seed's hold-out PLCC on Seeking will tank
and pull `min(PLCC_i)` below the spread bound. Watch this fold in
the per-seed JSON.

## 3. Seed-diversity hyperparameters

The ensemble's predictive variance is only useful if the seeds
**actually disagree on out-of-fold predictions**. The `max - min ≤
0.005` gate is tight enough that the *natural* seed-to-seed variance
from random init alone usually clears it; the failure mode is
*insufficient* diversity (all seeds collapse to the same near-mode
solution), not excess diversity.

| Knob                           | ADR-0303 default | Effect on diversity                                                                                  |
|--------------------------------|------------------|------------------------------------------------------------------------------------------------------|
| Random init RNG                | `torch.manual_seed(seed)` | Drives weight init; the dominant diversity source.                                          |
| Mini-batch shuffle order       | seeded per epoch | Secondary diversity source; matters when batch-size << corpus-size.                                  |
| Dropout-at-train               | None (deterministic) | The `FRRegressor` (ADR-0235) has no dropout; not a diversity knob here.                          |
| Weight-decay                   | 1e-5             | Regularisation; uniform across seeds. *Not* a diversity knob — varying it would invalidate the gate. |
| Learning rate                  | 5e-4             | Uniform; same rationale as weight-decay.                                                              |
| Epochs                         | 200              | Long enough that all seeds reach the local minimum; reducing it widens spread.                       |
| Bootstrap of training rows     | not applied      | Could be enabled per Lakshminarayanan 2017 §3.2 to raise diversity if 5 seeds collapse.              |

**Diagnostic for HOLD-on-spread cases**:

- **Spread `max - min > 0.005` with all PLCC ≥ 0.95**: seeds are
  stable individually but disagree per-fold. Most likely cause:
  one fold (almost always Seeking_25fps) has a seed-dependent
  optimum. Remediation: re-seed (cheap), or enable bootstrap to
  raise inter-seed diversity in a controlled way.
- **Spread `max - min > 0.005` with some PLCC < 0.95**: one or more
  seeds are individually under-fit. Remediation: increase epochs
  (200 → 400 — note: do **not** lower the per-seed gate threshold;
  per the [no-test-weakening rule](../../CLAUDE.md#12-hard-rules-for-every-session)
  fix the implementation, not the gate).
- **Spread `≤ 0.005` but mean `< 0.95`**: tight ensemble agreement
  on a poor fit. Most likely a corpus regression. Re-derive the
  Phase A canonical-6 features and confirm against the deterministic
  v2 baseline first — Research-0067 §Reproducer.

## Summary

The harness ADR-0309 ships requires no new corpus; the local 9-source
Netflix Public Dataset is sufficient. The 9-fold LOSO ordering and
the 200-epoch / 5e-4 / 1e-5 hyperparameters are inherited from
ADR-0291 / ADR-0303 and should be held uniform across seeds —
diversity comes from RNG seed alone, not hyperparameter spread.
Watch the `Seeking_25fps` fold for the dominant failure mode if the
gate's `max - min ≤ 0.005` ever fires.

## References

- Lakshminarayanan, B., Pritzel, A., Blundell, C. (2017). _Simple
  and Scalable Predictive Uncertainty Estimation using Deep
  Ensembles._ NIPS 2017.
- [Research-0067](0067-fr-regressor-v2-prod-loso.md) — deterministic
  v2 LOSO baseline (per-fold PLCC table inherited above).
- [Research-0075](0075-fr-regressor-v2-ensemble-prod-flip.md) —
  parent: ensemble theory + conformal calibration sketch + the
  `max - min ≤ 0.005` derivation.
- [ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md) —
  gate definition consumed by this digest.
- [ADR-0309](../adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
  — the harness ADR this digest accompanies.
