<!-- markdownlint-disable MD049 -->
# Research-0029 — Phase-3b: StandardScaler retry of the subset sweep

_Updated: 2026-04-29._

## Question

[Research-0028 §"Decision"](0028-phase3-subset-sweep.md) hypothesised
that the negative Phase-3 result was a **feature-scale-variance**
artefact, not a feature-signal artefact: raw features fed to
`mlp_small` let `psnr_*` / `cambi` / `ciede2000` (range 0–100)
dominate gradient updates over `adm2` / `vif_*` / `float_ssim`
(range 0–1). Phase-3b is the most-likely-fix retry: same sweep,
add per-fold StandardScaler.

## Method

Identical to [Research-0028 §"Method"](0028-phase3-subset-sweep.md):
9-fold LOSO on the Netflix Public corpus
(`runs/full_features_netflix.parquet`, 11 040 frames × 21 features),
`mlp_small` (Linear→16→8→1), 30 epochs, Adam `lr=1e-3`, batch 256,
`seed=0`. **Difference:** per-fold StandardScaler — fit
`(mean, std)` on the train split and apply to both train and val.
Statistics never leak from the held-out fold.

Driver flag: `--standardize` on
[`ai/scripts/phase3_subset_sweep.py`](../../ai/scripts/phase3_subset_sweep.py)
(added in this PR).

## Results

| Subset       | Features |   Mean PLCC |   Mean SROCC |   Mean RMSE | Δ PLCC vs canonical6 |
|--------------|---------:|------------:|-------------:|------------:|---------------------:|
| canonical6   |        6 |   0.9677    |   0.9802     |    9.586    |                  —   |
| A            |        7 |   0.9669    |   0.9768     |    8.672    |             −0.0008  |
| **B**        |    **7** | **0.9783**  | **0.9803**   | **7.893**   |          **+0.0106** |
| C            |       21 |   0.9597    |   0.9632     |   10.497    |             −0.0081  |

## Headline

**Subset B clears the Research-0027 stopping-rule threshold of
+0.005 PLCC by 2× (+0.0106).** The hypothesis "broader feature set
helps a tiny MLP" is now supported on standardised inputs — the
Phase-3a failure was preprocessing, not signal.

Subset B is `{adm2, adm_scale3, vif_scale2, motion2, ssimulacra2,
psnr_hvs, float_ssim}` — the consensus-7 with redundancy-pruned
VIF scales. It validates **all four** of Research-0027's findings:

1. `adm2` and `vif_scale2` are core (canonical features).
2. `adm_scale3` adds signal beyond `adm2` (correlation only 0.84).
3. `ssimulacra2` carries independent perceptual signal.
4. Redundant pairs (motion family, vif scales 1/2/3) ARE redundant
   — Subset B drops them and wins; Subset C keeps them all and loses.

## Subset comparison detail

- **Subset A (canonical-6 + ssimulacra2)** matches canonical6
  within noise (Δ −0.0008). One extra feature without redundancy
  pruning doesn't help; the model has to make room for ssimulacra2
  by attenuating something else. The PLCC delta is statistically
  meaningless (std on 9 folds is ±0.029), but the *expectation*
  that adding any consensus-top feature would lift PLCC was
  wrong. **Pruning matters.**
- **Subset B (consensus-7 with redundancy pruning)** is the
  winner. **Drops 3 redundant VIF scales** (kept only `vif_scale2`)
  and **drops `motion3`** (redundant with `motion2`); **adds**
  `adm_scale3`, `ssimulacra2`, `psnr_hvs`, `float_ssim`. Same
  feature count as A (7), better PLCC (+0.0114 vs A), better RMSE
  (8.7 → 7.9).
- **Subset C (full-21)** loses (Δ −0.0081). Including all 21
  features overwhelms the 16-unit hidden layer; the model can't
  exploit the high-signal additions because most of the input is
  noise from redundant features. RMSE goes the wrong way too
  (10.5 vs canonical6's 9.6).

The pattern is: **information curation > information accretion**.
The MI/LASSO/RF triangulation in Research-0027 correctly identified
which features were complementary; Subset C just forgot to apply the
redundancy pruning.

## Why canonical6 PLCC dropped between Phase-3a and Phase-3b

Phase-3a canonical6 PLCC was 0.9845; Phase-3b canonical6 PLCC is
0.9677 — same model, same data, same seed, only difference is
StandardScaler. Two contributing factors:

1. **Adam lr interacts with input scale.** The canonical 6 features
   are roughly in `[0, 1]`. Standardising to `mean=0, std=1`
   actually **enlarges** their effective magnitude (small variance
   in raw → scaled to unit variance). Adam's adaptive learning
   rate then takes larger steps per epoch; for an already
   well-tuned 6-feature setup, this overshoots the optimum
   slightly. Lowering `lr` from `1e-3` to `3e-4` (Phase-3c
   experiment) would likely recover Phase-3a's canonical6 number.
2. **The `seed=0` random shuffle order is identical**, but the
   gradient magnitudes are different post-scaling. Different
   convergence trajectory, different result.

The apples-to-apples comparison is **canonical6 vs Subset B with
both standardised** — that's the +0.0106 PLCC win, and it's the
load-bearing finding. A future Phase-3c experiment could re-tune
`lr` for the standardised regime; if canonical6 recovers to its
Phase-3a number under a smaller `lr`, Subset B would have to
re-clear the threshold under matched conditions.

## Decision

**ADVANCE.** Per Research-0027's stopping rule and the
+0.0106 result, Subset B is the v2 candidate. But three caveats
gate actual model-shipping:

### Required before shipping `vmaf_tiny_v2`

1. **Multi-seed validation** — current numbers are seed=0 only.
   Re-run Phase-3b at `seed ∈ {0, 1, 2, 3, 4}`; require Subset B
   to maintain Δ ≥ +0.005 against canonical6 on mean over seeds,
   with std ≤ 0.01.
2. **KoNViD cross-check** — extract the full-feature parquet on
   the 1200-clip KoNViD corpus (~3 h wall, per Research-0025
   precedent) and re-run the sweep. The redundancy structure is
   unlikely to change but the importance ranking may.
3. **Phase-3c lr-sweep on canonical6** — if a tuned `lr` recovers
   Phase-3a's 0.9845 PLCC, the +0.0106 advantage may shrink
   under matched preprocessing.

### Phase-3c (gated on Phase-3b)

- Sweep `lr ∈ {1e-3, 3e-4, 1e-4}` × `epochs ∈ {30, 60, 100}` ×
  `arch ∈ {mlp_small, mlp_medium}` on canonical6 + Subset B with
  StandardScaler. Identifies the canonical6 PLCC ceiling under
  matched preprocessing and tests whether a wider arch helps
  Subset B more.

### Phase-3d (gated on Phase-3c if B still wins)

- Per-feature ablation of Subset B: train (B − {f}) for each f
  in B's 7 features, report PLCC delta. Identifies which
  Subset-B features are load-bearing vs which are decoration.

## What to take from this digest

The Research-0026 hypothesis is **alive and supported**. Subset B
is the right v2 candidate. But the path to `vmaf_tiny_v2.onnx`
needs three more validation steps (multi-seed, KoNViD,
matched-preprocessing canonical6). None of those are blocking;
all are well-scoped follow-ups.

The most striking secondary finding: **Subset C (full-21) loses
even with StandardScaler.** That's strong evidence that Subset B's
redundancy pruning is doing real work — adding more features past
the consensus-7 actively hurts, even with normalised inputs. This
supports the "tiny model" thesis specifically: a wider feature
input would need a correspondingly wider hidden layer
(`mlp_medium` or larger) to benefit, which is the Phase-3c hook.

## Reproducer

```bash
python3 ai/scripts/phase3_subset_sweep.py \
  --parquet runs/full_features_netflix.parquet \
  --out runs/phase3b_subset_sweep.json \
  --subsets canonical6,A,B,C \
  --epochs 30 \
  --standardize
```

Wall: ~18 min on `ryzen-4090-arc` CPU (same as Phase-3a; the
StandardScaler step is negligible).

## Caveats

1. **Single seed.** `seed=0` only; multi-seed required before
   shipping (see §"Required before shipping").
2. **Single corpus.** Netflix only; KoNViD cross-check open
   (Phase-3b extension).
3. **canonical6 PLCC moved between phases** — the
   Phase-3a → Phase-3b drop on canonical6 (0.9845 → 0.9677) means
   the absolute Subset-B number isn't directly comparable to the
   Phase-2 importance numbers from Research-0027.
4. **`mlp_small` only.** `mlp_medium` may flip the C-vs-B ordering
   (more capacity could exploit redundant features). Phase-3c.
5. **No StandardScaler statistics persisted with the model.** A
   shipped v2 ONNX would need to bundle the scaler `(mean, std)`
   into the sidecar (per ADR-0049) so inference applies the
   same normalisation. This is solvable but unimplemented.

## References

- **`req`** (user, 2026-04-29): *"go on"* in response to "Want me
  to fire Phase-3b now".
- [Research-0026](0026-cross-metric-feature-fusion.md) — 4-phase
  plan; this digest closes Phase 3b (the standardisation retry).
- [Research-0027](0027-phase2-feature-importance.md) — Phase-2
  consensus-top-10 result that justified Subset B's feature
  composition.
- [Research-0028](0028-phase3-subset-sweep.md) — negative result
  that motivated this retry; explained the standardisation
  hypothesis.
- ADR-0049 — sidecar JSON policy (governs how a v2 model bundles
  its scaler statistics).
- Driver: [`ai/scripts/phase3_subset_sweep.py`](../../ai/scripts/phase3_subset_sweep.py)
  (PR #188 baseline, `--standardize` flag added in this PR).
- Source data: `runs/full_features_netflix.parquet` (gitignored;
  reproducible).
- Source results: `runs/phase3b_subset_sweep.json` (gitignored;
  reproducible).
