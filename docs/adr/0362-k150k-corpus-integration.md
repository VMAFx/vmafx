<!-- markdownlint-disable MD013 MD060 -->
# ADR-0362 — K150K-A corpus integration: FR-from-NR extraction of FULL_FEATURES

| Field   | Value                            |
|---------|----------------------------------|
| Status  | Accepted                         |
| Date    | 2026-05-09                       |
| Tags    | ai, training-data, corpus, k150k, full-features, fork-local |

## Context

KoNViD-150k-A (K150K-A) is the largest publicly available no-reference (NR)
video quality corpus: 152,265 clips each carrying a per-clip mean-opinion-score
(MOS) aggregated from crowd-sourced ratings.  Integrating it into the tiny-AI
training pipeline requires mapping from the NR setting (no reference video) to
the full-reference (FR) VMAF extractor interface.

The existing training corpora (Netflix Public, BVI-DVC, KoNViD-1k, YouTube-UGC
subset) cover at most ~15,000 clips total.  Adding K150K-A increases training
scale by an order of magnitude and covers a wider distribution of user-generated
content quality levels.

The FULL_FEATURES set (Research-0026) — 22 features including ADM sub-bands,
VIF sub-bands, motion, PSNR, SSIM/MS-SSIM, CAMBI, ciede2000, psnr_hvs,
ssimulacra2, and the VMAF teacher — is the target feature space for the Phase 3
tiny-AI models.

## Decision

Use the **FR-from-NR adapter** (ADR-0346): decode each K150K-A clip once to
raw YUV and feed the same buffer as both `--reference` and `--distorted` in the
libvmaf CLI.  Run all 11 FULL_FEATURES extractors plus the vmaf_v0.6.1 model
for the VMAF teacher score.  Aggregate per-frame values to per-clip mean + std.

Output: `runs/full_features_k150k.parquet` (gitignored).  One row per clip,
48 columns: `clip_name`, `mos`, `width`, `height`, plus `<feat>_mean` and
`<feat>_std` for each of the 22 FEATURE_NAMES.

Hardware: RTX 4090 via `build-cpu/tools/vmaf --backend cuda` (fork build).

## Alternatives considered

| Alternative | Why rejected |
|---|---|
| Full `NrToFrAdapter` Python pipeline | 5–10× compute overhead from the re-encoding step; not needed when the MOS is the training target and FR features at identity suffice for content fingerprinting. |
| Canonical-6 features only (adm2, vif_scale\*, motion, vmaf) | Wastes the CUDA call — adding the remaining 16 features costs negligible extra per-frame time once the YUV decode is done. |
| KoNViD-1k only | Only ~1,200 clips; K150K-A is the same domain at 100× scale. |
| Skip corpus entirely | Leaves tiny-AI training data-constrained in the UGC domain; K150K-A is the highest-leverage single dataset addition available. |

## Consequences

**Positive:**

- Training corpus grows from ~15,000 clips to ~167,000 clips.
- K150K-A's MOS distribution spans a wider quality range than the Netflix
  reference corpus, improving model calibration at low-quality content.
- Fully restartable extraction (`.done` checkpoint + atomic parquet flush).

**Negative:**

- `ciede2000` and `psnr_hvs` are all-NaN for every K150K-A clip.  The libvmaf
  ciede2000 and psnr_hvs implementations return `null` when ref == distorted
  (identity pair) — this is correct behaviour, not a bug.  Downstream loaders
  must handle NaN columns gracefully (e.g. drop or impute before training).
- ADM, VIF, SSIM, MS-SSIM, and VMAF all floor at their identity values
  (1.0 / trivial) and carry zero discriminative signal for model training.
  Only CAMBI, motion, motion2, motion3, and ssimulacra2 remain informative.
- Full run ETA: ~296 h single-process sequential at ~7 s/clip on an RTX 4090.
  Parallelisation via `--limit` batches + `xargs -P` or a task queue is a
  follow-up.

## References

- `req`: "Write a K150K full-feature extraction script + run it on the local
  CUDA card..." (paraphrased: user requested the extraction pipeline, ADR,
  research digest, and all six ADR-0108 deliverables in this PR).
- [ADR-0346](0346-fr-from-nr-adapter.md) — FR-from-NR adapter pattern.
- [Research-0026](../research/0026-full-features-set.md) — FULL_FEATURES 22-feature set.
- [Research-0067](../research/0067-k150k-corpus-integration.md) — companion digest.
- [ADR-0108](0108-deep-dive-deliverables-rule.md) — six deep-dive deliverables rule.
