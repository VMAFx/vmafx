# AGENTS.md — internal/app/bootstrap

The composition stanza every vmafx Go binary starts from (ADR-1119) and
the single OpenTelemetry entry point for the fleet (ADR-0782). Operator
guide: [docs/development/observability.md](../../../docs/development/observability.md);
fleet-wide invariants: [cmd/AGENTS.md](../../../cmd/AGENTS.md).

## Rebase-sensitive invariants

1. **`Base` is `golusoris.Core + otel.Module + fx.Supply(version.Get()) +
   fx.Decorate(withServiceIdentity)`, in that shape.** `otel.Module` is the
   only OTel initialiser in the tree; `withServiceIdentity` is a
   root-scope decorator of golusoris's `otel.Options` (service.version from
   `pkg/version`, `OTEL_SERVICE_NAME` honoured behind the
   `VMAFX_OTEL_SERVICE_NAME` config key). It must stay a decorator, not a
   replacement provider, so golusoris's `loadOptions` (env/file parsing,
   derived binary name) keeps running. `TestBase_OTelIsNoopWithoutEndpoint`
   and `TestBase_ServiceIdentityPrecedence` lock the precedence:
   `VMAFX_OTEL_SERVICE_NAME` > `OTEL_SERVICE_NAME` > derived;
   `VMAFX_OTEL_SERVICE_VERSION` > `pkg/version`.

2. **No-op without an endpoint is the contract.** With none of
   `OTEL_EXPORTER_OTLP_*_ENDPOINT` / `VMAFX_OTEL_ENDPOINT` set, golusoris
   returns empty `Providers` and leaves the global TracerProvider alone.
   `internal/oteltest.Recorder` depends on that slot staying free.

3. **`HTTPTracing` is opt-in per root, not part of `Base`.** It is an
   `fx.Decorate` of `http.Handler` (what `httpx/server.Module` consumes); a
   graph without golusoris.HTTP has nothing to decorate. Roots with
   golusoris.HTTP add it (server, controller); hand-rolled servers call
   `TraceHTTPHandler` directly (mcp). Span naming (`<METHOD> <path>`,
   `/swagger/*` collapse) and the probe/scrape filter live in this package
   only — `TestTraceHTTPHandler_SpanNamesAndFilters` and
   `TestHTTPTracing_DecoratesGolusorisServerHandler` lock them.

4. **`FxLogger()` is for long-running services.** `vmafx-tune`'s one-shot
   graphs use `fx.NopLogger` instead (its AGENTS.md #11).

## Test requirements

```bash
go test ./internal/app/bootstrap/
```

Every test here touches process-global state (env, the global tracer);
none may use `t.Parallel()`.
