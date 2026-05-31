<!-- markdownlint-disable MD060 -->
# Research-0110: testdata bench_perf portability

- **Date**: 2026-05-14
- **Area**: benchmark harnesses
- **Related ADR**: [ADR-0429](../adr/0429-testdata-bench-perf-portability.md)

## Question

What is the smallest change that turns `testdata/bench_perf.py` from a
single-workstation script into a reusable benchmark harness without changing
the committed performance snapshots?

## Findings

- `testdata/benchmark_netflix.py` already honours `VMAF_FFMPEG` and
  `VMAF_YUVDIR`, so it can run from a worktree or CI host.
- `testdata/bench_perf.py` still hardcoded
  `/home/kilian/dev/ffmpeg-8/install/bin/ffmpeg` and a large external BBB MP4
  under `/home/kilian/dev/ffmpeg-testing/`.
- The in-tree 4K raw BBB pair is present and is the useful required
  performance fixture. The 1080p raw pair and MP4 decode fixture are not always
  present in fresh checkouts.
- The script had no dry-run/list mode, so argument and command construction
  could not be unit-tested without FFmpeg and GPU hardware.

## Decision Matrix

| Option | Pros | Cons | Result |
|---|---|---|---|
| Keep hardcoded paths | No code churn. | Only works on one workstation; blocks raw tests when optional MP4 is absent. | Rejected. |
| Move all benchmark logic to `bench_all.sh` | One benchmark entry point. | Loses the FFmpeg lavfi/decode path this script measures. | Rejected. |
| Add CLI/env configuration plus dry-run/list modes | Keeps the existing workload, makes local paths explicit, and gives CI a hardware-free test seam. | Slightly larger script surface. | Chosen. |

## Validation

- Unit tests cover fixture selection, backend validation, and FFmpeg argv
  construction without executing FFmpeg.
- `--list-tests` and `--dry-run` exercise the operator-facing paths without
  touching GPU hardware.
