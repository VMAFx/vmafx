# Research-0655: Saliency Feature Materializer

## Question

How should existing AI feature tables receive real saliency signals without
turning every trainer into an implicit video decoder and saliency runner?

## Findings

- Existing refreshed corpus tables are row-oriented JSONL/parquet assets, so
  saliency needs to be added as row-level aggregates before model selection can
  measure it.
- `vmaftune.saliency.compute_saliency_map` already owns the model invocation and
  expects raw `yuv420p` plus explicit geometry.
- Most local corpora are encoded video files, not raw YUV, so a reusable
  materializer must decode a bounded sample through FFmpeg and must probe
  geometry when table rows are incomplete.
- A status column is more useful than fail-fast behaviour for large local
  sweeps: operators need to know which rows decoded, which were skipped, and
  which failed because the source or geometry was missing.

## Result

Ship one table materializer under `ai/scripts/` with JSONL/parquet I/O,
bounded FFmpeg decode, ffprobe fallback, `saliency_mean` / `saliency_var`
columns, and status values for operational triage.

## Verification

`ai/tests/test_materialize_saliency_features.py` covers JSONL roundtrip,
existing-column skips, missing-source status, ffprobe geometry fallback, and
the fake FFmpeg-to-saliency handoff.
