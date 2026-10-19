<!-- markdownlint-disable MD013 MD025 MD033 MD060 -->

# ADR-0673: Saliency Materializer Batch Manifest

- **Status**: Accepted (status update 2026-10-06 below)
- **Date**: 2026-05-21
- **Deciders**: Lusoris
- **Tags**: ai, saliency, materializer, provenance, fork-local

## Context

[ADR-0655](0655-saliency-feature-materializer.md) introduced
`ai/scripts/materialize_saliency_features.py` as the single table-shaped
saliency enrichment path. [ADR-0672](0672-saliency-materializer-temporal-controls.md)
then added explicit model and temporal-reducer metadata so refreshed tables
can compare `saliency_student_v1`, U2NetP, EMA, max, and motion-weighted
saliency rows.

The remaining operational gap is execution shape. The current refresh backlog
needs the same saliency materializer run over CHUG, KoNViD, UGC, Netflix, and
BVI-derived tables before predictor and MOS-head retrains can measure the
signal. Hand-written shell loops lose per-table config, overwrite policy,
row-failure status, and provenance, and they are easy to forget after context
compression.

## Decision

Add `ai/scripts/batch_materialize_saliency_features.py`, a manifest-driven
orchestrator over the existing single-table materializer. The manifest carries
shared defaults plus a `tables[]` array with per-table `id`, `input`,
`output`, optional `audit_json`, and any `SaliencyMaterializeConfig` override.
Relative paths resolve from the manifest directory unless `--base-dir` is
given. The runner writes each output table, optional per-table audit JSON, and
one `saliency-materializer-batch-v1` report with ADR-0661 run provenance.

The batch script must not own decoding or saliency semantics. It imports
`materialize_rows()`, `read_table()`, and `write_table()` from the single-table
script so row statuses and output columns stay identical.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Manifest-driven batch wrapper over the shared materializer | Repeatable multi-table refreshes; one provenance report; no duplicate decode/model code | Adds one operator-facing CLI | Chosen: it closes the immediate saliency execution gap while preserving ADR-0655's shared row semantics |
| Keep using shell loops | No new Python surface | No stable config schema, no batch report, weak resume/audit story | Rejected: this is exactly how saliency table refreshes get lost or mixed across models |
| Add corpus-specific materializers | Each corpus can hard-code roots and columns | Duplicates row IO/status/model logic and violates the shared-materializer invariant | Rejected: the current table schema already represents the corpus differences |
| Fold batch mode into the single-table CLI | One script name | Makes the single-table parser carry two unrelated invocation shapes | Rejected: a thin orchestration script is easier to test and keep out of trainer hot loops |

## Consequences

- **Positive**: Saliency refreshes across CHUG, KoNViD, UGC, Netflix, and BVI
  can be launched from one versioned manifest with per-table audits.
- **Negative**: Operators have one more AI script to validate when changing
  saliency materializer config fields.
- **Neutral / follow-ups**: Use the batch manifest on refreshed tables, rerun
  the signal-mix audit, and measure predictor / MOS-head retrain impact.

## References

- [ADR-0655](0655-saliency-feature-materializer.md) — shared saliency table
  materializer.
- [ADR-0672](0672-saliency-materializer-temporal-controls.md) — model and
  temporal-reducer metadata for saliency rows.
- [ADR-0661](0661-ai-run-manifest-provenance.md) — shared AI run provenance.
- [Research-0693](../research/0693-saliency-materializer-batch-manifest.md)
  — implementation digest.
- Source: req — "well go on i guess we have enough backlog..."
- Source: req — "well and in this audit perhaps find gaps that we have no metric/signal for at all or so"

### Status update 2026-10-05: Accepted

Per [ADR-0106](0106-adr-maintenance-rule.md) the body above is frozen; this
note records why the status line changed from Proposed, as found by the
2026-10-05 ADR status sweep. ai/scripts/batch_materialize_saliency_features.py
and its test exist as the manifest-driven batch orchestrator over the
single-table materializer. Shipped in PR #1496. Evidence on `master`:
PR #1496, `caefbe3ff`.

Verification command:

```bash
ls ai/scripts/batch_materialize_saliency_features.py ai/tests/test_batch_materialize_saliency_features.py
```

## Status update 2026-10-06: The unfilled template block was removed

Until this date the file began with an unfilled template block from the ADR
number allocator (stub comments, the title `<fill in title>`, `<fill in>`
sections), followed by this record. The block carried no decision and made the
first heading, the Deciders line and the Tags line unreadable by tools; it was
removed. The record itself is unchanged.

The body above is unchanged.
