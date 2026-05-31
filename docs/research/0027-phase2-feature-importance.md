<!-- markdownlint-disable MD049 -->
# Research-0027 — Phase-2 feature correlation, MI, and importance results

_Updated: 2026-04-29._

## Question

[Research-0026 §"Experimental plan" Phase 2](0026-cross-metric-feature-fusion.md)
asks: across the 21 bit-exact features the fork can extract beyond
the canonical 6, **which carry signal that's NOT already captured by
the `vmaf_v0.6.1` 6-tuple?** Phase-2's deliverable is a go/no-go
decision on whether Phase-3 (the MLP arch sweep on Top-K subsets) is
worth the cost.

This digest is the **go-signal report**: results say yes, with a
specific shortlist.

## Method

Full-feature parquet extracted over the Netflix Public 9-source × 70
distortion corpus (per `ai/scripts/extract_full_features.py` from
PR #186) — **11 040 frame rows × 21 feature columns + `vmaf` target**.
0 NaN rows after extraction (all 21 metrics emit cleanly through the
fork's libvmaf CLI). Wall time on `ryzen-4090-arc` was ~118 minutes
on CPU, dominated by `cambi` and `ssimulacra2` (~70 % of per-frame
extraction cost).

`ai/scripts/feature_correlation.py` computes:

1. **Pearson correlation matrix** + redundant-pair list at
   `|r| ≥ 0.95` threshold.
2. **Mutual information (MI)** from each feature to `vmaf` target
   (`sklearn.feature_selection.mutual_info_regression`,
   `random_state=0`).
3. **LASSO importance** — `LassoCV(cv=5)` on standardized features,
   `|coefficient|` magnitude as importance.
4. **Random-forest importance** — `RandomForestRegressor(n_estimators=100)`,
   `feature_importances_` (Gini-mean-decrease).
5. **Consensus top-K** = features ranked top-K by **all three**
   importance methods (intersection).

## Per-method top-10

### Mutual information (MI)

| Rank | Feature        | MI score |
|-----:|----------------|---------:|
|    1 | `adm2`         |  2.0424  |
|    2 | `adm_scale2`   |  1.8166  |
|    3 | `adm_scale1`   |  1.6710  |
|    4 | `adm_scale3`   |  1.5585  |
|    5 | `ssimulacra2`  |  1.5499  |
|    6 | `psnr_hvs`     |  1.5301  |
|    7 | `vif_scale1`   |  1.4953  |
|    8 | `float_ms_ssim`|  1.4696  |
|    9 | `vif_scale2`   |  1.4461  |
|   10 | `float_ssim`   |  1.4156  |

ADM dominates the top of the MI ranking; `ssimulacra2` and
`psnr_hvs` are the strongest non-canonical entries (both score
above 6 of the canonical-6 features).

### LASSO importance

| Rank | Feature        |  |coef| |
|-----:|----------------|-------:|
|    1 | `adm2`         |  13.61 |
|    2 | `vif_scale2`   |   9.77 |
|    3 | `float_ssim`   |   7.73 |
|    4 | `ssimulacra2`  |   6.57 |
|    5 | `motion2`      |   5.33 |
|    6 | `adm_scale1`   |   2.60 |
|    7 | `ciede2000`    |   1.72 |
|    8 | `adm_scale3`   |   1.66 |
|    9 | `psnr_cr`      |   0.93 |
|   10 | `vif_scale0`   |   0.62 |

LASSO splits the load across categories: `adm2` is dominant but
`vif_scale2` and `float_ssim` are large too. **`float_ssim` is a
top-3 LASSO entry** but doesn't appear in the canonical 6 —
strong signal that a v2 model could exploit it.

### Random-forest importance

| Rank | Feature        | Importance |
|-----:|----------------|-----------:|
|    1 | `adm2`         |   **0.948**|
|    2 | `vif_scale1`   |   0.0086   |
|    3 | `adm_scale2`   |   0.0086   |
|    4 | `vif_scale2`   |   0.0075   |
|    5 | `motion2`      |   0.0075   |
|    6 | `vif_scale3`   |   0.0059   |
|    7 | `motion3`      |   0.0025   |
|    8 | `adm_scale3`   |   0.0024   |
|    9 | `motion`       |   0.0022   |
|   10 | `ssimulacra2`  |   0.0012   |

RF concentrates **94.8 % of importance on `adm2` alone**. The Gini-mean-
decrease metric is known to under-weight features highly correlated
with the dominant feature (the tree splits on `adm2` first; downstream
splits don't gain much). Take this ranking as "what could a single-
feature model achieve" rather than "what features add value to a
multi-feature model".

## Consensus top-10 (top-K=10 across all three methods)

```text
adm2, adm_scale3, ssimulacra2, vif_scale2
```

**Only 4 features appear in the top-10 of MI ∧ LASSO ∧ RF**. The
intersection at K=10 narrows brutally: methods agree most on `adm2`
(unsurprising) and surface three additional features as collectively
informative.

### What the consensus tells us

- **`adm2`** — the canonical aggregate. Must keep.
- **`vif_scale2`** — a canonical scale (already in `DEFAULT_FEATURES`).
  Its top-10 presence in all three methods confirms VIF carries
  independent signal beyond ADM.
- **`adm_scale3`** — **canonical does NOT include this**. Comes
  from the same extractor (`adm`) so cost is free, and it carries
  signal not redundant with `adm2` (correlation 0.84 in the matrix).
- **`ssimulacra2`** — **canonical does NOT include this**, and
  ssimulacra2 is the only modern perceptual metric in the top-10.
  Strong candidate for a v2 model.

The two non-canonical entries (`adm_scale3`, `ssimulacra2`) are the
first-priority adds for Phase-3.

## Redundant-pair clusters

11 feature pairs at `|r| ≥ 0.95`:

| Pair                            |       r |
|---------------------------------|--------:|
| `motion2` ↔ `motion3`           |  0.9926 |
| `vif_scale2` ↔ `vif_scale3`     |  0.9918 |
| `vif_scale1` ↔ `vif_scale2`     |  0.9861 |
| `vif_scale1` ↔ `ssimulacra2`    |  0.9807 |
| `adm2` ↔ `adm_scale2`           |  0.9804 |
| `float_ssim` ↔ `float_ms_ssim`  |  0.9802 |
| `adm2` ↔ `adm_scale1`           |  0.9662 |
| `motion` ↔ `motion2`            |  0.9621 |
| `vif_scale1` ↔ `vif_scale3`     |  0.9580 |
| `motion` ↔ `motion3`            |  0.9546 |
| `vif_scale3` ↔ `float_ssim`     |  0.9531 |

### Implications for Phase-3 subset selection

- **Motion family is internally redundant**: `motion`, `motion2`,
  `motion3` cluster pairwise above 0.95. Including more than one
  motion variant adds no information. Keep `motion2` (canonical)
  and drop `motion`/`motion3` from the v2 candidate pool.
- **VIF scales 1/2/3 are pairwise redundant** (≥ 0.96), and
  `vif_scale1 ↔ ssimulacra2 = 0.98` is the most surprising cross-
  family redundancy. ssimulacra2 may be picking up the same
  multi-scale spatial info VIF does, just through a different
  decomposition.
- **`float_ssim` ↔ `float_ms_ssim` = 0.98** — keeping both is
  pointless; pick the cheaper one (`float_ssim`).
- **`adm2` ↔ `adm_scale1/2` = 0.97/0.98** — the per-scale ADM
  features below scale 3 add little beyond the aggregate. **Only
  `adm_scale3` is far enough from `adm2` to add signal** (matches
  the consensus top-10 finding).

## Recommended Phase-3 subsets

Three candidate subsets for the MLP arch sweep, ordered by Phase-3
cost:

### Subset A: canonical-7 (canonical + ssimulacra2)

```text
adm2, vif_scale0, vif_scale1, vif_scale2, vif_scale3,
motion2, ssimulacra2
```

Single-feature add. Tests the hypothesis "adding one modern
perceptual metric to vmaf_v0.6.1's exact feature set improves it".
**Recommended as the primary Phase-3 candidate** — if it doesn't beat
canonical-6, the full-set hypothesis is dead.

### Subset B: consensus-7 (canonical + adm_scale3 + ssimulacra2 − redundant)

```text
adm2, adm_scale3, vif_scale2, motion2, ssimulacra2,
psnr_hvs, float_ssim
```

Drops the redundant VIF scales 0/1/3 (covered by `vif_scale2` per
the redundancy matrix), adds the two consensus non-canonical
features and the next-strongest MI entry (`psnr_hvs`). 7 features
keep the model the same size as canonical-6 + one but with more
diverse signal.

### Subset C: full-21 ceiling

All 21 features — sanity ceiling for "what's the absolute best a
model can do on this corpus." Most useful as a denominator for
Phase-3's per-feature ablation.

## Decision

**Phase-3 GO.** Run the MLP arch sweep on Subsets A, B, and C against
the Netflix corpus + (eventually) KoNViD parquet.

### Subset-priority ordering for Phase-3

1. **Subset A** (canonical-6 + ssimulacra2) — fastest to validate,
   answers the most-conservative hypothesis.
2. **Subset B** (consensus-7) — most likely to be the Pareto-optimal
   v2 if it beats canonical-6.
3. **Subset C** (full-21) — sanity ceiling; only train this one if A
   and B both pass.

### Phase-3 stopping rules

- If **Subset A's** mean LOSO PLCC fails to beat canonical-6 by
  ≥ 0.005, stop. The hypothesis is dead and the canonical-6 stays.
- If **Subset A** wins but **Subset B** does not, ship Subset A as
  `vmaf_tiny_v2_canonical7.onnx` and stop.
- If **Subset B** wins, ship Subset B as `vmaf_tiny_v2_consensus7.onnx`
  with a sidecar `feature_set: "consensus-7"` flag.
- If **Subset C** wins materially over Subset B (≥ 0.005 PLCC), ship
  it too as `vmaf_tiny_v2_full21.onnx` with a documented
  extraction-cost-vs-accuracy tradeoff.

## Notes / caveats

1. **Single-corpus measurement.** All numbers above are from the
   Netflix Public 9-source corpus alone. The KoNViD-1k corpus
   parquet (per Research-0025) was not extracted in this Phase-2
   pass because of the ~3-hour-per-1200-clip cost; full-feature
   KoNViD extraction is a Phase-3 prerequisite if Subset B/C
   advance. The redundancy structure (e.g. motion family clustering)
   is unlikely to differ between corpora, but the importance
   rankings may.
2. **Random-forest concentration on `adm2`** is interpretable but
   not as informative as it looks: see §"Random-forest importance"
   note about correlated-feature underweighting. Do NOT cite the
   "94.8 % importance on `adm2`" number without that context.
3. **Per-scale ADM redundancy hides per-frame outlier signal.**
   `adm_scale3` carries top-10 consensus rank but its correlation
   with `adm2` is **only 0.84** (not in the redundant-pair list).
   This is the strongest evidence that v2 should expand the ADM
   surface, not just substitute one for another.
4. **`lpips` was excluded from Phase 1/2** (Research-0026 Q1).
   This digest does NOT speak to whether a DNN-based feature could
   improve a v2 model — that's a separate axis.
5. **Consensus top-K is conservative by construction**. K=10 with
   3 methods means each feature must land in the top-47 % of every
   ranking. Loosening to K=15 yields 7 consensus features
   (adds `adm_scale1`, `motion2`, `vif_scale1`); loosening to K=20
   yields 12 (adds `adm_scale2`, `psnr_hvs`, `float_ssim`,
   `vif_scale3`, `motion`).

## Reproducer

```bash
# 1. Full-feature extraction (~118 min wall on ryzen-4090-arc CPU):
python3 ai/scripts/extract_full_features.py \
  --out runs/full_features_netflix.parquet

# 2. Correlation + MI + LASSO + RF analysis (<1 min):
python3 ai/scripts/feature_correlation.py \
  --parquet runs/full_features_netflix.parquet \
  --out runs/full_features_correlation.json \
  --top-k 10 \
  --redundancy-threshold 0.95
```

The JSON report includes ADR-0661 `run_provenance` with the analyzer
entrypoint, original argv, parsed target / threshold / top-K arguments, source
parquet path, and output report path. Treat that block as the replay contract
when comparing refreshed feature-correlation runs across corpora.

## References

- **`req`** (user, 2026-04-29): *"and rebase #185"* + *"yeah write up"*
  in response to Phase-2 result summary.
- [Research-0026](0026-cross-metric-feature-fusion.md) — the
  4-phase plan this digest closes Phase 2 of.
- [Research-0023](0023-loso-3arch-results.md) §5 — content-distribution
  variance source (orthogonal axis to feature-set; addressed by
  Research-0025).
- [Research-0025](0025-foxbird-resolved-via-konvid.md) — data-side
  variance resolved by KoNViD; this digest opens the feature-side.
- ADR-0049 — tiny-AI sidecar JSON policy (governs how a v2 model
  declares its `feature_set`).
- PR #185 — `FULL_FEATURES` registry (the 21-feature inventory this
  digest exercised).
- PR #186 — Phase-2 analysis scripts (`extract_full_features.py` +
  `feature_correlation.py`).
- Source data: `runs/full_features_netflix.parquet`
  (11 040 rows × 25 cols, gitignored).
- Source analysis: `runs/full_features_correlation.json` (full
  Pearson matrix + per-method rankings + consensus, gitignored).
