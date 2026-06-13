## CI: fix build-matrix stale `libvmaf` source paths + ASan + motion_v2 coverage leaks

Three CI regressions fixed in a single sweep:

1. **libvmaf-build-matrix.yml**: `meson setup libvmaf core/build` failed on all
   20+ matrix jobs (Linux, MinGW64, Windows CUDA/SYCL) because the ADR-0700 rename
   moved the source tree from `libvmaf/` to `core/` but the four `meson setup` /
   `ninja -C` invocations in the build-matrix workflow were not updated.
   Fixed by replacing every `libvmaf` source-dir reference with `core`.

2. **tests-and-quality-gates.yml** (ASan sanitizer step): `test_gpu_picture_pool_uaf`
   aborted under ASan because the allocator interceptor returned SIGABRT instead of
   NULL for the intentionally-oversized allocation that exercises the NULL-return
   failure path of `vmaf_gpu_picture_pool_init`. Fixed by adding
   `ASAN_OPTIONS: allocator_may_return_null=1` to the sanitizer test step, matching
   the POSIX `malloc` contract the test relies on.

3. **test_integer_motion_v2_coverage** (LSan leak under ASan+LSan): The three
   multi-frame tests (`test_motion_v2_three_frame_flow`,
   `test_motion_v2_moving_average_branch`, `test_motion_v2_10bit_extract`) manually
   set `ctx->fex->prev_ref = refs[i-1]` as a raw struct copy, bypassing the
   `vmaf_picture_ref` ref-count protocol. The PREV_REF wrapper in
   `feature_extractor.cpp` correctly managed `prev_ref` after every successful
   extract, but the manual overwrite orphaned the internally-taken reference on the
   last frame (count stayed at 2; `context_destroy` released one, the test loop
   released one, but neither released the wrapper's internal reference). Fix: remove
   all manual `prev_ref` assignments and `memset` calls; let the PREV_REF wrapper
   manage the field automatically as it does in production (`libvmaf.c`).
   Also adds `vmaf_picture_pool_flush()` to `picture.c`/`picture.h` to allow
   unit tests to drain the global pixel-buffer pool at teardown.
