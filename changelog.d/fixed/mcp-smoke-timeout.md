- The required MCP Smoke check no longer times out on healthy runs. Most runs
  take 11 minutes against a 12-minute limit, so a slightly slow runner
  cancelled a run whose every step passed and turned `master` red; the limit
  is now 25 minutes.
