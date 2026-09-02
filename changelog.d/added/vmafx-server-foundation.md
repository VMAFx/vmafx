- **vmafx-mcp HTTP transport + observability foundation (ADR-0701).**
  The `vmaf-mcp` / `vmafx-mcp` server now supports `--transport http` mode, which
  starts an aiohttp HTTP listener with four endpoints:
  - `GET /healthz` — liveness probe (always 200 while the process is alive);
  - `GET /readyz` — readiness probe (200 once the vmaf binary is reachable, 503 otherwise);
  - `GET /metrics` — Prometheus exposition format (`vmaf_scoring_requests_total`,
    `vmaf_scoring_errors_total`, `vmaf_scoring_duration_seconds`);
  - `POST /v1/score` — REST wrapper over the `vmaf_score` tool for curl-based scoring.

  Structured JSON logging replaces the root handler when HTTP mode is active.
  SIGTERM and SIGINT trigger graceful shutdown via the asyncio event loop's
  `finally` block. 12-factor env-var config: `VMAFX_PORT`, `VMAFX_LOG_LEVEL`,
  `VMAFX_VMAF_BINARY`, `VMAFX_MODEL_DIR`.

  HTTP mode requires the `[http]` optional extra (`aiohttp>=3.9`, `prometheus-client>=0.20`);
  the base install is unaffected. The default stdio transport (JSON-RPC over
  stdin/stdout) is unchanged for existing IDE integrations.

  Foundation for the k8s deployment stack paired with the Helm chart (ADR-0699,
  PR #1570) and production Dockerfile (ADR-0698, PR #1572).

  References: [ADR-0701](docs/adr/0701-vmafx-cloud-native-redesign.md),
  [docs/mcp/http-transport.md](docs/mcp/http-transport.md).
