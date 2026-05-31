<!-- markdownlint-disable MD060 -->
# ADR-0434: CHUG Parquet Metadata Enrichment

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: Lusoris maintainers
- **Tags**: ai, hdr, chug, training, fork-local

## Context

Local CHUG FULL_FEATURES extraction can run for many hours. A run that
started before the `extract_k150k_features.py --metadata-jsonl` sidecar
path existed produces a useful feature parquet, but it lacks CHUG
content identity and deterministic split columns. Rerunning the feature
pass only to recover metadata wastes local CUDA/CPU time and delays HDR
experiments.

## Decision

Add `ai/scripts/enrich_k150k_parquet_metadata.py`, an operator utility
that reads an existing FULL_FEATURES parquet and a corpus JSONL sidecar,
matches rows by `clip_name` to each JSONL row's basename, and fills CHUG
metadata columns into the parquet. The default mode fills only missing
metadata cells and preserves feature/MOS columns. Operators can pass
`--overwrite-metadata` when deliberately replacing existing metadata.

The utility writes atomically and defaults to in-place rewrite so it can
be run directly against `.workingdir2/chug/training/full_features_chug.parquet`
after extraction finishes.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Rerun extraction with `--metadata-jsonl` | Reuses the normal extractor path | Wastes a long-running local feature job and GPU/CPU time | Rejected; metadata can be joined without touching feature values |
| Add only an extractor restart mode | Keeps one script surface | Still requires restarting the extraction driver and complicates checkpoint semantics | Rejected; post-hoc enrichment is simpler and safer |
| Always overwrite metadata | Deterministic replacement | Can silently discard manual split fixes or prior audits | Rejected; fill-missing default preserves existing operator intent |

## Consequences

- **Positive**: Current local CHUG feature runs can be salvaged for
  content-level validation without rerunning extraction.
- **Negative**: The join depends on stable clip basenames; renamed
  parquets still require an explicit sidecar with matching names.
- **Neutral / follow-ups**: Future corpus adapters can reuse the same
  parquet-enrichment shape when they emit compatible JSONL sidecars.

## References

- [ADR-0433](0433-chug-content-splits-and-hdr-audit.md)
- Source: `req` — "implement everything that is not blocked by the model"
