## OpenTelemetry traces + metrics — Phase 1 (vmafx-controller)

Adopts the OpenTelemetry Go SDK across all VMAFX Go services with a
per-service opt-in rollout, starting with `vmafx-controller`.

**What is new:**

- `pkg/observability.InitOTel(ctx, serviceName, log)` — one-call
  helper that wires global OTel `TracerProvider` + `MeterProvider`
  with OTLP/HTTP export and returns a shutdown function.
- `vmafx-controller` now emits a server span for every gRPC RPC via
  `otelgrpc.NewServerHandler()`. The W3C `traceparent` header is
  propagated automatically.
- Standard OTel SDK env vars are honoured: `OTEL_EXPORTER_OTLP_ENDPOINT`,
  `OTEL_SERVICE_NAME`, `OTEL_TRACES_SAMPLER_ARG`, `OTEL_SDK_DISABLED`.
- Defaults: 1 % head-based trace sampler, 60 s metric export interval.
- No-op when `OTEL_EXPORTER_OTLP_ENDPOINT` is unset — existing deployments
  do not regress.

**Operator guide:** [`docs/development/observability.md`](docs/development/observability.md).

**Design rationale:** [ADR-0927](docs/adr/0927-opentelemetry-traces-metrics-phase1.md).

**Preserved unchanged:** existing `log/slog` JSON logs and Prometheus
`/metrics` endpoints. OTel is additive.

**Next:** Phase 2 wires `vmafx-node`, then `vmafx-server`, `vmafx-mcp`,
`vmafx-tune` — one PR per service.
