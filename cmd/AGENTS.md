# AGENTS.md — cmd/

7 Go binaries. Per-binary guides next to each `main.go`
(`cmd/vmafx-*/AGENTS.md`). Invariants spanning all binaries below.

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
   `golusoris.HTTP` puts `bootstrap.HTTPTracing` next to it (server,
   controller). Hand-rolled `*http.Server` wraps handler with
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

5. **`vmafx-ort-runner` stays OTel-free** (ADR-1134, its AGENTS.md #5).
   Inference span belongs to caller (`pkg/ai.Registry.Infer`); do not "fix"
   runner by adding init to it.

6. **`VMAFX_` env prefix everywhere** (ADR-1119 §2): every root replaces
   `config.Options` with `EnvPrefix: "VMAFX_"`. OTel knobs = `VMAFX_OTEL_*`
   (`otel.*` koanf keys) on every binary; standard
   `OTEL_EXPORTER_OTLP_*_ENDPOINT` / `OTEL_SDK_DISABLED` /
   `OTEL_SERVICE_NAME` variables work in addition. Operator guide:
   [docs/development/observability.md](../docs/development/observability.md).

7. **Server/controller scoring-service plumbing has one owner.** Prometheus
   registry construction, libvmaf scorer lifecycle, legacy health/readiness
   probes, and JSON response writing live in
   `internal/app/scoringservice`. Binary-local methods are adapters only; do
   not restore parallel implementations. The shared tests pin exact probe
   bytes and fail-loud response-write logging.

## Test requirements

```bash
go test ./internal/app/bootstrap/ ./cmd/vmafx-tune/cmd/ ./cmd/vmafx-operator/ ./pkg/ai/
# cgo packages link libvmaf from core/build-cpu/src (see go-ci.yml):
CGO_LDFLAGS=-L$PWD/core/build-cpu/src LD_LIBRARY_PATH=$PWD/core/build-cpu/src \
  go test ./cmd/vmafx-server/ ./cmd/vmafx-controller/ ./cmd/vmafx-node/ ./cmd/vmafx-mcp/
```

Span tests install process-global recorder via `internal/oteltest`;
must not use `t.Parallel()`.
