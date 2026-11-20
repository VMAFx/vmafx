## Affected test timeouts (2026-11-20)

- `scripts/ci/run_affected_suites.py`: default per-test timeout 300 s and wall
  cap multiplier `min(4, 1 + max(load1, load5) / CPUs)`; suite overrides win.
- `scripts/ci/tests/test_run_affected_suites.py`: timeout and load controls.
- `docs/development/test-suites.md`: runner timeout behavior.
- `changelog.d/fixed/affected-tests-load-scaled-cap.md`: release note.
