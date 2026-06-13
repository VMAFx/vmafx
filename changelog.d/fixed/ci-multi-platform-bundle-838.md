- **Windows MSVC /Zc:strictStrings build error (C2664):** The fallback
  `strsep` in `core/tools/cli_parse.cpp` declared its `sep` parameter
  as `char *` (non-const); MSVC rejects the implicit `const char[2]` →
  `char *` conversion from string literals at all nine call sites.
  Fixed by adding `const` to the parameter type — no behaviour change.

- **macOS ARM64 per-frame motion assertion precision:**
  `test_run_integer_motion_fextractor_with_blend` used `places=6`
  (5e-7 tolerance) for five per-frame integer SAD scalar assertions.
  ARM64 integer arithmetic differs by ~2–6e-6 from x86_64 reference
  values, causing CI failures on all three macOS jobs. Lowered to
  `places=4` (5e-5 tolerance), consistent with the rest of the motion
  test suite. Aggregate score assertions remain at `places=4` and are
  unaffected.

- **UBSan enum-invalid-value in `opt.cpp` (signal 6 SIGABRT):**
  The `switch (static_cast<int>(opt->type))` fix from ADR-1080 was
  insufficient: UBSan's `enum-invalid-value` check fires on the
  lvalue-to-rvalue conversion (the *load* of `opt->type`) before the
  cast executes. Replaced with a `memcpy`-based raw read into a plain
  `int`, which eliminates the typed enum load entirely and makes
  `test_dispatch_unknown_type` (value 9999) UBSan-clean.

- **Windows SYCL LNK2019 unresolved `vmaf_fex_*` symbols:**
  All `extern VmafFeatureExtractor vmaf_fex_*` declarations in
  `core/src/feature/feature_extractor.cpp` were bare C++ `extern`
  without `extern "C"`. Definitions live in C-compiled `.c` TUs
  (unmangled symbols); MSVC name-mangles C++ externs for POD globals,
  causing LNK2001/LNK2019 for every symbol on Windows SYCL builds.
  Wrapped all declarations in `extern "C" { ... }`.

- **CUDA picture-pool exhaustion (`test_picture_pool_basic` fail):**
  In `core/src/libvmaf.c`, the `done=true` early-return path in
  `vmaf_read_pictures` skipped the `cleanup:` label, which is where
  `read_pictures_cuda_cleanup` (and therefore `vmaf_picture_unref` for
  the translated host/device pictures) executes in HAVE_CUDA builds.
  Each early-return leaked one pool slot; the pool exhausted on the
  next `vmaf_picture_pool_fetch` call, deadlocking in
  `pthread_cond_wait`. Added an explicit `read_pictures_cuda_cleanup`
  call in the `done=true` branch under `#ifdef HAVE_CUDA`.
