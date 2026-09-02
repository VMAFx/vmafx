<!-- markdownlint-disable MD013 MD060 -->
# ADR-0655: Saliency Feature Materializer

- **Status**: Accepted
- **Date**: 2026-05-20
- **Deciders**: Lusoris, Codex
- **Tags**: ai, saliency, training-data, docs

## Context

The signal-mix audit and CHUG HDR MOS experiments both need to answer whether
saliency helps once it is present as a real numeric signal, not as placeholder
zeros. Existing predictor plumbing can preserve saliency columns once they
exist, but historical feature tables still need an explicit enrichment pass.

The enrichment pass must work on existing JSONL/parquet corpus tables without
coupling every trainer to the saliency model stack. It also needs bounded decode
cost because operators will run it over large local corpora while other refresh
jobs are active.

## Decision

Add `ai/scripts/materialize_saliency_features.py`, a table-shaped materializer
that reads JSONL or parquet rows, resolves a source clip path, decodes a bounded
sample to temporary `yuv420p` via FFmpeg, invokes the fork saliency helper, and
writes `saliency_mean`, `saliency_var`, and an optional status column.

The default row contract is deliberately conservative:

- source clip path from `src`;
- geometry from `width` / `height`, with ffprobe fallback;
- bounded decode via `--max-frames` and saliency sampling via
  `--frame-samples`;
- status values that distinguish existing rows, missing sources, missing
  geometry, decode failures, and model failures.

The script is a preparation surface, not a trainer side effect. Trainers should
consume already materialised saliency columns and fail or report missing-signal
coverage explicitly instead of silently running saliency inference inside a
training loop.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Compute saliency inside each trainer | No extra operator command; every trainer can request the signal directly | Duplicates decode/model code, makes training runs non-deterministically slower, and hides missing-signal coverage | Rejected because corpus enrichment should be observable and reusable |
| Add a one-off CHUG-only enrichment script | Fastest route for the current HDR run | Leaves KoNViD/UGC/Netflix refresh tables without the same path and repeats the scaffold problem in the next corpus | Rejected because the audit needs a cross-corpus table utility |
| Materialize table rows as a standalone script | Reuses existing saliency helper, gives operators a status column, and keeps trainers simple | Adds a new user-facing script and docs surface | Chosen as the narrowest reusable path |
| Require raw YUV inputs only | Avoids ffmpeg decode variance | Most MOS/HDR corpora are MP4/encoded assets; operators would need a separate decode pipeline first | Rejected because the materializer is meant to unblock real corpus tables |

## Consequences

- **Positive**: Refreshed AI tables can now carry real saliency aggregates
  before MOS-head and predictor retrains.
- **Positive**: The status column makes bulk corpus quality visible without
  scraping stderr.
- **Negative**: Large runs still require local video access and model weights;
  CI covers the control flow with fake decoders rather than shipping a real
  video/model fixture.
- **Neutral / follow-ups**: Run this materializer over refreshed CHUG,
  KoNViD/UGC, and Netflix-derived tables, then re-run the signal-mix audit to
  measure whether saliency should stay in the production feature mix.

## References

- [ADR-0286](0286-saliency-student-fork-trained-on-duts.md)
- [ADR-0396](0396-video-saliency-extension.md)
- [ADR-0649](0649-chug-hdr-wide-mos-feature-schema.md)
- [Research-0655](../research/0655-saliency-feature-materializer.md)
- Source: `req` - "well and in this audit perhaps find gaps that we have no metric/signal for at all or so"
- Source: `req` - "yeah every possible gain through intersection (even of not yet included metrics)"
