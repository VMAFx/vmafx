### Fixed

- **core/picture_pool**: Port ADR-0960 (round-25 audit A.2/A.3 error-path
  `pthread_cond_signal` on `return_to_pool` and `pic->priv = nullptr` dangling-pointer
  guards), ADR-1020 (slot snapshot to local while holding mutex before unlock in
  `vmaf_picture_pool_fetch`), and ADR-0778 (two-pass picture preallocation to prevent
  buffer leak on failure) from `core/src/picture_pool.c` into the live C++ twin
  `core/src/picture_pool.cpp`. Add `test_picture_pool_cpp_error_paths` in
  `core/test/meson.build` to directly exercise the C++ twin under the test suite.
