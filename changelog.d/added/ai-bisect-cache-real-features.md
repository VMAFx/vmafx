`ai/scripts/build_bisect_cache.py` can now materialise the
bisect-model-quality fixture layout from a real DMOS/MOS-aligned feature
parquet via `--source-features` and `--target-column`, while preserving
the deterministic synthetic default used by CI.
