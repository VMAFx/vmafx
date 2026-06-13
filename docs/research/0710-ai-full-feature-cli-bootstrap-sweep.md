<!-- markdownlint-disable MD013 -->
# AI full-feature CLI bootstrap sweep

Date: 2026-05-22

## Scope

This sweep applies the accepted AI script helper pattern to the remaining
full-feature and corpus-prep operators that sit between extraction, aggregation,
and report selection:

- `ai/scripts/enrich_k150k_parquet_metadata.py`
- `ai/scripts/combine_full_feature_parquets.py`
- `ai/scripts/extract_full_features.py`
- `ai/scripts/merge_corpora.py`
- `ai/scripts/konvid_to_vmaf_pairs.py`
- `ai/scripts/konvid_to_full_features.py`
- `ai/scripts/phase3_subset_sweep.py`
- `ai/scripts/bvi_dvc_to_full_features.py`

`ai/scripts/extract_k150k_features.py` was deliberately left untouched while
the long-running K150K refresh job is active.

## Findings

The scripts still carried three forms of repeated setup:

1. local `sys.path` mutation for `ai/src`, repository-root imports, or
   `tools/vmaf-tune/src`;
2. direct `argparse.ArgumentParser(...)` construction where the shared
   `aiutils.cli_helpers` formatting and argv capture already fit;
3. a copied BVI-DVC `FULL_FEATURES` / extractor tuple that duplicated the
   canonical `ai.data.feature_extractor` registry.

The duplicated BVI-DVC feature tuple matched the canonical registry at the time
of this sweep, but keeping a second copy makes the next feature-list expansion
easy to miss. Importing the canonical list and deriving extractors through
`_extractors_for(FULL_FEATURES)` keeps BVI-DVC aligned with KoNViD and Netflix
full-feature acquisition.

## Decision

Use `bootstrap_ai_script()` for all touched direct-entry scripts, use
`make_argument_parser()` / `collect_cli_argv()` for parser setup, and keep the
BVI-DVC feature set sourced from the canonical feature extractor registry.

No new ADR is needed because this implements the already accepted helper
invariants from ADR-0680 and ADR-0681.

## Validation

- `.venv/bin/python -m pytest ai/tests/test_enrich_k150k_parquet_metadata.py ai/tests/test_combine_full_feature_parquets.py ai/tests/test_legacy_corpus_extraction_manifests.py ai/tests/test_merge_corpora.py ai/tests/test_konvid_full_features.py ai/tests/test_bvi_dvc_dir_mode.py ai/tests/test_phase3_subset_sweep.py -q`
- `.venv/bin/ruff check ai/scripts/enrich_k150k_parquet_metadata.py ai/scripts/combine_full_feature_parquets.py ai/scripts/extract_full_features.py ai/scripts/merge_corpora.py ai/scripts/konvid_to_vmaf_pairs.py ai/scripts/konvid_to_full_features.py ai/scripts/phase3_subset_sweep.py ai/scripts/bvi_dvc_to_full_features.py`
- `.venv/bin/black --check ai/scripts/enrich_k150k_parquet_metadata.py ai/scripts/combine_full_feature_parquets.py ai/scripts/extract_full_features.py ai/scripts/merge_corpora.py ai/scripts/konvid_to_vmaf_pairs.py ai/scripts/konvid_to_full_features.py ai/scripts/phase3_subset_sweep.py ai/scripts/bvi_dvc_to_full_features.py`
