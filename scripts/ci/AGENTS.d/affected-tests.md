---
paths:
  - scripts/ci/run_affected_suites.py
  - scripts/ci/tests/test_run_affected_suites.py
  - scripts/lib/push_head.py
  - .pre-commit-config.yaml
invariant: Focused push tests reuse suite registry; exact names only, unique short names, unmatched paths NOT RUN.
area: gates
---
# Affected push tests

- Runner: `run_affected_suites.py`; suite registry, locks, venv cache reused.
- `--focused`: changed tests; exact path/module tokens; unique tracked basenames;
  unique Python stems in imports. Pure selector; no second mapping.
- Unmatched source: `NOT RUN <path>: no test names this file`.
- Empty suite: `NOT RUN (no test names a changed file)`; zero passed tests.
- `--strict`: unrun suite fails. Other statuses: 0 / 1 / 2 unchanged.
- `--merge-base REF`: merge-base to HEAD; exclusive with base/head/files.
- Shared `scripts/lib/push_head.py`: outgoing ref != checkout HEAD -> refuse;
  all `GIT_` environment removed from ref-resolution subprocess.
- Hook `affected-tests`: always, serial, pre-push; first venv build, ai ~6 GB.
- Regression: `tests/test_run_affected_suites.py`; mypy consumer:
  `/scripts/git-hooks/test-pre-push-mypy.py`.
