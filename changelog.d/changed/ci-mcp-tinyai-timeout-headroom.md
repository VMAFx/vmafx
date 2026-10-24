- **The `MCP Smoke` and `Tiny AI` jobs no longer time out on a slow but healthy
  run.** Their limits rise to 35 and 45 minutes, at least 1.25 times the longest
  successful run measured on 2026-10-22 (24 and 31 minutes). Each job's
  `Install build deps` step now has its own 8-minute limit, and apt retries a
  failed download three times with a 30-second connection and data timeout, so a
  stalled package mirror fails or recovers within minutes instead of using up the
  whole job (`.github/workflows/tests-and-quality-gates.yml`).
