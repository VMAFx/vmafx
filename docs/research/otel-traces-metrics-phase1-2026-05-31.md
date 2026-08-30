# Research digest: OpenTelemetry traces + metrics Phase 1 (2026-05-31)

## Scope

Evaluates the OpenTelemetry Go SDK as the distributed-tracing and
metrics surface for VMAFX Phase 4b Go services, and specifies the
Phase 1 pilot in `vmafx-controller`. Companion to
[ADR-0927](../adr/0927-opentelemetry-traces-metrics-phase1.md).

## Today

`pkg/observability` (ADR-0703) provides:

- `NewLogger(level)` → `*slog.Logger` with JSON handler to stdout.
- `NewMetrics(reg)` → Prometheus `*Metrics` struct with counters,
  histograms, and gauges for `score_requests_total`,
  `score_duration_seconds`, `jobs_submitted_total`, etc.
- `SetControllerSources(queue, registry)` → live gauges
  (`jobs_pending`, `jobs_running`, `nodes_live`).
- `WaitForShutdown(ctx, log, timeout)` / `NewShutdownContext()` — signal
  graceful-drain plumbing.

What is **missing**: distributed traces across process boundaries.
When `vmafx-controller.SubmitJob` enqueues a job, the
`vmafx-node.PullWork` → libvmaf score → `ReportResult` chain has no
correlation id, no causal parent, no end-to-end latency attribution.
Prometheus alone can show the *rate* of slow scores but cannot pin
*which* scheduler decision produced *which* slow score.

## OTel Go SDK evaluation

Stable as of `v1.44.0` (March 2024); `go.opentelemetry.io/otel`,
`otel/sdk/trace`, `otel/sdk/metric`, and the OTLP HTTP exporters are
GA. The gRPC instrumentation lives in
`go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc`
(also GA). Wire-up takes ~80 LOC of Go for a service:

```go
shutdown := observability.InitOTel(ctx, "vmafx-controller", log)
defer shutdown(shutdownCtx)
srv := grpc.NewServer(grpc.StatsHandler(otelgrpc.NewServerHandler()))
```

The SDK installs **global** providers; library code retrieves them via
`otel.GetTracerProvider()` / `otel.GetMeterProvider()` without taking
a dep on the SDK package. This matches the slog convention
(`slog.Default()`) and keeps the call sites clean.

## Collector vs direct exporters

OTel supports three deployment shapes for export:

1. **App → collector → backend**. App uses generic OTLP exporter; the
   collector handles fan-out, sampling tail decisions, batching,
   retries, and redaction. Operator picks the backend(s).
2. **App → backend (direct)**. App imports a backend-specific exporter
   (`exporters/jaeger`, `exporters/zipkin`, vendor SDKs). Backend
   lock-in; simpler topology.
3. **Hybrid**. Some apps direct, others via collector. Inconsistent
   ops story; not recommended.

Shape **(1)** wins for VMAFX because: (a) operators have very
different backend preferences (Jaeger for self-hosted, Honeycomb /
Datadog / Grafana Cloud for hosted), (b) the legacy Jaeger client
libraries are officially deprecated, (c) tail-based sampling and
redaction are operator concerns the app cannot solve. The single
extra hop is cheap relative to the libvmaf scoring path it
instruments (libvmaf scoring is O(100 ms) per frame; OTel collector
ingest is O(100 µs) per span).

## Sampling

Two choices: **head-based** (decide at trace start) or **tail-based**
(decide after the trace completes). Head sampling is cheap, lossy at
low rates, and lives in the app. Tail sampling is expensive, lossless
for "interesting" traces, and lives in the collector.

Phase 1 ships head-based 1 % (`ParentBased(TraceIDRatioBased(0.01))`).
Reasoning:

- Tail sampling requires collector-side configuration the operator
  must set up; we don't want to ship app-side defaults that *assume*
  the collector is configured a particular way.
- 1 % at production fan-out (5 spans per `SubmitJob`, ~10/s steady
  state per controller) yields ~5 sampled traces per minute — enough
  for p99 attribution.
- Operators can raise the rate via `OTEL_TRACES_SAMPLER_ARG=1.0`
  during incidents without a redeploy.
- `ParentBased` ensures a sampled controller trace pulls all
  downstream node spans with it; without it, controller and node
  sample independently and traces look truncated.

## Metric export interval

OTel periodic readers fire every `WithInterval(d)`. Default upstream
SDK: 60 s. Prometheus scrape default: 15 s. Pick the one that already
matches the surrounding deployment — 60 s here, because the OTel
metrics export complements (does not replace) the Prometheus scrape;
shorter intervals would double the on-wire cost of the same data.

## Slog bridge (deferred)

OTel has a `log/slog` bridge
(`go.opentelemetry.io/contrib/bridges/otelslog`) that emits slog
records as OTel log records. It works today but requires every
`slog.Logger` instance in the codebase to be created via the bridge
factory.

Today the controller still uses **zap** in two places
(`cmd/vmafx-controller/nodes/registry.go`,
`pkg/observability/observability.go` via the `WaitForShutdown` log
arg). Migration to slog is **modernization #3**; the OTel-logs
bridge lands in Phase 3 after that migration completes.

Until then, trace correlation in logs happens via a `slog.Attr` that
injects the active span's trace-id when one exists; that addition is
small and lands in Phase 2 with the HTTP middleware.

## What we **didn't** decide

- **Tail-based sampling collector config.** Operator concern; not
  Phase 1 scope.
- **OTel metrics replacing Prometheus client.** The OTel Prometheus
  exporter exists but is Beta; we want stable. Phase 4 revisits.
- **Trace-id in HTTP response headers.** Useful for "give me this
  trace" workflows; lands with the HTTP middleware in Phase 2.

## References

- [OTel Go SDK API stability](https://opentelemetry.io/docs/languages/go/#api-stability).
- [otelgrpc instrumentation README](https://pkg.go.dev/go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc).
- [W3C traceparent spec](https://www.w3.org/TR/trace-context/).
- ADR-0703, ADR-0709, ADR-0711, ADR-0713.
