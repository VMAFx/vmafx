- **The `Install build deps` step of `MCP Smoke` and `Tiny AI` no longer cancels
  healthy runs on a slow package mirror.** Its limit rises from 8 to 20 minutes,
  at least 1.25 times the slowest healthy run measured on 2026-10-22 (11 minutes
  51 seconds); apt's retries and 30-second timeouts still end a real stall
  (`.github/workflows/tests-and-quality-gates.yml`).
