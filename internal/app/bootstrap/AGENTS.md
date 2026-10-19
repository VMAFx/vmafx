# AGENTS.md — internal/app/bootstrap

Composition stanza every vmafx Go binary starts from (ADR-1119). Single
OpenTelemetry entry point for the fleet (ADR-0782). Operator guide:
[docs/development/observability.md](../../../docs/development/observability.md).
Fleet-wide invariants: [cmd/AGENTS.md](../../../cmd/AGENTS.md).

## Rebase-sensitive invariants

1. **`Base` = `Core + otel.Module + fx.Supply(version.Get()) +
   fx.Decorate(withServiceIdentity) + fx.Invoke(func(*otel.Providers) {})`,
   in that shape.** Invoke builds providers: fx builds provider only for
   dependant; no service takes `*otel.Providers`; without invoke no binary
   exported anything (`TestBase_ConstructsProvidersNobodyRequests`).
   `otel.Module` = only OTel initialiser in tree. `withServiceIdentity` = root-scope
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
   Graph without `HTTP` -> nothing to decorate. Roots with
   `HTTP` add it (server, controller); hand-rolled servers call
   `TraceHTTPHandler` directly (mcp). Span naming (`<METHOD> <path>`,
   `/swagger/*` collapse) + probe/scrape filter live only in this package —
   `TestTraceHTTPHandler_SpanNamesAndFilters` +
   `TestHTTPTracing_DecoratesGolusorisServerHandler` lock them.

4. **`Core` / `HTTP` = golusoris modules composed here, never the root
   package `github.com/golusoris/golusoris`** (ADR-1899). Root package
   imports every bundle (notify -> go-mail -> `x/crypto/md4`, core/crypto ->
   argon2id -> `x/crypto/argon2`, river, AWS SDK, casbin, ...) into every
   binary; composing `config, log, clock, id, validate` and
   `router, server` dropped 59 modules from go.mod. `core/crypto` left out:
   no vmafx binary uses PasswordHasher / Encryptor. Never re-import root;
   new module -> import its own package and add to the bundle here.

5. **`FxLogger()` = long-running services only.** `vmafx-tune`'s one-shot
   graphs use `fx.NopLogger` instead (its AGENTS.md #11).

## Test requirements

```bash
go test ./internal/app/bootstrap/
```

Every test here touches process-global state (env, global tracer); none
may use `t.Parallel()`.
