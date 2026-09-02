- The K150K/CHUG feature extractor now extracts `speed_temporal` and
  `speed_chroma` (per lawrence's `hdr_custom_features.py` Slack
  recipe). Both are CPU-only extractors (no CUDA twin in libvmaf
  yet); they ride the existing CPU residual pass alongside the
  pre-existing residual entries. `FEATURE_NAMES` gains 4 new
  appended columns (`speed_temporal`, `speed_chroma_u`,
  `speed_chroma_v`, `speed_chroma_uv`) — column order is preserved
  for existing readers; new columns are append-only at the end of
  the tuple. Output parquet schema bumps from 22 → 25 logical
  features (44 → 50 mean/std columns).
