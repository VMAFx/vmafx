## Affected test timeouts (2026-11-20)

- `scripts/ci/run_affected_suites.py`: default per-test timeout 300 s and wall
  cap multiplier `min(4, 1 + max(load1, load5) / CPUs)`; suite overrides win,
  and suites without a per-test timeout retain the unscaled base cap.
  Separate 60 s startup probe checks stock `pytest_timeout` module registration
  before `pytest_configure`: same interpreter, environment and test paths, but
  `python -c` entry rather than `-m pytest`, without the runner's timeout flags.
  Initial conftests load in both startups. Wrapper modules and registration
  conditional on those flags or later hooks are outside the probe's scope.
  Missing or disabled plugins cannot drop explicit registry timeouts; discard
  any probe-time JUnit output before the test run.
- `scripts/ci/tests/test_run_affected_suites.py`: timeout and load controls.
- `docs/development/test-suites.md`: runner timeout behavior.
- `changelog.d/fixed/affected-tests-load-scaled-cap.md`: release note.
