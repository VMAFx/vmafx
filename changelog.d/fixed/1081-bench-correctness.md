**fix(tools/bench):** `vmaf_picture_alloc` return values are now checked in
`bench_feature` and `run_feature_collect`; a failed allocation no longer
causes a null pointer dereference in `yuv_pair_read_frame` (ADR-1081).

**fix(tools/bench):** the end-of-stream flush `vmaf_read_pictures(NULL, NULL, 0)`
return is now captured and logged in both functions; pooling/aggregation
failures are no longer silently swallowed in validation mode (ADR-1081).

**fix(tools/vmaf):** `run_frame_loop` now uses `CLOCK_MONOTONIC`
(POSIX) / `QueryPerformanceCounter` (Windows) via a new `wall_time_s()`
helper instead of `clock()` / `CLOCKS_PER_SEC`. The FPS figure in the
progress spinner is now wall-clock accurate under multi-threaded runs
(previously over-counted by up to `n_threads`) (ADR-1081).
