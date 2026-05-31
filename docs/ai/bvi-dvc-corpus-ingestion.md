<!-- markdownlint-disable MD060 -->
# BVI-DVC corpus ingestion → fr_regressor_v2

The BVI-DVC dataset (Ma, Zhang, Bull 2021) is a 4-tier 4:2:0 10-bit
YCbCr reference corpus distributed by the Bristol Visual Information
Lab. This page documents how to bring BVI-DVC into the
`fr_regressor_v2` training corpus alongside the existing Netflix
Public drop and what to expect from doing so.

See [ADR-0310](../adr/0310-bvi-dvc-corpus-ingestion.md) for the
decision record and
[Research-0082](../research/0082-bvi-dvc-corpus-feasibility.md) for
the license / overlap / fold-expansion analysis.

## 1. Dataset overview

BVI-DVC ships **reference-only** material — no human DMOS scores —
across four resolution tiers, encoded in the filename prefix:

| Tier | Resolution  | Notes                                        |
|------|-------------|----------------------------------------------|
| A    | 3840 × 2176 | UHD, large clip files (~370 MiB each)        |
| B    | 1920 × 1088 | HD, the most common production resolution    |
| C    | 960 × 544   | Quarter-HD                                   |
| D    | 480 × 272   | Mobile / preview tier, fastest to iterate on |

Content categories include high-motion sports, urban / architectural
walks, natural scenes, and a handful of texture-heavy clips. Compared
to the Netflix Public drop (9 sources, mostly cinematic film_drama
plus two sports / two wildlife), BVI-DVC widens content diversity —
particularly on high-motion and texture-heavy material that the
Netflix drop under-represents.

Citation:

> Di Ma, Fan Zhang, David R. Bull. *BVI-DVC: A Training Database for
> Deep Video Compression*. IEEE Transactions on Multimedia, 2021.

## 2. Where to download

BVI-DVC is distributed by the University of Bristol via Zenodo /
the lab's research-data portal. **The fork does not redistribute
the corpus** (license is research-only — see
[Research-0082](../research/0082-bvi-dvc-corpus-feasibility.md) §2).
Obtain it from the upstream source and place it locally:

```text
.workingdir2/BVI-DVC Part 1.zip   # original archive (~84 GiB)
.workingdir2/bvi-dvc-extracted/   # gitignored extraction target
```

Both paths are gitignored. The repo never commits BVI-DVC YUV / MP4
data, the extracted parquet, or any cached `vmaf` JSON — only derived
training weights and scripts ship in-tree. This is the same posture
the fork already takes for the Netflix Public drop (ADR-0203).

### 2.1 If you already have the YUVs extracted

If you have already extracted the archive (e.g. a directory of raw
`.yuv` files or the local lossless `.mkv` bundle), pass `--bvi-dir`
instead of `--bvi-zip` to skip the streaming-extraction step entirely:

```bash
python ai/scripts/bvi_dvc_to_full_features.py \
    --bvi-dir /path/to/bvi-dvc-extracted \
    --tier D \
    --vmaf-bin core/build-cpu/tools/vmaf \
    --out runs/full_features_bvi_dvc_D.parquet
```

`--bvi-dir` and `--bvi-zip` are mutually exclusive. Omitting both falls
back to `--bvi-zip` using the default path or the `$VMAF_BVI_DVC_ZIP`
environment variable. See [ADR-0527](../adr/0527-bvi-dvc-pre-extracted-dir-input.md)
for the design rationale.

Accepted file types in `--bvi-dir` mode:

| Suffix | Behaviour |
|--------|-----------|
| `.yuv` | Used directly as the reference; width, height, fps, and bit-depth are parsed from the filename. No intermediate decode step. |
| `.mp4` | Decoded to raw YUV via ffmpeg (same as the zip path). |
| `.mkv` | Decoded to raw YUV via ffmpeg (same path as `.mp4`; used by the local lossless bundle). |

Files that do not match the BVI-DVC naming convention
(`<Stem>_<W>x<H>_<fps>fps_<depth>bit_420.<ext>`) are skipped with a
warning. Files whose resolution does not map to one of the four
canonical tiers (A/B/C/D) are also skipped with a warning.

## 3. Pipeline

The end-to-end ingestion is two stages:

```text
   .workingdir2/BVI-DVC Part 1.zip    OR    /path/to/bvi-dvc-extracted/
              │                                          │
              │  --bvi-zip (default)                    │  --bvi-dir (ADR-0527)
              └──────────────────────┬──────────────────┘
                                     │  (1) feature parquet
                                     ▼
              ai/scripts/bvi_dvc_to_full_features.py
                                     │      → runs/full_features_bvi_dvc_<tier>.parquet
                                     │
                                     │  (2) corpus JSONL (fr_regressor_v2 schema)
                                     ▼
              ai/scripts/bvi_dvc_to_corpus_jsonl.py
                                     │      → runs/bvi_dvc_corpus.jsonl
                                     │
                                     │  (3) merge with Netflix shard
                                     ▼
              ai/scripts/merge_corpora.py
                                           → runs/fr_v2_train_corpus.jsonl
```

Stage (1) is the **per-frame feature parquet** consumed by the
`vmaf_tiny_v*` and `fr_regressor_v1` trainers. It runs libvmaf with
the current `FULL_FEATURES` pool (25 feature columns as of the SpEED
chroma/temporal refresh) and writes one parquet row per frame.
This stage already existed in tree; ADR-0310 added the downstream
corpus-JSONL reshape.
The stage also writes `runs/full_features_bvi_dvc_<tier>.manifest.json`
by default, or a caller-selected path via `--manifest-out`. That sidecar
records whether the run used `--bvi-zip` or `--bvi-dir`, the tier,
cache/model/vmaf inputs, CRF/codec recipe, selected clip count, emitted
row/column counts, feature order, extractor list, and ADR-0661
`run_provenance`. Keep it beside the local parquet so later training
refreshes can prove which BVI-DVC material was scored.

Stage (2) is **new in ADR-0310**. It re-shapes the BVI-DVC encodes
into the vmaf-tune Phase A corpus row schema
([`CORPUS_ROW_KEYS`](../../tools/vmaf-tune/src/vmaftune/__init__.py))
that `fr_regressor_v2` consumes. One JSONL row per `(source,
preset, CRF)` tuple, mirroring what `vmaf-tune corpus` would emit
if it had a BVI-DVC adapter.

Stage (3) is the merge utility added by ADR-0310. It de-duplicates
by `(src_sha256, encoder, preset, crf)` so re-runs and overlap with
other corpora cannot inflate the training set.

## 4. Run command

Once the parquet exists (stage 1) and the JSONL adapter has produced
`runs/bvi_dvc_corpus.jsonl` (stage 2), merge with the Netflix shard:

```bash
python ai/scripts/merge_corpora.py \
    --inputs runs/netflix_corpus.jsonl runs/bvi_dvc_corpus.jsonl \
    --output runs/fr_v2_train_corpus.jsonl
```

The summary line lands on stderr:

```text
[merge_corpora] rows_in=N rows_out=M duplicates=K unique_sources=S \
    -> runs/fr_v2_train_corpus.jsonl
```

The trainer then consumes the merged JSONL exactly as it consumes a
single-source corpus today:

```bash
python ai/scripts/train_fr_regressor_v2.py \
    --corpus runs/fr_v2_train_corpus.jsonl \
    --epochs 200 --seed 0
```

## 5. Expected impact on fr_regressor_v2

The Netflix-only LOSO baseline (see
[ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md)) leaves
9 folds × ~24 rows / fold (216 rows total). Adding BVI-DVC's tier-D
clips (~120 sources) roughly **triples** the training corpus and
expands the LOSO partition from 9 source-folds to 9 + N folds, where
N is the number of BVI-DVC sources retained after dedup.

LOSO methodology is unchanged: each fold holds out one source, trains
on the remainder, and reports per-fold PLCC / SROCC / RMSE against the
`vmaf_v0.6.1` per-frame teacher. Aggregate quality is the mean ± std
across folds. This is the same gate
[ADR-0303](../adr/0303-fr-regressor-v2-ensemble-prod-flip.md) uses for the
production-flip decision.

Ship-gate posture: a corpus expansion that **does not raise** mean
LOSO PLCC by at least one σ above the Netflix-only baseline is not
shipped to production weights. The inclusion criterion is empirical;
ADR-0310's decision is to make the corpus available for training, not
to commit ahead of measurement to a production weights flip.

## 6. Reproducibility

Every step is deterministic given the same input archive. The
feature-extraction stage caches per-clip libvmaf JSON under
`$VMAF_TINY_AI_CACHE_BVI_DVC_FULL` (default
`~/.cache/vmaf-tiny-ai-bvi-dvc-full/`); re-runs with the same archive
hit the cache. The corpus-JSONL stage and the merge are pure
transforms with no hidden state.

CI cannot retrain end-to-end (the corpus is non-redistributable); the
`merge_corpora` smoke test under `ai/tests/test_merge_corpora.py`
covers the schema contract on synthetic fixture rows and runs in
under one second.
