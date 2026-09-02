# Research-0651: CHUG HDR Row Metadata

## Question

What non-model HDR signal can be made available immediately for CHUG
training rows after the signal-mix audit showed missing HDR display/panel
context?

## Findings

- `ai/scripts/chug_extract_features.py` already probes CHUG clips with
  ffprobe for `--audit-output`, including transfer characteristics,
  primaries, pixel format, colorspace/range, and static metadata fields.
- The audit output is corpus-level. It is useful for preflight checks, but
  it is not row-local and can drift from feature JSONL shards after
  split filtering, cache reuse, or partial reruns.
- The same ffprobe payload can be normalized into compact fields without
  committing CHUG data or model artifacts. `transfer_class` gives stable
  buckets (`pq`, `hlg`, `sdr`, `unknown`), while raw normalized ffprobe
  strings stay available for debugging.

## Decision Input

Preserve the normalized metadata directly in the local-only CHUG feature
rows:

- `feature_ref_*` for the matched reference.
- `feature_dis_*` for the distorted ladder row.
- Suffixes: `codec_name`, `pix_fmt`, `color_transfer`,
  `transfer_class`, `color_primaries`, `color_space`, `color_range`,
  `max_content_nits`, and `max_average_nits`.

This does not solve panel/display tuning by itself; it removes the
immediate data plumbing gap so later HDR model experiments can consume
clip-signalled HDR metadata without another extraction pass.

## Validation Plan

- Unit-test the metadata normalizer against ffprobe-like payloads,
  including missing fields.
- Unit-test the CHUG materialiser so emitted feature rows contain both
  reference and distorted metadata.
- Keep the existing end-to-end degraded-pair smoke focused on feature
  correctness; the metadata path remains mocked there to avoid ffprobe
  dependence in fast tests.
