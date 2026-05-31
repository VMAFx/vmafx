<!-- markdownlint-disable MD060 -->
# Research-0669: AI Materializer Audit Provenance

## Summary

ADR-0661 made training, export, evaluation, and validation JSON artifacts
traceable, but the refreshed feature-table materializers still had a gap:
their audit files could prove row counts and match rates, but not the exact
CLI, source tables, label/score inputs, thresholds, or output targets that
created those tables.

The gap matters because materialized tables are durable inputs to later
training. MOS labels, saliency features, second-opinion scorer joins, and
signal-mix audit reports can change model selection even when no model
checkpoint is produced in the same command.

## Files Audited

- `ai/scripts/materialize_mos_labels.py`
- `ai/scripts/materialize_second_opinion_features.py`
- `ai/scripts/materialize_saliency_features.py`
- `ai/scripts/signal_mix_audit.py`
- `docs/ai/{mos-label-materializer,second-opinion-features,saliency-feature-materializer,signal-mix-audit}.md`
- ADR-0661 and `aiutils.run_manifest`

## Findings

- MOS label and second-opinion materializers already wrote optional audit JSON,
  but those files only contained join counters.
- The saliency materializer had no audit JSON option despite producing
  retraining-critical feature columns.
- Signal-mix JSON reports listed table findings but not the thresholds or
  input paths that produced the report.
- All four surfaces fit ADR-0661 without a new schema. They need compact
  command/input/output provenance, not full environment snapshots.

## Decision Matrix

| Option | Pros | Cons | Result |
|---|---|---|---|
| Add `run_provenance` only to trainers | Smallest scope | Materialized feature tables remain hard to reproduce | Rejected |
| Add bespoke audit metadata per materializer | Localized fields | Repeats path and argv normalization; drifts from ADR-0661 | Rejected |
| Reuse `aiutils.run_manifest` for materializer/audit JSON | Shared schema; hashes source inputs; keeps report targets deterministic | Slightly larger audit JSON | Chosen |

## Outcome

The MOS label, second-opinion, saliency, and signal-mix audit JSON outputs now
carry `run_provenance`. The saliency materializer also gains `--audit-json` so
its row counters and effective config can be retained beside enriched feature
tables.

## Validation

```bash
.venv/bin/ruff check \
  ai/scripts/materialize_mos_labels.py \
  ai/scripts/materialize_second_opinion_features.py \
  ai/scripts/materialize_saliency_features.py \
  ai/scripts/signal_mix_audit.py \
  ai/tests/test_materialize_mos_labels.py \
  ai/tests/test_second_opinion_features.py \
  ai/tests/test_materialize_saliency_features.py \
  ai/tests/test_signal_mix_audit.py

.venv/bin/python -m pytest \
  ai/tests/test_materialize_mos_labels.py \
  ai/tests/test_second_opinion_features.py \
  ai/tests/test_materialize_saliency_features.py \
  ai/tests/test_signal_mix_audit.py -q
```
