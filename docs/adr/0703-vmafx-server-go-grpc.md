<!-- markdownlint-disable MD013 MD060 -->
# ADR-0703: vmafx-server in Go — gRPC + HTTP, observability

- **Status**: Proposed
- **Date**: 2026-05-28
- **Deciders**: lusoris
- **Tags**: `server`, `go`, `grpc`, `http`, `observability`, `cloud-native`, `vmafx`

## Context

ADR-0701 added an optional `--transport http` mode to the Python `vmaf-mcp` server (PR #1583).
The Python implementation covers the Phase-3A cloud-native goals (liveness/readiness probes,
Prometheus scraping, `/v1/score`) and remains the default transport for IDE/MCP-client
compatibility.

However, the Python server has three structural limitations that become material at scale:

1. **Startup cost**: CPython + aiohttp cold-starts in 1–3 seconds; a Go binary starts in
   under 50 ms — critical for k8s rolling deployments where readiness probe latency directly
   affects pod-swap time.
2. **Single binary**: The Python server requires a venv (~80 MB), a Python runtime, and a
   libvmaf subprocess; the Go binary links libvmaf via cgo and ships as a single ~10 MB
   distroless-compatible binary with no interpreter.
3. **gRPC**: Typed, schema-first, bidirectional-streaming-capable RPC with generated client
   stubs in any language — the Python HTTP transport speaks REST only.

The user requested a production Go gRPC + HTTP server as a Phase-4 language-modernisation
deliverable (memory `project_vmafx_phase4_language_modernization`).

## Decision

We will add `cmd/vmafx-server/` — a static Go binary that:

- Exposes a **gRPC service** (`VmafxScoring`) defined in `proto/vmafx.proto`.
- Exposes **HTTP/JSON endpoints** (`/healthz`, `/readyz`, `/metrics`, `/v1/score`) with
  full parity with the Python implementation from PR #1583.
- Links **libvmaf via cgo** (`pkg/libvmaf/`) for local-dev builds; the production path uses
  a precompiled `libvmaf.so` at `/usr/local/lib` via `LD_LIBRARY_PATH`.
- Emits **structured JSON logs** via Go 1.21+ stdlib `log/slog`.
- Exposes **Prometheus metrics** via `github.com/prometheus/client_golang`.
- Handles **SIGTERM graceful shutdown** (30 s drain window, `pkg/observability`).
- Reads **12-factor env config**: `VMAFX_PORT`, `VMAFX_GRPC_PORT`, `VMAFX_LOG_LEVEL`,
  `VMAFX_VMAF_BINARY`, `VMAFX_MODEL_DIR`.

The **Python implementation from PR #1583 stays** and remains the default transport for
`vmaf-mcp --transport http`. It will be removed in a Stage-3 cleanup PR after the Go server
reaches production parity and the Helm chart / CI matrices have been updated.

### File layout

```text
cmd/vmafx-server/
  main.go           — CLI flags, bootstrap, errgroup
  grpc_server.go    — gRPC service implementation
  http_server.go    — HTTP handlers
  main_test.go      — httptest + mock scorer integration tests
gen/go/
  vmafx.pb.go       — vendored protobuf stubs
  vmafx_grpc.pb.go  — vendored gRPC stubs
pkg/libvmaf/
  libvmaf.go        — cgo wrapper (New / Score / Close)
  libvmaf_test.go   — unit tests with vmaf CLI stub
pkg/observability/
  observability.go  — slog logger, Prometheus registry, shutdown context
proto/
  vmafx.proto       — service definition (buf-linted)
  buf.yaml          — buf lint config
buf.gen.yaml        — buf codegen config
go.mod / go.sum
Dockerfile.go-server — multi-stage: golang:1.23-bookworm → distroless
```

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Extend the Python server further (FastAPI, gRPC-gateway) | Minimal rewrite; no new language | Still Python startup cost; gRPC-gateway adds proto → OpenAPI friction; no single-binary deployment | Phase-4 goal is a Go binary per the memory entry |
| Replace Python with Go entirely (no compat layer) | Simplest final state | Breaks existing MCP/stdio integrations in flight; too risky before full parity is validated | Python layer stays until parity is confirmed; cleanup PR follows |
| Use Rust instead of Go | Best raw performance; zero-cost abstractions | No existing cgo FFI path in the fork; smaller contributor pool; significantly longer initial build | Go cgo covers the libvmaf ABI cleanly; Rust FFI is a future option if Rust tooling lands |
| HTTP only, no gRPC | Fewer dependencies | No typed schema; no generated client stubs; no streaming | gRPC was explicitly requested (Phase-4 decision) |
| Direct cgo API calls (bypass vmaf binary) | Eliminates subprocess | Reimplements vmaf.c pipeline (picture alloc, feature dispatch, pooling) — significant scope | Binary delegation is what the Python layer does; forward-compatible cgo API path left as extension |

## Consequences

- **Positive**: Single ~10 MB Go binary startable in under 50 ms; gRPC service with
  generated clients in any language; Prometheus metrics with Go runtime + process
  collectors; distroless-compatible; SIGTERM graceful shutdown drains in-flight RPCs.
- **Negative**: Go toolchain added to the build matrix; `go.mod` introduces a new
  dependency tree; cgo requires libvmaf headers at build time (covered by
  `libvmaf-dev` in the Dockerfile).
- **Neutral / follow-ups**: Python compat layer removal PR to follow once CI matrices
  confirm Go parity; Helm chart `values.yaml` `image.repository` updated to
  `ghcr.io/vmafx/vmafx-server`; Netflix golden smoke test validates the `/v1/score`
  endpoint produces the expected VMAF score (≈ 76.6683) for the canonical test pair.

## References

- [ADR-0701](0701-vmafx-cloud-native-redesign.md) — Python HTTP transport + observability
  foundation (PR #1583); this ADR is a child/successor.
- [ADR-0686](0686-vmafx-rebrand-aggressive-modernization.md) — VMAFX rebrand umbrella.
- `req` — "vmafx-server in Go (gRPC + HTTP)" (per user task specification, 2026-05-28).
