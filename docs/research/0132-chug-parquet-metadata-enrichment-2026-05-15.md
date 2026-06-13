# CHUG Parquet Metadata Enrichment

## Finding

The local CHUG FULL_FEATURES extraction can outlive the code branch that
started it. A parquet produced without `--metadata-jsonl` still carries
valid feature and MOS columns, but it cannot drive content-level held-out
validation because `split`, `chug_split_key`, raw 0-100 MOS, and ladder
identity are missing.

## Change

- Add `ai/scripts/enrich_k150k_parquet_metadata.py` to join an existing
  FULL_FEATURES parquet with a corpus JSONL sidecar by `clip_name`.
- Fill missing metadata cells by default so previously written feature /
  MOS columns are not modified.
- Keep an explicit `--overwrite-metadata` mode for deliberate operator
  corrections.
- Document the recovery command in the CHUG and K150K operator pages.

## Alternatives

See [ADR-0434](../adr/0434-chug-parquet-metadata-enrichment.md).
