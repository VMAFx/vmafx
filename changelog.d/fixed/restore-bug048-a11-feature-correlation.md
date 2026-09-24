<!-- markdownlint-disable MD041 MD013 -->
- **AI feature correlation non-numeric column filter restored (BUG-048 Sec A11)**:
  Restores the non-numeric column filter in `ai/scripts/feature_correlation.py`
  originally landed in commit `5fc73913b` (#1552) and silently reverted in
  `384d97d03`:
  - `ai/scripts/feature_correlation.py`: filter candidate columns through
    `select_dtypes(include="number")` before calling
    `to_numpy(dtype=np.float64)`, preventing `ValueError: could not convert
    string to float` when processing parquets with string metadata columns
    (e.g. `codec`, `chug_orientation`); exclude all-null numeric features before
    complete-case filtering; and record both skipped sets in the JSON report.
  - `ai/tests/test_feature_correlation.py`: regression test
    `test_corr_main_skips_non_numeric_columns` verifies that string and metadata
    columns and unavailable all-null features are safely skipped, omitted from
    analysis, and numeric feature columns are retained in schema order.
