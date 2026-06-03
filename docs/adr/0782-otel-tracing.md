# ADR-0782: OpenTelemetry tracing and metrics schema for the VMAFX platform

- **Status**: Accepted
- **Date**: 2026-05-29
- **Deciders**: lusoris
- **Tags**: `observability`, `go`, `platform`, `adr-0782`

## Context

The four Go binaries that form the VMAFX distributed platform
(vmafx-controller, vmafx-node, vmafx-server, vmafx-mcp — ADR-0703, ADR-0711,
ADR-0713) had no distributed tracing or structured metrics beyond the
per-binary Prometheus `/metrics` endpoint.  Diagnosing latency spikes across
the controller→node→scorer pipeline required correlating separate log streams
by hand.

The platform needed:

1. Distributed trace spans for the five hot-path operations: job submit,
   encoder dispatch, frame extraction, VMAF scoring, ONNX inference.
2. OTel-native metrics for job queue depth, jobs in-flight, GPU utilisation,
   frames/sec, and score latency (p50/p99) — surfaced to both Prometheus and
   any OTLP backend.
3. An OTLP exporter wired to `OTEL_EXPORTER_OTLP_ENDPOINT` (default
   `localhost:4317`) so operators can point at Jaeger, Tempo, or a managed
   OTLP gateway without recompiling.
4. An optional OTel collector sidecar deployable via Helm without requiring
   changes to the VMAFX binaries.
5. A Grafana dashboard covering the above signals.

Tracing must be **best-effort and non-blocking**: a missing or unreachable
OTLP collector must never prevent the binary from starting or serving
requests.

## Decision

We will add OpenTelemetry SDK wiring to `pkg/observability` using
`go.opentelemetry.io/otel` v1.44 and export via OTLP/gRPC.  The bootstrap
function `InitOTel(ctx, service, version)` returns a no-op provider pair on
any failure, so startup remains safe.  All four binaries call `InitOTel`
early in `main()` and defer the shutdown flush.

Span names and attribute keys are defined once in
`pkg/observability/otel_instruments.go` to prevent cardinality drift.

OTel-native `OTelMetrics` instruments (UpDownCounters, Histograms, Gauge)
are registered alongside the existing Prometheus counters; both signal paths
co-exist without replacing each other.  The Prometheus `/metrics` endpoint
continues to function as the primary scrape target.

Controller-specific Prometheus counters (`JobsSubmitted`, `JobsCompleted`,
`JobsFailed`, plus gauge functions for queue depth and active node count) are
added to `pkg/observability.Metrics` to remove the undefined-symbol
references that existed in the grpc_server.go call sites.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Prometheus-only (status quo) | Zero new deps | No distributed tracing; manual log correlation | Insufficient for cross-binary latency diagnosis |
| OpenCensus | Mature | Deprecated; no active SDK | Sunset in 2023; migration to OTel is the vendor recommendation |
| Datadog agent sidecar | Full observability stack | Vendor lock-in; licence cost; no self-hosted path | Violates the fork's preference for open/self-hosted tooling |
| Jaeger SDK directly | Simpler setup | Vendor-specific; does not support metrics | OTLP is the vendor-neutral path |

## Consequences

**Positive**:

- Distributed trace correlation across controller → node → scorer in a single
  Jaeger/Tempo/Grafana Tempo view.
- p50/p99 score latency visible without joining log lines.
- GPU utilisation per node surfaced to Grafana without custom log parsing.
- OTel collector sidecar deployable via `--set otelCollector.enabled=true`
  with zero changes to the VMAFX binaries.
- No startup regression: all OTel failures are non-fatal.

**Negative**:

- Eight new direct Go module dependencies; increases build surface.
- OTel SDK adds ~5 MB to each binary (pre-stripped).

**Neutral / follow-ups**:

- gRPC interceptors (`otelgrpc`) are imported but not yet wired into the
  gRPC server setup.  That is a separate PR (automatic span propagation from
  client to server via W3C TraceContext headers).
- The `otelhttp` middleware is not yet applied to the HTTP mux.
  Follow-up: wrap `runHTTP`'s mux with `otelhttp.NewHandler`.
- GPU utilisation reporting requires `vmafx-node` to read vendor-specific
  sysfs/nvml interfaces; the gauge instrument is registered but only non-zero
  when nodes report the value via a future Heartbeat extension.

## References

- [go.opentelemetry.io/otel v1.44 release notes](https://github.com/open-telemetry/opentelemetry-go/releases/tag/v1.44.0)
- [OTLP specification](https://opentelemetry.io/docs/specs/otlp/)
- [W3C Trace Context](https://www.w3.org/TR/trace-context/)
- ADR-0703: vmafx-server Go gRPC + HTTP service.
- ADR-0711: vmafx-controller Phase 4b.1 scope expansion.
- ADR-0713: vmafx-node Go worker binary.
- `req`: "Wire OpenTelemetry across vmafx-controller, vmafx-node,
  vmafx-server, vmafx-mcp."
