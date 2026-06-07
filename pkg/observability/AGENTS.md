# AGENTS.md — pkg/observability

## Package role

`pkg/observability` is the shared telemetry surface for every VMAFX Go
service. It provides:

- `NewLogger(level)` — `log/slog` JSON logger.
- `NewMetrics(reg)` + `SetControllerSources` — Prometheus instruments.
- `InitOTel(ctx, serviceName, log)` — OpenTelemetry trace + meter
  provider wiring (ADR-0927).
- `WaitForShutdown` / `NewShutdownContext` — signal + graceful-drain.

Operator guide: [`docs/development/observability.md`](../../docs/development/observability.md).

## Rebase-sensitive invariants

1. **`InitOTel` returns a no-op shutdown when `OTEL_EXPORTER_OTLP_ENDPOINT`
   is unset.** This is the documented contract (ADR-0927) and existing
   deployments depend on the "tracing disabled by default" behaviour.
   `TestInitOTel_NoEndpoint_ReturnsNoop` locks it in. Do not change the
   default to "fail closed".

2. **Defaults are ADR-locked.** `DefaultTraceSampleRatio = 0.01` and
   `DefaultMetricExportInterval = 60*time.Second` are guarded by
   `TestDefaults_ADR0927`. Changing them requires updating ADR-0927
   first.

3. **`InitOTel` installs *global* providers via `otel.SetTracerProvider`
   / `otel.SetMeterProvider`.** Library code retrieves them via
   `otel.GetTracerProvider()` and must **never** import the SDK packages
   directly. If you find a library calling `sdktrace.NewTracerProvider`,
   that is a bug — refactor it through `InitOTel`.

4. **OTel is additive to slog + Prometheus.** Phase 1 (ADR-0927)
   explicitly preserves both. Do not delete `NewLogger`, `NewMetrics`,
   or the Prometheus `/metrics` endpoint as part of OTel work. The
   slog → OTel logs bridge is Phase 3 and depends on modernization
   #3 (zap → slog migration) completing first.

5. **Narrow-interface trick (`jobQueueSource`, `nodeRegistrySource`)
   stays.** These exist to avoid an import cycle between
   `pkg/observability` and `cmd/vmafx-controller/queue`. Do not
   replace them with concrete types from the controller package.

## Test requirements

```bash
go test ./pkg/observability/...
```

Tests must not require a live OTel collector. Use a bogus endpoint
(`http://127.0.0.1:14318`) — the OTLP HTTP exporter is constructed
lazily and `shutdown` is bounded by the test context.

## OTel rollout discipline

When wiring OTel into a new service (Phase 2+), the call site is **always**:

```go
shutdown := observability.InitOTel(ctx, "<service-name>", log)
defer func() {
    shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
    defer cancel()
    if err := shutdown(shutdownCtx); err != nil {
        log.Warn("otel: shutdown returned error", "error", err)
    }
}()
```

The 5 s bound is mandatory — without it, a misconfigured collector
hangs process exit forever.

## `ObserveScoreLatency` context requirement (ADR-1095)

`ObserveScoreLatency(ctx, om, start, attrs...)` takes the caller's request
context as its first argument.  Pass the handler `ctx`, not
`context.Background()` — the SDK reads the active span ID from the context
to attach trace exemplars to histogram data points.  Using
`context.Background()` silently discards baggage and breaks exemplar linking.
