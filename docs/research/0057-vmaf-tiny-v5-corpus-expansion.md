# Research-0057 — vmaf_tiny_v5 corpus expansion (5-corpus = 4-corpus + UGC)

- **Status**: Active
- **Companion ADR**: [ADR-0287](../adr/0287-vmaf-tiny-v5-corpus-expansion.md)
- **Date**: 2026-05-03

## Question

Does adding YouTube-UGC vp9 (orig, dis) pairs to the existing
4-corpus parquet (Netflix Public + KoNViD-1k + BVI-DVC A+B+C+D)
buy measurable headroom over `vmaf_tiny_v2`'s shipped Netflix-LOSO
PLCC = 0.9978 ± 0.0021 baseline, holding architecture and
hyperparameters constant?

The arch ladder (v2 → v3 → v4) saturated at v4 (mlp_large, ADR-0242)
without unlocking further headroom. This digest tests the
orthogonal hypothesis — *more data*, same architecture — for whether
a less-curated, distribution-shifted corpus (UGC web video, encoded
to three VP9 ladder rungs) helps the regressor generalise.

## Corpus access audit

| Candidate | URL | Access | Outcome |
| --- | --- | --- | --- |
| **YouTube UGC** | `gs://ugc-dataset/` (also `media.withyoutube.com`) | Direct GCS bucket, CC-BY per ATTRIBUTION | **Used** — 30 smallest 4-tuples ingested |
| LIVE-VQC | `live.ece.utexas.edu/research/LIVE_VQC/index.html` | UT-Austin landing page | **Skipped** — URL returns 404 (corpus unreachable 2026-05-03) |
| MCL-V | `mcl.usc.edu/mcl-v-database/` | Google Drive ZIP behind a copyright-acceptance UI | **Skipped** — programmatic confirm-token flow not implementable without baking in fake credentials, per parent task instruction |
| TID2013 | tampere.fi | Email-form download | **Skipped** — image-level only, not useful for video VMAF |

## Methodology

- **New rows source**: `gs://ugc-dataset/vp9_compressed_videos/` —
  the 30 smallest stems (each is an `(orig.mp4, cbr.webm, vod.webm,
  vodlb.webm)` 4-tuple). Picked smallest by total compressed bytes
  to keep ingest wall-time and disk reasonable for a probe run.
  Total compressed download = 1.25 GB.
- **Decoding**: each stem decoded to a common geometry (640×360,
  yuv420p, 8-bit) via ffmpeg with `scale=W:H:flags=bicubic`,
  capped at 300 frames per clip (~10 s @ 30 fps). The 360p height
  cap keeps the YUV intermediate at ~330 KB/frame and the VMAF
  compute fast; the trade-off is documented in the ADR's
  Alternatives table.
- **Pair generation**: each 4-tuple yields three (orig, dis) pairs
  (`orig→cbr`, `orig→vod`, `orig→vodlb`), so 30 stems × 3 dis
  variants = 90 ref/dis pairs.
- **Feature extraction**: `build-cpu/tools/vmaf` with the canonical-6
  feature flags (`adm`, `vif`, `motion`) plus the bundled
  `vmaf_v0.6.1` predictor as the per-frame teacher. Output one row
  per (pair, frame) with the same column schema as the existing
  4-corpus parquet (canonical-6 columns + `vmaf` populated; the
  remaining 16 columns are NaN — v5 only consumes the canonical-6
  inputs anyway).
- **Combined corpus**: 4-corpus (330 499 rows) + UGC (27 000 rows)
  = 357 499 rows in the v5 training parquet.
- **Training**: identical recipe to v2 — `mlp_small` (6 → 16 → 8 →
  1, 257 params), Adam @ lr=1e-3, MSE, 90 epochs, batch_size 256,
  seed=0. Corpus-wide StandardScaler fit on training rows; baked
  into the exported ONNX as Constant nodes (ADR-0216 trust-root).
- **LOSO eval**: 9-fold leave-one-source-out on the Netflix subset
  only, for both v2-baseline (4-corpus, hold out 1 NF source) and
  v5-candidate (5-corpus, hold out 1 NF source) — same axes so the
  delta is attributable to the corpus expansion.
- **Decision rule**: ship v5 iff `mean_v5_PLCC - mean_v2_PLCC ≥ σ_v2`
  (i.e. ≥ 1 v2 LOSO standard deviation). Otherwise file as a
  research finding and do not ship.

## Findings

### Corpus-distribution skew

UGC clip-level VMAF (90 pairs, 27 000 frames) clusters at the high
end of the scale:

| Stat | UGC | 4-corpus base |
| --- | ---: | ---: |
| min | 8.43 | 0.77 |
| 25 % | 89.04 | 67.97 |
| **median** | **94.45** | **73.60** |
| 75 % | 96.52 | 78.58 |
| max | 100.00 | 100.00 |
| mean | 91.45 | 72.76 |

UGC's vp9 cbr/vod/vodlb encodes are typically high-VMAF — they're
production-quality YouTube ladder rungs, not the broad
codec-degradation sweep that the BVI-DVC and Netflix encodes
provide. Adding 27 000 high-VMAF rows shifts the training-set
class distribution toward the high end, which is a known risk for
regressor calibration in the 60–80 VMAF region.

### LOSO PLCC delta (v5 vs v2 baseline)

Single-seed, seed=0, 9-fold Netflix LOSO. Both arms train an
identical `mlp_small` (6 → 16 → 8 → 1, ~257 params) at 90 epochs,
batch_size 256, Adam @ lr=1e-3, MSE — only the training-corpus
input differs.

| Metric | v2 (4-corpus) | v5 (5-corpus = 4-corpus + UGC) | Δ |
| --- | ---: | ---: | ---: |
| mean PLCC | 0.99987 ± 0.00013 | 0.99988 ± 0.00006 | +0.00005 |
| mean SROCC | 0.99896 ± 0.00139 | 0.99884 ± 0.00167 | -0.00012 |
| mean RMSE | 0.418 ± 0.195 | 0.322 ± 0.136 | -0.096 |

**Decision: defer.** The PLCC delta is +0.00005, well below the
1-σ_v2 threshold (0.00013 in-run, or 0.0021 against the
shipped-v2 published axis). Both arms saturate at PLCC ≈ 0.9999;
the 4-corpus baseline is already so strong on Netflix LOSO that
adding UGC has no measurable PLCC effect. The mean RMSE
improvement (-0.096 absolute, ~23 % relative) is the only
positive signal — the v5 estimator's per-frame error magnitude
shrinks slightly — but it is not the ship gate the parent task
defined and is not large enough on its own to motivate a second
production checkpoint.

SROCC actually drifts -0.00012 (v5 worse), consistent with the
"corpus-distribution skew" concern: the high-VMAF UGC rows tilt
the regressor toward saturation, marginally hurting rank
discrimination on the Netflix held-out folds. Per-fold metrics
pinned in `runs/vmaf_tiny_v5_loso_metrics.json`.

## Reproducer

```bash
# Fetch the 30 smallest 4-tuples from the UGC bucket (~1.25 GB)
python3 ai/scripts/fetch_youtube_ugc_subset.py \
    --out-dir .workingdir2/ugc/download \
    --n-stems 30 \
    --manifest .workingdir2/ugc/manifest.json

# Decode + extract features (90 pairs, 27 000 rows, ~40 s wall on
# an 8-core CPU)
python3 ai/scripts/extract_ugc_features.py \
    --manifest .workingdir2/ugc/manifest.json \
    --yuv-dir .workingdir2/ugc/yuv \
    --vmaf-bin build-cpu/tools/vmaf \
    --out-parquet runs/full_features_ugc.parquet \
    --manifest-out runs/full_features_ugc.manifest.json \
    --max-height 360 \
    --max-frames 300 \
    --threads 8

# 9-fold Netflix LOSO (trains 18 models — 9 v2-baseline + 9 v5-
# candidate); writes the JSON report.
python3 ai/scripts/eval_loso_vmaf_tiny_v5.py \
    --parquet-base  runs/full_features_4corpus.parquet \
    --parquet-extra runs/full_features_ugc.parquet \
    --out-json      runs/vmaf_tiny_v5_loso_metrics.json
```

## Threats to validity

1. **Corpus skew** — UGC's high-VMAF cluster may dilute the
   regressor's discrimination in the perceptually-interesting
   60–80 region. Multi-seed re-runs with stratified-VMAF sampling
   (e.g. balance UGC contribution by VMAF decile) is the obvious
   follow-up if v5 underperforms.
2. **Frame-cap artefact** — capping every UGC pair at 300 frames
   (~10 s) may bias toward early-clip statistics; the 4-corpus
   sources span 700–1620 frames per clip. Consider equalising
   per-pair row counts in a follow-up.
3. **Resolution downsample** — 720p / 1080p UGC sources decoded
   to 640×360 yuv420p; this is below the 4-corpus 1920×1080 base
   geometry. The geometry shift is captured in the canonical-6
   features (which are scale-aware via VIF / ADM scales) but is a
   known confounder. Re-running at 720p decode would double the
   YUV intermediate to ~24 GB and the VMAF compute proportionally;
   acceptable as a follow-up if the 360p probe shows positive
   signal.
4. **Single-seed training** — v5 used seed=0, like v2's ship
   recipe; a multi-seed sweep (5 seeds × 9 folds = 45 trainings)
   would tighten the PLCC error bar to compare against the
   published 0.9978 ± 0.0021 v2 figure on the same multi-seed
   axis. Deferred.

## References

- v2 baseline ADR: [ADR-0216](../adr/0216-vmaf-tiny-v2.md)
- v3 arch ladder: [ADR-0241](../adr/0241-vmaf-tiny-v3-mlp-medium.md)
- v4 arch ladder: ADR-0242 (mlp_large)
- This digest's ADR: [ADR-0287](../adr/0287-vmaf-tiny-v5-corpus-expansion.md)
- YouTube UGC dataset homepage: <https://media.withyoutube.com/>
- YouTube UGC GCS bucket: `gs://ugc-dataset/`
- UGC paper: Wang et al., "YouTube UGC Dataset for Video
  Compression Research" (CoINVQ.pdf at the bucket root)
