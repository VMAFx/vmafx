<!-- markdownlint-disable MD060 -->
# ADR-0433: CHUG Content Splits And HDR Audit

- **Status**: Accepted
- **Date**: 2026-05-15
- **Deciders**: Lusoris maintainers
- **Tags**: ai, hdr, chug, training, fork-local

## Context

CHUG rows are bitrate-ladder variants of shared UGC-HDR source content.
Splitting individual rows independently would allow the same source
content to appear in both training and validation, inflating HDR
experiments before any model decision is involved. The local CHUG
pipeline also needs a pre-training HDR metadata check so malformed
transfer / primaries signalling is caught before feature rows are used.

## Decision

`ai/scripts/chug_extract_features.py` will assign deterministic
train/validation/test partitions by hashing `chug_content_name` with
seed `chug-hdr-v1` into an 80/10/10 split. Every materialised feature
row records `split`, `chug_split_key`, and `chug_split_policy`. The same
script will optionally write a local ffprobe-backed HDR metadata audit
JSON before feature extraction. `train_konvid_mos_head.py` will consume
those explicit split labels when present, using `train` rows for fitting
and `val` (or `test` when no `val` exists) as the held-out evaluation set.
The FR-from-NR full-feature parquet extractor will accept an optional
`--metadata-jsonl` sidecar so CHUG runs keep the same content and split
metadata in parquet outputs. `train_konvid_mos_head.py` will also accept
those FULL_FEATURES parquet files directly via `--feature-parquet`,
mapping `<feat>_mean` aggregates back to the trainer's canonical feature
columns.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Row-level random split | Simple and balances row counts closely | Leaks bitrate-ladder variants of the same source content across validation | Rejected; leakage is worse than minor split imbalance |
| Manifest-order split | Deterministic without hashing | Depends on upstream CSV ordering and can cluster related content accidentally | Rejected; hash partitioning is stable and order-independent |
| Separate audit script | Keeps the materialiser narrower | Operators can forget the audit before training; duplicated JSONL loading / clip path handling | Rejected; the audit is a pre-training guard for the same local feature job |

## Consequences

- **Positive**: CHUG HDR experiments get leakage-resistant splits and a
  repeatable metadata audit without waiting on any model work.
- **Negative**: Small local `--max-rows` runs may not contain all three
  splits because the split unit is content, not row.
- **Neutral / follow-ups**: CHUG calibration jobs can now train from
  JSONL materialiser rows or FULL_FEATURES parquet rows; production HDR
  status still depends on the final teacher/model decision.

## References

- [ADR-0426](0426-chug-hdr-corpus-ingestion.md)
- [ADR-0427](0427-chug-hdr-feature-materialisation.md)
- Source: `req` — "implement everything that is not blocked by the model"
