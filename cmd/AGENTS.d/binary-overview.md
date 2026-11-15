---
paths:
  - cmd/vmafx-*/**/*.go
invariant: Every root sets EnvPrefix VMAFX_; span tests use internal/oteltest without t.Parallel.
---
<!-- markdownlint-disable MD013 MD029 MD060 -->
# Go binary composition, env prefix and test rules (`cmd/`)

| Binary             | Shape                                        | Composition                                                                   |
|--------------------|----------------------------------------------|-------------------------------------------------------------------------------|
| `vmafx-server`     | long-running fx service (gRPC + HTTP)        | `bootstrap.Base` + `bootstrap.HTTP` + `bootstrap.HTTPTracing` + `grpc.Module` |
| `vmafx-controller` | long-running fx service (gRPC + HTTP)        | same as server, plus the JWT auth interceptors                                |
| `vmafx-node`       | long-running fx service (gRPC + HTTP)        | `bootstrap.Base` + `grpc.Module` + `bootstrap.HTTP` + `bootstrap.HTTPTracing` |
| `vmafx-operator`   | long-running fx service (controller-runtime) | `bootstrap.Base` + `k8s/operator.Module`                                      |
| `vmafx-mcp`        | fx app around an MCP transport (stdio/HTTP)  | `bootstrap.Base` + hand-rolled transport lifecycle                            |
| `vmafx-tune`       | cobra CLI; one fx graph per subcommand       | `bootstrap.Base` inside `cmd/vmafx-tune/cmd/golusoris.go::withGolusoris`      |
| `vmafx-ort-runner` | one-shot subprocess, stdlib `flag` only      | none (ADR-1134)                                                               |

## Rebase-sensitive invariants

6. **`VMAFX_` env prefix everywhere** (ADR-1119 §2): every root replaces
   `config.Options` with `EnvPrefix: "VMAFX_"`. OTel knobs = `VMAFX_OTEL_*`
   (`otel.*` koanf keys) on every binary; standard
   `OTEL_EXPORTER_OTLP_*_ENDPOINT` / `OTEL_SDK_DISABLED` /
   `OTEL_SERVICE_NAME` variables work in addition. Operator guide:
   [docs/development/observability.md](../../docs/development/observability.md).

## Test requirements

```bash
go test ./internal/app/bootstrap/ ./cmd/vmafx-tune/cmd/ ./cmd/vmafx-operator/ ./pkg/ai/
# cgo packages link libvmaf from core/build-cpu/src (see go-ci.yml):
CGO_LDFLAGS="-L$PWD/core/build-cpu/src -lvmaf -lm" \
  LD_LIBRARY_PATH=$PWD/core/build-cpu/src \
  go test ./cmd/vmafx-server/ ./cmd/vmafx-controller/ ./cmd/vmafx-node/ ./cmd/vmafx-mcp/
```

Span tests install process-global recorder via `internal/oteltest`;
must not use `t.Parallel()`.
