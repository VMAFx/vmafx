---
name: run-netflix-bench
description: Run the Netflix benchmark suite (testdata/bench_all.sh) and diff the output against testdata/netflix_benchmark_results.json. Reports any delta per (resolution, feature, backend).
---
<!-- markdownlint-disable MD013 -->

# /run-netflix-bench

## Steps

1. `./build/tools/vmaf` and `./build/tools/vmaf_bench` must exist; if not ->
   run `/build-vmaf --backend=<target>` first.
2. `cd testdata && ./bench_all.sh > /tmp/bench-run.json`.
3. Compare: `python3 testdata/compare_combined.py /tmp/bench-run.json \
   testdata/netflix_benchmark_results.json`.
4. Row with delta > 1e-6 (relative) -> emit summary line with
   `(resolution, feature, backend, expected, got, delta)`.
5. All rows within tolerance -> exit 0; else exit 1.

## Notes

- `testdata/netflix_benchmark_results.json` fork-committed (not Netflix golden
  data); regenerate only via `/regen-snapshots` with justification.
- NOT Netflix CPU golden-data gate; gate = Python `assertAlmostEqual` suite
  invoked by `make test-netflix-golden`.
