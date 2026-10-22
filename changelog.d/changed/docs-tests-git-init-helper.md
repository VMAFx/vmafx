- **The docs test suites share one disposable-repository helper.**
  `scripts/docs/tests/test_generators.py` and `test_credits.py` call
  `git_init()` from `scripts/docs/tests/fixture_repo.py` instead of each
  running `git init` with the caller's `GIT_*` variables stripped inline
  (HISS-19).
