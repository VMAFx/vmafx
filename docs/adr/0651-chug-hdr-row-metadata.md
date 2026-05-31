<!-- markdownlint-disable MD013 MD060 -->
# ADR-0651: Preserve CHUG HDR Metadata On Feature Rows

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris maintainers
- **Tags**: ai, chug, hdr, metadata, training

## Context

CHUG is the fork's active HDR MOS corpus while the upstream Netflix HDR
model remains unavailable. `ai/scripts/chug_extract_features.py` already
probes clip HDR metadata for the `--audit-output` preflight, but that
information was stranded in a corpus-level audit JSON. The emitted
training rows only carried libvmaf feature aggregates plus CHUG manifest
labels, so HDR transfer, primaries, pixel format, range, and static
luminance metadata could not be consumed by later heads without rerunning
ffprobe out of band.

## Decision

We will preserve normalized ffprobe HDR/display metadata on every CHUG
feature row as `feature_ref_*` fields for the matched reference clip and
`feature_dis_*` fields for the distorted ladder clip. The row suffixes are
`codec_name`, `pix_fmt`, `color_transfer`, normalized `transfer_class`,
`color_primaries`, `color_space`, `color_range`, `max_content_nits`, and
`max_average_nits`. Missing fields remain explicit as `unknown` or `null`;
the extractor does not infer display-panel capability from clip metadata.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep metadata only in `--audit-output` | Minimal row schema churn; existing audit remains compact | Model training cannot join metadata reliably after sharding, caching, or row filtering | Rejected because HDR model work needs row-local signals, not a sidecar that can drift |
| Preserve raw ffprobe JSON per row | Lossless and future-proof | Bloats JSONL rows and forces every trainer to parse unstable ffprobe structure | Rejected because the trainer needs a stable compact schema |
| Normalize only `transfer_class` | Smallest useful HDR categorical signal | Loses primaries/range/pixel-format/static-luminance signals that help debug malformed HDR and later panel tuning | Rejected because the existing audit already proves those fields are available |

## Consequences

- **Positive**: CHUG feature shards carry HDR transfer and static metadata
  alongside the feature aggregates they describe, so later HDR heads can
  consume them without a second probe pass.
- **Negative**: `chug_extract_features.py` performs two cached ffprobe
  lookups per unique clip path during materialisation.
- **Neutral / follow-ups**: Panel/display capability remains a separate
  dataset or operator-profile input. Future trainer schemas may one-hot or
  numeric-normalize these row fields, but the materialiser keeps them as
  source metadata.

## References

- [ADR-0426](0426-chug-hdr-corpus-ingestion.md)
- [ADR-0427](0427-chug-hdr-fr-feature-materializer.md)
- Source: `req` — "well yeah and chug is hdr mos... so thats different because netflix (current) model is 8bit only etc..."
- Source: `req` — "implement everything that is not blocked by the model"
