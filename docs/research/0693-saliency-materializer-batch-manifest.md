# Research-0693: Saliency Materializer Batch Manifest

## Problem

The signal-mix backlog needs saliency columns on several refreshed feature
tables before predictor and MOS-head retraining can measure the gain. The
single-table materializer is correct, but running it by hand across CHUG,
KoNViD, UGC, Netflix, and BVI tables creates a weak audit trail:

- per-table roots and path columns can drift across shell invocations;
- reducer/model choices can be documented in one place and executed
  differently in another;
- partial row failures are easy to miss when only stderr is retained;
- retraining jobs need a stable list of generated saliency tables.

## Finding

A manifest wrapper is enough. The existing `materialize_saliency_features.py`
already owns row IO, FFmpeg decode, ffprobe fallback, saliency invocation,
status columns, and temporal/model metadata. The missing piece is a repeatable
operator surface that applies the same shared defaults to many tables and emits
one machine-readable report.

The batch wrapper should therefore import the shared materializer functions and
avoid new row semantics. The manifest can stay small:

- `defaults` maps directly to `SaliencyMaterializeConfig`;
- each table has `id`, `input`, `output`, optional `audit_json`, and overrides;
- relative paths resolve from the manifest directory, making local run bundles
  movable under `.workingdir2` or `runs/`;
- the batch report summarizes row counters and includes ADR-0661
  `run_provenance`.

## Decision Drivers

- Keep saliency inference out of trainer hot loops.
- Preserve one implementation of row status values and saliency metadata.
- Make multi-table saliency refreshes replayable after context compression.
- Let downstream training select saliency tables by manifest/report instead of
  reconstructing shell history.

## Implementation Notes

`ai/scripts/batch_materialize_saliency_features.py` adds the batch CLI and
schema `saliency-materializer-batch-v1`. It returns non-zero when any table
has failed rows, unless `--allow-row-failures` is explicit for exploratory
coverage audits. Per-table `audit_json` files capture the effective config and
row counters; the batch report carries the shared run provenance block.

The tests use pre-populated saliency rows for the success path so they exercise
manifest parsing, path resolution, output writes, report generation, and
provenance without depending on FFmpeg or ONNX saliency runtimes.

## Follow-Up

Use the batch runner on refreshed CHUG, KoNViD, UGC, Netflix, and BVI tables,
then rerun the signal-mix audit and model retraining jobs with the resulting
saliency tables.
