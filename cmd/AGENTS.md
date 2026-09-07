# AGENTS.md — cmd/

The seven Go binaries. Per-binary guides live next to each `main.go`
(`cmd/vmafx-*/AGENTS.md`); this file holds the invariants that span all of
them.

| Binary             | Shape                                        | Composition                                                                   |
|--------------------|----------------------------------------------|-------------------------------------------------------------------------------|
| `vmafx-server`     | long-running fx service (gRPC + HTTP)        | `bootstrap.Base` + `golusoris.HTTP` + `bootstrap.HTTPTracing` + `grpc.Module` |
| `vmafx-controller` | long-running fx service (gRPC + HTTP)        | same as server, plus the JWT auth interceptors                                |
| `vmafx-node`       | long-running fx service (gRPC only)          | `bootstrap.Base` + `grpc.Module`                                              |
| `vmafx-operator`   | long-running fx service (controller-runtime) | `bootstrap.Base` + `k8s/operator.Module`                                      |
| `vmafx-mcp`        | fx app around an MCP transport (stdio/HTTP)  | `bootstrap.Base` + hand-rolled transport lifecycle                            |
| `vmafx-tune`       | cobra CLI; one fx graph per subcommand       | `bootstrap.Base` inside `cmd/vmafx-tune/cmd/golusoris.go::withGolusoris`      |
| `vmafx-ort-runner` | one-shot subprocess, stdlib `flag` only      | none (ADR-1134)                                                               |

## Rebase-sensitive invariants

1. **Every binary initialises OpenTelemetry through the shared helper,
   `internal/app/bootstrap`, and nowhere else** (ADR-0782, ADR-1119).
   `bootstrap.Base` carries golusoris's `otel.Module` (OTLP/gRPC exporter,
   W3C propagators, fx `OnStop` flush, silent no-op without an endpoint)
   and the `withServiceIdentity` decorator that supplies `service.version`
   from `pkg/version` and honours `OTEL_SERVICE_NAME`. Do not call
   `pkg/observability.InitOTel`, `otel.New`, or `sdktrace.NewTracerProvider`
   from a `main` package — two providers cannot both be the global, and a
   second init path is exactly the boilerplate ADR-1119 removed. Each
   binary's `TestOTelWiredThroughBootstrap` locks this in (no-op providers
   without an endpoint, `service.name` = binary name, `service.version` =
   `pkg/version`).

2. **HTTP surfaces are traced through `bootstrap`, not per binary.** A
   root that wires `golusoris.HTTP` puts `bootstrap.HTTPTracing` next to it
   (server, controller); a hand-rolled `*http.Server` wraps its handler with
   `bootstrap.TraceHTTPHandler` as the outermost layer (mcp). The span name
   (`<METHOD> <path>`, Swagger subtree collapsed) and the probe/scrape
   filter (`/healthz`, `/readyz`, `/livez`, `/startupz`, `/metrics`) are
   defined once there.

3. **gRPC spans come from golusoris.** Servers get the `otelgrpc` stats
   handler from `grpc.Module`; clients dial through `grpc.NewConnFactory()`
   (operator) or attach `otelgrpc.NewClientHandler()` (`pkg/score`) so the
   `traceparent` crosses every hop (ADR-1095). Never add a bare
   `grpc.NewClient` / `grpc.DialContext` in a binary without the handler.

4. **Application spans use the ADR-0782 names from
   `pkg/observability/otel_instruments.go`** (`observability.StartSpan` /
   `EndSpan`); do not invent names inline. Per-binary job spans:
   `vmafx.job.submit` (controller), `vmafx.scoring` /
   `vmafx.frame.extraction` / `vmafx.onnx.inference` (node),
   `vmafx.mcp.tool` (mcp, `tools.go::addRawTool`), `vmafx.tune.command`
   (tune, `withGolusoris`), `vmafx.onnx.inference` (tune via `pkg/ai`).

5. **`vmafx-ort-runner` stays OTel-free** (ADR-1134, its AGENTS.md #5).
   The inference span belongs to the caller (`pkg/ai.Registry.Infer`); do
   not "fix" the runner by adding an init to it.

6. **`VMAFX_` env prefix everywhere** (ADR-1119 §2): every root replaces
   `config.Options` with `EnvPrefix: "VMAFX_"`, so the OTel knobs are
   `VMAFX_OTEL_*` (`otel.*` koanf keys) on every binary; the standard
   `OTEL_EXPORTER_OTLP_*_ENDPOINT` / `OTEL_SDK_DISABLED` /
   `OTEL_SERVICE_NAME` variables work in addition. The operator guide is
   [docs/development/observability.md](../docs/development/observability.md).

## Test requirements

```bash
go test ./internal/app/bootstrap/ ./cmd/vmafx-tune/cmd/ ./cmd/vmafx-operator/ ./pkg/ai/
# cgo packages link libvmaf from core/build-cpu/src (see go-ci.yml):
CGO_LDFLAGS=-L$PWD/core/build-cpu/src LD_LIBRARY_PATH=$PWD/core/build-cpu/src \
  go test ./cmd/vmafx-server/ ./cmd/vmafx-controller/ ./cmd/vmafx-node/ ./cmd/vmafx-mcp/
```

Span tests install a process-global recorder via `internal/oteltest`;
they must not use `t.Parallel()`.
