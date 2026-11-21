- **The fail-closed CI audit now runs when a tracked `pyproject.toml` changes.**
  The `fail-closed-ci-contract` pre-commit hook includes `pyproject.toml` in its
  file pattern, so the pytest warning-filter audit runs locally as well as in CI.
