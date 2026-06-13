- **CodeQL error/warning sweep — 18 alerts cleared (2026-05-09)** —
  this PR removes the actionable error- and warning-level CodeQL
  findings that survived the prior security passes. Three classes:
  (1) `cpp/world-writable-file-creation` (3 sites in `svm.cpp`,
  `libvmaf.c`, `feature/cambi.c`) — `fopen("w")` defaults to mode
  `0666 & ~umask` per POSIX `fopen(3)`; the call sites now use
  `open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644)` followed by `fdopen()`
  so the on-disk permissions are pinned regardless of the calling
  process's umask. (2) `cpp/path-injection` (2 sites in
  `tools/vmaf_bench.c`) — the developer benchmark binary's
  `VMAF_TEST_DATA` env-var input now flows through `realpath(3)` /
  `_fullpath` before `fopen`, so symlinks and `..` sequences are
  collapsed and non-existent paths fail-fast. (3)
  `cpp/inconsistent-null-check` (9 sites in `test/test_model.c` and
  `src/framesync.c`) — eight test-side `vmaf_dictionary_get()`
  results are now NULL-checked before deref (tightened, not relaxed,
  per `feedback_no_test_weakening`); one `malloc()` in
  `framesync.c::vmaf_framesync_acquire_new_buf` was crashing on
  OOM and now returns `-ENOMEM` while releasing the acquire-lock.
  (4) `cpp/guarded-free` (4 sites in `dict.c`,
  `feature/feature_extractor.c`, `feature/feature_collector.c`) —
  redundant `if (ptr) free(ptr)` patterns dropped; `free(NULL)` is
  well-defined per C99 §7.20.3.2 / POSIX `free(3)`. No behaviour
  change in the dispatch path; the fixes close GitHub
  code-scanning alerts 63, 160, 161, 242–247, 271, 363–364, 404–406,
  710, 767, 768.
