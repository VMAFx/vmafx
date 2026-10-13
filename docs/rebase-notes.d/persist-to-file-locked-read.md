## `persist_to_file` reads its cache under the file lock (2026-10-13)

- `compat/python-vmaf/tools/decorator.py` is Netflix-inherited. The fork reads the
  cache of `persist_to_file` through `_read_json_cache()`, and every read, the one at
  decoration time included, runs inside `_file_lock(f"{file_name}.lock")`, the lock
  that also covers `_write_json_cache_atomic()`. On Windows an unlocked open that
  overlaps a peer's `os.replace` fails with `PermissionError` 13.
- `_read_json_cache()` returns `{}` for an absent file and raises
  `PersistCacheError` for any other read or parse error; a call no longer swallows the
  error and overwrites the file.
- On sync: keep the locked reads and the raising `_read_json_cache()`.
  `test_persist_to_file_touches_cache_only_under_file_lock` and
  `test_persist_to_file_corrupt_cache_at_call_time_raises_without_overwrite` in
  `compat/python-vmaf/tests/test_decorator_extended.py` fail on the old form.
