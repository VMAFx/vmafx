<!-- markdownlint-disable MD060 -->
# ADR-0927: OpenTelemetry traces + metrics — Phase 1 pilot in vmafx-controller

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: observability, otel, go, controller, vmafx-rebrand, phase4b, modernization

## Context

The VMAFX Phase 4b distributed platform (controller, node, MCP server,
vmafx-server, vmafx-tune) already emits structured logs via `log/slog`
and Prometheus metrics via `github.com/prometheus/client_golang`. What
is missing is **distributed tracing**: when a `SubmitJob` RPC fans out
to a node `PullWork` → scoring → `ReportResult`, today there is no way
to follow the causal chain across process boundaries, attribute latency
to subsystems, or correlate a slow score with the controller scheduler
decision that produced it.

OpenTelemetry (OTel) is the CNCF-graduated, vendor-neutral standard for
traces, metrics, and logs. It has stable Go SDKs
(`go.opentelemetry.io/otel`, `otel/sdk/trace`, `otel/sdk/metric`) and
idiomatic instrumentation libraries for gRPC (`otelgrpc`) and net/http
(`otelhttp`). The fork's production deployment target (Kubernetes, per
ADR-0709 / ADR-0711) is the natural fit for OTel collectors as a
sidecar / DaemonSet pattern.

Phase 1 scope: pilot OTel in `vmafx-controller` only. Wire the SDK,
instrument the gRPC server with `otelgrpc.NewServerHandler()`, validate
that traces flow to an OTel collector endpoint, and prove the sampling
and configuration story before extending to `vmafx-node`,
`vmafx-server`, `vmafx-mcp`, and `vmafx-tune` in subsequent PRs.

## Decision

We will adopt the **OpenTelemetry Go SDK** for distributed traces and
metrics across all VMAFX Go services, with a per-service opt-in rollout
starting in `vmafx-controller`. We will export over **OTLP** to a
user-deployed **OpenTelemetry Collector** (sidecar or DaemonSet in
Kubernetes; standalone process on bare-metal dev hosts), which in turn
fans out to whichever traces / metrics backend the operator prefers
(Jaeger, Tempo, Honeycomb, Grafana Cloud, etc.). Existing `log/slog`
structured logging and Prometheus `/metrics` endpoints are **preserved
unchanged** — OTel is additive.

**Defaults:**

- Traces: head-based sampler, **1 %** sample rate
  (`TraceIDRatioBased(0.01)`), tunable via `OTEL_TRACES_SAMPLER_ARG`.
- Metrics: **always on** (no sampling); periodic reader, 60 s export
  interval.
- Endpoint: `OTEL_EXPORTER_OTLP_ENDPOINT` (default
  `http://localhost:4318` for HTTP/protobuf, `localhost:4317` for gRPC).
- Service name: `OTEL_SERVICE_NAME` (defaults to the service binary
  name passed to `InitOTel`, e.g. `vmafx-controller`).
- When the endpoint is unset, the SDK initialises with a **no-op
  exporter** so untyped deployments do not regress; a single
  `slog.Info("otel: no endpoint configured, tracing disabled")` line
  records the condition.

**Slog bridge:** Phase 1 does **not** ship the `slog → OTel logs`
bridge. The bridge depends on modernization #3 (slog migration of
remaining `zap` call sites under `cmd/vmafx-controller/nodes/` and
`pkg/observability`) landing first. Until then, log correlation
happens via trace-id injection into slog records, which
`pkg/observability` will add in Phase 2.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| **OTel Collector (chosen)** | Vendor-neutral; one wire format (OTLP) for all backends; supports sampling, batching, retries, redaction at the collector; CNCF-graduated; Kubernetes-native (sidecar / DaemonSet); decouples app from backend choice. | One extra hop; collector must be deployed and monitored. | Chosen. The decoupling is the point — operators can swap Jaeger ↔ Tempo ↔ Honeycomb without touching the app. |
| Jaeger client (direct) | Simpler one-hop topology; mature Go client. | Jaeger client libraries are **deprecated** in favour of OTel (since 2023); single-backend lock-in; no metrics story. | Deprecation alone disqualifies. |
| Grafana Tempo (direct) | First-class Grafana integration. | Same backend lock-in; no metrics story; Tempo prefers OTLP ingestion anyway. | Strictly worse than OTLP-to-collector-to-Tempo. |
| Prometheus-only (status quo) | Already deployed; zero new dependencies; well-understood. | No distributed traces; no causal-chain debugging; cannot attribute fan-out latency. | Insufficient for distributed-system debugging — the reason this ADR exists. |
| Hand-rolled trace headers | Zero dependencies. | Reinvents W3C `traceparent`, sampler semantics, exporter back-pressure, batching. Five-engineer-year project. | Trivially worse than adopting the standard. |

**Sampling rate (1 %):** 10 % and 100 % alternatives were considered.
1 % keeps storage cost manageable at production fan-out (a single
`SubmitJob` produces ~5 spans across controller + node) while
preserving enough samples for p99 latency attribution. Operators can
raise the rate via `OTEL_TRACES_SAMPLER_ARG=0.10` for incident
investigation without a redeploy.

**Periodic-reader interval (60 s):** matches the Prometheus scrape
default the fork already uses; shorter intervals (10 s, 15 s) inflate
collector load without improving the observable signal because the
metrics are mostly request-counter cumulatives, not gauges.

## Consequences

- **Positive:**
  - Distributed traces across `controller → node → scoring` causal
    chains (full chain lands once Phase 2 wires the node).
  - Vendor-neutral export — operators pick the backend.
  - Existing Prometheus `/metrics` and slog JSON logs continue to
    work; Phase 1 is purely additive.
  - One reusable helper (`pkg/observability.InitOTel`) for every
    subsequent service.
- **Negative:**
  - Two new direct dependencies (`go.opentelemetry.io/otel` family,
    `otelgrpc`); ~10 transitive (otlp exporters, propagators, sdk).
  - Operators who want traces must deploy and operate an OTel
    collector.
  - Phase 1 ships traces only on `vmafx-controller`; the cross-service
    causal chain is incomplete until `vmafx-node` and `vmafx-server`
    follow.
- **Neutral / follow-ups:**
  - **Phase 2** (separate ADRs / PRs): wire OTel in `vmafx-node`,
    `vmafx-server`, `vmafx-mcp`, `vmafx-tune` — one PR per service so
    rollback is per-service.
  - **Phase 3**: slog → OTel-logs bridge (depends on modernization
    #3 completing the zap → slog migration).
  - **Phase 4**: replace `prometheus/client_golang` instruments with
    OTel-metrics emitters when the OTel Prometheus exporter reaches
    stable maturity (today: Beta).
  - Update `docs/development/observability.md` operator guide as each
    service is wired.
  - Update `docs/development/k8s-deployment.md` with an example OTel
    collector DaemonSet manifest in Phase 2.

## References

- ADR-0703: vmafx-server Go gRPC + HTTP service (origin of `pkg/observability`).
- ADR-0709: VMAFX Phase 4b distributed platform.
- ADR-0711: vmafx-controller Phase 4b.1 scope.
- ADR-0713: vmafx-node Phase 4b.1 scope (next target after Phase 1
  stabilises).
- [OpenTelemetry Go SDK](https://github.com/open-telemetry/opentelemetry-go).
- [OTel Collector](https://opentelemetry.io/docs/collector/).
- Source: `req` (paraphrased) — user direction to wire OpenTelemetry
  traces and metrics across all Go services, Phase 1 pilot in
  vmafx-controller, with an ADR for the project-wide plan.
