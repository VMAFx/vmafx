---
name: run-netflix-bench
description: Run the Netflix benchmark suite (testdata/bench_all.sh) and diff the output against testdata/netflix_benchmark_results.json. Reports any delta per (resolution, feature, backend).
---
<!-- markdownlint-disable MD013 -->

# /run-netflix-bench

## Steps

1. `./build/tools/vmaf`, `./build/tools/vmaf_bench` missing ->
   `/build-vmaf --backend=<target>` first.
2. `cd testdata && ./bench_all.sh > /tmp/bench-run.json`.
3. `python3 testdata/compare_combined.py /tmp/bench-run.json testdata/netflix_benchmark_results.json`.
4. delta > 1e-6 (relative) ->
   `(resolution, feature, backend, expected, got, delta)`.
5. ok -> exit 0. else 1.

## Notes

- `testdata/netflix_benchmark_results.json`: not golden.
  regen: `/regen-snapshots` + justification.
- Netflix golden-data gate = Python `assertAlmostEqual` suite,
  `make test-netflix-golden`.
