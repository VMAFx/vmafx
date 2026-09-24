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
    complete-case filtering; exclude zero-variance features both globally and
    after complete-case filtering; serialize unavailable optional analysis as
    empty strict-JSON maps instead of `NaN`; and record all skipped sets in the
    JSON report. Non-finite feature/target rows are removed before analysis,
    non-finite redundancy thresholds are rejected by argparse, and the complete
    report is validated as RFC-8259 JSON before its atomic write.
  - `ai/tests/test_feature_correlation.py`: regression test
    `test_corr_main_skips_non_numeric_columns` verifies that string and metadata
    columns and unavailable all-null features are safely skipped, omitted from
    analysis, constant features cannot warn or enter consensus through a tie,
    non-finite inputs cannot publish `NaN`/`Infinity`, and varying numeric
    feature columns are retained in schema order.
