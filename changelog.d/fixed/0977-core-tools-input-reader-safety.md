- `core/tools/y4m_input.c` and `core/tools/yuv_input.c`: input-reader
  safety fixes (ADR-0977). Three coordinated defects in the vendored
  Daala YUV / Y4M parsers + the `vmaf_bench` binary:
  (1) `y4m_input_open_impl` ignored failed `malloc()` returns and
  surfaced a NULL `dst_buf` to the caller, causing a SIGSEGV at the
  first `video_input_fetch_frame` via `fread(NULL, …)` — now returns
  -1 cleanly and frees any partial allocation;
  (2) `dst_buf_sz` was computed in `int` / `unsigned` precision in
  both readers, wrapping to a too-small size for headers near the
  32-bit ceiling and corrupting the heap on the first read — now
  cast to `size_t` before the multiply, matching the existing
  `(size_t)pic_w * pic_h * 3 * 2` precedent in the 4:4:4 paths;
  (3) `vmaf_bench::bench_feature` leaked `VmafCudaState` /
  `VmafSyclState` on every success and most error paths because
  the state pointers were local to their `#ifdef` blocks — now
  hoisted to function scope and freed under a single
  `bench_cleanup` label, mirroring the T5 state-leak audit fix
  already in `run_feature_collect`.
- New regression test `test_y4m_alloc_failure` (fast suite, POSIX-only)
  drives the y4m parser against a 65535×65535 4:4:4 12-bit header
  with `RLIMIT_AS` clamped below the required buffer size, forcing
  `malloc` to fail deterministically. Fails on pre-fix tree
  (parser reports success then crashes); passes post-fix.
