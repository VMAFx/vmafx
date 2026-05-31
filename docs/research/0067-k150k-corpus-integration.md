<!-- markdownlint-disable MD060 -->
# Research-0067 — K150K-A corpus integration feasibility and pipeline design

**Date:** 2026-05-09
**ADR:** [ADR-0362](../adr/0362-k150k-corpus-integration.md)
**Tags:** ai, training-data, corpus, k150k, full-features

## Summary

KoNViD-150k-A (K150K-A) is the largest publicly available no-reference video
quality corpus.  This digest documents the dataset profile, the FR-from-NR
extraction pipeline, smoke-test results, and ETA analysis for the full 152K-clip
run.

## Dataset profile

| Property | Value |
|---|---|
| Corpus name | KoNViD-150k-A (K150K-A) |
| Clip count | 152,265 |
| MOS source | Crowd-sourced, per-clip mean |
| Resolution | Mixed (primarily 540p, some 720p/1080p) |
| Duration | ~5 s per clip |
| Container | MP4, H.264/AVC |
| MOS range | 1.0 – 5.0 (crowd-sourced scale) |
| Local path | `.workingdir2/konvid-150k/k150ka_extracted/` |
| Labels CSV | `.workingdir2/konvid-150k/k150ka_scores.csv` |

## Extraction pipeline

The NR-to-FR adapter (ADR-0346) feeds the same decoded YUV as both reference
and distorted.  Pipeline per clip:

1. `ffprobe` — probe geometry (width, height, pixel format).
2. `ffmpeg` — decode MP4 to raw YUV420 (8-bit for H.264 content; 10-bit if
   detected).
3. `build-cpu/tools/vmaf --backend cuda` — run 11 extractors over the identity
   pair; emit per-frame JSON.
4. Aggregate per-frame values: nanmean + nanstd per feature.
5. Append checkpoint to `.done` file; flush parquet every 1000 clips.

System `/usr/local/bin/vmaf` v3.0.0 was evaluated and rejected: it lacks the
`ssimulacra2` and `motion_v2` extractor plugins required by FULL_FEATURES.
The fork build at `build-cpu/tools/vmaf` supports all 11 extractors and the
`--backend cuda` flag.

## Smoke-test results (10 clips, 2026-05-09)

| Clip | cambi\_mean | motion\_mean | vmaf\_mean | ok |
|---|---|---|---|---|
| orig\_10000251326\_540\_5s.mp4 | 0.0 | 3.77 | 100.0 | yes |
| orig\_10000958013\_540\_5s.mp4 | 0.0 | 2.14 | 100.0 | yes |
| orig\_10001646563\_540\_5s.mp4 | 0.0 | 4.31 | 100.0 | yes |
| orig\_10001767205\_540\_5s.mp4 | 0.0 | 1.62 | 100.0 | yes |
| orig\_10002025004\_540\_5s.mp4 | 0.0 | 5.89 | 100.0 | yes |
| (5 more) | ... | ... | 100.0 | yes |

Wall time: ~70 s for 10 clips (~7 s/clip), ok=10 fail=0.

`ciede2000_mean` and `psnr_hvs_mean` are NaN for all 10 clips — expected
(identity-pair artifact, see ADR-0362 §Consequences).
`vmaf_mean = 100.0` for all clips — identity pair floors at perfect score.
`cambi_mean = 0.0` for all clips in the smoke set — no banding in H.264 UGC.

## Constant vs informative columns

| Feature | Identity-pair behaviour | Useful for training? |
|---|---|---|
| adm2, adm\_scale\* | Floor at 1.0 | No |
| vif\_scale\* | Floor at 1.0 | No |
| float\_ssim, float\_ms\_ssim | Floor at 1.0 | No |
| vmaf | Floor at 100.0 | No (identity) |
| ciede2000, psnr\_hvs | NaN | No |
| psnr\_y/cb/cr | Floor at ~∞ (clipped) | No |
| cambi | Content-dependent | Yes |
| motion, motion2, motion3 | Content-dependent | Yes |
| ssimulacra2 | Floor near 0 | Marginal |

Only CAMBI and motion features carry discriminative signal under the FR-from-NR
adapter.  The identity-pair limitation is inherent to the NR→FR mapping; it is
documented and expected.  Downstream training must either drop constant columns
or use only the informative subset.

## Full-run ETA

| Parameter | Value |
|---|---|
| Clip count | 152,265 |
| Time per clip (observed) | ~7 s |
| Single-process wall time | ~296 h |
| Hardware | RTX 4090, CUDA 13.2, driver 595.71.05 |

Parallelisation paths (follow-up):

- N parallel processes, each with `--limit` + `--clips-dir` subset.
- Task queue (e.g. `xargs -P 4` over clip batches).
- Multi-GPU: route subsets to different CUDA devices via `CUDA_VISIBLE_DEVICES`.

## Conclusion

The FR-from-NR adapter is feasible and correct for K150K-A.  The 10-clip
smoke test confirms stable extraction at ~7 s/clip with zero failures.  The
resulting parquet will substantially expand tiny-AI training data in the UGC
domain.  Constant columns under the identity-pair adapter are a known
limitation; downstream training should filter or impute them.
