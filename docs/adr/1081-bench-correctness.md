<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-1081: vmaf_bench correctness — unchecked alloc returns and wall-clock timer

- **Status**: Accepted
- **Date**: 2026-06-06
- **Deciders**: Lusoris
- **Tags**: `tools`, `bench`, `correctness`, `clock`

## Context

Two correctness defects were identified in the benchmark and CLI tools during a
focused audit of `core/tools/vmaf_bench.c` and `core/tools/vmaf.c`.

**vmaf_bench.c — unchecked `vmaf_picture_alloc` returns.**
`bench_feature` (warm-up frame, lines 498-499 before this fix) and its timed
loop (lines 517-518) called `vmaf_picture_alloc` without checking the return
value.  `vmaf_picture_alloc` returns a negative errno on `-ENOMEM` or
`-EINVAL`; on failure it leaves the `VmafPicture` zero-initialised.  The
immediately-following `yuv_pair_read_frame` writes to `ref->data[0]`, which
is then a null pointer dereference.  The same pattern repeated in
`run_feature_collect` (lines 715-716 before this fix).

**vmaf_bench.c — silently discarded flush return.**
Both `bench_feature` and `run_feature_collect` called
`vmaf_read_pictures(vmaf, NULL, NULL, 0)` (the end-of-stream flush) and
discarded the return value without a `(void)` cast or error check.  A
non-zero return from the flush signals a pooling or aggregation failure;
swallowing it means validation mode can report `PASS` even when the
extractor failed to finalise scores.

**vmaf.c — `clock()` for wall-time FPS spinner.**
`run_frame_loop` used `clock()` / `CLOCKS_PER_SEC` to compute the FPS
figure shown in the progress spinner.  `clock()` measures aggregate CPU
process time: under a multi-threaded run (`n_threads > 1`, the default)
each worker thread's CPU time is summed, so `clock()` over-counts elapsed
time by up to `n_threads`.  The displayed FPS was therefore implausibly
high for any multi-threaded invocation.  `vmaf_bench.c` already uses
`CLOCK_MONOTONIC` (and `QueryPerformanceCounter` on Windows) for its
timing; `vmaf.c` should be consistent.

## Decision

1. In `bench_feature` and `run_feature_collect` in `vmaf_bench.c`:
   check every `vmaf_picture_alloc` return and propagate errors through
   the existing `bench_cleanup` / early-return unwind paths.
2. Capture and log the `vmaf_read_pictures(vmaf, NULL, NULL, 0)` flush
   return in both functions.
3. In `vmaf.c`, introduce a file-static `wall_time_s()` helper
   (`CLOCK_MONOTONIC` on POSIX, `QueryPerformanceCounter` on Windows)
   and replace the `clock()` / `CLOCKS_PER_SEC` expression in
   `run_frame_loop` with calls to `wall_time_s()`.

No public API, no CLI flag, and no score computation path is changed.
The FPS figure shown by the spinner will now reflect wall time rather
than CPU time.

## Alternatives considered

- **`(void)`-cast the alloc returns**: rejected.  The return encodes a
  real error (`-ENOMEM`, `-EINVAL`); silencing it with a cast perpetuates
  the latent null-deref.
- **Keep `clock()` but divide by `n_threads`**: rejected.  The thread
  count is not available to `run_frame_loop` (it is buried in `cfg`
  before `vmaf_init`), and the correction is approximate.  Monotonic
  wall time is the correct primitive.
- **Add a `time.h`-only portable fallback instead of QPC on Windows**:
  `time()` has 1-second granularity and `clock()` has the CPU-time
  problem.  QPC is the standard Windows high-resolution wall-clock
  primitive; `<windows.h>` is already conditionally included in the
  file for `isatty` / `fileno`.

## Consequences

- `bench_feature` and `run_feature_collect` now surface allocation
  failures instead of crashing with a null dereference.
- Flush errors in validation mode are now surfaced on `stderr` rather
  than silently ignored.
- The FPS spinner in `vmaf` (the main CLI) now reports wall-clock FPS,
  which is accurate regardless of `--threads`.

## References

- `core/tools/vmaf_bench.c` — `bench_feature` and `run_feature_collect`
- `core/tools/vmaf.c` — `run_frame_loop`, `wall_time_s` helper
- ADR-1081 (this document)
