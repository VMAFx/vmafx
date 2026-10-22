- **`MCP Smoke` and `Tiny AI` no longer cancel a healthy run after a slow
  package mirror.** Their job limits rise to 55 minutes, the apt step's 20
  minutes plus 1.25 times the longest healthy build measured on 2026-10-22, and
  `scripts/ci/check_job_timeout_budget.py` now fails any workflow job whose
  limit is below the sum of its step limits (ADR-3367,
  `.github/workflows/tests-and-quality-gates.yml`).
