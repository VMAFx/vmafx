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

Additional invariants locked in by `coverage_gaps_test.go`:

- `WaitForShutdown` must return after receiving SIGTERM (not only on context
  cancel). `TestWaitForShutdown_SIGTERMDelivery` sends a real SIGTERM to
  `os.Getpid()`; do not run that test with `t.Parallel()`.
- `SetControllerSources` must register only the queue gauges when `r == nil`,
  and only the node gauge when `q == nil`. The half-nil branches are tested
  independently of the both-nil and both-non-nil cases.
- `AttrJobID`, `AttrModel`, `AttrBackend`, `AttrNodeID`, `AttrVendor`,
  `AttrStatus` key strings are schema-locked to the values in ADR-0782.
  Any rename is a breaking OTLP consumer change — update the ADR first.

## OTel rollout discipline

The rollout is complete (epic #1241, ADR-0782): every Go binary initialises
OpenTelemetry through `internal/app/bootstrap` — `bootstrap.Base` carries
golusoris's `otel.Module` (ADR-1119) plus the `service.name` /
`service.version` decorator — and nowhere else. See `cmd/AGENTS.md` #1.

`InitOTel` in this package is the ADR-0927 Phase 1 helper that predates the
golusoris migration. **No `cmd/` binary calls it any more** (ADR-1119 replaced
the per-binary "InitOTel + shutdown dance" with `bootstrap.Base`); it remains
because ADR-0927 is Accepted and its contract (invariants 1–3 above) is still
tested. Do not wire it into a binary next to `bootstrap.Base` — two global
providers cannot coexist (golusoris `otel/AGENTS.md`: "Don't wire two OTel
modules"). Retiring it is a superseding-ADR decision, not a cleanup.

What a new service or a new request path does instead:

- fx service: start from `bootstrap.Base`; add `bootstrap.HTTPTracing` next
  to `golusoris.HTTP`; gRPC spans come with `grpc.Module`.
- one-shot CLI: build `bootstrap.Base` per invocation
  (`cmd/vmafx-tune/cmd/golusoris.go::withGolusoris` is the template).
- application spans: `observability.StartSpan` / `EndSpan` with a name from
  `otel_instruments.go` (`SpanJobSubmit`, `SpanScoring`, `SpanFrameExtraction`,
  `SpanONNXInference`, `SpanMCPTool`, `SpanTuneCommand`) — add the constant
  here first; never inline a string.
- tests: `internal/oteltest.Recorder` installs an in-memory recorder as the
  global provider (no collector needed); such tests must not be parallel.

## `ObserveScoreLatency` context requirement (ADR-1095)

`ObserveScoreLatency(ctx, om, start, attrs...)` takes the caller's request
context as its first argument.  Pass the handler `ctx`, not
`context.Background()` — the SDK reads the active span ID from the context
to attach trace exemplars to histogram data points.  Using
`context.Background()` silently discards baggage and breaks exemplar linking.
