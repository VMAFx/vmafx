# Research-0694: Second-Opinion Materializer Batch Manifest

## Problem

The signal-mix audit needs no-reference and subjective-MOS second-opinion
evidence on the same refreshed tables that feed MOS heads and predictor
training. The existing second-opinion materializer joins one feature table
with one or more external scorer sidecars, but a complete refresh needs several
tables processed with the same key policy and scorer labels.

Manual loops are not enough for training evidence. They make it hard to prove
which feature tables were joined, which scorer sidecars were used, whether
`missing_policy=fail` was active, and where the per-table audits live.

## Finding

The existing single-table materializer is the right semantic boundary. It
already parses wrapper payloads, detects duplicate keys, namespaces
`second_opinion_<scorer>_*` columns, and preserves the ADR-0657 rule that this
repo does not invoke external VQA competitors. The missing piece is a thin
orchestration layer that records the list of table joins as one replayable
batch artifact.

## Decision Drivers

- Keep external scorer execution outside the repo.
- Avoid duplicating score-row parsing and join semantics.
- Make multi-table second-opinion refreshes citeable by downstream training.
- Keep `missing_policy=fail` easy to apply consistently across promotion-grade
  tables.

## Implementation Notes

`ai/scripts/batch_materialize_second_opinion_features.py` reads a JSON manifest
with `defaults` plus `tables[]`. Each table carries `features`, `scores`,
`out`, optional `audit_json`, and join-option overrides. Relative paths resolve
from the manifest directory by default. The runner calls
`materialize_second_opinion_features.materialize()` for every table, writes a
`second-opinion-materializer-batch-v1` report, and stamps ADR-0661
`run_provenance`.

The tests cover two-table success, failure propagation when
`missing_policy=fail` finds incomplete score coverage, and manifest validation
for unknown keys.

## Follow-Up

Generate scorer sidecars for refreshed CHUG, KoNViD, UGC, Netflix, and BVI
tables, run the batch manifest, then rerun the signal-mix audit and retrain
candidate models with the joined `second_opinion_*` columns.
