---
paths:
  - dev/scripts/smoke-probe-loop.sh
  - dev/scripts/test-smoke-probe-loop.sh
  - dev/scripts/dev-mcp-probe.sh
  - dev/docker-compose.yml
invariant: Exclusive backend selector, backend_used check; Go vmafx-mcp stdio; stable keys; failed sub-check exits 1; paired tests.
---
<!-- markdownlint-disable MD013 -->
# Smoke-probe contract (Research-2083)

`dev/scripts/smoke-probe-loop.sh` is evidence-producing code, not liveness
ping. Preserve all of these constraints when CLI, Go MCP server, or probe
schema is rebased:

1. Every CLI run uses exclusive `--backend cpu|cuda|sycl|hip` selector,
   writes `--json` to real temporary file, and validates both
   `pooled_metrics.vmaf.mean` and matching `backend_used` receipt. Raw YUV
   geometry is `--pixel_format 420 --bitdepth 8`; `yuv420p`, retired
   `--cuda` / `--sycl` / `--hip` switches, and `--no_prediction` are invalid
   probe contracts.
2. MCP probes execute production `vmafx-mcp` Go binary over stdio, perform
   `initialize` followed by `notifications/initialized`, and only then call
   `list_extractors` or `vmaf_score`. client keeps stdin open until    response with request ID 2 arrives; EOF disconnects Go SDK session and
   can otherwise race response. Do not restore retired
   `vmaf-mcp-server`, `list_features`, or `compute_vmaf` operations.
3. JSON keys `mcp_results.list_features` and
   `mcp_results.compute_vmaf` remain stable for existing probe consumers even
   though underlying tool names changed. Error text is JSON-encoded, and
   helper results use non-whitespace delimiter so empty score cannot
   shift duration and error fields.
4. Keep `dev/scripts/test-smoke-probe-loop.sh` and    `test-dev-mcp-smoke-probe` pre-commit hook coupled to changes in either
   script. MCP stub responds before EOF so test also rejects stdio
   close-before-response race. It covers healthy path, error strings
   containing JSON metacharacters/control bytes, and backend receipt
   mismatch.
5. Failed sub-check fails probe: `run_probe` returns 1, `--once` exits 1,
   `FAILED sub-checks:` names each one; record written either way. Never
   restore `exit 0` after `run_probe`. `PROBE_BACKENDS` (default
   `cpu cuda sycl hip`) = declared host backends; left-out backend not run,
   entry `"probed": false`, no error. Unknown name exits 2.
6. `--healthy` = compose healthcheck of `smoke-probe-cron`: newest record
   younger than 2 intervals + 300 s, no error. No record = unhealthy.
7. `_mcp_call` sets `VMAF_MCP_ALLOW` to `${TESTDATA}` plus caller's value
   for probe's own `vmafx-mcp` process only: server in container starts in
   `/build/vmaf`, no repository root, golden pair otherwise refused. Never
   widen `pkg/libvmaf/paths.go` roots or image/compose `VMAF_MCP_ALLOW` for
   probe. MCP `isError` text lands in record after
   `invalid vmaf_score response:`.
