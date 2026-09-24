- Repaired test-build identity seams behind 25 open CodeQL
  `cpp/unused-static-function` findings. Redundant `pdjson.c` and
  `thread_pool.c` compilations were removed, the three orphan picture-test
  targets now share `test_picture_impl` while the pool error-path target keeps
  compiling `picture_pool.c` directly, intentional source-based test copies now
  isolate their private helper identities in test-local libraries, and the
  allocation-only `vector_unchanged` helper is emitted only with its non-LTO
  test harness. The production ABI and runtime behavior are unchanged; hosted
  closure remains pending a fresh default-branch analysis.
