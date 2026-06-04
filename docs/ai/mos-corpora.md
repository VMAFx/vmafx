<!-- markdownlint-disable MD013 MD060 -->
# MOS-corpus ingestion family

The VMAFx fork trains no-reference and mixed-reference VQA models against
human Mean Opinion Score labels. Several public video-quality corpora are
supported. Each corpus ships its own adapter script that produces a
**corpus JSONL** shard; the shards are then unified via
`ai/scripts/aggregate_corpora.py` before the trainer consumes them.

This page is the index for the entire family. Follow the per-corpus links
for acquisition steps, operator flags, and schema details.

## Available corpora

| Corpus | Clips | MOS scale | Size (approx.) | Adapter script | Per-corpus doc |
|--------|------:|-----------|----------------|----------------|----------------|
| KonViD-1k | 1 200 | 1–5 ACR Likert | ~2.3 GB | `ai/scripts/konvid_1k_to_corpus_jsonl.py` | [konvid-1k-ingestion.md](konvid-1k-ingestion.md) |
| KonViD-150k | ~150 000 | 1–5 ACR Likert | ~120–200 GB | `ai/scripts/konvid_150k_to_corpus_jsonl.py` | [konvid-150k-ingestion.md](konvid-150k-ingestion.md) |
| LSVQ | ~39 000 | 1–5 ACR Likert | ~500 GB (whole) | `ai/scripts/lsvq_to_corpus_jsonl.py` | [lsvq-ingestion.md](lsvq-ingestion.md) |
| YouTube UGC | ~1 500 | 1–5 ACR Likert | ~2 TB (whole) | `ai/scripts/youtube_ugc_to_corpus_jsonl.py` | [youtube-ugc-ingestion.md](youtube-ugc-ingestion.md) |
| Waterloo IVC 4K-VQA | 1 200 | 0–100 continuous | multi-TB (whole) | `ai/scripts/waterloo_ivc_to_corpus_jsonl.py` | [waterloo-ivc-4k-ingestion.md](waterloo-ivc-4k-ingestion.md) |
| LIVE-VQC | 585 | 0–100 continuous | ~few GB | `ai/scripts/live_vqc_to_corpus_jsonl.py` | [live-vqc-ingestion.md](live-vqc-ingestion.md) |
| CHUG UGC-HDR | 5 992 | 0–100 continuous, mapped to 1–5 at ingest | tens of GB | `ai/scripts/chug_to_corpus_jsonl.py` | [chug-ingestion.md](chug-ingestion.md) |
| BVI-DVC (no-MOS FR shard) | ~120+ | n/a — no human MOS | ~84 GiB archive | `ai/scripts/bvi_dvc_to_corpus_jsonl.py` | [bvi-dvc-corpus-ingestion.md](bvi-dvc-corpus-ingestion.md) |

BVI-DVC is a reference-only corpus without human MOS labels. It feeds the
`fr_regressor_v2` encode-grid trainer via `ai/scripts/merge_corpora.py`
rather than the MOS-aggregation path; it is listed here for completeness
because `merge_corpora.py` and `aggregate_corpora.py` are sibling utilities
(see [multi-corpus-aggregation.md](multi-corpus-aggregation.md) §3).

### Citations

| Corpus | Citation |
|--------|---------|
| KonViD-1k | Hosu, Hahn, Jenadeleh, Lin, Men, Szirányi, Li, Saupe. *The Konstanz natural video database (KoNViD-1k).* QoMEX 2017. <http://database.mmsp-kn.de> |
| KonViD-150k | Götz-Hahn, Hosu, Lin, Saupe. *KonVid-150k: A Dataset for No-Reference Video Quality Assessment of Videos in-the-Wild.* IEEE Access 2021. <https://database.mmsp-kn.de/konvid-150k-vqa-database.html> |
| LSVQ | Ying, Mandal, Ghadiyaram, Bovik. *Patch-Based No-Reference Image and Video Quality Assessment.* ICCV 2021. <https://github.com/baidut/PatchVQ> |
| YouTube UGC | Wang, Inguva, Adsumilli. *YouTube UGC Dataset for Video Compression Research.* MMSP 2019. <https://research.google/pubs/youtube-ugc-dataset-for-video-compression-research/> |
| Waterloo IVC 4K-VQA | Li, Duanmu, Liu, Wang. *4K-VQA: A 4K Video Quality Assessment Database.* ICIAR 2019. <https://ivc.uwaterloo.ca/database/4KVQA.html> |
| LIVE-VQC | Sinno, Bovik. *Large-Scale Study of Perceptual Video Quality.* IEEE TIP 2019. <https://live.ece.utexas.edu/research/LIVEVQC/> |
| CHUG | Saini, Bovik, Birkbeck, Wang, Adsumilli. *CHUG: Crowdsourced User-Generated HDR Video Quality Dataset.* ICIP 2025. <https://doi.org/10.1109/ICIP55913.2025.11084488> |
| BVI-DVC | Ma, Zhang, Bull. *BVI-DVC: A Training Database for Deep Video Compression.* IEEE TMM 2021. |

## Output schema — corpus JSONL

Every adapter produces one JSON object per line. The fields below are
present in every shard output by the corpora above:

```jsonc
{
  "src":               "clip_or_basename.mp4",   // filename within the dataset
  "src_sha256":        "<64-hex>",               // SHA-256 of clip bytes, 1 MiB chunks
  "src_size_bytes":    1234567,
  "width":             1920,
  "height":            1080,
  "framerate":         30.0,
  "duration_s":        8.0,
  "pix_fmt":           "yuv420p",
  "encoder_upstream":  "h264",                   // codec reported by ffprobe
  "mos":               3.42,                     // native-scale MOS (NOT normalised here)
  "mos_std_dev":       0.51,                     // inter-rater std; 0.0 if unpublished
  "n_ratings":         50,                       // number of crowdworker ratings
  "corpus":            "konvid-1k",              // stable corpus label
  "corpus_version":    "konvid-1k-2017",         // dataset release identifier
  "ingested_at_utc":   "2026-05-08T12:00:00+00:00"
}
```

Key invariants:

- `src_sha256` is the deduplication key across corpora. It is computed by
  the adapter, never taken from the dataset's own metadata.
- `mos` is recorded **verbatim** from the dataset's native scale at ingest
  time — normalisation to a unified axis is the aggregator's job.
- `mos_std_dev` of `0.0` signals that the dataset did not publish
  inter-rater spread (distinct from a real zero spread).
- The schema is **disjoint** from the vmaf-tune Phase A `CORPUS_ROW_KEYS`
  row (no `vmaf_score`, `encoder`, `preset`, `crf`). Mixing them requires
  the appropriate merge utility — see §Combining corpora below.

## Combining corpora

### MOS-labelled corpora → unified training JSONL

Use `ai/scripts/aggregate_corpora.py` (PR #518, [ADR-0340](../adr/0340-multi-corpus-aggregation.md)).
It normalises each shard to a common 0–100 axis, deduplicates by
`src_sha256`, and emits a unified JSONL the v2 trainer can consume directly:

```bash
python ai/scripts/aggregate_corpora.py \
    --inputs .workingdir2/konvid-150k/konvid_150k.jsonl \
             .workingdir2/lsvq/lsvq.jsonl \
             .workingdir2/waterloo-ivc-4k/waterloo_ivc_4k.jsonl \
             .workingdir2/youtube-ugc/youtube_ugc.jsonl \
    --output .workingdir2/aggregated/unified_corpus.jsonl
```

The command writes `.workingdir2/aggregated/unified_corpus.manifest.json` by
default. That manifest records source-shard hashes, MOS scale conversions,
dedup counters, corpus-source overrides, and ADR-0661 `run_provenance`.
Use `--manifest-out PATH` when the JSONL and manifest need to live in a
separate experiment bundle.

Each source adapter writes the same kind of replay sidecar before aggregation.
By default `chug_to_corpus_jsonl.py`, `konvid_1k_to_corpus_jsonl.py`,
`konvid_150k_to_corpus_jsonl.py`, `youtube_ugc_to_corpus_jsonl.py`,
`lsvq_to_corpus_jsonl.py`, `live_vqc_to_corpus_jsonl.py`, and
`waterloo_ivc_to_corpus_jsonl.py` write `<output>.manifest.json`; pass
`--manifest-out PATH` when storing a dated experiment bundle. The source
manifests record the corpus root, manifest CSV, progress path, row caps,
written/skipped/dedup counters, and ADR-0661 `run_provenance`.

For a one-command discover-then-train workflow:

```bash
bash ai/scripts/run_aggregated_training.sh
```

See [multi-corpus-aggregation.md](multi-corpus-aggregation.md) for the
full scale-conversion table, dedup policy, and failure-mode reference.

### Feature tables + MOS labels → MOS-head parquet

Real MOS-head training tables must already carry `mos` or `mos_raw_0_100`.
When a refreshed feature parquet only contains extracted metrics, join the
labels first with `ai/scripts/materialize_mos_labels.py`:

```bash
.venv/bin/python ai/scripts/materialize_mos_labels.py \
    --features runs/full_features_konvid_refresh_20260520_with_folds.parquet \
    --labels .corpus/konvid-150k/konvid_150k.jsonl \
    --feature-key-column key \
    --label-key-column src \
    --feature-key-regex '([0-9]{6,})' \
    --label-key-regex '([0-9]{6,})' \
    --out runs/full_features_konvid_refresh_20260520_with_mos.parquet \
    --audit-json runs/full_features_konvid_refresh_20260520_with_mos.audit.json
```

The materializer fails by default below 95% unique-key coverage and refuses to
overwrite existing MOS columns unless `--overwrite` is passed. See
[mos-label-materializer.md](mos-label-materializer.md) for the key-matching
and audit schema.

### Encode-grid corpora (Netflix + BVI-DVC) → FR-regressor JSONL

Use `ai/scripts/merge_corpora.py` (PR #310).
This utility operates on the vmaf-tune Phase A `CORPUS_ROW_KEYS` schema
and deduplicates by `(src_sha256, encoder, preset, crf)`:

```bash
python ai/scripts/merge_corpora.py \
    --inputs runs/netflix_corpus.jsonl runs/bvi_dvc_corpus.jsonl \
    --output runs/fr_v2_train_corpus.jsonl
```

This command writes `runs/fr_v2_train_corpus.manifest.json` by default with
the input shard hashes, required vmaf-tune corpus keys, the natural dedup key
`(src_sha256, encoder, preset, crf)`, summary counters, and ADR-0661
`run_provenance`. Pass `--manifest-out PATH` to place the sidecar elsewhere.
The older BVI-DVC JSONL adapter follows the same convention: direct
`bvi_dvc_to_corpus_jsonl.py` runs write `<output>.manifest.json` with cache
inputs, row counts, adapter labels, row schema version, and `run_provenance`.
It emits current vmaf-tune v3 additive columns with explicit unavailable
defaults for HDR, shot, canonical-feature aggregates, and encoder-internal
signals when those fields are not present in the cached libvmaf JSON.

## Shared ingestion infrastructure (ADR-0371)

All MOS-corpus adapter scripts share a common base class defined in
`ai/src/corpus/base.py` (`PYTHONPATH=ai/src`). The base class provides:

- **`sha256_file(path)`** — SHA-256 computed in 1 MiB chunks (dedup key).
- **`probe_geometry(clip_path, ...)`** — ffprobe wrapper; returns a dict
  with `width`, `height`, `framerate`, `duration_s`, `pix_fmt`,
  `encoder_upstream`, or `None` on probe failure. Injected via the
  `runner` kwarg for unit tests.
- **`load_progress` / `save_progress` / `mark_done` / `mark_failed` /
  `should_attempt`** — atomic tempfile-rename progress state (JSON) so
  multi-hour runs are safe to Ctrl-C and resume.
- **`read_sha_index(jsonl_path)`** — builds a `set[str]` of
  already-ingested `src_sha256` values from a partially-written JSONL
  so re-runs skip duplicates.
- **`download_clip(...)`** — curl-based download with configurable
  timeout, returning `(ok, reason)`. Also injectable via `runner`.
- **`RunStats`** — dataclass accumulating `written`,
  `skipped_download`, `skipped_broken`, `dedups` with a computed
  `attrition_pct`.
- **`CorpusIngestBase`** — abstract base class. Subclass, set
  `corpus_label`, implement
  `iter_source_rows(clips_dir) -> Iterator[(clip_path, row_dict)]`,
  and call `ingest.run()`.

Adding a new MOS corpus:

```python
# ai/scripts/my_corpus_to_corpus_jsonl.py
from corpus.base import CorpusIngestBase, utc_now_iso

class MyCorpusIngest(CorpusIngestBase):
    corpus_label = "my-corpus"

    def iter_source_rows(self, clips_dir):
        for row in parse_my_csv(...):
            yield clips_dir / row["filename"], row
```

See [ADR-0371](../adr/0371-corpus-ingest-base-class.md) and the unit tests at
`ai/tests/test_corpus_base.py` for the full contract.

## Per-corpus quick-start commands

### KonViD-1k (1 200 clips, ~2.3 GB, ~5 min)

```bash
# 1. Fetch + extract (idempotent — skips completed files).
python ai/scripts/fetch_konvid_1k.py
#    → ~/datasets/konvid-1k/KoNViD_1k_videos/  (default location)
#    → ~/datasets/konvid-1k/fetch_manifest.json

# 2. Convert to JSONL.
python ai/scripts/konvid_1k_to_corpus_jsonl.py
#    → .workingdir2/konvid-1k/konvid_1k.jsonl

# Smoke (5 clips only):
python ai/scripts/konvid_1k_to_corpus_jsonl.py --max-rows 5
```

### KonViD-150k (~150 000 clips, ~120–200 GB)

```bash
# Drop manifest.csv first:  https://database.mmsp-kn.de/konvid-150k-vqa-database.html

python ai/scripts/konvid_150k_to_corpus_jsonl.py
#    → .workingdir2/konvid-150k/konvid_150k.jsonl
#    Resumable — safe to Ctrl-C and re-run.

# Smoke (50 clips):
python ai/scripts/konvid_150k_to_corpus_jsonl.py --max-rows 50
```

### LSVQ (~39 000 clips, ~500 GB whole)

```bash
# Drop LSVQ_whole_train CSV at .workingdir2/lsvq/manifest.csv, then:
python ai/scripts/lsvq_to_corpus_jsonl.py          # laptop subset (500 clips)
python ai/scripts/lsvq_to_corpus_jsonl.py --full   # whole corpus
#    → .workingdir2/lsvq/lsvq.jsonl
```

### YouTube UGC (~1 500 clips, ~2 TB whole)

```bash
# Drop original_videos.csv at .workingdir2/youtube-ugc/manifest.csv, then:
python ai/scripts/youtube_ugc_to_corpus_jsonl.py          # laptop subset (300 clips)
python ai/scripts/youtube_ugc_to_corpus_jsonl.py --full   # whole corpus
#    → .workingdir2/youtube-ugc/youtube_ugc.jsonl
```

### Waterloo IVC 4K-VQA (1 200 clips, staged locally)

```bash
# Extract bulk archives to .workingdir2/waterloo-ivc-4k/clips/
# Drop scores.txt at .workingdir2/waterloo-ivc-4k/manifest.csv, then:
python ai/scripts/waterloo_ivc_to_corpus_jsonl.py          # default subset
python ai/scripts/waterloo_ivc_to_corpus_jsonl.py --full   # whole corpus
#    → .workingdir2/waterloo-ivc-4k/waterloo_ivc_4k.jsonl
```

Note: Waterloo IVC 4K-VQA uses a **0–100 continuous scale**, not 1–5 ACR Likert.
The aggregator converts it via identity (no rescaling needed). See
[multi-corpus-aggregation.md](multi-corpus-aggregation.md) §2 for the conversion
table.

### CHUG UGC-HDR (5 992 clips, S3-hosted)

```bash
mkdir -p .workingdir2/chug
curl -L https://raw.githubusercontent.com/shreshthsaini/CHUG/master/chug.csv \
  -o .workingdir2/chug/manifest.csv

PYTHONPATH=ai/src python ai/scripts/chug_to_corpus_jsonl.py          # 500-row subset
PYTHONPATH=ai/src python ai/scripts/chug_to_corpus_jsonl.py --full   # whole corpus
#    → .workingdir2/chug/chug.jsonl
```

CHUG is UGC-HDR and reports MOS on a **0–100 continuous scale**. The
adapter preserves that source value as `mos_raw_0_100` and maps
trainer-facing `mos` onto `[1, 5]` via `1 + 4 * mos_raw_0_100 / 100`
so the existing MOS-head trainer can consume the rows directly. The
adapter also preserves CHUG bitrate-ladder, orientation, manifest
geometry, and content-name metadata under `chug_*` optional fields.

### LIVE-VQC (585 clips, ~few GB)

```bash
# Drop manifest CSV at .workingdir2/live-vqc/manifest.csv
# and clips at .workingdir2/live-vqc/clips/, then:
python ai/scripts/live_vqc_to_corpus_jsonl.py          # laptop subset (200 clips)
python ai/scripts/live_vqc_to_corpus_jsonl.py --full   # whole corpus (585 clips)
#    → .workingdir2/live-vqc/live_vqc.jsonl
```

Note: LIVE-VQC uses a **0–100 continuous scale** (same as Waterloo IVC 4K-VQA).
Obtain the dataset from <https://live.ece.utexas.edu/research/LIVEVQC/>.
Two manifest shapes accepted: headerless `<filename>,<mos>` (minimal MOS export)
or standard named-column CSV. See [live-vqc-ingestion.md](live-vqc-ingestion.md)
for acquisition and operator flag details.

## KonViD MOS head v1

After building a unified JSONL from KonViD-1k and KonViD-150k, the fork can
train a lightweight MLP that maps libvmaf canonical-6 features plus saliency
and TransNet shot-metadata to a scalar MOS prediction in [1, 5]. This is the
`konvid_mos_head_v1` model (PR #491, ADR-0336):

```bash
# Smoke (synthetic corpus — no real data needed, ~30 s):
python ai/scripts/train_konvid_mos_head.py --smoke

# Production (real KonViD JSONL drops on disk):
python ai/scripts/train_konvid_mos_head.py \
    --konvid-1k   .workingdir2/konvid-1k/konvid_1k.jsonl \
    --konvid-150k .workingdir2/konvid-150k/konvid_150k.jsonl
#    → model/konvid_mos_head_v1.onnx
#    → model/konvid_mos_head_v1.json  (manifest sidecar)

# Production from refreshed feature parquet, after MOS materialisation:
python ai/scripts/train_konvid_mos_head.py \
    --konvid-1k /tmp/no-konvid-1k.jsonl \
    --konvid-150k /tmp/no-konvid-150k.jsonl \
    --feature-parquet runs/full_features_konvid_refresh_20260520_with_mos.parquet
```

Real mode fails when the input paths yield zero MOS-labelled rows. Use
`--smoke` for synthetic pipeline checks; use the MOS label materializer for
real feature tables that do not yet carry `mos`.

CHUG HDR MOS training is a separate local experiment because CHUG is HDR
subjective-MOS data and the current Netflix VMAF teacher is SDR/8-bit.
The CHUG wrapper defaults to the `chug-hdr-wide-v1` feature schema, which
uses the CHUG temporal quantiles/std columns and HDR ladder metadata in
addition to the canonical-6 feature means:

```bash
python ai/scripts/train_chug_hdr_mos_head.py
#    → .workingdir2/chug/chug_hdr_mos_head_v1.onnx
#    → .workingdir2/chug/chug_hdr_mos_head_v1.json
```

To train against a target HDR display instead of treating MOS as
display-invariant, provide a display profile:

```bash
python ai/scripts/train_chug_hdr_mos_head.py \
  --display-profile-json .workingdir2/chug/display-profile.json
```

With that flag and no explicit `--feature-schema`, the wrapper uses
`chug-hdr-display-v1`, a 45-column schema that appends normalized panel
and viewing-context features to `chug-hdr-wide-v1`. The manifest records
the normalized display profile and the source JSON sha256.

For an apples-to-apples ablation against the older 11-feature baseline,
add `--feature-schema konvid-v1`. Do not use that ablation as the
default CHUG command; it discards CHUG-specific signal that was already
materialised by the extractor.

See [models/konvid_mos_head_v1.md](models/konvid_mos_head_v1.md) for the
full model card (architecture, I/O contract, production-flip gate, and
predictor integration).

## License and redistribution posture

The fork ships **adapter scripts and schemas only**. No corpus clips, no
per-clip MOS values, and no derived feature caches are committed. Only the
trained ONNX weights (derived from the corpora) are redistributable, with
attribution following the source licence:

| Corpus | Licence |
|--------|---------|
| KonViD-1k | Research-use, citation required |
| KonViD-150k | Research-use, citation required |
| LSVQ | CC-BY-4.0 |
| YouTube UGC | Creative Commons Attribution |
| Waterloo IVC 4K-VQA | Permissive academic, attribution required |
| LIVE-VQC | Research-use, attribution required |
| CHUG UGC-HDR | CC BY-NC / CC BY-NC-SA mismatch; treat as non-commercial/share-alike until clarified |
| BVI-DVC | Research-use (non-redistributable) |

For the full licence analysis per corpus see the respective ADR:
[ADR-0325](../adr/0325-konvid-150k-corpus-ingestion.md) (KonViD),
[ADR-0333](../adr/0333-lsvq-corpus-ingestion.md) (LSVQ),
[ADR-0334 / ADR-0368](../adr/0368-youtube-ugc-corpus-ingestion.md) (YouTube UGC),
[ADR-0369](../adr/0369-waterloo-ivc-4k-corpus-ingestion.md) (Waterloo IVC),
[ADR-0370](../adr/0370-live-vqc-corpus-ingestion.md) (LIVE-VQC),
[ADR-0426](../adr/0426-chug-hdr-corpus-ingestion.md) (CHUG),
[ADR-0310](../adr/0310-bvi-dvc-corpus-ingestion.md) (BVI-DVC).

## Related

- [multi-corpus-aggregation.md](multi-corpus-aggregation.md) — unified-scale aggregation
- [bvi-dvc-corpus-ingestion.md](bvi-dvc-corpus-ingestion.md) — encode-grid shard (no MOS)
- [models/konvid_mos_head_v1.md](models/konvid_mos_head_v1.md) — trained MOS-prediction model
- [training-data.md](training-data.md) — Netflix Public corpus (FR training shard)
- [ADR-0340](../adr/0340-multi-corpus-aggregation.md) — aggregation decision record
