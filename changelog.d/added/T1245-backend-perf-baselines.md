- **Per-backend performance baseline harness.** New `testdata/bench_backends.py`
  measures `vmaf` CLI throughput one backend at a time and reports the median of
  N timed runs (default 3, after a discarded warmup) with the min/max spread and
  the 1-minute load average sampled around every cell. It runs each fixture
  against both `model/vmaf_v0.6.1.json` and the resolved default model
  (`vmaf_v1.0.16_3d0h`), so the cost a caller pays when passing no `--model` is
  now a measured figure rather than an assumption. `--dry-run` prints the exact
  command lines without touching hardware. Documented in
  [`docs/development/backend-perf-baselines.md`](docs/development/backend-perf-baselines.md);
  methodology in ADR-1185.
