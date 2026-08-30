<!-- markdownlint-disable MD060 -->
# MOS Label Materializer

`ai/scripts/materialize_mos_labels.py` joins subjective MOS labels onto
already-extracted feature tables. Use it before a real MOS-head training run
when the feature table has metrics but not `mos` / `mos_raw_0_100` columns.

The script is table-side only. It does not download corpora, extract VMAF
features, or train a model.

## Inputs

Feature tables may be parquet, JSONL/NDJSON, or JSON objects with `rows`.
Label tables may be parquet, JSONL/NDJSON, JSON objects with `rows`, or CSV.

Both sides need a stable clip key. The script can infer common key columns
such as `key`, `video_id`, `clip_name`, `filename`, `src`, or `path`; pass
explicit columns when the table is ambiguous.

MOS labels may be stored as either:

| Column | Scale | Output |
|---|---|---|
| `mos` | 1-5 | copied to `mos`, mapped to `mos_raw_0_100` |
| `mos_raw_0_100` | 0-100 | mapped to `mos`, copied to `mos_raw_0_100` |

Optional label metadata such as `mos_std_dev`, `n_ratings`, `split`,
`corpus`, and `corpus_version` is copied onto matching feature rows.

## Example

Join KoNViD-150K labels onto a refreshed full-feature parquet whose feature
keys and source filenames both contain the numeric clip id:

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

The default `--min-match-rate 0.95` is promotion-oriented: it fails the run if
fewer than 95% of unique feature keys receive a label. Lower it only for
exploratory audits where missing rows are expected and visible in
`mos_label_status`.

When `--audit-json` is set, the audit file includes ADR-0661
`run_provenance` with the script entrypoint, argv, parsed arguments, feature
table, label table inputs, output table target, and audit target. Keep that
audit next to refreshed training tables so MOS-head training evidence can be
reproduced without shell history.

## Batch Manifest

Use `ai/scripts/batch_materialize_mos_labels.py` when several refreshed
feature tables need the same label-join policy. Paths in the manifest are
relative to the manifest file unless `--base-dir` is supplied.

### Corpus-specific manifests

The repo ships ready-to-use batch manifests under `ai/configs/`:

| Manifest | Corpora | Key schema |
|---|---|---|
| `ai/configs/mos-label-batch-konvid.json` | KonViD-1k, KonViD-150k | `key` (feature) ↔ `src` (label); 6+ digit numeric regex |
| `ai/configs/mos-label-batch-chug.json` | CHUG UGC-HDR | `chug_video_id` on both sides; `key_normalize: raw` |

Run the KonViD batch (after feature extraction and corpus JSONL ingestion):

```bash
.venv/bin/python ai/scripts/batch_materialize_mos_labels.py \
  --manifest ai/configs/mos-label-batch-konvid.json \
  --report-json .workingdir2/konvid-mos-batch.report.json \
  --report-md .workingdir2/konvid-mos-batch.report.md
```

Run the CHUG batch:

```bash
.venv/bin/python ai/scripts/batch_materialize_mos_labels.py \
  --manifest ai/configs/mos-label-batch-chug.json \
  --report-json .workingdir2/chug-mos-batch.report.json \
  --report-md .workingdir2/chug-mos-batch.report.md
```

Paths in the shipped manifests follow the default layout from
`konvid_to_full_features.py` (`runs/`) and `chug_extract_features.py`
(`.corpus/chug/` for input, `.workingdir2/chug/runs/` for output). Pass
`--base-dir` to override the relative-path root for non-default layouts.

### Custom manifest

```json
{
  "defaults": {
    "min_match_rate": 0.95,
    "key_normalize": "auto"
  },
  "tables": [
    {
      "id": "konvid",
      "features": "runs/full_features_konvid_refresh.parquet",
      "labels": [".corpus/konvid-150k/konvid_150k.jsonl"],
      "feature_key_column": "key",
      "label_key_column": "src",
      "feature_key_regex": "([0-9]{6,})",
      "label_key_regex": "([0-9]{6,})",
      "out": "runs/full_features_konvid_refresh_with_mos.parquet",
      "audit_json": "runs/full_features_konvid_refresh_with_mos.audit.json"
    }
  ]
}
```

Each table may override any single-run join option from `defaults`, including
`feature_key_column`, `label_key_column`, `label_mos_column`, `key_normalize`,
`feature_key_regex`, `label_key_regex`, `min_match_rate`, `status_column`, and
`overwrite`. The batch report uses schema `mos-label-materializer-batch-v1` and
carries ADR-0661 `run_provenance`.

## Output Columns

| Column | Meaning |
|---|---|
| `mos` | Subjective MOS on the 1-5 training scale. |
| `mos_raw_0_100` | Same MOS on a 0-100 scale for cross-corpus audits. |
| `mos_label_status` | `ok` or `missing-label`. |
| `mos_label_source` | Label file that supplied the row. |
| `mos_std_dev`, `mos_n_ratings`, `split`, `corpus`, `corpus_version` | Optional label metadata when present. |

Existing MOS columns are not overwritten unless `--overwrite` is passed.
Duplicate label keys with conflicting MOS values are rejected; they normally
mean the wrong labels were joined or a stale rerun was mixed into the label
directory.

## Trainer Contract

`ai/scripts/train_konvid_mos_head.py` no longer falls back to synthetic data
when a real corpus path yields zero labelled rows. Use:

- `--smoke` for dependency-free synthetic CI/load-path checks;
- `materialize_mos_labels.py` for real feature tables that need MOS labels;
- `--allow-synthetic-fallback` only for deliberate legacy debugging.

This keeps real-looking output filenames from hiding synthetic checkpoints.
