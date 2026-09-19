# AGENTS.md — pkg/observability

## Package role

`pkg/observability` is shared telemetry surface for every VMAFX Go
service. Provides:

- `NewLogger(level)` — `log/slog` JSON logger.
- `NewMetrics(reg)` + `SetControllerSources` — Prometheus instruments.
- `InitOTel(ctx, serviceName, log)` — OpenTelemetry trace + meter
  provider wiring (ADR-0927).
- `WaitForShutdown` / `NewShutdownContext` — signal + graceful-drain.

Operator guide: [`docs/development/observability.md`](../../docs/development/observability.md).

## Rebase-sensitive invariants

1. **`InitOTel` returns a no-op shutdown when `OTEL_EXPORTER_OTLP_ENDPOINT`
   is unset.** Documented contract (ADR-0927); existing deployments
   depend on "tracing disabled by default" behaviour.
   `TestInitOTel_NoEndpoint_ReturnsNoop` locks it in. Do not change
   default to "fail closed".

2. **Defaults are ADR-locked.** `DefaultTraceSampleRatio = 0.01` and
   `DefaultMetricExportInterval = 60*time.Second` are guarded by
   `TestDefaults_ADR0927`. Changing them requires updating ADR-0927
   first.

3. **`InitOTel` installs *global* providers via `otel.SetTracerProvider`
   / `otel.SetMeterProvider`.** Library code retrieves them via
   `otel.GetTracerProvider()`, must **never** import SDK packages
   directly. Library calling `sdktrace.NewTracerProvider` directly is
   bug — refactor through `InitOTel`.

4. **OTel is additive to slog + Prometheus.** Phase 1 (ADR-0927)
   explicitly preserves both. Do not delete `NewLogger`, `NewMetrics`,
   or Prometheus `/metrics` endpoint as part of OTel work. slog → OTel
   logs bridge is Phase 3, depends on modernization #3 (zap → slog
   migration) completing first.

5. **Narrow-interface trick (`jobQueueSource`, `nodeRegistrySource`)
   stays.** These exist to avoid import cycle between
   `pkg/observability` and `cmd/vmafx-controller/queue`. Do not
   replace them with concrete types from controller package.

## Test requirements

```bash
go test ./pkg/observability/...
```

Tests must not require live OTel collector. Use bogus endpoint
(`http://127.0.0.1:14318`) — OTLP HTTP exporter constructed lazily,
`shutdown` bounded by test context.

Additional invariants locked in by `coverage_gaps_test.go`:

- `WaitForShutdown` must return after receiving SIGTERM (not only on
  context cancel). `TestWaitForShutdown_SIGTERMDelivery` sends real
  SIGTERM to `os.Getpid()`; do not run that test with `t.Parallel()`.
- `SetControllerSources` must register only queue gauges when
  `r == nil`, and only node gauge when `q == nil`. Half-nil branches
  tested independently of both-nil and both-non-nil cases.
- `AttrJobID`, `AttrModel`, `AttrBackend`, `AttrNodeID`, `AttrVendor`,
  `AttrStatus` key strings are schema-locked to values in ADR-0782.
  Any rename is breaking OTLP consumer change — update ADR first.

## OTel rollout discipline

Rollout is complete (epic #1241, ADR-0782): every Go binary initialises
OpenTelemetry through `internal/app/bootstrap` — `bootstrap.Base`
carries golusoris's `otel.Module` (ADR-1119) plus `service.name` /
`service.version` decorator — and nowhere else. See `cmd/AGENTS.md` #1.

`InitOTel` in this package is ADR-0927 Phase 1 helper that predates
golusoris migration. **No `cmd/` binary calls it any more** (ADR-1119
replaced per-binary "InitOTel + shutdown dance" with `bootstrap.Base`);
remains because ADR-0927 is Accepted, contract (invariants 1–3 above)
still tested. Do not wire it into binary next to `bootstrap.Base` —
two global providers cannot coexist (golusoris `otel/AGENTS.md`:
"Don't wire two OTel modules"). Retiring it is superseding-ADR
decision, not cleanup.

What new service or new request path does instead:

- fx service: start from `bootstrap.Base`; add `bootstrap.HTTPTracing`
  next to `golusoris.HTTP`; gRPC spans come with `grpc.Module`.
- one-shot CLI: build `bootstrap.Base` per invocation
  (`cmd/vmafx-tune/cmd/golusoris.go::withGolusoris` is template).
- application spans: `observability.StartSpan` / `EndSpan` with name
  from `otel_instruments.go` (`SpanJobSubmit`, `SpanScoring`,
  `SpanFrameExtraction`, `SpanONNXInference`, `SpanMCPTool`,
  `SpanTuneCommand`) — add constant here first; never inline string.
- tests: `internal/oteltest.Recorder` installs in-memory recorder as
  global provider (no collector needed); such tests must not be
  parallel.

## `ObserveScoreLatency` context requirement (ADR-1095)

`ObserveScoreLatency(ctx, om, start, attrs...)` takes caller's request
context as first argument. Pass handler `ctx`, not
`context.Background()` — SDK reads active span ID from context to
attach trace exemplars to histogram data points. Using
`context.Background()` silently discards baggage, breaks exemplar
linking.
