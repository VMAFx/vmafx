## Added

- **`ai/tests/test_aiutils_misc.py`**: covers `aiutils.time_utils.now_iso_8601`
  and the `aiutils.__getattr__` lazy-import shim for `write_parquet_atomic`.
- **`ai/tests/test_run_manifest_branches.py`**: branch-coverage top-up for
  `aiutils.run_manifest` — opaque-type fallback in `normalise_manifest_value`,
  directory and missing-path variants in `describe_path`, `None` and sequence
  entries in `describe_paths`, and the `repo_relative_path` out-of-repo fallback.
- **`ai/tests/test_subprocess_utils.py`**: covers all branches of
  `aiutils.subprocess_utils.run_cmd` (capture mode, check=False, timeout,
  kwargs forwarding).
- **`ai/tests/test_parquet_utils.py`**: round-trip write/read, overwrite, atomic
  cleanup on exception, and kwargs forwarding for `aiutils.parquet_utils.write_parquet_atomic`.
- **`ai/tests/test_jsonl_utils.py`** (fix): replaced import of
  `dumps_jsonl_row` (absent on master) with tests that match the actual
  `aiutils.jsonl_utils` API (`iter_jsonl` only).
- Overall `aiutils` statement coverage: 73% → 100%.
