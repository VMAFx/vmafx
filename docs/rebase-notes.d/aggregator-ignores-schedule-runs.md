## The push aggregator leaves out scheduled runs (2026-10-15)

- `foreignRun()` in `.github/workflows/required-aggregator.yml` also returns true for a
  `schedule` workflow run. Keep the line when rebasing the aggregator; a sync that drops it
  lets a scheduled run's skipped check run fail a master push again.
- `scripts/ci/tests/test_aggregator_event_scope.py` guards it.
