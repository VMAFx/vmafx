<!-- markdownlint-disable MD029 MD049 MD060 -->
# Research-0026 — Cross-metric feature fusion for tiny-AI

_Updated: 2026-04-28._

## Question

The fork's tiny-AI training pipeline (ADR-0203, Research-0019, PR #180
combined trainer) currently uses **only the 6 features** from the
canonical `vmaf_v0.6.1` model:

| Feature | Source | Type |
|---|---|---|
| `adm2` | float_adm | Detail-loss aggregate |
| `vif_scale0` | float_vif | Visual-info fidelity scale 0 |
| `vif_scale1` | float_vif | Visual-info fidelity scale 1 |
| `vif_scale2` | float_vif | Visual-info fidelity scale 2 |
| `vif_scale3` | float_vif | Visual-info fidelity scale 3 |
| `motion2` | integer_motion | Frame-to-frame motion energy |

The fork **registers 19 extractors** that produce a much wider feature
set:

| Extractor | Features produced |
|---|---|
| float_adm / integer_adm | `adm`, `adm2`, `adm_scale0..3` |
| float_vif / integer_vif | `vif`, `vif_scale0..3` |
| float_motion | `motion`, `motion2` |
| integer_motion_v2 | `motion3` (5-frame window) |
| float_ansnr | `float_ansnr`, `float_anpsnr` |
| float_psnr / psnr | `psnr_y`, `psnr_cb`, `psnr_cr` |
| float_ssim / ssim | `ssim` |
| float_ms_ssim | `float_ms_ssim` |
| float_moment | image moments (mean, var) |
| cambi | `cambi` (banding) |
| ciede | `ciede2000` (color difference) |
| psnr_hvs (CPU SIMD'd) | `psnr_hvs`, `psnr_hvs_y/cb/cr` |
| ssimulacra2 | `ssimulacra2` |
| lpips | `lpips` (DNN-based perceptual) |

That's ≈ **27 distinct per-frame features** the fork can extract
today, all bit-exact across CPU + AVX2 + AVX-512 + NEON, and most
also bit-exact (or near so) across CUDA / SYCL / Vulkan.

**The question this digest scopes:** does training on the broader
feature set produce a better tiny model than the canonical 6
features, and which feature subset is Pareto-optimal?

## Why this matters now

[Research-0025](0025-foxbird-resolved-via-konvid.md) demonstrated
that **adding more data** (KoNViD-1k) tightens LOSO PLCC standard
deviation by 5.6× and resolves the FoxBird outlier. The natural
next axis is **adding more features** — same data, richer signal.
Three possible outcomes:

1. **Strong gain.** A wider feature set captures perceptual aspects
   the 6-feature canonical misses (e.g. CAMBI for banding,
   SSIMULACRA 2 for chroma). The new model becomes
   `vmaf_tiny_v2.onnx`.
2. **Marginal gain.** Cross-metric correlation is high enough that
   the 6 features already capture most of the signal; adding more
   yields ≤ 0.5 pp PLCC improvement at the cost of slower
   extraction. Conclusion: stay on 6 features; this digest closes
   "no significant gain measured".
3. **Mixed gain.** Specific failure modes (FoxBird-class banding,
   chroma artifacts, color-space mismatch) get fixed; mean PLCC
   barely moves. Conclusion: ship a model variant per failure mode,
   not a one-size-fits-all replacement.

Research-0023 / 0025 already show that the FoxBird-class outlier
was a **content-distribution** problem solvable with more data.
Outcome (3) above hypothesises there's a **feature-set** problem
hiding underneath that more data masks but doesn't eliminate.

## Experimental plan

### Phase 1 — Inventory + parquet expansion (1 PR)

1. Extend
   [`ai/data/feature_extractor.py`](../../ai/data/feature_extractor.py)
   to optionally extract **all 27 features**, not just the 6
   canonical ones. Gate by a CLI flag (`--feature-set
   {canonical,full,custom}`) so existing 6-feature flows are
   unchanged by default.
2. Re-run KoNViD acquisition + Netflix corpus extraction with
   `--feature-set full`. Estimated wall: KoNViD ~2 hours
   (4 × current), Netflix ~10 minutes. Output:
   `ai/data/konvid_vmaf_pairs_full.parquet` and
   `ai/data/netflix_pairs_full.parquet` (gitignored).
3. Verify against existing 6-feature parquets that the canonical
   columns are byte-identical (sanity check on the extraction
   pipeline).

**Deliverable:** PR adding `--feature-set` flag + new parquets.
Documented in `docs/ai/training-data.md`. No model change yet.

### Phase 2 — Correlation + mutual-information analysis

4. Pairwise Pearson correlation matrix over the 27 features on the
   combined corpus (~280 K frames). Heatmap in
   `docs/research/0026-feature-correlation-heatmap.png`.
   Hypothesis: VIF scales 0-3 are highly intra-correlated; PSNR-Y
   and SSIM correlate strongly; `motion2` and `motion` are nearly
   redundant; `adm` and `adm2` track within ~0.95.
5. Pairwise mutual-information matrix (handles non-linear
   relations Pearson misses). Hypothesis: `cambi` and `ssimulacra2`
   carry information orthogonal to the canonical 6 (banding +
   perceptual color, neither captured by VIF/ADM/motion).
6. **Feature-importance ranking** via three independent methods,
   targets = `vmaf_v0.6.1` per-frame teacher score:
   - LASSO regression on standardized features (sparsity-inducing).
   - Random-forest feature importance (model-free non-linear).
   - SHAP values from a gradient-boosted regressor.
   Cross-check rankings — features ranked top-10 by **all three**
   methods are the highest-leverage candidates for the v2 model.

**Deliverable:** Notebook in `ai/scripts/feature_analysis.ipynb`
or `.py` (whichever ships); aggregated tables in this digest.

### Phase 3 — Tiny-AI v2 architecture sweep

7. Train `mlp_small` / `mlp_medium` on three feature subsets:
   - **Canonical-6** (baseline; what we have today).
   - **Top-K** = canonical-6 ∪ top-K-additional from Phase 2's
     consensus ranking, K ∈ {2, 4, 6, 12}.
   - **Full-27** (sanity ceiling).
   30 epochs each, `--val-mode netflix-source-and-konvid-holdout`,
   same seed.
8. **LOSO sweep on the winning subset** — 9-fold per-source held
   out (matches Research-0025 §"LOSO sweep" methodology). Mean ±
   std PLCC / SROCC / RMSE.
9. **Per-failure-mode evaluation:**
   - Banding-heavy clips (need separate CAMBI fixture set).
   - Chroma-artifact clips (need 4:4:4 distorted pairs).
   - High-motion + low-light (FoxBird-class — already covered).

**Deliverable:** Research-0027 digest with the sweep results +
recommended `vmaf_tiny_v2.onnx` registration if outcome (1) or
(3) materialises.

### Phase 4 — Latency + size tradeoff

10. Measure `vmaf` CLI extraction time per (ref, dis) pair on each
    feature set. Tradeoff: full-27 extraction is bounded by the
    slowest extractor (CAMBI on 4K, ~30 % of frame time).
11. ONNX file-size comparison: `vmaf_tiny_v1.onnx` (1.3 KB) vs
    Full-27 mlp_small (~5 KB) — both still tiny.
12. Decide: which subset becomes default for `vmaf-tiny`? Per
    ADR-0042 / ADR-0049 doc-substance + sidecar policy, ship as
    `vmaf_tiny_v2.onnx` with explicit `feature_set` field in the
    sidecar JSON so downstream tooling knows.

## Open questions

- **Q1.** Should `lpips` (the only DNN-based feature) be in the
  candidate pool? It's expensive (ORT inference per frame) and
  blurs the line between "tiny model on classical features" and
  "ensemble of DNNs". Recommend **excluding lpips** from Phase 1/2
  and revisiting only if classical features can't close the gap.
- **Q2.** Do we want the v2 to be **drop-in compatible** with
  `vmaf_v0.6.1` (same 6-feature input layout) or a **new public
  API**? Drop-in compat means Phase 3 must include a constrained
  variant; new API means a richer feature loader.
- **Q3.** Per-fold or per-clip outliers — is FoxBird the only
  case? Phase 2's mutual-information analysis on the
  `Seeking` clip (which had RMSE 6.66 in Research-0025's LOSO,
  the worst by RMSE) should reveal whether it's a content-
  distribution issue or a feature-set issue.

## Cost estimate

| Phase | Wall time | New artefacts |
|---|---|---|
| 1 — full-feature parquet | 2-3 h re-extraction | 2 parquets, gitignored |
| 2 — correlation/MI/importance | 30 min compute | 1 notebook, 3 plots, in-digest tables |
| 3 — arch sweep | 1-2 h training | ~12 ONNXes per arch, in `runs/` (gitignored) |
| 4 — latency/size | 30 min benchmarking | benchmark JSON in `testdata/` |
| **Total** | **~5-7 h focused** | digest + Research-0027 follow-up |

Compare to the 8 h estimate for the upstream-port chains
(Research-0024) — broadly similar effort with much higher
expected upside.

## Recommendation

- **Adopt this as a tracked T-NN backlog item.** Strategy: do
  Phase 1 + Phase 2 in one session (parquet + analysis) → ship
  Research-0027 with the correlation/importance numbers and a
  go/no-go decision on Phase 3.
- **Defer Phase 3 unless Phase 2 shows ≥ 1 feature with
  consensus top-3 importance that's NOT in the canonical 6.**
  Phase 2 is cheap (~3 h) and answers the question. Phase 3 only
  fires if Phase 2 says yes.
- **Phase 4 is automatic** if Phase 3 lands a v2 model.

## References

- **`req`** (user, 2026-04-28): *"are we actually training the
  ai models on all metrics combined? and try to find more
  usage/overlaps/relations whatever?"*
- [Research-0019](0019-tiny-ai-netflix-training.md) — Netflix
  training methodology survey.
- [Research-0023](0023-loso-3arch-results.md) — 3-arch LOSO on
  canonical 6 features.
- [Research-0025](0025-foxbird-resolved-via-konvid.md) — combined
  corpus resolves content-distribution variance; this digest
  attacks the orthogonal feature-set axis.
- ADR-0042 — Tiny-AI doc-substance per-PR rule.
- ADR-0049 — Tiny-AI sidecar JSON policy.
- ADR-0203 — Netflix-corpus training prep.
- Li et al. 2016 (Netflix VMAF paper) — original 6-feature SVR.
