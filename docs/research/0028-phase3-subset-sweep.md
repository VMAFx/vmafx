<!-- markdownlint-disable MD049 MD060 -->
# Research-0028 — Phase-3 MLP arch sweep on Top-K feature subsets

_Updated: 2026-04-29._

## Question

[Research-0027 §"Decision"](0027-phase2-feature-importance.md) ratified
Phase-3 GO and named three feature subsets to sweep:

- **Subset A** — canonical-6 ∪ {`ssimulacra2`} (7 features).
- **Subset B** — consensus-7 (`adm2`, `adm_scale3`, `vif_scale2`,
  `motion2`, `ssimulacra2`, `psnr_hvs`, `float_ssim`).
- **Subset C** — full-21 (sanity ceiling).

The stopping rule was: *"If Subset A's mean LOSO PLCC fails to beat
canonical6 by ≥ 0.005, the hypothesis is dead and canonical-6 stays."*

This digest reports the empirical result.

## Method

- Same parquet as Research-0027 (`runs/full_features_netflix.parquet`,
  11 040 frames × 21 features).
- `mlp_small` (Linear(N→16) → ReLU → Linear(16→8) → ReLU →
  Linear(8→1)) trained for **30 epochs**, Adam `lr=1e-3`, batch 256,
  `seed=0` per fold.
- 9-fold leave-one-source-out across the Netflix Public sources.
- Per-fold metrics: PLCC, SROCC, RMSE; aggregated as mean ± std
  across folds.
- Driver: [`ai/scripts/phase3_subset_sweep.py`](../../ai/scripts/phase3_subset_sweep.py).
- **Features fed to the model raw — no standardisation, no
  normalisation.** This is load-bearing for interpreting the
  result; see §"Why this is surprising" below.

## Results

| Subset      | Features |   Mean PLCC |   Mean SROCC |    Mean RMSE | Δ PLCC vs canonical6 |
|-------------|---------:|------------:|-------------:|-------------:|---------------------:|
| canonical6  |        6 | **0.9845**  | **0.9895**   |     15.201   |                  —   |
| A           |        7 |   0.9655    |   0.9740     |    **9.129** |             −0.0190  |
| B           |        7 |   0.9697    |   0.9825     |    **8.910** |             −0.0147  |
| C           |       21 |   0.9779    |   0.9809     |    **8.498** |             −0.0066  |

Per-fold detail in
[`runs/phase3_subset_sweep.json`](../../runs/phase3_subset_sweep.json)
(gitignored; reproducible from canonical command in §"Reproducer").

## Headline finding

**All three subsets LOSE on PLCC. All three subsets WIN big on RMSE.**

The PLCC stopping rule from Research-0027 fires: Subset A is 0.019
*below* canonical6, not 0.005 above. By the documented criterion,
the hypothesis "broader feature set helps a tiny MLP" is killed.

But every subset cuts RMSE by **~40 %**:

- canonical6 RMSE 15.20 → A 9.13 (−40 %)
- canonical6 RMSE 15.20 → B 8.91 (−41 %)
- canonical6 RMSE 15.20 → C 8.50 (−44 %)

That's a *huge* absolute-fit improvement. Mean RMSE is in raw
`vmaf_v0.6.1` score units (0–100 range), so canonical6 is averaging
~15-point errors per frame; the broader subsets average ~9 points.
The MLP-with-more-features predicts absolute scores much better but
ranks them slightly worse.

## Why this is surprising

If `ssimulacra2`, `adm_scale3`, etc. carry independent signal
(Research-0027 §"Consensus top-10" said they do), adding them
should at minimum match canonical6's PLCC and improve RMSE — not
trade them off. PLCC measures linear correlation; SROCC measures
rank order. Both hovering 0.96–0.98 with new features in train
means the model *knows* the absolute score better but loses
relative-ordering precision.

The most likely cause is **feature-scale variance**. The canonical
6 features are all roughly in `[0, 1]` after the
`vmaf_v0.6.1` pipeline normalises them. The expanded set is not:

| Feature            | Typical range          |
|--------------------|------------------------|
| `adm2`, `vif_*`, `float_ssim`, `float_ms_ssim` | `[0, 1]` |
| `motion2`, `motion3`            | `[0, ~30]` |
| `psnr_y`, `psnr_cb`, `psnr_cr`  | `[0, ~100]` |
| `cambi`                         | `[0, ~100]` |
| `ciede2000`                     | `[0, ~100]` |
| `psnr_hvs`                      | `[0, ~100]` |
| `ssimulacra2`                   | `[~−1, ~100]` |

A 16-unit hidden layer fed unnormalised inputs lets the
`psnr_*`/`cambi`/`ciede2000` features dominate gradient updates by
two orders of magnitude. The MLP shifts its weights to fit those
big-magnitude features and underweights the high-signal-but-small-
range `adm2`/`vif_*` block. PLCC drops because the absolute
prediction range is now driven by the wrong features; RMSE drops
because in absolute MSE terms, the model is closer to the right
answer (just the wrong "shape").

Two confounders also worth flagging:

1. **30 epochs** may be insufficient for the higher-dimensional
   inputs to converge. Canonical6 only has 257 parameters; Subset C
   on `mlp_small` has 65 + the increased input layer ~ 433
   parameters. More parameters need more epochs.
2. **`mlp_small` itself** may be too small for 21-feature input.
   The 16-unit hidden bottleneck is reasonable for 6 inputs;
   it may compress away too much for 21.

## Decision

Per Research-0027's stopping rule, **canonical-6 stays as the
default**. No `vmaf_tiny_v2` ships from this Phase-3 result.

But the RMSE evidence is too strong to fully retire the hypothesis.
Three concrete follow-up experiments scoped:

### Phase-3b — feature standardisation

Re-run the same sweep with `sklearn.preprocessing.StandardScaler`
fit on the train split (mean=0, std=1 per feature). This is the
single most likely fix and adds maybe ~10 lines to
`phase3_subset_sweep.py`. Stopping rule unchanged.

### Phase-3c — wider MLP / more epochs

If 3b still doesn't beat canonical6, sweep:

- `mlp_medium` (`Linear(N→64) → ReLU → Linear(64→32) → ReLU → Linear(32→1)`)
- 60 and 100 epochs (vs canonical 30)
- `lr` ∈ `{1e-3, 3e-4, 1e-4}`

Cheap because each fold is <30 s on the small-feature corpus.

### Phase-3d — feature ablation

For each feature in Subset C, run "C minus that one feature" and
check PLCC delta. Identifies which features actively hurt PLCC
in this experimental setup. Useful regardless of whether 3b/3c
recover the result.

## What to take from Phase-2 vs Phase-3

Research-0027 said `ssimulacra2` and `adm_scale3` carry signal in
isolation. Phase-3 says **a tiny MLP cannot exploit that signal at
the existing architecture / preprocessing settings**. Both can be
true. The Phase-2 importance metrics (MI, LASSO, RF) are robust to
feature scaling because they normalise internally. The Phase-3 MLP
is not.

The honest interpretation: *the canonical 6-feature flow is
extremely well-tuned for the* `mlp_small` *architecture; you cannot
trivially expand it without re-tuning everything else*.

## Reproducer

```bash
python3 ai/scripts/phase3_subset_sweep.py \
  --parquet runs/full_features_netflix.parquet \
  --out runs/phase3_subset_sweep.json \
  --subsets canonical6,A,B,C \
  --epochs 30
```

Wall time: ~30 s per subset per fold = ~18 min for all 4 subsets ×
9 folds on `ryzen-4090-arc` CPU.

The JSON report includes ADR-0661 `run_provenance` with the analyzer
entrypoint, original argv, parsed subset / seed / standardization arguments,
source parquet path, and output report path. The block is a top-level sibling
to the subset result keys.

## Caveats

1. **Single-corpus measurement.** Same caveat as Research-0027 §1
   — Netflix Public 9-source only. KoNViD-1k cross-check still
   open if the broader-feature hypothesis is revived in 3b/3c.
2. **No standardisation.** This is the headline caveat; treat the
   PLCC ranking as "what an out-of-the-box drop-in tells you,"
   not "what the broader feature set is theoretically capable of."
3. **`mlp_small` is the only architecture tested.** `mlp_medium` /
   `linear` may surface different orderings. Phase-3c covers this.
4. **Single seed.** All numbers from `seed=0`. Variance across
   seeds is unmeasured here; if Phase-3b lands a meaningful Δ,
   re-run with 3 seeds and report mean ± seed-std before shipping
   any v2 model.

## References

- **`req`** (user, 2026-04-29): *"yeah"* in response to "Want me to
  write up Research-0027" + *"yeah then go on with ai phase 3?"*.
- [Research-0027](0027-phase2-feature-importance.md) — the
  GO-signal digest this Phase-3 sweep tests.
- [Research-0026](0026-cross-metric-feature-fusion.md) — 4-phase
  plan; this digest closes Phase 3 (negative result, follow-ups
  named).
- ADR-0049 — tiny-AI sidecar JSON policy (still relevant for any
  future v2 that emerges from 3b/3c).
- Driver: [`ai/scripts/phase3_subset_sweep.py`](../../ai/scripts/phase3_subset_sweep.py)
  (PR-pending).
- Source data: `runs/full_features_netflix.parquet` (gitignored;
  reproducible).
- Source results: `runs/phase3_subset_sweep.json` (gitignored;
  reproducible).
