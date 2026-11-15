---
paths:
  - cmd/vmafx-*/**/*.go
  - internal/app/bootstrap/*.go
  - pkg/observability/otel_instruments.go
  - pkg/score/*.go
invariant: Init OTel via bootstrap.Base only; HTTP/gRPC spans from bootstrap + golusoris; ADR-0782 names.
---
<!-- markdownlint-disable MD013 MD029 -->
# OpenTelemetry init, HTTP/gRPC tracing and span names (ADR-0782, ADR-1119)

## Rebase-sensitive invariants

1. **Every binary inits OpenTelemetry via `internal/app/bootstrap` only**
   (ADR-0782, ADR-1119). `bootstrap.Base` carries golusoris `otel.Module`
   (OTLP/gRPC exporter, W3C propagators, fx `OnStop` flush, silent no-op
   without endpoint) and `withServiceIdentity` (`service.version` from
   `pkg/version`, honours `OTEL_SERVICE_NAME`). Do not call
   `pkg/observability.InitOTel`, `otel.New`, or `sdktrace.NewTracerProvider`
   from `main` package: two providers cannot both be global; second init
   path = boilerplate ADR-1119 removed. Binary
   `TestOTelWiredThroughBootstrap` locks this in (no-op providers without
   endpoint, `service.name` = binary name, `service.version` = `pkg/version`).

2. **HTTP surfaces traced via `bootstrap`, not per binary.** Root wiring
   `bootstrap.HTTP` puts `bootstrap.HTTPTracing` next to it (server,
   controller, node). Hand-rolled `*http.Server` wraps handler with
   `bootstrap.TraceHTTPHandler` outermost (mcp). Span name (`<METHOD> <path>`,
   Swagger subtree collapsed) and probe/scrape filter (`/healthz`, `/readyz`,
   `/livez`, `/startupz`, `/metrics`) defined once there.

3. **gRPC spans come from golusoris.** Servers get `otelgrpc` stats handler
   from `grpc.Module`; clients dial via `grpc.NewConnFactory()` (operator) or
   attach `otelgrpc.NewClientHandler()` (`pkg/score`) -> `traceparent` crosses
   every hop (ADR-1095). Never add bare `grpc.NewClient` / `grpc.DialContext`
   in binary without handler.

4. **Application spans use ADR-0782 names from
   `pkg/observability/otel_instruments.go`** (`observability.StartSpan` /
   `EndSpan`); do not invent names inline. Per-binary job spans:
   `vmafx.job.submit` (controller), `vmafx.scoring` /
   `vmafx.frame.extraction` / `vmafx.onnx.inference` (node),
   `vmafx.mcp.tool` (mcp, `tools.go::addRawTool`), `vmafx.tune.command`
   (tune, `withGolusoris`), `vmafx.onnx.inference` (tune via `pkg/ai`).
