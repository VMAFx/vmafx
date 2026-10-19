## Renovate: one manager owns the CI Python interpreter (2026-10-14)

- `renovate.json` gains a regex custom manager for `PYTHON_CI_VERSION` in
  `build-config.env` and every workflow `python-version:` pin, a package rule
  that disables the github-actions manager's `python` (`uses-with`)
  dependency, and a `CI Python interpreter` group rule with
  `allowedVersions: "<3.15.0"` (ADR-3312). Fork-local file; upstream Netflix has
  no Renovate configuration.
- **On sync**: nothing to merge from upstream. When adding a workflow that
  installs Python, spell the pin as `python-version: "X.Y.Z"` so the manager
  and `check-workflow-versions.py` both read it;
  `scripts/ci/tests/test_renovate_file_patterns.py` fails when they disagree.
