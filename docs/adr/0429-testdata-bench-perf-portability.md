<!-- markdownlint-disable MD060 -->
# ADR-0429: testdata bench_perf is configurable

- **Status**: Accepted
- **Date**: 2026-05-14
- **Deciders**: Lusoris maintainers
- **Tags**: benchmarks, testdata, tooling

## Context

`testdata/bench_perf.py` captures the FFmpeg lavfi path used by the fork's
historical `perf_benchmark_results.json` snapshot. Unlike
`testdata/benchmark_netflix.py`, it still embedded one workstation's FFmpeg
path and one external Big Buck Bunny MP4 path. That made the raw-YUV benchmark
unnecessarily fail on hosts that had the committed raw fixtures but not the
external MP4.

## Decision

`bench_perf.py` will accept CLI options and matching environment variables for
the FFmpeg binary, backend selection, run count, timeout, SYCL render node,
runtime library path, output path, and optional MP4 reference. Missing optional
fixtures are skipped by default; `--require-all` restores strict lab-run
behaviour. `--list-tests` and `--dry-run` expose hardware-free validation
surfaces.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep hardcoded local paths | Zero migration for the original host. | Fresh checkouts and worktrees fail before measuring available raw fixtures. | Keeps the gap open. |
| Delete `bench_perf.py` and use `bench_all.sh` only | Fewer harnesses. | `bench_all.sh` measures the `vmaf` CLI path, not FFmpeg decode/upload/filter overhead. | The workloads answer different questions. |
| Require every fixture by default | Makes lab runs strict. | Optional external MP4 absence blocks the committed raw fixture benchmarks. | Strictness remains available through `--require-all`. |

## Consequences

- **Positive**: the FFmpeg lavfi benchmark can run from arbitrary worktrees and
  can be smoke-tested without FFmpeg/GPU hardware.
- **Negative**: the script has a slightly larger CLI surface to document.
- **Neutral / follow-ups**: the committed JSON snapshots are unchanged; formal
  snapshot regeneration remains a separate, hardware-pinned action.

## References

- [docs/benchmarks.md](../benchmarks.md)
- [testdata/benchmark_netflix.py](../../testdata/benchmark_netflix.py)
- Source: `req` ("do next gap")
