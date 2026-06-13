<!-- markdownlint-disable MD013 MD060 -->
# ADR-0701: vmafx-server HTTP transport + observability foundation

- **Status**: Proposed
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `mcp`, `server`, `http`, `observability`, `cloud-native`, `k8s`, `vmafx`

## Context

The VMAFX rebrand (ADR-0686) includes a cloud-native redesign of the MCP server,
making it a first-class server-mode process deployable in Kubernetes alongside
cloud-native observability tooling. The existing MCP server runs exclusively over
stdio (JSON-RPC over stdin/stdout), which is the correct default for IDE/MCP-client
integration but is unsuitable for:

- Kubernetes liveness/readiness probes (`/healthz`, `/readyz`).
- Prometheus-based metrics scraping (`/metrics`).
- REST clients that do not speak the JSON-RPC MCP protocol.
- Container-orchestrated deployments that need SIGTERM graceful-shutdown support.

The server is implemented in Python (`mcp-server/vmaf-mcp/`). A rewrite in Go or gRPC
was considered but deferred (see Alternatives below); extending the existing Python
service with an optional HTTP transport is sufficient for the Phase 3A foundation.

## Decision

We will add an optional `--transport http` mode to the existing `vmaf-mcp` / `vmafx-mcp`
entry point. When activated, the server starts an aiohttp HTTP listener on a configurable
port (default 8080) and exposes:

- `GET /healthz` — liveness probe (always 200 while the process is alive).
- `GET /readyz` — readiness probe (200 once the vmaf binary is reachable; 503 otherwise).
- `GET /metrics` — Prometheus exposition format via `prometheus-client`.
- `POST /v1/score` — thin JSON wrapper over the existing `_run_vmaf_score` tool.

The implementation lives in `mcp-server/vmaf-mcp/src/vmaf_mcp/http_transport.py` and is
gated behind an optional dependency group `[http]` (`aiohttp>=3.9.0`,
`prometheus-client>=0.20.0`) to keep the base MCP install light.

12-factor (§III) environment-variable config: `VMAFX_PORT`, `VMAFX_LOG_LEVEL`,
`VMAFX_VMAF_BINARY`, `VMAFX_MODEL_DIR`. CLI flags take precedence; env vars take
precedence over compiled-in defaults.

Structured JSON logging replaces the root logger's handlers when HTTP mode is active.
SIGTERM and SIGINT trigger graceful shutdown; in-flight requests drain within the
`asyncio` event loop's finally block.

The default stdio transport remains unchanged so existing IDE integrations are unaffected.

The Kubernetes Helm chart (PR #1570) and production Dockerfile (PR #1572) that consume
this server are separate PRs.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Rewrite in Go + gRPC | Native k8s ecosystem, excellent concurrency model, single binary | Complete rewrite; breaks MCP-over-stdio compatibility; multi-month scope | Deferred to future phase — Python extension achieves Phase 3A goals at far lower cost |
| Add FastAPI instead of aiohttp | More ergonomic routing, automatic OpenAPI docs | Heavier dep tree (Starlette + pydantic); FastAPI default startup is 4x slower than aiohttp for a thin proxy | aiohttp sufficient; FastAPI overhead not justified for a thin proxy |
| Expose full MCP JSON-RPC over HTTP | Maximum protocol parity with stdio | Clients need a full MCP library; operator `curl /healthz` check becomes impossible | REST probes are the primary use case for k8s deployment |
| Separate sidecar process for HTTP probes | No changes to the main server | Adds an extra process per pod; the sidecar must still call the main process to determine readiness | Complexity without benefit; the main process can expose its own probes |

## Consequences

- **Positive**: vmafx-mcp is now deployable in Kubernetes with standard health and
  readiness probes; Prometheus can scrape VMAF scoring throughput and latency;
  operators can call `/v1/score` from `curl` without an MCP client library; SIGTERM
  graceful shutdown prevents dropped scoring requests during pod eviction.
- **Negative**: Two new optional dependencies (`aiohttp`, `prometheus-client`) must be
  installed for HTTP mode; they are gated behind `[http]` and are not pulled in by the
  default install.
- **Neutral / follow-ups**: The Helm chart (PR #1570) and production Dockerfile
  (PR #1572) are follow-on deliverables that wire this transport into a full k8s
  deployment. A future PR may add WebSocket push for long-running scoring jobs.

## References

- [ADR-0686](0686-vmafx-rebrand-aggressive-modernization.md) — VMAFX rebrand umbrella.
- Related PRs: #1570 (Helm chart), #1572 (production Dockerfile).
- Source: `req` — "Full server-mode redesign (Recommended)" (user popup answer, 2026-05-28).
