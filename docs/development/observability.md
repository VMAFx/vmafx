<!-- markdownlint-disable MD013 MD060 -->
# Observability — VMAFX Go services

This page is the operator guide for telemetry emitted by the VMAFX Go
services (`vmafx-controller`, `vmafx-node`, `vmafx-server`,
`vmafx-mcp`, `vmafx-tune`). It covers three signals:

| Signal       | Mechanism                                  | Status                                                     |
|--------------|--------------------------------------------|------------------------------------------------------------|
| **Logs**     | `log/slog` JSON to stdout                  | Production (all services)                                  |
| **Metrics**  | Prometheus `/metrics` endpoint             | Production (controller + server)                           |
| **Traces**   | OpenTelemetry OTLP → user-deployed collector | **Phase 1** — pilot in `vmafx-controller` only (ADR-0927) |

Logs and Prometheus metrics are unchanged by the Phase 1 OTel rollout.
This document covers the new OTel surface; the historical slog +
Prometheus surfaces are documented inline in `pkg/observability/observability.go`.

## OpenTelemetry quick start

`vmafx-controller` exports traces and (eventually) metrics over **OTLP
HTTP/protobuf**. Point it at an OpenTelemetry Collector on the network
path and the controller will start emitting spans for every gRPC RPC.

### Minimum operator setup

1. **Deploy an OTel collector.** The simplest local-dev recipe is the
   `otel/opentelemetry-collector-contrib` Docker image with a default
   config that prints to stdout:

    ```bash
    docker run --rm \
      -p 4317:4317 -p 4318:4318 \
      otel/opentelemetry-collector-contrib:latest
    ```

   In Kubernetes, deploy the collector as a sidecar or DaemonSet —
   see `docs/development/k8s-deployment.md` (Phase 2 will add an
   example manifest).

2. **Point the controller at it.** Set the standard OTel SDK env
   variable:

    ```bash
    export OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4318
    ./vmafx-controller
    ```

3. **Verify.** The controller logs one line at startup:

    ```json
    {"level":"INFO","msg":"otel: initialised",
     "service":"vmafx-controller",
     "endpoint":"http://localhost:4318",
     "trace_sample_ratio":0.01,
     "metric_export_interval":"1m0s"}
    ```

   Make a gRPC `Score` or `SubmitJob` call; the collector should print
   a span batch within ~5 s.

### Configuration

All knobs are standard OTel SDK env vars. The fork does **not** layer
any private configuration on top.

| Variable                        | Default                       | Meaning                                                       |
|---------------------------------|-------------------------------|---------------------------------------------------------------|
| `OTEL_EXPORTER_OTLP_ENDPOINT`   | _(unset → tracing disabled)_  | OTLP HTTP collector URL. Unset = no-op providers.             |
| `OTEL_SERVICE_NAME`             | _(matches binary name)_       | Overrides the `service.name` resource attribute.              |
| `OTEL_TRACES_SAMPLER_ARG`       | `0.01` (1 %)                  | Head-based trace sample ratio in `[0.0, 1.0]`.                |
| `OTEL_SDK_DISABLED`             | `false`                       | `true` forces no-op providers regardless of other vars.       |
| `OTEL_RESOURCE_ATTRIBUTES`      | _(unset)_                     | Comma-separated `key=value` resource attributes (OTel std).   |

The defaults are locked in `pkg/observability/otel.go` and asserted by
`TestDefaults_ADR0927`; changing them requires updating ADR-0927.

### Sampling strategy

The default sampler is `ParentBased(TraceIDRatioBased(0.01))`. That
means:

- **Root spans** are sampled at the configured ratio (1 % by default).
- **Child spans inherit** the sampling decision of their parent, so a
  trace is either sampled in full or dropped in full — never half a
  trace.
- A client that already made a sampling decision (e.g. a load
  generator setting `sampled=1` on the inbound `traceparent`) is
  honoured.

For **incident investigation**, raise the rate without redeploying:

```bash
OTEL_TRACES_SAMPLER_ARG=1.0 ./vmafx-controller   # 100 % sampling
```

### What gets instrumented (Phase 1)

| Surface                                           | Spans?                                       | Notes                                                      |
|---------------------------------------------------|----------------------------------------------|------------------------------------------------------------|
| `VmafxScoring` gRPC (`Score`, `Health`)           | Yes — via `otelgrpc.NewServerHandler()`      | One server span per RPC, populated with status + duration. |
| `VmafxController` gRPC (`SubmitJob`, `GetJob`, …) | Yes — same handler                           | Same.                                                      |
| HTTP `/v1/score`, `/healthz`, `/readyz`           | No — Phase 2 will add `otelhttp` middleware. | Prometheus counters continue to track these.               |
| Outbound libvmaf CLI invocations                  | No — Phase 2 will add a wrapping span.       |                                                            |

The W3C `traceparent` header propagates automatically on every
controller-originated gRPC call (once Phase 2 wires `otelgrpc`
client-side dial options on the node-pull-work path).

### Backends

OTLP is the wire format; pick any backend that ingests OTLP:

- **Local dev**: `otel-collector-contrib` → stdout (debug)
- **Self-hosted**: Jaeger 1.50+ (native OTLP), Grafana Tempo, SigNoz
- **Hosted**: Honeycomb, Lightstep / ServiceNow, Grafana Cloud, Datadog,
  New Relic — all accept OTLP

The collector handles fan-out, filtering, and redaction; the app does
not need to know which backend is in use.

### Disabling OTel entirely

Two ways:

- Leave `OTEL_EXPORTER_OTLP_ENDPOINT` unset (default). The SDK installs
  no-op providers; the only cost is one info log line at startup.
- Set `OTEL_SDK_DISABLED=true`. Same effect; useful when an unrelated
  process in the same pod injects the endpoint env var.

### Rollout roadmap

| Phase | Services                          | Status                |
|-------|-----------------------------------|-----------------------|
| 1     | `vmafx-controller`                | This PR (ADR-0927)    |
| 2     | `vmafx-node`, HTTP middleware     | Backlog               |
| 3     | `vmafx-server`, `vmafx-mcp`       | Backlog               |
| 4     | `vmafx-tune` (CLI long-runner)    | Backlog               |
| 5     | `slog → OTel logs` bridge         | Blocked on mod #3     |

Each phase ships as its own PR with its own ADR (when a non-trivial
design call is involved) so rollback is per-service.

## See also

- [ADR-0927](../adr/0927-opentelemetry-traces-metrics-phase1.md) — design rationale.
- [ADR-0703](../adr/0703-vmafx-server-go-grpc-http.md) — origin of `pkg/observability`.
- [ADR-0709](../adr/0709-vmafx-phase4b-distributed-platform.md) — distributed-platform context.
- [`pkg/observability/observability.go`](../../pkg/observability/observability.go) — slog + Prometheus surfaces.
- [`pkg/observability/otel.go`](../../pkg/observability/otel.go) — OTel `InitOTel` helper.
- [OpenTelemetry Go SDK docs](https://opentelemetry.io/docs/languages/go/).
