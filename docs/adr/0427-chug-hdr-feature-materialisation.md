<!-- markdownlint-disable MD013 MD060 -->
# ADR-0427: Materialise CHUG HDR Features With Reference-Aligned Pairs

- **Status**: Accepted
- **Date**: 2026-05-14
- **Deciders**: Lusoris maintainers
- **Tags**: ai, hdr, corpus, mos

## Context

ADR-0426 adds CHUG ingestion as local MOS-corpus rows, but those rows
alone are not enough for the existing MOS-head trainer: the trainer
uses canonical libvmaf features when present and otherwise fills the
feature vector with zeros. CHUG is a bitrate ladder with one reference
row and six distorted rows per `chug_content_name`, so it can provide
real full-reference HDR feature evidence once local clips are paired.

The non-trivial decision is spatial alignment. CHUG distorted ladder
rows are 360p, 720p, and 1080p, while the matching reference row is the
reference-resolution clip. The raw-YUV libvmaf CLI path expects equal
reference/distorted geometry, so the materialiser must choose an
alignment policy before feature extraction.

## Decision

We will materialise CHUG feature rows with a local-only script that
pairs each distorted row with the matching `chug_content_name`
reference row, decodes both sides to 10-bit 4:2:0 YUV, scales the
distorted side to the reference geometry, runs libvmaf feature
extraction, and writes clip-level mean/p10/p90/std feature rows under
`.workingdir2/chug/`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Treat CHUG as no-reference MOS only | Simple; no reference pairing or scaling policy | Leaves MOS training on missing-feature zeros; does not unlock HDR feature learning | Rejected because CHUG contains explicit reference rows and the fork already has FR feature extractors |
| Downscale the reference to each distorted resolution | Avoids upscaling distorted pixels | Changes the reference signal per ladder rung and makes high-quality rungs less comparable to the source HDR master | Rejected because the source reference should stay the anchor geometry |
| Scale distorted rows to reference geometry | Preserves one reference anchor per content and matches streaming-display comparison practice | Adds an ffmpeg scale step before libvmaf and costs more local compute | Chosen because it yields comparable FR rows across ladder rungs |

## Consequences

- **Positive**: CHUG can feed MOS-head training with real canonical/full
  feature values instead of default zeros.
- **Positive**: Feature rows stay local-only, preserving the ADR-0426
  license posture for CHUG media, MOS labels, and derived features.
- **Negative**: Full materialisation is a local hardware job; decoding,
  scaling, and libvmaf feature extraction over 5,136 distorted rows is
  intentionally outside CI.
- **Neutral / follow-ups**: Future HDR-specific production claims still
  require either a fork-owned HDR model or Netflix's released HDR model;
  this decision only unlocks subjective HDR feature rows.

## References

- [ADR-0426](0426-chug-hdr-corpus-ingestion.md)
- [Research-0101](../research/0101-training-discovery-synthesis-2026-05-14.md)
- Source: `req` — "yeah well download, prep and train lol... thats a local hardware background job..."
- Source: `req` — "and then lets unlock fucking hdr baby"
