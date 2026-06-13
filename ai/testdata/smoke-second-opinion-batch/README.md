<!-- markdownlint-disable MD013 MD060 -->
# Second-Opinion Batch Materializer — Smoke Run

Demonstrates `ai/scripts/batch_materialize_second_opinion_features.py` against
small synthetic fixture tables.  Run all commands from the **repo root**.

## Prerequisites

```bash
pip install -e ai
```

## Run

```bash
mkdir -p /tmp/vmafx-smoke-second-opinion
PYTHONPATH=ai/scripts python ai/scripts/batch_materialize_second_opinion_features.py \
  --manifest ai/testdata/smoke-second-opinion-batch/batch.json \
  --base-dir . \
  --report-json /tmp/vmafx-smoke-second-opinion/smoke.report.json \
  --report-md   /tmp/vmafx-smoke-second-opinion/smoke.report.md
```

Expected stderr:

```text
second-opinion batch materialize: tables=2 input_rows=5 output_rows=5 failed_tables=0
```

## What to inspect

| Output path | What to check |
|---|---|
| `/tmp/vmafx-smoke-second-opinion/smoke_table_a.jsonl` | Has `second_opinion_fork_nr_score`, `_status=ok`, `_runtime_ms` columns |
| `/tmp/vmafx-smoke-second-opinion/smoke_table_a.audit.json` | Has `run_provenance.schema == "ai-run-provenance-v1"` |
| `/tmp/vmafx-smoke-second-opinion/smoke.report.json` | `schema == "second-opinion-materializer-batch-v1"`, `summary.tables == 2`, `summary.output_rows == 5` |
| `/tmp/vmafx-smoke-second-opinion/smoke.report.md` | Human-readable table with `ok` status for both tables |

## Fixture data

| File | Description |
|---|---|
| `fixtures/features_a.jsonl` | 3-row feature table keyed on `video_id` |
| `fixtures/features_b.jsonl` | 2-row feature table keyed on `video_id` |
| `fixtures/scores_fork_nr_a.jsonl` | 3 matching `fork-nr` score rows for table A |
| `fixtures/scores_fork_nr_b.jsonl` | 2 matching `fork-nr` score rows for table B |

Substitute real corpus feature shards and scorer sidecars (DOVER, Q-Align,
FAST-VQA, fork-NR, …) to run a production batch join.  See
[`docs/ai/second-opinion-features.md`](../../../../docs/ai/second-opinion-features.md)
for the full operator guide and manifest schema reference.

## Related

- ADR-0657: table-side second-opinion joiner
- ADR-0674: batch manifest design
- ADR-0991: this smoke-run scaffold
- `docs/ai/second-opinion-features.md`: operator guide
