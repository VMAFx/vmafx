- **Fewer pull-request workflows, held under a budget (ADR-3314).**
  OpenSSF Scorecard reads only the first 30 check suites of a commit, and 31
  pull-request workflows pushed CodeQL out of that window, so the SAST check
  scored 7. `doxygen-public-api.yml`, `scorecard-policy.yml` and
  `upstream-consumers.yml` are now jobs of `docs.yml`, `security-scans.yml` and
  `ffmpeg-integration.yml` under their old check names; `ci-escalate.yml`,
  `observability-compose.yml` and `e2e-k8s.yml` are called from the new
  `pr-label-lanes.yml` on their labels. `scripts/ci/pr_workflow_budget.py`
  (pre-commit and `make lint-actions`) fails when more than 27 workflows start
  on `pull_request`.
