- **changed(ai):** `aiutils.parquet_utils.write_parquet_atomic` now
  emits parquet schema v2 by default — zstd at compression level 3 in
  place of snappy, columns reordered into a canonical layout
  (`clip_id`, `frame_idx`, sorted features, labels, metadata), and file
  metadata that carries `vmafx_schema_version=2` plus a
  `vmafx_pipeline_hash` git short SHA. Reduces K150K / CHUG cold
  storage by roughly 20-30 % on real (mixed-dtype) data and removes
  per-script "find the score column" boilerplate. Legacy v1 files
  remain readable; a new `read_parquet_with_schema()` helper returns
  the detected version. Callers can opt out of zstd-3 by passing an
  explicit `compression=` keyword. (ADR-0926)
