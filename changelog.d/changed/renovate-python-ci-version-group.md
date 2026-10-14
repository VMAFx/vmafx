- **Renovate raises the CI Python interpreter in one pull request.** A custom
  manager now owns `PYTHON_CI_VERSION` in `build-config.env` together with every
  `python-version:` pin under `.github/workflows/`, and the github-actions
  manager no longer raises those pins on its own, so an interpreter update no
  longer lands as a workflow-only pull request that the single-source check
  refuses. Updates stay below Python 3.15 during the 1.0.0 freeze (ADR-3312).
