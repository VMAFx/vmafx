- `vmaf_tiny_v5` YouTube UGC corpus-expansion probe (deferred; ADR-0287). Adds the
  three-stage pipeline scaffold (`fetch_youtube_ugc_subset.py`,
  `extract_ugc_features.py`, `train_vmaf_tiny_v5.py`) plus a LOSO eval script and
  research digest. The probe validates whether expanding the four-corpus parquet
  with YouTube UGC clips moves the v3/v4 PLCC ceiling. **Status: deferred** —
  shipping the scripts + digest now so the corpus is reproducible later; no
  registry row added until a follow-up ships an ONNX that clears the v3 LOSO
  ceiling.
