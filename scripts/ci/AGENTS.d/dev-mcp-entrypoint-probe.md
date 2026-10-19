---
paths:
  - scripts/ci/tests/test-dev-mcp-entrypoint-probe.sh
  - dev/scripts/dev-mcp-entrypoint.sh
invariant: `_probe_with_retry` runs one program name as argv[0], never `eval`; keep it top-level with its exact opening line.
area: release
---
<!-- markdownlint-disable MD013 MD060 -->
# dev-MCP entrypoint probe

## Workflow coupling

| Script | Workflow lane(s) that invoke it | What couples them |
| --- | --- | --- |
| `tests/test-dev-mcp-entrypoint-probe.sh` | `.pre-commit-config.yaml` — `test-dev-mcp-entrypoint-probe` hook (pre-commit + pre-push); required `Pre-Commit` CI job | Guards `_probe_with_retry` in `dev/scripts/dev-mcp-entrypoint.sh`: probe is one program name run as argv[0], never `eval "${cmd}"` (same PR #350 / PR #414 history). test lifts function out of real entrypoint with `awk` between `^_probe_with_retry() {` and first `^}` — keep function at top level with that exact opening line, or test exits 2. It stubs `sleep`, so ten-attempt / always-return-0 contract is asserted too. End shell tests with `[[ "$fail" -eq 0 ]]`, not `exit 0`: trailing unconditional `exit` makes shellcheck 0.11 report every trap handler and stub as SC2329. |
