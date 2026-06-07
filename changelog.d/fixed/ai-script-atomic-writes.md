### Fixed

- **AI script resume correctness**: `write_text_atomic` added to `aiutils`; all
  per-clip cache JSON writes in `chug_extract_features`, `bvi_dvc_to_full_features`,
  `konvid_to_full_features`, and `extract_full_features` now use atomic
  tmp-rename, preventing partial-write corruption on interrupt. Final Parquet
  outputs in the same scripts migrated to `write_parquet_atomic`. The
  `aggregate_corpora` JSONL output and `write_manifest_json` are also made
  atomic. Resolves the class of bug where a crash mid-write left a corrupt
  cache file that permanently poisoned resume logic (ADR-1097).
