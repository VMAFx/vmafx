Fix three RC-gate failures surfaced by the pre-release validation matrix:

- **Go `t.Setenv` parallelism panic** (`cmd/vmafx-mcp/server_test.go`): removed
  `t.Parallel()` from `TestVmafScoreTool`, which called `t.Setenv` — a
  combination Go 1.22+ forbids because the env restore races with the parallel
  scheduler.

- **TSan allocator abort** (`core/test/meson.build`): added
  `TSAN_OPTIONS=allocator_may_return_null=1` alongside the existing
  `ASAN_OPTIONS` entry for `test_gpu_picture_pool_uaf`. The test intentionally
  requests a ~192 GB pool to exercise the alloc-failure UAF path; without the
  TSan override libtsan called `abort()` (exit 66) rather than returning NULL.

- **`test_cli` DNN-absent hard-fail** (`core/test/dnn/test_cli.sh`): added an
  early probe that detects `--tiny-model … built without DNN support` and exits
  77 (meson skip) instead of 1 when ORT was absent at configure time. The test
  is not disabled; it skips only when DNN is genuinely not compiled in.
