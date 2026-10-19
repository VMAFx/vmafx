- **The Coverage Gate runs its Python suite in four shard steps of at most
  30 minutes each.** `scripts/ci/run_pytest_shard.py` keeps every fourth test
  of pytest's filtered collection, and one assertion step fails the job when
  any shard fails. The three-day test-timeout exception of
  [ADR-3337](docs/adr/3337-praetor-pin-320b058d-test-timeout.md) is removed.
