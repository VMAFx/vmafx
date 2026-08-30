# Research-0695: MOS Label Materializer Batch Manifest

## Problem

Real MOS-head refreshes need labelled feature tables, but labels and feature
rows are usually produced by separate corpus-specific ingestion jobs. The
single-table MOS materializer closes that join for one feature table at a time.
A full refresh still needs several joins with the same key, regex, coverage,
and overwrite policy, plus one artifact that proves exactly which tables were
labelled.

Manual loops are weak evidence. They make it hard to prove that the KoNViD,
CHUG/HDR, UGC, and BVI tables were all labelled under the same coverage
threshold and with the intended audit sidecars.

## Finding

The existing `materialize_mos_labels.materialize()` function is the correct
semantic boundary. It owns key inference, regex extraction, MOS scale
conversion, duplicate-label rejection, match-rate enforcement, and output
column policy. A batch runner should only resolve a manifest, call the shared
function for each table, and summarize the per-table audits.

## Decision Drivers

- Keep MOS row parsing and match-rate semantics in one implementation.
- Make multi-table label refreshes replayable by downstream training.
- Keep per-table audit JSON plus one batch-level report with ADR-0661
  provenance.
- Preserve the explicit table-side contract from ADR-0663: no extraction and
  no training inside the label materializer.

## Implementation Notes

`ai/scripts/batch_materialize_mos_labels.py` reads a JSON manifest with
`defaults` plus `tables[]`. Each table carries `features`, `labels`, `out`,
optional `audit_json`, and join-option overrides. Relative paths resolve from
the manifest directory by default. The runner calls
`materialize_mos_labels.materialize()` for every table, writes a
`mos-label-materializer-batch-v1` report, and stamps ADR-0661
`run_provenance`.

Tests cover two-table success, failure propagation when a table misses its
configured `min_match_rate`, and manifest validation for unknown keys.

## Follow-Up

Run the batch manifest on refreshed KoNViD, CHUG/HDR, UGC, and BVI feature
tables, then rerun MOS-head training and the signal-mix audit using the
generated `mos` / `mos_raw_0_100` columns and batch report.
