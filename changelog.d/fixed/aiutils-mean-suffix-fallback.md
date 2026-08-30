Restore `_mean`-suffix column fallback in `train_konvid_mos_head._row_to_features`:
parquet corpora produced by the CHUG materialiser store per-clip temporal
averages as `<feature>_mean` columns; when a mixed parquet file is loaded,
pandas fills absent primary-name slots with NaN rather than omitting the key,
so the fallback must trigger on non-finite values, not just `None`.  Regression
introduced by PR #908 (aiutils refactor).  Also adds `pythonpath = ["ai/src",
"ai/scripts"]` to `[tool.pytest.ini_options]` so `aiutils` is importable
without a pip-editable install.
