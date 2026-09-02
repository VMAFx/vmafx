Fix CHUG parquet schema: remove always-NaN `vmaf` column from feature extraction.

The `vmaf` feature was listed in `FEATURE_NAMES` but never emitted by the
extraction pipeline (no `--model` argument passed to libvmaf). In self-vs-self
(NR-from-FR adapter) mode, a model score would be near-constant and provide no
training signal for MOS-head regression. Per Research-0135, removed the column
to keep the schema honest and reduce parquet size. Affected scripts:
`ai/scripts/extract_k150k_features.py` (FEATURE_NAMES, _METRIC_ALIASES,
docstring). Parquet schema now 46 columns (21 features × 2 aggregates) instead
of 48.
