- Add the ADR-0642 AI refresh path: `ai/scripts/konvid_to_full_features.py`,
  a KoNViD-1k full-feature
  extraction driver that regenerates `runs/full_features_konvid*.parquet`
  with current fork libvmaf features and deterministic five-fold output.
- Refresh the YouTube UGC extractor to emit the current `FULL_FEATURES`
  schema with an explicit `vmaf_v0.6.1` model path instead of the old
  canonical-6-only table.
- Add `ai/scripts/combine_full_feature_parquets.py` so refreshed
  Netflix/KoNViD/BVI/UGC shards rebuild aggregate full-feature tables
  reproducibly.
