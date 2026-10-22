## Job time limits cover their step limits (2026-10-22)

- `MCP Smoke` and `Tiny AI` in `tests-and-quality-gates.yml` have 55-minute
  job limits; the comment above each names the measurement.
  **On sync**: a step `timeout-minutes` raise raises its job's limit in the
  same change; `scripts/ci/check_job_timeout_budget.py` (ADR-3367) fails
  otherwise.
