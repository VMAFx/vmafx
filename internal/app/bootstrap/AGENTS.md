# AGENTS.md — internal/app/bootstrap

Composition stanza every vmafx Go binary starts from (ADR-1119). Single
OpenTelemetry entry point for the fleet (ADR-0782). Operator guide:
[docs/development/observability.md](../../../docs/development/observability.md).
Fleet-wide invariants: [cmd/AGENTS.md](../../../cmd/AGENTS.md).

## Rebase-sensitive invariants

1. **`Base` is `golusoris.Core + otel.Module + fx.Supply(version.Get()) +
   fx.Decorate(withServiceIdentity)`, in that shape.** `otel.Module` =
   only OTel initialiser in tree. `withServiceIdentity` = root-scope
   decorator of golusoris's `otel.Options` (service.version from
   `pkg/version`; `OTEL_SERVICE_NAME` honored behind
   `VMAFX_OTEL_SERVICE_NAME` config key). Must stay decorator, never
   replacement provider — golusoris's `loadOptions` (env/file parsing,
   derived binary name) keeps running. `TestBase_OTelIsNoopWithoutEndpoint`
   + `TestBase_ServiceIdentityPrecedence` lock precedence:
   `VMAFX_OTEL_SERVICE_NAME` > `OTEL_SERVICE_NAME` > derived;
   `VMAFX_OTEL_SERVICE_VERSION` > `pkg/version`.

2. **No endpoint -> no-op. Contract.** None of
   `OTEL_EXPORTER_OTLP_*_ENDPOINT` / `VMAFX_OTEL_ENDPOINT` set -> golusoris
   returns empty `Providers`, leaves global TracerProvider alone.
   `internal/oteltest.Recorder` depends on that slot staying free.

3. **`HTTPTracing` = opt-in per root, not part of `Base`.**
   `fx.Decorate` of `http.Handler` (what `httpx/server.Module` consumes).
   Graph without golusoris.HTTP -> nothing to decorate. Roots with
   golusoris.HTTP add it (server, controller); hand-rolled servers call
   `TraceHTTPHandler` directly (mcp). Span naming (`<METHOD> <path>`,
   `/swagger/*` collapse) + probe/scrape filter live only in this package —
   `TestTraceHTTPHandler_SpanNamesAndFilters` +
   `TestHTTPTracing_DecoratesGolusorisServerHandler` lock them.

4. **`FxLogger()` = long-running services only.** `vmafx-tune`'s one-shot
   graphs use `fx.NopLogger` instead (its AGENTS.md #11).

## Test requirements

```bash
go test ./internal/app/bootstrap/
```

Every test here touches process-global state (env, global tracer); none
may use `t.Parallel()`.
