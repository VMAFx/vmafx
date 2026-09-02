- Added wall-clock perf regression gate
  (`scripts/perf/check-regression.py`) that compares a fresh
  `bench-multi-resolution.sh` run against the committed
  `testdata/perf_multi_resolution.json` baseline and fails CI when any
  `(resolution, backend, metric)` cell regresses by more than +/- 5%
  wall-clock. Wired into `tests-and-quality-gates.yml` as a CPU-only
  job with `continue-on-error: true` for one release cycle so
  cross-runner variance data can inform whether the 5% tolerance
  needs tightening before promotion to a required check. See
  [ADR-0907](docs/adr/0907-perf-regression-gate-wall-clock.md).
