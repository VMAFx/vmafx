# CHUG HDR Audit + Content Splits

## Finding

The CHUG materialisation path already pairs distorted rows with the
matching reference content, but it did not make validation leakage hard
to do accidentally. CHUG is a bitrate-ladder corpus, so row-level splits
can place the same `chug_content_name` in both train and validation.

The local pipeline also lacked a compact HDR signalling audit. The
ingest JSONL records geometry and pix format, but training runs need a
pre-flight summary of transfer characteristics, primaries, and malformed
PQ/HLG rows before feature rows are trusted.

## Change

- Add deterministic 80/10/10 content-level splits keyed by
  `chug_content_name`.
- Persist `split`, `chug_split_key`, and `chug_split_policy` into each
  CHUG feature row.
- Add `--split`, `--split-seed`, and `--split-manifest` to the feature
  materialiser.
- Add `--audit-output` to write an ffprobe-backed HDR metadata audit
  with transfer, primaries, pix-fmt, split, missing-file, probe-failure,
  and malformed-HDR counters.
- Teach `train_konvid_mos_head.py` to consume explicit split labels
  when present, training on `train` rows and validating on `val` (or
  `test` if no validation rows exist).
- Teach the generic `extract_k150k_features.py` FR-from-NR parquet path
  to preserve CHUG JSONL side metadata via `--metadata-jsonl`, so local
  full-feature HDR sweeps do not drop the content-safe split column.
- Teach `train_konvid_mos_head.py` to consume those FULL_FEATURES parquet
  tables directly via `--feature-parquet`, using `<feat>_mean` aggregates
  as canonical trainer features.

## Alternatives

See [ADR-0433](../adr/0433-chug-content-splits-and-hdr-audit.md).
