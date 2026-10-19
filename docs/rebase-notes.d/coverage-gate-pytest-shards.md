## Coverage Gate: Python suite in four shards (2026-10-17)

- The single 55-minute pytest step of the `coverage` job in
  `.github/workflows/tests-and-quality-gates.yml` is now a preparation step and
  four steps `Run Python test suite, shard N/4` (ids
  `python_coverage_shard_N`), each under `timeout --kill-after=30s 30m`
  through `scripts/ci/run_pytest_shard.py`. The step
  `Assert every Python coverage shard passed` reads all four outcomes.
- A rebase that touches the job keeps every step at 30 minutes or less (the
  praetor test-timeout audit flags any `<n>m` above 30 on a line, comments
  included) and does not bring back `.config/lint-exceptions.d/test-timeout.toml`.
  `scripts/ci/test_fail_closed_ci.py` and
  `scripts/ci/tests/test_run_pytest_shard.py` guard the layout and the shard
  selection.
