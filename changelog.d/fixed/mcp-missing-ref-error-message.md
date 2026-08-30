- **MCP `vmaf_score` missing-`ref` error:** restored the `missing required
  argument: 'ref'` phrasing (with the `no_reference` hint) on both the Go and
  Python servers. ADR-1117's `no_reference` support had changed the message to
  `'ref' is required …`, which no longer matched the `_call_tool` hardening
  contract (`test_mcp_hardening_wave1.py`) — turning the master-push-only "MCP
  Smoke" gate red. Both servers now emit the identical, contract-matching
  message.
