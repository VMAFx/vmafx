- **The allocation-failure tests no longer double-free under an optimised LTO
  build.** `test_fex_ctx_vector` and `test_registration_partial_copy` inject
  failures by intercepting a symbol — the first through GNU `-Wl,--wrap=`, the
  second through ELF interposition. Both redirect *references* resolved at link
  time, and the project builds with `b_lto=true`, so LTO resolves those calls
  internally before the linker ever sees a reference and neither interceptor
  runs. That is not a quiet loss of coverage: the injected allocation then
  succeeds, `feature_extractor_vector_append` takes ownership of a context the
  test still holds, and the run ends in `free(): double free detected in tcache
  2` — reported as SIGSEGV on the CI runner. Debug builds hid it because nothing
  inlines at `-O0`. The two harnesses are now compiled in only where they can
  actually intercept, so a debug or `-Db_lto=false` build and the sanitizer lane
  still carry the fault-injection coverage while the release+LTO lane builds the
  identity tests without it. `override_options : ['b_lto=false']` on the test
  targets is **not** an alternative — those targets consume LTO bitcode from the
  libraries and fail to link with `file format not recognized`.
