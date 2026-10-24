## Apt step cap for published-rc-licence-companions.yml (2026-10-23)

- The "Fetch the corresponding source" step in `published-rc-licence-companions.yml` carries a `timeout-minutes: 48` cap, sized at least 1.25x the slowest measured healthy run of 37.7m (2026-10-23).
- No runner apt archive cache is configured: package installation runs inside `docker run` of the release builder container, not on the runner.
- The `companion` job limit remains 240 minutes, covering the 48-minute step cap plus 1.25x the rest of the job.
- `published-rc-licence-companions.yml` leaves the uncapped-apt exception list in `scripts/ci/check_job_timeout_budget.py`.
