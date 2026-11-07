- **Every CI job now carries its own `timeout-minutes`.** The 28 jobs that ran
  on GitHub's 360-minute default are bounded at the larger of 5 minutes and
  twice the 95th percentile of their recent successful runs, and
  `scripts/ci/check_job_timeout_budget.py` fails in pre-commit and
  `make verify` when a job that runs steps declares no limit. The polling
  Required Checks Aggregator keeps its 240-minute deadline under a dated
  category D exception in `.config/lint-exceptions.d/job-timeout.toml`.
