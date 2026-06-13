# Saliency Feature Materializer

`ai/scripts/materialize_saliency_features.py` appends saliency aggregates to an
existing AI feature table. It reads `.jsonl` or `.parquet`, resolves one source
clip per row, decodes a bounded sample through FFmpeg, runs the fork saliency
helper, and writes `saliency_mean` / `saliency_var` plus an optional status
column.

Use it before retraining predictor or MOS-head models when the current table
has placeholder or missing saliency columns.

## Usage

```bash
PYTHONPATH=. .venv/bin/python ai/scripts/materialize_saliency_features.py \
  --input runs/full_features_chug_hdr.jsonl \
  --output runs/full_features_chug_hdr_saliency.jsonl \
  --root .corpus/chug \
  --path-column src \
  --model-id saliency_student_v2 \
  --temporal-aggregator ema \
  --ema-alpha 0.6 \
  --max-frames 8 \
  --frame-samples 8 \
  --audit-json runs/full_features_chug_hdr_saliency.audit.json
```

The same command works for parquet by using `.parquet` input and output paths.
Parquet support uses the local pandas/pyarrow stack; JSONL only needs the
standard library plus the saliency runtime dependencies.

`--temporal-aggregator` matches the `vmaf-tune` saliency reducers:
`mean`, `ema`, `max`, or `motion-weighted`. Use `mean` for the historical
clip average, `ema` when later frames should dominate but earlier frames still
matter, `max` when any salient frame should mark the clip, and
`motion-weighted` for a cheap video-saliency proxy that weights changing frames
more heavily. `--ema-alpha` controls the current-frame weight for `ema`.

`--model-id` records the model identity used for the run. It defaults to
`saliency_student_v1` when `--model-path` is omitted, or to the model-path stem
when a custom ONNX is supplied. Pass explicit ids such as
`saliency_student_v2` or `u2netp_mirror_v1` when comparing model families.

`--audit-json` writes row counters, the effective materializer config, and
ADR-0661 `run_provenance` for the input table, optional root/model path, output
table target, and audit target. Use it for any saliency-enriched table that
feeds retraining or signal-mix comparisons.

## Row Contract

Default input columns:

| Column | Meaning |
| --- | --- |
| `src` | Absolute source path, or a path relative to `--root`. Override with `--path-column`. |
| `width` | Source width in pixels. Missing or invalid values fall back to ffprobe, then to `--default-width`. |
| `height` | Source height in pixels. Missing or invalid values fall back to ffprobe, then to `--default-height`. |

**Raw YUV corpora** (`*.yuv`) need two extra flags to decode correctly:

- `--default-width` / `--default-height`: used when the feature table has no
  geometry columns and ffprobe cannot probe raw YUV files. For the Netflix
  Public corpus all distorted files are stored at their reference resolution
  (1920×1080) regardless of the encode-ladder height in the filename; pass
  `--default-width 1920 --default-height 1080`.
- When the source extension is `.yuv`, the materializer automatically prepends
  `-f rawvideo -video_size WxH -pix_fmt yuv420p` before `-i` so ffmpeg can
  decode the raw bitstream.

**Per-frame tables** (e.g. the Netflix refresh parquet — one row per frame per
clip) are handled efficiently: saliency is computed once per unique source file
and re-used for all rows that reference the same file, avoiding redundant decodes.

Output columns:

| Column | Meaning |
| --- | --- |
| `saliency_mean` | Mean value of the returned saliency mask. |
| `saliency_var` | Variance of the returned saliency mask. |
| `saliency_status` | Row status. Disable with `--status-column ""`. |
| `saliency_model_id` | Model id recorded for rows materialized in this run. Disable with `--model-id-column ""`. |
| `saliency_aggregator` | Temporal reducer used for rows materialized in this run. Disable with `--aggregator-column ""`. |
| `saliency_ema_alpha` | EMA alpha recorded for rows materialized in this run. Disable with `--ema-alpha-column ""`. |

Rows that already contain finite `saliency_mean` and `saliency_var` are skipped
unless `--overwrite` is set. Skipped rows keep their existing metadata; the
materializer does not invent a model id for older saliency columns whose origin
is unknown.

## Status Values

| Status | Meaning |
| --- | --- |
| `ok` | Row decoded and saliency aggregates were written. |
| `skipped-existing` | Existing finite saliency columns were preserved. |
| `missing-source` | The configured path column was empty or did not resolve to a file. |
| `missing-geometry` | Geometry was absent and ffprobe could not recover it. |
| `decode-failed` | FFmpeg did not produce the temporary raw `yuv420p` sample. |
| `model-failed` | The saliency helper raised an error. |

The process exits `0` when all rows are `ok` or `skipped-existing`; it exits
`1` when any row failed.

## Batch Manifest

Use `ai/scripts/batch_materialize_saliency_features.py` when a refresh run needs
the same saliency model applied to several tables. Paths in the manifest are
relative to the manifest file by default; pass `--base-dir` when the manifest
is stored away from the run directory.

```json
{
  "defaults": {
    "model_id": "saliency_student_v2",
    "temporal_aggregator": "ema",
    "ema_alpha": 0.6,
    "max_frames": 8,
    "frame_samples": 8
  },
  "tables": [
    {
      "id": "chug_hdr",
      "input": "runs/full_features_chug_hdr.parquet",
      "output": "runs/full_features_chug_hdr.saliency.parquet",
      "audit_json": "runs/full_features_chug_hdr.saliency.audit.json",
      "root": ".corpus/chug"
    },
    {
      "id": "konvid",
      "input": "runs/full_features_konvid_refresh_20260520.parquet",
      "output": "runs/full_features_konvid_refresh_20260520.saliency.parquet"
    },
    {
      "id": "netflix",
      "input": "runs/full_features_netflix_refresh_20260520.parquet",
      "output": "runs/full_features_netflix_refresh_20260520.saliency.parquet",
      "path_column": "dis_basename",
      "root": ".corpus/netflix/dis",
      "default_width": 1920,
      "default_height": 1080
    }
  ]
}
```

**Netflix corpus note**: The Netflix refresh parquet uses `dis_basename`
(not `src`) as the path column, and its distorted YUVs live under
`.corpus/netflix/dis/`. All files are 1920×1080 raw YUV regardless of the
encode-ladder height in the filename, so `default_width: 1920` and
`default_height: 1080` must be set. The materializer caches saliency per
unique file, processing each of the 70 unique clips once instead of ~160 times.

```bash
PYTHONPATH=. .venv/bin/python ai/scripts/batch_materialize_saliency_features.py \
  --manifest runs/saliency-batch.json \
  --report-json runs/saliency-batch.report.json \
  --report-md runs/saliency-batch.report.md
```

Each table may override any single-run materializer option from the defaults.
The batch report uses schema `saliency-materializer-batch-v1` and carries
ADR-0661 `run_provenance`; per-table `audit_json` files retain the effective
config and row counters for the table. The batch command exits non-zero if any
table has failed rows. Use `--allow-row-failures` only for exploratory audits
where partial saliency coverage is intentionally kept for later filtering.

## Corpus-Specific Manifests

In-tree batch manifests live under `ai/batch-manifests/saliency/` (ADR-0993).
Each file documents the correct `path_column`, `root`, and any geometry fallback
for that corpus.

### KoNViD-150K

The `konvid_150k.jsonl` corpus table has `src` (relative clip filename),
`width`, and `height` columns. Clips live at
`.corpus/konvid-150k/k150ka_extracted/`. The manifest is fully wired:

```bash
PYTHONPATH=ai/scripts:ai/src:tools/vmaf-tune/src \
  python3 ai/scripts/batch_materialize_saliency_features.py \
  --manifest ai/batch-manifests/saliency/konvid-150k.json \
  --report-json .workingdir2/saliency-runs/konvid-150k/report.json \
  --report-md .workingdir2/saliency-runs/konvid-150k/report.md
```

Smoke run (10 rows only, requires `head -10` truncation of the JSONL input):

```bash
head -10 .corpus/konvid-150k/konvid_150k.jsonl > /tmp/k150k_smoke.jsonl
PYTHONPATH=ai/scripts:ai/src:tools/vmaf-tune/src \
  python3 ai/scripts/materialize_saliency_features.py \
  --input /tmp/k150k_smoke.jsonl \
  --output /tmp/k150k_smoke_saliency.jsonl \
  --root .corpus/konvid-150k/k150ka_extracted \
  --path-column src --width-column width --height-column height \
  --model-id saliency_student_v1 --max-frames 4 \
  --audit-json /tmp/k150k_smoke_saliency.audit.json
```

### YouTube UGC

The `full_features_ugc_refresh_20260520.parquet` uses a `source` column with
corpus identifiers (`ugc-Gaming_1080P-223e-cbr`), not file paths. See the
`_status` / `_resolution` comments in `ai/batch-manifests/saliency/ugc.json`
for the two unblocking options. The recommended path is to generate a
`ugc_corpus.jsonl` via `ai/scripts/youtube_ugc_to_corpus_jsonl.py` first.

### BVI-DVC

The `full_features_bvi_dvc_D_refresh_20260520.parquet` uses a `key` column
with encode parameters (`DAdvertisingMassagesBangkokVidevo_480x272_25fps_10bit_420`).
Raw reference YUVs at `.corpus/bvi-dvc-raw/` are the natural saliency source.
The manifest in `ai/batch-manifests/saliency/bvi-dvc.json` documents the
geometry mapping and `default_width`/`default_height` values needed for the
3840×2176 raw YUV sources.

## Reproducer

```bash
PYTHONPATH=. .venv/bin/python -m pytest ai/tests/test_materialize_saliency_features.py -q
PYTHONPATH=. .venv/bin/python -m pytest ai/tests/test_batch_materialize_saliency_features.py -q
```
