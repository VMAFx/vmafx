Fix four CLI plumbing bugs in the shipped `vmaf` binary, all rooted in drift
between the dead `core/tools/vmaf.c` C twin and the live C++23 `vmaf.cpp` /
`cli_parse.cpp` translation units (ADR-0809) that the binary actually compiles:

- `vmaf --help` now prints usage to **stdout** and exits **0** (previously
  stderr + exit 1), so it can be piped or grepped without a phantom error. An
  actual usage error still writes to stderr and exits 1.
- The return value of `vmaf_write_output_with_format` is now **checked**: a
  failed write (bad path, ENOSPC, permission denied) emits
  `problem writing output to <path> (err=<n>)` and exits non-zero instead of a
  silent exit 0 over a stale or partial output file.
- The FPS meter now reports **wall-clock** FPS via `clock_gettime(CLOCK_MONOTONIC)`
  (QueryPerformanceCounter on Windows). It previously used `clock()` /
  `CLOCKS_PER_SEC`, which sums CPU time across worker threads and over-counts the
  rate by up to n_threads.
- A **no-frames guard** rejects a run that decodes zero frames (empty/short
  input, frame-skip past EOF) with a `no frames decoded` diagnostic and a
  dedicated exit code `VMAF_EXIT_NO_FRAMES_DECODED` (101). Without it the pooling
  path computed `picture_index - 1`, underflowing the unsigned counter to
  UINT_MAX and feeding a garbage index range into `vmaf_score_pooled`.

Also deletes the dead, unreferenced `core/tools/vmaf.c` (the `vmaf` binary builds
from `vmaf.cpp`; `vmaf.c` was in no Meson target) and re-points the stale
`.semgrep.yml` / `.cppcheck-suppressions.txt` / docs-drift hook / sync-upstream
skill / AGENTS references at `vmaf.cpp`. `cli_parse.c` is retained — it is still
the translation unit compiled into the `test_cli_parse`, `test_cli_parse_long_only_args`,
and `fuzz_cli_parse` harnesses, and already carried the matching `--help` fix.

Golden-safe: CLI plumbing only; the testdata 576x324 pair still scores pooled
mean VMAF 94.32301 on CPU.
