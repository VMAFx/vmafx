<!-- markdownlint-disable MD060 -->
# Second-opinion feature materializer

`ai/scripts/materialize_second_opinion_features.py` joins externally generated
NR/MOS scores onto refreshed feature tables. Use it after running fork-local
or third-party scorers such as `fork-nr-metric`, DOVER, Q-Align, FAST-VQA,
MUSIQ, or CLIP-IQA, and before retraining MOS heads, predictor models, or
running the signal-mix audit.

The script does not run, vendor, or link any external scorer. It only reads
score JSON/JSONL files and appends namespaced columns:

| Column | Meaning |
|---|---|
| `second_opinion_<scorer>_score` | Clip-level MOS/VQA score from the scorer. |
| `second_opinion_<scorer>_status` | `ok`, `missing`, or `bad`. |
| `second_opinion_<scorer>_runtime_ms` | Runtime reported by the scorer, when available. |
| `second_opinion_<scorer>_frames` | Number of frame scores averaged when the score came from frame JSON. |
| `second_opinion_<scorer>_source` | Score file that supplied the row. |

## Inputs

Feature tables may be parquet, JSONL/NDJSON, or a JSON object with `rows`.
Score files may be JSONL/NDJSON or JSON. Each score row needs:

- a row key such as `clip_id`, `video_id`, `filename`, `name`, or `path`;
- a `competitor` field, unless passed as `LABEL=path` through `--scores`;
- either a scalar score field such as `score`, `mos`,
  `predicted_mos`, `predicted_vmaf_or_mos`, or a wrapper-style `frames[]`
  list with per-frame `predicted_vmaf_or_mos`.

External-bench wrapper payloads are accepted directly when they include a clip
key. Their per-frame scores are averaged if no summary-level score is present.

## Examples

Join DOVER and the fork NR scorer onto a CHUG feature shard:

```bash
.venv/bin/python ai/scripts/materialize_second_opinion_features.py \
  --features .corpus/chug/training/fr_canonical_shards/shard_000.features.jsonl \
  --scores dover-mobile=.workingdir2/second-opinion/dover-chug.jsonl \
  --scores fork-nr-metric=.workingdir2/second-opinion/fork-nr-chug.jsonl \
  --out .workingdir2/second-opinion/shard_000.with-second-opinion.jsonl \
  --audit-json .workingdir2/second-opinion/shard_000.audit.json
```

Use `--missing-policy fail` for promotion-grade tables where every row must
have every scorer. Use the default `mark` during exploratory audits; missing
scores remain visible as `second_opinion_<scorer>_status = "missing"`.

When `--audit-json` is set, the audit file includes ADR-0661
`run_provenance` with the feature table, score sidecar paths, parsed join
options, output table target, and audit target. Keep that audit with any
derived table used for retraining; external scorer sidecars can be rebuilt only
if their exact join inputs are preserved.

## Batch Manifest

Use `ai/scripts/batch_materialize_second_opinion_features.py` when the same
scorer family needs to be joined across multiple refreshed tables. Paths in the
manifest are relative to the manifest file unless `--base-dir` is supplied.

```json
{
  "defaults": {
    "missing_policy": "fail",
    "key_normalize": "auto"
  },
  "tables": [
    {
      "id": "chug_hdr",
      "features": ".corpus/chug/training/fr_canonical_shards/shard_000.features.jsonl",
      "scores": [
        "dover-mobile=.workingdir2/second-opinion/dover-chug.jsonl",
        "fork-nr-metric=.workingdir2/second-opinion/fork-nr-chug.jsonl"
      ],
      "out": ".workingdir2/second-opinion/shard_000.with-second-opinion.jsonl",
      "audit_json": ".workingdir2/second-opinion/shard_000.audit.json"
    }
  ]
}
```

```bash
.venv/bin/python ai/scripts/batch_materialize_second_opinion_features.py \
  --manifest .workingdir2/second-opinion/batch.json \
  --report-json .workingdir2/second-opinion/batch.report.json \
  --report-md .workingdir2/second-opinion/batch.report.md
```

Each table may override any single-run join option from `defaults`, including
`missing_policy`, `key_column`, `score_key_field`, `score_field`,
`key_normalize`, `prefix`, and `overwrite`. The batch report uses schema
`second-opinion-materializer-batch-v1` and carries ADR-0661 `run_provenance`.

## Smoke-Run Scaffold

A self-contained smoke run with synthetic fixture tables is committed under
`ai/testdata/smoke-second-opinion-batch/`. Run it from the repo root to verify
the full pipeline end-to-end before using real corpus data:

```bash
mkdir -p /tmp/vmafx-smoke-second-opinion
PYTHONPATH=ai/scripts python ai/scripts/batch_materialize_second_opinion_features.py \
  --manifest ai/testdata/smoke-second-opinion-batch/batch.json \
  --base-dir . \
  --report-json /tmp/vmafx-smoke-second-opinion/smoke.report.json \
  --report-md   /tmp/vmafx-smoke-second-opinion/smoke.report.md
```

Expected output: `tables=2 input_rows=5 output_rows=5 failed_tables=0`.
See `ai/testdata/smoke-second-opinion-batch/README.md` for the full inspection
checklist.

## Reading The Columns

Second-opinion columns are advisory evidence, not replacements for reference
metrics or subjective labels. Their main value is intersection:

- disagreeing with VMAF on UGC/HDR clips that humans rate poorly;
- separating no-reference camera defects from encode artifacts;
- giving MOS heads an extra axis when FR features saturate;
- showing signal-mix audits that NR/MOS evidence is present rather than
  silently missing.

Do not commit derived score tables that contain licensed third-party outputs
or private MOS labels unless the dataset and scorer licences explicitly allow
redistribution.
