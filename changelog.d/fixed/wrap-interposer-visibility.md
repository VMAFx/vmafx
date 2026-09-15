- **The `--wrap` interposers are exported, so the SYCL and MSVC+CUDA lanes link
  again.** libvmaf builds with `-fvisibility=hidden`, and that applies to the
  tests too. `--wrap` rewrites libvmaf's own `calloc`/`malloc`/`strdup`/`realloc`
  references to `__wrap_*`, but a hidden definition in the executable cannot
  satisfy a reference coming from a shared object. GNU ld with gcc tolerated it;
  icpx did not, failing with `hidden symbol '__wrap_calloc' ... is referenced by
  DSO` and `final link failed: bad value` on `Ubuntu SYCL`, `Ubuntu SYCL+CUDA`,
  `FFmpeg SYCL` and `Windows MSVC+CUDA`. Every wrapper in
  `test_fex_pool_growth.c` and `test_fex_ctx_vector.cpp` now carries
  `__attribute__((visibility("default")))`; `nm` confirms all four allocator
  wrappers are global symbols.
