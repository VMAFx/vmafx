## Pull-request workflow budget and label lanes (2026-10-14)

- `pr-label-lanes.yml` is the only workflow that listens for `labeled`; it
  calls `ci-escalate.yml`, `observability-compose.yml` and `e2e-k8s.yml`
  through `workflow_call`. **On sync**: an upstream or fork workflow that adds
  a `pull_request` trigger goes into an existing workflow as a job, or into
  `pr-label-lanes.yml` as a call when a label starts it.
- `doxygen-public-api.yml`, `scorecard-policy.yml` and
  `upstream-consumers.yml` are gone; their jobs live in `docs.yml`,
  `security-scans.yml` and `ffmpeg-integration.yml` with the same check names.
  **On sync**: do not bring the files back.
- `scripts/ci/pr_workflow_budget.py` holds the count at 27 (ADR-3314).
