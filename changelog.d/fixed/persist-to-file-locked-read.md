- Python harness: `persist_to_file` reads its JSON cache only while it holds the
  cache's file lock, at decoration time too. On Windows an open of the cache that
  overlapped another process's `os.replace` failed with a sharing violation, which
  turned the `Windows MSVC+CUDA (full)` lane on `master` red. A cache file that cannot
  be read when a call adds an entry now raises `PersistCacheError` instead of being
  overwritten (`compat/python-vmaf/tools/decorator.py`).
